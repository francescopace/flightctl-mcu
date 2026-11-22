/*
 * HTTP/2 client (RFC 9113) on mbedTLS and BSD sockets (mbedtls_net), so the
 * same code runs on POSIX hosts, ESP-IDF (lwIP) and Zephyr. TLS 1.3 only,
 * with the client certificate for Flight Control; HTTP/2 is negotiated with
 * ALPN ("h2"), and a server that does not offer it is refused.
 *
 * One connection carries the requests of every thread that shares a session:
 * the agent's long-poll and its status updates go over one TLS connection.
 * There is no reader thread. A thread waiting for its response reads the
 * connection's frames while no other thread does, and hands each one to the
 * stream it belongs to; the others wait on a condition variable. Every read
 * and write of the TLS context is under one lock; waiting for data is not.
 *
 * Left out, a device being the only client: server push (refused in
 * SETTINGS), HPACK's dynamic table (size 0 announced to the server, so the
 * decoder holds no state between header blocks), priorities. Bodies are
 * read as they arrive, never a whole frame at once: the only 16 KB buffer
 * of a connection is the TLS record's.
 */
#include "fc_http.h"

#include <errno.h>
#include <pthread.h>

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if !defined(__ZEPHYR__)
#include <netdb.h>
#endif
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>

#include "fc_crypto.h"
#include "fc_port.h"
#include "fc_tls_pool.h"
#include "fc_heap_hold.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/platform.h"
#include "psa/crypto.h"
#if defined(FC_TLS_DEBUG) /* a diagnostic build: mbedTLS's own log, at this level */
#include "mbedtls/debug.h"
#endif
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#define FC_HTTP_MAX_RESPONSE (512 * 1024)
/* A response's header block, as sent (HPACK). */
#define MAX_HEADER_BLOCK 8192
/* Writes never block longer than this: on a half-open connection (WiFi lost)
 * send() would otherwise wait for TCP to give up, minutes on lwIP. */
#define SEND_TIMEOUT_S 30
/* A frame that has begun arrives whole within this. */
#define FRAME_TIMEOUT_MS 30000
/* Frames up to HTTP/2's minimum, which the server may not lower. */
#define OUR_MAX_FRAME 16384
/* The default window, ours and the server's at first. */
#define DEFAULT_WINDOW 65535
/* Received bytes are acknowledged (WINDOW_UPDATE) in steps of this. */
#define UPDATE_AT 16384
/* A reader waiting for a frame checks its deadline at least this often. */
#define SLICE_MS 1000

enum {
    F_DATA = 0,
    F_HEADERS = 1,
    F_PRIORITY = 2,
    F_RST_STREAM = 3,
    F_SETTINGS = 4,
    F_PUSH_PROMISE = 5,
    F_PING = 6,
    F_GOAWAY = 7,
    F_WINDOW_UPDATE = 8,
    F_CONTINUATION = 9,
};
#define FL_END_STREAM  0x1
#define FL_ACK         0x1
#define FL_END_HEADERS 0x4
#define FL_PADDED      0x8
#define FL_PRIORITY    0x20

#define E_NO_ERROR    0x0
#define E_PROTOCOL    0x1
#define E_FLOW        0x3
#define E_FRAME_SIZE  0x6
#define E_REFUSED     0x7
#define E_CANCEL      0x8
#define E_COMPRESSION 0x9

static const char PREFACE[] = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";

/* ---- traffic counters ----------------------------------------------------- */

/* Raw bytes on the wire (TCP payload, TLS included), for traffic figures.
 * Counted by every thread that talks to a server: under a lock, since a
 * 64-bit add is not atomic on 32-bit chips. */
static pthread_mutex_t s_count_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned long long s_tx_bytes, s_rx_bytes;
static unsigned long s_handshakes;

static void count(unsigned long long *counter, unsigned long long n)
{
    pthread_mutex_lock(&s_count_lock);
    *counter += n;
    pthread_mutex_unlock(&s_count_lock);
}

unsigned long fc_http_handshakes(void)
{
    pthread_mutex_lock(&s_count_lock);
    unsigned long n = s_handshakes;
    pthread_mutex_unlock(&s_count_lock);
    return n;
}

void fc_http_traffic(unsigned long long *tx, unsigned long long *rx)
{
    pthread_mutex_lock(&s_count_lock);
    *tx = s_tx_bytes;
    *rx = s_rx_bytes;
    pthread_mutex_unlock(&s_count_lock);
}

static int count_send(void *ctx, const unsigned char *buf, size_t len)
{
    int rc = mbedtls_net_send(ctx, buf, len);
    if (rc > 0) {
        count(&s_tx_bytes, (unsigned long long)rc);
    }
    return rc;
}

static int count_recv_timeout(void *ctx, unsigned char *buf, size_t len, uint32_t timeout)
{
    int rc = mbedtls_net_recv_timeout(ctx, buf, len, timeout);
    if (rc > 0) {
        count(&s_rx_bytes, (unsigned long long)rc);
    }
    return rc;
}

/* ---- TLS receive buffer pool (fc_tls_pool.h) ------------------------------ */

#if defined(ESP_PLATFORM) && !defined(__ZEPHYR__) /* Zephyr's ESP32 HAL defines it too */
#define TLS_HEAP_CALLOC(n, size) fc_idf_tls_heap_calloc((n), (size))
#else
#define TLS_HEAP_CALLOC(n, size) calloc((n), (size))
#endif

#ifndef FC_TLS_POOL_PORT_STORAGE
static void *fc_tls_pool_storage_alloc(void)
{
    return TLS_HEAP_CALLOC(1, FC_TLS_POOL_SLOT_SIZE);
}

static void fc_tls_pool_storage_free(void *ptr)
{
    free(ptr);
}
#endif

static pthread_mutex_t s_pool_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned char *s_slot[FC_TLS_POOL_MAX]; /* each its own block; the extra one may be NULL */
static int s_pool_used[FC_TLS_POOL_MAX];
static int s_pool_release; /* the extra block goes back to the heap when given back */
static int s_pool_now, s_pool_peak, s_pool_hooked, s_pool_logged, s_pool_short;

int fc_tls_pool_init(void)
{
    int rc = 0;
    pthread_mutex_lock(&s_pool_lock);
    for (int i = 0; i < FC_TLS_POOL_SLOTS; i++) {
        if (s_slot[i] == NULL && (s_slot[i] = fc_tls_pool_storage_alloc()) == NULL) {
            rc = -1;
        }
    }
    int hook = !s_pool_hooked;
    s_pool_hooked = 1;
    pthread_mutex_unlock(&s_pool_lock);
    if (rc != 0) {
        fc_port_log(FC_LOG_WARN, "no room for every TLS buffer (%d KB each): the rest come from the heap",
                    FC_TLS_POOL_SLOT_SIZE / 1024);
    }
#if defined(MBEDTLS_PLATFORM_MEMORY) && !defined(MBEDTLS_PLATFORM_CALLOC_MACRO)
    /* mbedTLS allocates through the pool from now on (ESP-IDF and Zephyr
     * build it with MBEDTLS_PLATFORM_MEMORY; a host's usually has not, and
     * there the pool stays unused). What it allocated before is freed by
     * fc_tls_free as well: not the pool's, so free(), as before. */
    if (hook) {
        mbedtls_platform_set_calloc_free(fc_tls_calloc, fc_tls_free);
    }
#else
    (void)hook;
#endif
    return rc;
}

/* Caller checked the size against the slot. */
static void *tls_pool_take_bytes(size_t total)
{
    unsigned char *p = NULL;
    int any = 0, first = 0, short_first = 0;
    pthread_mutex_lock(&s_pool_lock);
    for (int i = 0; i < FC_TLS_POOL_MAX; i++) {
        any |= s_slot[i] != NULL;
        if (s_slot[i] != NULL && !s_pool_used[i]) {
            s_pool_used[i] = 1;
            p = s_slot[i];
            if (++s_pool_now > s_pool_peak) {
                s_pool_peak = s_pool_now;
            }
            first = !s_pool_logged;
            s_pool_logged = 1;
            break;
        }
    }
    if (p == NULL && any && !s_pool_short) {
        short_first = s_pool_short = 1;
    }
    pthread_mutex_unlock(&s_pool_lock);
    /* Once each: that the pool serves, and that it ran short. */
    if (first) {
        fc_port_log(FC_LOG_INFO, "TLS receive buffers: from the reserved block(s)");
    }
    if (short_first) {
        fc_port_log(FC_LOG_WARN, "TLS receive buffers: all reserved blocks in use, one from the heap");
    }
    if (p != NULL) {
        memset(p, 0, total);
    }
    return p;
}

void *fc_tls_pool_take(size_t n, size_t size)
{
    if (size != 0 && n > SIZE_MAX / size) {
        return NULL;
    }
    size_t total = n * size;
    return total >= FC_TLS_POOL_MIN && total <= FC_TLS_POOL_SLOT_SIZE ? tls_pool_take_bytes(total) : NULL;
}

int fc_tls_pool_give(void *p)
{
    int mine = 0;
    unsigned char *drop = NULL;
    pthread_mutex_lock(&s_pool_lock);
    for (int i = 0; p != NULL && i < FC_TLS_POOL_MAX; i++) {
        if (s_slot[i] == p) {
            mine = 1;
            if (s_pool_used[i]) {
                s_pool_used[i] = 0;
                s_pool_now--;
            }
            if (i >= FC_TLS_POOL_SLOTS && s_pool_release) {
                drop = s_slot[i];
                s_slot[i] = NULL;
                s_pool_release = 0;
            }
            break;
        }
    }
    pthread_mutex_unlock(&s_pool_lock);
    fc_tls_pool_storage_free(drop);
    return mine;
}

int fc_tls_pool_extra(int on)
{
    const int i = FC_TLS_POOL_SLOTS; /* the extra block */
    unsigned char *drop = NULL;
    int rc = 0;
    pthread_mutex_lock(&s_pool_lock);
    if (on) {
        s_pool_release = 0;
        if (s_slot[i] == NULL && (s_slot[i] = fc_tls_pool_storage_alloc()) == NULL) {
            rc = -1;
        }
    } else if (s_slot[i] != NULL) {
        if (s_pool_used[i]) {
            s_pool_release = 1;
        } else {
            drop = s_slot[i];
            s_slot[i] = NULL;
        }
    }
    pthread_mutex_unlock(&s_pool_lock);
    fc_tls_pool_storage_free(drop);
    if (rc != 0) {
        fc_port_log(FC_LOG_WARN, "no room for a second TLS buffer (%d KB): it will come from the heap",
                    FC_TLS_POOL_SLOT_SIZE / 1024);
    }
    return rc;
}

static unsigned s_tls_alloc_failures;

void *fc_tls_calloc(size_t n, size_t size)
{
    void *p = fc_tls_pool_take(n, size);
    if (p == NULL) {
        p = TLS_HEAP_CALLOC(n, size);
#if defined(FC_IDF_DYNAMIC_TLS_RX)
        if (p == NULL && n == 1 && size > 0 && size < FC_TLS_POOL_MIN && fc_idf_allocating_tls_rx()) {
            p = tls_pool_take_bytes(size);
        }
#endif
        if (p == NULL && n && size) {
            __atomic_fetch_add(&s_tls_alloc_failures, 1, __ATOMIC_RELAXED);
        }
    }
    return p;
}

void fc_tls_free(void *p)
{
    if (p != NULL && !fc_tls_pool_give(p)) {
        free(p);
    }
}

void fc_tls_pool_usage(int *now, int *peak)
{
    pthread_mutex_lock(&s_pool_lock);
    *now = s_pool_now;
    *peak = s_pool_peak;
    pthread_mutex_unlock(&s_pool_lock);
}

/* ---- holds and the heap's low (fc_heap_hold.h) ----------------------------- */

#define HEAP_HOLDS 3 /* at once: the agent's connection, MQTT's, an app's load */

static pthread_mutex_t s_meter_lock = PTHREAD_MUTEX_INITIALIZER;
static struct {
    void *p;
    long n;
} s_holds[HEAP_HOLDS];
static long s_held;                                /* bytes held now */
static long s_low[FC_HEAP_LOWS], s_low_raw[FC_HEAP_LOWS];
static unsigned char s_low_open, s_watching;       /* stretches open (bits); the port's watch on */

/* The port's low since the last fold, the bytes held meanwhile added, into
 * every open stretch; then watching again while one is open. Between the
 * two calls, microseconds, a dip of another thread is not seen. */
static void meter_fold(void)
{
    if (s_watching) {
        long raw = fc_port_heap_watch(0);
        s_watching = 0;
        for (int i = 0; raw >= 0 && i < FC_HEAP_LOWS; i++) {
            if (s_low_open & (1u << i)) {
                if (s_low[i] < 0 || raw + s_held < s_low[i]) {
                    s_low[i] = raw + s_held;
                }
                if (s_low_raw[i] < 0 || raw < s_low_raw[i]) {
                    s_low_raw[i] = raw;
                }
            }
        }
    }
    if (s_low_open) {
        s_watching = fc_port_heap_watch(1) == 0;
    }
}

void *fc_heap_hold(size_t leave)
{
    long big = fc_port_heap_largest();
    if (big < (long)(FC_HOLD_MIN + leave)) {
        return NULL;
    }
    long n = big - (long)leave - 64;
#ifdef FC_HEAP_HOLD_MAX
    /* A PSRAM user heap can be several MiB. Preserve room for app linear
     * memory without starving concurrent TLS and storage allocations. */
    if (n > FC_HEAP_HOLD_MAX) {
        n = FC_HEAP_HOLD_MAX;
    }
#endif
    pthread_mutex_lock(&s_meter_lock);
    int slot = -1;
    for (int i = 0; i < HEAP_HOLDS && slot < 0; i++) {
        slot = s_holds[i].p == NULL ? i : -1;
    }
    void *p = NULL;
    if (slot >= 0) {
        /* Counted before it is taken: the heap's own low falls with it. */
        meter_fold();
        s_held += n;
        if ((p = malloc((size_t)n)) != NULL) {
            s_holds[slot].p = p;
            s_holds[slot].n = n;
        } else {
            s_held -= n; /* the low of a moment over by n: never under */
        }
    }
    pthread_mutex_unlock(&s_meter_lock);
    return p;
}

void *fc_heap_trim_hold(void *hold, size_t limit)
{
    pthread_mutex_lock(&s_meter_lock);
    for (int i = 0; hold && i < HEAP_HOLDS; i++) {
        if (s_holds[i].p == hold && limit > 0 && limit < (size_t)s_holds[i].n) {
            void *trimmed = realloc(hold, limit);
            if (trimmed != NULL) {
                meter_fold();
                s_held -= s_holds[i].n - (long)limit;
                s_holds[i].p = hold = trimmed;
                s_holds[i].n = (long)limit;
            }
            break;
        }
    }
    pthread_mutex_unlock(&s_meter_lock);
    return hold;
}

void fc_heap_unhold(void *hold)
{
    if (hold == NULL) {
        return;
    }
    /* Given back before it is no longer counted. */
    pthread_mutex_lock(&s_meter_lock);
    for (int i = 0; i < HEAP_HOLDS; i++) {
        if (s_holds[i].p == hold) {
            free(hold);
            meter_fold();
            s_held -= s_holds[i].n;
            s_holds[i].p = NULL;
            break;
        }
    }
    pthread_mutex_unlock(&s_meter_lock);
}

int fc_heap_low_begin(void)
{
    int k = -1;
    pthread_mutex_lock(&s_meter_lock);
    for (int i = 0; i < FC_HEAP_LOWS && k < 0; i++) {
        k = s_low_open & (1u << i) ? -1 : i;
    }
    if (k >= 0) {
        meter_fold();
        s_low_open |= (unsigned char)(1u << k);
        s_low[k] = s_low_raw[k] = -1;
        if (!s_watching) {
            s_watching = fc_port_heap_watch(1) == 0;
        }
        if (!s_watching) {
            s_low_open &= (unsigned char)~(1u << k); /* the port cannot watch (or another caller does) */
            k = -1;
        }
    }
    pthread_mutex_unlock(&s_meter_lock);
    return k;
}

static long low_read(int k, long *raw, int end)
{
    long low = -1, r = -1;
    pthread_mutex_lock(&s_meter_lock);
    if (k >= 0 && k < FC_HEAP_LOWS && (s_low_open & (1u << k))) {
        meter_fold();
        low = s_low[k];
        r = s_low_raw[k];
        if (end) {
            s_low_open &= (unsigned char)~(1u << k);
            if (!s_low_open && s_watching) {
                fc_port_heap_watch(0);
                s_watching = 0;
            }
        }
    }
    pthread_mutex_unlock(&s_meter_lock);
    if (raw != NULL) {
        *raw = r;
    }
    return low;
}

long fc_heap_low_peek(int stretch, long *raw)
{
    return low_read(stretch, raw, 0);
}

long fc_heap_low_end(int stretch, long *raw)
{
    return low_read(stretch, raw, 1);
}

/* ---- init and logging ----------------------------------------------------- */

static int s_inited;

int fc_http_global_init(void)
{
    if (s_inited) {
        return 0;
    }
#if defined(SIGPIPE) && !defined(ESP_PLATFORM)
    /* A write on a connection the server already closed must fail, not kill
     * the process (POSIX hosts; lwIP has no signals). */
    signal(SIGPIPE, SIG_IGN);
#endif
    fc_tls_pool_init();
    /* TLS, keys and the random generator are all PSA's: one that does not
     * start (no entropy, for one) is found here, with a line in the log. */
    unsigned char probe[4];
    if (fc_random(probe, sizeof(probe)) != 0) {
        fc_port_log(FC_LOG_ERROR, "the cryptography does not start (PSA: no random numbers)");
        return -1;
    }
    s_inited = 1;
    return 0;
}

static void log_mbedtls(const char *what, int rc)
{
    char buf[128];
    /* mbedtls_strerror does not know PSA's codes; this one is the usual
     * cause on a small heap. */
    if (rc == PSA_ERROR_INSUFFICIENT_MEMORY) {
        snprintf(buf, sizeof(buf), "out of memory");
    } else {
        mbedtls_strerror(rc, buf, sizeof(buf));
    }
    fc_port_log(FC_LOG_WARN, "%s: -0x%04x %s", what, -rc, buf);
}

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* ---- HPACK (RFC 7541): the decoder for responses, literals for requests --- */

/* The static table (Appendix A); names, and values where it has them. */
static const char *const HPACK_STATIC[62][2] = {
    {NULL, NULL},
    {":authority", ""},
    {":method", "GET"},
    {":method", "POST"},
    {":path", "/"},
    {":path", "/index.html"},
    {":scheme", "http"},
    {":scheme", "https"},
    {":status", "200"},
    {":status", "204"},
    {":status", "206"},
    {":status", "304"},
    {":status", "400"},
    {":status", "404"},
    {":status", "500"},
    {"accept-charset", ""},
    {"accept-encoding", "gzip, deflate"},
    {"accept-language", ""},
    {"accept-ranges", ""},
    {"accept", ""},
    {"access-control-allow-origin", ""},
    {"age", ""},
    {"allow", ""},
    {"authorization", ""},
    {"cache-control", ""},
    {"content-disposition", ""},
    {"content-encoding", ""},
    {"content-language", ""},
    {"content-length", ""},
    {"content-location", ""},
    {"content-range", ""},
    {"content-type", ""},
    {"cookie", ""},
    {"date", ""},
    {"etag", ""},
    {"expect", ""},
    {"expires", ""},
    {"from", ""},
    {"host", ""},
    {"if-match", ""},
    {"if-modified-since", ""},
    {"if-none-match", ""},
    {"if-range", ""},
    {"if-unmodified-since", ""},
    {"last-modified", ""},
    {"link", ""},
    {"location", ""},
    {"max-forwards", ""},
    {"proxy-authenticate", ""},
    {"proxy-authorization", ""},
    {"range", ""},
    {"referer", ""},
    {"refresh", ""},
    {"retry-after", ""},
    {"server", ""},
    {"set-cookie", ""},
    {"strict-transport-security", ""},
    {"transfer-encoding", ""},
    {"user-agent", ""},
    {"vary", ""},
    {"via", ""},
    {"www-authenticate", ""},
};
#define HPACK_STATIC_LAST 61

/* The Huffman code (Appendix B) is canonical: codes of the same length are
 * consecutive, in the order of their symbols. So the code is given by how
 * many symbols each length has and the symbols in code order; EOS is the
 * one 30-bit code past the last. */
static const uint8_t HUFF_COUNT[30] = {0, 0, 0, 0, 10, 26, 32, 6, 0, 5, 3, 2, 6, 2, 3,
                                       0, 0, 0, 3, 8, 13, 26, 29, 12, 4, 15, 19, 29, 0, 3};
static const uint8_t HUFF_SYM[256] = {
    48,  49,  50,  97,  99,  101, 105, 111, 115, 116, 32,  37,  45,  46,  47,  51,
    52,  53,  54,  55,  56,  57,  61,  65,  95,  98,  100, 102, 103, 104, 108, 109,
    110, 112, 114, 117, 58,  66,  67,  68,  69,  70,  71,  72,  73,  74,  75,  76,
    77,  78,  79,  80,  81,  82,  83,  84,  85,  86,  87,  89,  106, 107, 113, 118,
    119, 120, 121, 122, 38,  42,  44,  59,  88,  90,  33,  34,  40,  41,  63,  39,
    43,  124, 35,  62,  0,   36,  64,  91,  93,  126, 94,  125, 60,  96,  123, 92,
    195, 208, 128, 130, 131, 162, 184, 194, 224, 226, 153, 161, 167, 172, 176, 177,
    179, 209, 216, 217, 227, 229, 230, 129, 132, 133, 134, 136, 146, 154, 156, 160,
    163, 164, 169, 170, 173, 178, 181, 185, 186, 187, 189, 190, 196, 198, 228, 232,
    233, 1,   135, 137, 138, 139, 140, 141, 143, 147, 149, 150, 151, 152, 155, 157,
    158, 165, 166, 168, 174, 175, 180, 182, 183, 188, 191, 197, 231, 239, 9,   142,
    144, 145, 148, 159, 171, 206, 215, 225, 236, 237, 199, 207, 234, 235, 192, 193,
    200, 201, 202, 205, 210, 213, 218, 219, 238, 240, 242, 243, 255, 203, 204, 211,
    212, 214, 221, 222, 223, 241, 244, 245, 246, 247, 248, 250, 251, 252, 253, 254,
    2,   3,   4,   5,   6,   7,   8,   11,  12,  14,  15,  16,  17,  18,  19,  20,
    21,  23,  24,  25,  26,  27,  28,  29,  30,  31,  127, 220, 249, 10,  13,  22,
};

/* Decodes n bytes of Huffman code into out (out_cap bytes, NUL added).
 * Returns the decoded length, or -1: a code that is not a symbol, EOS, a
 * symbol past out_cap, or padding that is not up to 7 bits of ones. */
static long huff_decode(const uint8_t *in, size_t n, char *out, size_t out_cap)
{
    size_t len = 0;
    uint32_t code = 0, first = 0, raw = 0;
    int bits = 0;  /* bits of the current code so far */
    size_t index = 0;
    for (size_t i = 0; i < n; i++) {
        for (int b = 7; b >= 0; b--) {
            int bit = (in[i] >> b) & 1;
            code = (code << 1) | (uint32_t)bit;
            raw = (raw << 1) | (uint32_t)bit;
            bits++;
            uint32_t cnt = HUFF_COUNT[bits - 1];
            if (code - first < cnt) {
                if (len + 1 >= out_cap) {
                    return -1;
                }
                out[len++] = (char)HUFF_SYM[index + (code - first)];
                code = first = raw = 0;
                bits = 0;
                index = 0;
                continue;
            }
            index += cnt;
            first = (first + cnt) << 1;
            if (bits == 30) {
                return -1; /* EOS, or no such code */
            }
        }
    }
    /* What is left must be the start of EOS (all ones), shorter than a byte. */
    if (bits > 7 || raw != (bits ? (1u << bits) - 1 : 0)) {
        return -1;
    }
    out[len] = '\0';
    return (long)len;
}

/* An integer with an N-bit prefix (5.1). Returns bytes used, or -1. */
static int hpack_int(const uint8_t *p, size_t n, int prefix, uint32_t *out)
{
    if (n == 0) {
        return -1;
    }
    uint32_t max = (1u << prefix) - 1, v = p[0] & max;
    if (v < max) {
        *out = v;
        return 1;
    }
    for (size_t i = 1; i < n && i < 5; i++) {
        v += (uint32_t)(p[i] & 0x7f) << (7 * (i - 1));
        if (!(p[i] & 0x80)) {
            if (v > (1u << 24)) {
                return -1;
            }
            *out = v;
            return (int)i + 1;
        }
    }
    return -1;
}

/* A string literal (5.2), decoded into a new NUL-terminated string. Returns
 * bytes used, or -1. */
static int hpack_str(const uint8_t *p, size_t n, char **out)
{
    uint32_t len;
    int used = hpack_int(p, n, 7, &len);
    if (used < 0 || len > n - (size_t)used) {
        return -1;
    }
    const uint8_t *s = p + used;
    if (p[0] & 0x80) {
        size_t cap = (size_t)len * 8 / 5 + 2; /* the shortest code is 5 bits */
        *out = malloc(cap);
        if (*out == NULL || huff_decode(s, len, *out, cap) < 0) {
            free(*out);
            *out = NULL;
            return -1;
        }
    } else {
        *out = malloc((size_t)len + 1);
        if (*out == NULL) {
            return -1;
        }
        memcpy(*out, s, len);
        (*out)[len] = '\0';
    }
    return used + (int)len;
}

typedef struct {
    int status;             /* :status, 0 if none */
    long content_length;    /* -1 if none */
    char *www_authenticate; /* or NULL */
    int grpc_status;        /* grpc-status, -1 if none */
} hpack_resp_t;

static void hpack_take(hpack_resp_t *h, const char *name, char *value)
{
    if (value == NULL) {
        return;
    }
    if (strcmp(name, ":status") == 0) {
        h->status = atoi(value);
    } else if (strcmp(name, "content-length") == 0) {
        h->content_length = atol(value);
    } else if (strcmp(name, "grpc-status") == 0) {
        h->grpc_status = atoi(value);
    } else if (strcmp(name, "www-authenticate") == 0 && h->www_authenticate == NULL) {
        h->www_authenticate = value;
        return;
    }
    free(value);
}

/* Decodes a whole header block. Only the static table: a reference to the
 * dynamic one, which the device announced at size 0, is an error. */
static int hpack_decode(const uint8_t *p, size_t n, hpack_resp_t *h)
{
    h->status = 0;
    h->content_length = -1;
    h->www_authenticate = NULL;
    h->grpc_status = -1;
    size_t off = 0;
    while (off < n) {
        uint8_t b = p[off];
        uint32_t idx;
        int used;
        if (b & 0x80) { /* indexed field */
            if ((used = hpack_int(p + off, n - off, 7, &idx)) < 0 || idx == 0 || idx > HPACK_STATIC_LAST) {
                return -1;
            }
            hpack_take(h, HPACK_STATIC[idx][0], strdup(HPACK_STATIC[idx][1]));
            off += (size_t)used;
            continue;
        }
        if ((b & 0xe0) == 0x20) { /* dynamic table size update: ours is 0 */
            if ((used = hpack_int(p + off, n - off, 5, &idx)) < 0) {
                return -1;
            }
            off += (size_t)used;
            continue;
        }
        /* A literal: with incremental indexing (01), without (0000) or never
         * indexed (0001). Nothing is kept: the table is size 0. */
        int prefix = (b & 0xc0) == 0x40 ? 6 : 4;
        if ((used = hpack_int(p + off, n - off, prefix, &idx)) < 0 || idx > HPACK_STATIC_LAST) {
            return -1;
        }
        off += (size_t)used;
        char *name = NULL, *value = NULL;
        if (idx == 0) {
            if ((used = hpack_str(p + off, n - off, &name)) < 0) {
                return -1;
            }
            off += (size_t)used;
        }
        if ((used = hpack_str(p + off, n - off, &value)) < 0) {
            free(name);
            return -1;
        }
        off += (size_t)used;
        hpack_take(h, name ? name : HPACK_STATIC[idx][0], value);
        free(name);
    }
    return 0;
}

/* Request side: literals without indexing and without Huffman, indexed
 * fields for the static table's full matches. */
static size_t hpack_put_int(uint8_t *p, uint8_t first, int prefix, uint32_t v)
{
    uint32_t max = (1u << prefix) - 1;
    if (v < max) {
        p[0] = first | (uint8_t)v;
        return 1;
    }
    p[0] = first | (uint8_t)max;
    v -= max;
    size_t n = 1;
    while (v >= 0x80) {
        p[n++] = (uint8_t)(v & 0x7f) | 0x80;
        v >>= 7;
    }
    p[n++] = (uint8_t)v;
    return n;
}

/* A literal field without indexing (0000), or never indexed (0001) for a
 * secret, with the name from the static table. */
static size_t hpack_put_field(uint8_t *p, uint32_t name_idx, const char *value, int secret)
{
    size_t vl = strlen(value);
    size_t n = hpack_put_int(p, secret ? 0x10 : 0x00, 4, name_idx);
    n += hpack_put_int(p + n, 0x00, 7, (uint32_t)vl);
    memcpy(p + n, value, vl);
    return n + vl;
}

/* A literal field without indexing (0000) with a new name, lower case. */
static size_t hpack_put_named(uint8_t *p, const char *name, const char *value)
{
    size_t nl = strlen(name), vl = strlen(value);
    size_t n = 0;
    p[n++] = 0x00;
    n += hpack_put_int(p + n, 0x00, 7, (uint32_t)nl);
    memcpy(p + n, name, nl);
    n += nl;
    n += hpack_put_int(p + n, 0x00, 7, (uint32_t)vl);
    memcpy(p + n, value, vl);
    return n + vl;
}

/* ---- transport ------------------------------------------------------------ */

typedef struct conn conn_t;

/* How a connection moves bytes: TLS, or something else in the tests. */
typedef struct {
    /* Reads what is there, up to len: >0 bytes, 0 closed, <0 error. Called
     * when wait() said there is data, or in the middle of a frame. */
    int (*read)(conn_t *c, unsigned char *buf, size_t len);
    /* Writes up to len: >0 bytes written, <0 error. */
    int (*write)(conn_t *c, const unsigned char *buf, size_t len);
    /* 1 if there is something to read, 0 if not within timeout_ms, <0 error. */
    int (*wait)(conn_t *c, int timeout_ms);
    void (*close)(conn_t *c);
} transport_t;

typedef struct stream {
    struct stream *next;
    uint32_t id;
    enum { S_OPEN, S_DONE, S_FAILED } state;
    int got_frame;       /* the server answered something: not to be repeated */
    int retryable;       /* failed before the server took it */
    int busy;            /* the reader is handing it data, unlocked */
    int headers_done;    /* the final response's headers arrived */
    int end_with_headers; /* END_STREAM came on a HEADERS frame awaiting CONTINUATION */
    int status;
    long content_length;
    char *www_authenticate;
    uint8_t *hblock;     /* header block being assembled */
    uint16_t hlen, hcap; /* MAX_HEADER_BLOCK bounds both */
    /* The body: kept (buffered request) or handed on (streamed GET). */
    fc_http_data_cb on_data;
    void *ctx;
    int aborted;         /* on_data asked to stop */
    int discard_200_body;
    fc_http_data_cb receive_200;
    void *receive_ctx;
    char *body;
    size_t body_len, body_cap;
    long long got;       /* body bytes */
    int64_t send_window;
    uint32_t recv_unacked;
    int timeout_s;
    long long deadline;  /* no frame for it by then: timeout */
    int grpc_status_p1;  /* grpc-status + 1, 0 if none came */
    /* A bidirectional stream (fc_http_stream_*): what came and was not read
     * yet. Its window is credited as it arrives, so it is bounded here. */
    uint8_t *ring;
    size_t ring_cap, ring_head, ring_len;
} stream_t;

struct conn {
    const transport_t *tp;
    void *tctx; /* the tests' transport */
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca, own;
    mbedtls_pk_context key;

    pthread_mutex_t lock; /* everything below */
    pthread_cond_t cond;  /* a stream changed, the reader left, a window opened */
    pthread_mutex_t io;   /* the transport: one frame written or read at a time */
    int refs;             /* the session's and each request's */
    int dead;             /* no more frames: every open stream failed */
    int goaway;           /* the server is closing it: no new streams */
    int retired;          /* the session moved on: no new streams */
    int reading;          /* a thread reads frames */
    uint32_t next_id;
    uint32_t peer_max_frame;
    int64_t peer_init_window;
    int64_t send_window;
    uint32_t recv_unacked;
    uint32_t cont_id;     /* CONTINUATION expected for this stream, or 0 */
    stream_t *streams;
    char host[128];       /* what it was opened for */
    int port;
    uint32_t cert_id;
};

/* TLS session resumption is not used: Flight Control 1.3.1 issued tickets
 * but never resumed a session with them, so every reconnection is a full
 * handshake. Keeping the connection open is what saves the traffic. */
struct fc_http_session {
    pthread_mutex_t lock;
    conn_t *conn; /* open connection, or NULL */
};

/* Whether fd has something to read within timeout_ms: 1, 0, or -1. With
 * select(), which mbedTLS's own reads use: ESP-IDF's sockets have no
 * mbedtls_net_poll. */
static int fd_readable(int fd, int timeout_ms)
{
    fd_set rf;
    FD_ZERO(&rf);
    FD_SET(fd, &rf);
    struct timeval tv = {.tv_sec = timeout_ms / 1000, .tv_usec = (timeout_ms % 1000) * 1000};
    int rc = select(fd + 1, &rf, NULL, NULL, &tv);
    if (rc < 0) {
        return errno == EINTR ? 0 : -1;
    }
    return rc > 0 ? 1 : 0;
}

static int tls_read(conn_t *c, unsigned char *buf, size_t len)
{
    for (;;) {
        int rc = mbedtls_ssl_read(&c->ssl, buf, len);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE ||
            rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            if (mbedtls_ssl_get_bytes_avail(&c->ssl) == 0 && !mbedtls_ssl_check_pending(&c->ssl) &&
                fd_readable(c->net.fd, 0) == 0) {
                /* A record that carried no data: nothing more for now. */
                return MBEDTLS_ERR_SSL_WANT_READ;
            }
            continue;
        }
        if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == MBEDTLS_ERR_NET_CONN_RESET) {
            return 0;
        }
        return rc;
    }
}

static int tls_write(conn_t *c, const unsigned char *buf, size_t len)
{
    for (;;) {
        int rc = mbedtls_ssl_write(&c->ssl, buf, len);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        return rc;
    }
}

static int tls_wait(conn_t *c, int timeout_ms)
{
    if (mbedtls_ssl_get_bytes_avail(&c->ssl) > 0 || mbedtls_ssl_check_pending(&c->ssl)) {
        return 1;
    }
    return fd_readable(c->net.fd, timeout_ms);
}

static void tls_close(conn_t *c)
{
    mbedtls_ssl_close_notify(&c->ssl);
    mbedtls_net_free(&c->net);
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_x509_crt_free(&c->ca);
    mbedtls_x509_crt_free(&c->own);
    mbedtls_pk_free(&c->key);
}

static const transport_t TLS = {tls_read, tls_write, tls_wait, tls_close};

/* FNV-1a of the client certificate. Compared by content, not by pointer:
 * after a renewal the old certificate is freed and its address reused. */
static uint32_t cert_id(const char *pem)
{
    uint32_t h = 2166136261u;
    for (const char *p = pem; p && *p; p++) {
        h = (h ^ (uint8_t)*p) * 16777619u;
    }
    return h;
}

static conn_t *conn_new(const transport_t *tp, void *tctx)
{
    conn_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        return NULL;
    }
    c->tp = tp;
    c->tctx = tctx;
    /* Where mutexes come from a fixed pool (Zephyr), running out is said
     * here, not found later as a failing TLS handshake. */
    if (pthread_mutex_init(&c->lock, NULL) != 0) {
        free(c);
        fc_port_log(FC_LOG_ERROR, "http2: no mutex left for a connection");
        return NULL;
    }
    if (pthread_cond_init(&c->cond, NULL) != 0) {
        pthread_mutex_destroy(&c->lock);
        free(c);
        fc_port_log(FC_LOG_ERROR, "http2: no condition variable left for a connection");
        return NULL;
    }
    if (pthread_mutex_init(&c->io, NULL) != 0) {
        pthread_cond_destroy(&c->cond);
        pthread_mutex_destroy(&c->lock);
        free(c);
        fc_port_log(FC_LOG_ERROR, "http2: no mutex left for a connection");
        return NULL;
    }
    c->refs = 1;
    c->next_id = 1;
    c->peer_max_frame = OUR_MAX_FRAME;
    c->peer_init_window = DEFAULT_WINDOW;
    c->send_window = DEFAULT_WINDOW;
    return c;
}

static int send_frame_locked_io(conn_t *c, uint8_t type, uint8_t flags, uint32_t sid, const void *payload,
                                size_t len);

static void conn_free(conn_t *c)
{
    if (!c->dead) {
        /* Closed by this side: GOAWAY, no error, nothing processed past 0. */
        static const uint8_t bye[8] = {0};
        pthread_mutex_lock(&c->io);
        send_frame_locked_io(c, F_GOAWAY, 0, 0, bye, sizeof(bye));
        pthread_mutex_unlock(&c->io);
    }
    c->tp->close(c);
    pthread_mutex_destroy(&c->lock);
    pthread_cond_destroy(&c->cond);
    pthread_mutex_destroy(&c->io);
    free(c);
}

/* Drops a reference; the last one closes the connection. */
static void conn_put(conn_t *c)
{
    pthread_mutex_lock(&c->lock);
    int last = --c->refs == 0;
    pthread_mutex_unlock(&c->lock);
    if (last) {
        conn_free(c);
    }
}

/* With c->lock held: no more frames. Every open stream fails; one that the
 * server never answered can be sent again on another connection. */
static void conn_kill_locked(conn_t *c, const char *why)
{
    if (!c->dead) {
        fc_port_log(FC_LOG_DEBUG, "http2 connection to %s closed: %s", c->host, why);
    }
    c->dead = 1;
    for (stream_t *s = c->streams; s != NULL; s = s->next) {
        if (s->state == S_OPEN) {
            s->state = S_FAILED;
            s->retryable = !s->got_frame;
        }
    }
    pthread_cond_broadcast(&c->cond);
}

/* Writes a frame whole, under the transport lock. */
static int send_frame_locked_io(conn_t *c, uint8_t type, uint8_t flags, uint32_t sid, const void *payload,
                                size_t len)
{
    uint8_t h[9] = {(uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len, type, flags,
                    (uint8_t)(sid >> 24 & 0x7f), (uint8_t)(sid >> 16), (uint8_t)(sid >> 8), (uint8_t)sid};
    const uint8_t *parts[2] = {h, payload};
    size_t lens[2] = {sizeof(h), len};
    for (int i = 0; i < 2; i++) {
        size_t off = 0;
        while (off < lens[i]) {
            int rc = c->tp->write(c, parts[i] + off, lens[i] - off);
            if (rc <= 0) {
                if (rc < 0) {
                    log_mbedtls("write", rc);
                }
                return -1;
            }
            off += (size_t)rc;
        }
    }
    return 0;
}

static int send_frame(conn_t *c, uint8_t type, uint8_t flags, uint32_t sid, const void *payload, size_t len)
{
    pthread_mutex_lock(&c->io);
    int rc = send_frame_locked_io(c, type, flags, sid, payload, len);
    pthread_mutex_unlock(&c->io);
    if (rc != 0) {
        pthread_mutex_lock(&c->lock);
        conn_kill_locked(c, "write failed");
        pthread_mutex_unlock(&c->lock);
    }
    return rc;
}

static void send_u32(conn_t *c, uint8_t type, uint32_t sid, uint32_t v)
{
    uint8_t p[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    send_frame(c, type, 0, sid, p, sizeof(p));
}

/* The connection preface and our SETTINGS: no push, no dynamic table, no
 * stream opened by the server; header lists up to what the device keeps. */
static int send_preface(conn_t *c)
{
    static const uint8_t settings[] = {
        0, 1, 0, 0, 0, 0,                    /* HEADER_TABLE_SIZE 0 */
        0, 2, 0, 0, 0, 0,                    /* ENABLE_PUSH 0 */
        0, 3, 0, 0, 0, 0,                    /* MAX_CONCURRENT_STREAMS 0 (from the server) */
        0, 6, 0, 0, MAX_HEADER_BLOCK >> 8, 0, /* MAX_HEADER_LIST_SIZE */
    };
    pthread_mutex_lock(&c->io);
    size_t off = 0, n = sizeof(PREFACE) - 1;
    int rc = 0;
    while (off < n && rc >= 0) {
        rc = c->tp->write(c, (const uint8_t *)PREFACE + off, n - off);
        off += rc > 0 ? (size_t)rc : 0;
        if (rc == 0) {
            rc = -1;
        }
    }
    if (rc >= 0) {
        rc = send_frame_locked_io(c, F_SETTINGS, 0, 0, settings, sizeof(settings));
    }
    pthread_mutex_unlock(&c->io);
    return rc < 0 ? -1 : 0;
}

#if defined(FC_TLS_DEBUG)
static void tls_debug(void *ctx, int level, const char *file, int line, const char *msg)
{
    const char *f = strrchr(file, '/');
    fc_port_log(FC_LOG_INFO, "mbedtls %s:%d %s", f ? f + 1 : file, line, msg);
}
#endif

/* mbedtls_net_connect, given up after timeout_s: a host that drops the
 * packets would otherwise hold the caller for the system's TCP timeout. On
 * Zephyr the port's mbedtls_net_connect (fc_net_zephyr.c) is bounded already,
 * by CONFIG_NET_SOCKETS_CONNECT_TIMEOUT. */
#if defined(__ZEPHYR__)
int fc_net_connect_within(mbedtls_net_context *ctx, const char *host, const char *port, int timeout_s)
{
    return mbedtls_net_connect(ctx, host, port, MBEDTLS_NET_PROTO_TCP);
}
#else
int fc_net_connect_within(mbedtls_net_context *ctx, const char *host, const char *port, int timeout_s)
{
    struct addrinfo hints, *list = NULL;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    if (getaddrinfo(host, port, &hints, &list) != 0 || list == NULL) {
        return MBEDTLS_ERR_NET_UNKNOWN_HOST;
    }
    int ret = MBEDTLS_ERR_NET_CONNECT_FAILED;
    for (struct addrinfo *a = list; a != NULL && ret != 0; a = a->ai_next) {
        ctx->fd = (int)socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (ctx->fd < 0) {
            ret = MBEDTLS_ERR_NET_SOCKET_FAILED;
            continue;
        }
        int ok = mbedtls_net_set_nonblock(ctx) == 0;
        if (ok && connect(ctx->fd, a->ai_addr, a->ai_addrlen) != 0) {
            ok = errno == EINPROGRESS;
            if (ok) {
                fd_set w;
                FD_ZERO(&w);
                FD_SET(ctx->fd, &w);
                struct timeval tv = {.tv_sec = timeout_s};
                int err = 0;
                socklen_t len = sizeof(err);
                ok = select(ctx->fd + 1, NULL, &w, NULL, &tv) == 1 &&
                     getsockopt(ctx->fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 && err == 0;
            }
        }
        if (ok && mbedtls_net_set_block(ctx) == 0) {
            ret = 0;
        } else {
            mbedtls_net_free(ctx);
        }
    }
    freeaddrinfo(list);
    return ret;
}
#endif

static int conn_tls_open(conn_t *c, const fc_http_target_t *t)
{
    mbedtls_net_init(&c->net);
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_x509_crt_init(&c->ca);
    mbedtls_x509_crt_init(&c->own);
    mbedtls_pk_init(&c->key);
    if (fc_http_global_init() != 0) {
        return -1;
    }
    if (t->ca_pem == NULL) {
        fc_port_log(FC_LOG_ERROR, "%s: no CA, and only TLS is spoken", t->host);
        return -1;
    }
    char port[8];
    snprintf(port, sizeof(port), "%d", t->port);
    int rc;
    if ((rc = mbedtls_x509_crt_parse(&c->ca, (const unsigned char *)t->ca_pem, strlen(t->ca_pem) + 1)) != 0) {
        log_mbedtls("parse CA", rc);
        return -1;
    }
    if ((rc = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        log_mbedtls("ssl_config_defaults", rc);
        return -1;
    }
    mbedtls_ssl_conf_min_tls_version(&c->conf, MBEDTLS_SSL_VERSION_TLS1_3);
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
    mbedtls_ssl_conf_read_timeout(&c->conf, (uint32_t)t->timeout_s * 1000);
#if defined(FC_TLS_DEBUG)
    mbedtls_ssl_conf_dbg(&c->conf, tls_debug, NULL);
    mbedtls_debug_set_threshold(FC_TLS_DEBUG);
#endif
    static const char *alpn[] = {"h2", NULL};
    if ((rc = mbedtls_ssl_conf_alpn_protocols(&c->conf, alpn)) != 0) {
        log_mbedtls("alpn", rc);
        return -1;
    }
    if (t->cert_pem != NULL) {
        if ((rc = mbedtls_x509_crt_parse(&c->own, (const unsigned char *)t->cert_pem, strlen(t->cert_pem) + 1)) !=
            0) {
            log_mbedtls("parse client cert", rc);
            return -1;
        }
        if (t->hw_key) {
            if (fc_port_hwkey_pk(&c->key, &c->own.pk) != 0) {
                fc_port_log(FC_LOG_ERROR, "device key in hardware unavailable");
                return -1;
            }
        } else if ((rc = mbedtls_pk_parse_key(&c->key, (const unsigned char *)t->key_pem, strlen(t->key_pem) + 1,
                                              NULL, 0)) != 0) {
            log_mbedtls("parse client key", rc);
            return -1;
        }
        if ((rc = mbedtls_ssl_conf_own_cert(&c->conf, &c->own, &c->key)) != 0) {
            log_mbedtls("ssl_conf_own_cert", rc);
            return -1;
        }
    }
    if ((rc = mbedtls_ssl_setup(&c->ssl, &c->conf)) != 0) {
        log_mbedtls("ssl_setup", rc);
        return -1;
    }
    /* SNI is required: the Envoy Gateway routes TLS by hostname. */
    if ((rc = mbedtls_ssl_set_hostname(&c->ssl, t->host)) != 0) {
        log_mbedtls("ssl_set_hostname", rc);
        return -1;
    }
    rc = t->connect_timeout_s > 0 ? fc_net_connect_within(&c->net, t->host, port, t->connect_timeout_s)
                                  : mbedtls_net_connect(&c->net, t->host, port, MBEDTLS_NET_PROTO_TCP);
    if (rc != 0) {
        log_mbedtls("connect", rc);
        return -1;
    }
    struct timeval tv = {.tv_sec = SEND_TIMEOUT_S};
    setsockopt(c->net.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    mbedtls_ssl_set_bio(&c->ssl, &c->net, count_send, NULL, count_recv_timeout);
    unsigned failures = __atomic_load_n(&s_tls_alloc_failures, __ATOMIC_RELAXED);
    while ((rc = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            if (failures != __atomic_load_n(&s_tls_alloc_failures, __ATOMIC_RELAXED)) {
                fc_port_log(FC_LOG_WARN, "TLS allocation failed during handshake (TLS error -0x%04x)", (unsigned)-rc);
            } else {
                log_mbedtls("handshake", rc);
            }
            long largest = fc_port_heap_largest();
            if (largest >= 0) {
                fc_port_log(FC_LOG_WARN, "TLS handshake failed (free %ld B, largest %ld B)", fc_port_heap_free_now(),
                            largest);
            }
            /* All bits set: the handshake failed before the certificate
             * was verified, so there is nothing to tell. */
            uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);
            if (flags != 0 && flags != UINT32_MAX) {
                char vbuf[256];
                mbedtls_x509_crt_verify_info(vbuf, sizeof(vbuf), "  ", flags);
                fc_port_log(FC_LOG_WARN, "certificate verification:\n%s", vbuf);
            }
            return -1;
        }
    }
    pthread_mutex_lock(&s_count_lock);
    s_handshakes++;
    pthread_mutex_unlock(&s_count_lock);
    const char *proto = mbedtls_ssl_get_alpn_protocol(&c->ssl);
    if (proto == NULL || strcmp(proto, "h2") != 0) {
        fc_port_log(FC_LOG_ERROR, "%s:%d does not speak HTTP/2 (ALPN: %s)", t->host, t->port,
                    proto ? proto : "none");
        return -1;
    }
    /* From now on a read waits only for the rest of a record that began. */
    mbedtls_ssl_conf_read_timeout(&c->conf, FRAME_TIMEOUT_MS);
    return 0;
}

/* Opens a connection for t; the tests put their own transport here. */
static conn_t *(*s_open_hook)(const fc_http_target_t *t);

/* *hold (fc_heap_hold.h), when there is one, is given back once the TLS
 * handshake is over: what the connection keeps is allocated by then, and the
 * rest of the heap is everyone's again. Held while the HTTP/2 preface went out,
 * it left an emulated ESP32-C6 (release build, an app running) with 2.9 KB of
 * internal heap, too little for lwIP and the WiFi driver to take the ACK: the
 * preface's segment was sent again and again, and its write, under the
 * session's lock, kept the agent's two threads silent for minutes. When the
 * preface's TLS buffer (4.4 KB) did not fit, the write failed instead and the
 * whole connection was opened a second time. */
static conn_t *conn_open_once(const fc_http_target_t *t, void **hold)
{
    conn_t *c = s_open_hook ? s_open_hook(t) : conn_new(&TLS, NULL);
    if (c == NULL) {
        return NULL;
    }
    if (!s_open_hook && conn_tls_open(c, t) != 0) {
        c->dead = 1; /* nothing to say goodbye on */
        conn_free(c);
        return NULL;
    }
    fc_heap_unhold(*hold);
    *hold = NULL;
    snprintf(c->host, sizeof(c->host), "%s", t->host);
    c->port = t->port;
    c->cert_id = cert_id(t->cert_pem);
    if (send_preface(c) != 0) {
        c->dead = 1;
        conn_free(c);
        return NULL;
    }
    return c;
}

/* What a connection keeps for as long as it lives (its TLS context, the
 * HTTP/2 state) goes around the heap's largest block (fc_heap_hold.h). */
static conn_t *conn_open(const fc_http_target_t *t)
{
    /* A TLS handshake needs more than the smaller holes may have: 24 KB of
     * the block are left to it (all of it held, an ESP32-C5 failed its first
     * connection after boot, with 15 KB outside). */
    void *hold = fc_heap_hold(24 * 1024);
    conn_t *c = conn_open_once(t, &hold);
    if (c == NULL && hold != NULL) {
        /* The TLS connection did not open with the block held. */
        fc_heap_unhold(hold);
        hold = NULL;
        c = conn_open_once(t, &hold);
    }
    fc_heap_unhold(hold);
    return c;
}

/* ---- reading frames ------------------------------------------------------- */

static stream_t *find_stream(conn_t *c, uint32_t id)
{
    for (stream_t *s = c->streams; s != NULL; s = s->next) {
        if (s->id == id) {
            return s;
        }
    }
    return NULL;
}

/* Reads exactly n bytes of a frame that has begun. */
static int read_exact(conn_t *c, void *buf, size_t n)
{
    uint8_t *p = buf;
    size_t off = 0;
    long long deadline = now_ms() + FRAME_TIMEOUT_MS;
    while (off < n) {
        int w = c->tp->wait(c, SLICE_MS);
        if (w < 0) {
            return -1;
        }
        if (w == 0) {
            if (now_ms() >= deadline) {
                return -1;
            }
            continue;
        }
        pthread_mutex_lock(&c->io);
        int rc = c->tp->read(c, p + off, n - off);
        pthread_mutex_unlock(&c->io);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ) {
            continue;
        }
        if (rc <= 0) {
            if (rc < 0 && rc != MBEDTLS_ERR_SSL_TIMEOUT) {
                log_mbedtls("read", rc);
            }
            return -1;
        }
        off += (size_t)rc;
    }
    return 0;
}

/* Reads and drops n bytes. */
static int skip(conn_t *c, size_t n)
{
    uint8_t scratch[256];
    while (n > 0) {
        size_t k = n < sizeof(scratch) ? n : sizeof(scratch);
        if (read_exact(c, scratch, k) != 0) {
            return -1;
        }
        n -= k;
    }
    return 0;
}

/* Acknowledges received bytes, for the connection and the stream. */
static void window_update(conn_t *c, stream_t *s, size_t n)
{
    pthread_mutex_lock(&c->lock);
    uint32_t conn_inc = 0, stream_inc = 0, sid = 0;
    c->recv_unacked += (uint32_t)n;
    if (c->recv_unacked >= UPDATE_AT) {
        conn_inc = c->recv_unacked;
        c->recv_unacked = 0;
    }
    if (s != NULL && s->state == S_OPEN) {
        s->recv_unacked += (uint32_t)n;
        if (s->recv_unacked >= UPDATE_AT) {
            stream_inc = s->recv_unacked;
            s->recv_unacked = 0;
            sid = s->id;
        }
    }
    pthread_mutex_unlock(&c->lock);
    if (conn_inc) {
        send_u32(c, F_WINDOW_UPDATE, 0, conn_inc);
    }
    if (stream_inc) {
        send_u32(c, F_WINDOW_UPDATE, sid, stream_inc);
    }
}

/* With c->lock held: a stream's response is over, or failed. */
static void stream_end_locked(conn_t *c, stream_t *s, int failed)
{
    if (s->state == S_OPEN) {
        s->state = failed ? S_FAILED : S_DONE;
    }
    pthread_cond_broadcast(&c->cond);
}

static void stream_cancel(conn_t *c, stream_t *s)
{
    send_u32(c, F_RST_STREAM, s->id, E_CANCEL);
    pthread_mutex_lock(&c->lock);
    stream_end_locked(c, s, 1);
    pthread_mutex_unlock(&c->lock);
}

static int on_data(conn_t *c, uint32_t sid, uint8_t flags, size_t len)
{
    size_t pad = 0, left = len;
    if (flags & FL_PADDED) {
        uint8_t pl;
        if (len < 1 || read_exact(c, &pl, 1) != 0) {
            return -1;
        }
        pad = pl;
        left--;
        if (pad > left) {
            return -1;
        }
    }
    left -= pad;
    pthread_mutex_lock(&c->lock);
    stream_t *s = sid ? find_stream(c, sid) : NULL;
    if (sid == 0) {
        pthread_mutex_unlock(&c->lock);
        return -1;
    }
    int deliver = s != NULL && s->state == S_OPEN && s->headers_done;
    if (deliver) {
        s->busy = 1;
        s->got_frame = 1;
        s->deadline = now_ms() + (long long)s->timeout_s * 1000;
    }
    pthread_mutex_unlock(&c->lock);

    uint8_t chunk[512];
    int failed = 0;
    while (left > 0) {
        size_t k = left < sizeof(chunk) ? left : sizeof(chunk);
        if (read_exact(c, chunk, k) != 0) {
            if (deliver) {
                pthread_mutex_lock(&c->lock);
                s->busy = 0;
                pthread_cond_broadcast(&c->cond);
                pthread_mutex_unlock(&c->lock);
            }
            return -1;
        }
        left -= k;
        if (!deliver || failed) {
            continue;
        }
        s->got += (long long)k;
        if (s->ring != NULL) {
            pthread_mutex_lock(&c->lock);
            if (s->ring_len + k > s->ring_cap) {
                failed = 1; /* more than the reader keeps: the stream ends */
            } else {
                for (size_t i = 0; i < k; i++) {
                    s->ring[(s->ring_head + s->ring_len + i) % s->ring_cap] = chunk[i];
                }
                s->ring_len += k;
            }
            pthread_mutex_unlock(&c->lock);
        } else if (s->on_data != NULL) {
            if (s->status == 200 && !s->aborted && s->on_data(chunk, k, s->ctx) != 0) {
                s->aborted = 1;
                failed = 1;
            }
        } else if (s->got > FC_HTTP_MAX_RESPONSE) {
            fc_port_log(FC_LOG_WARN, "response too large");
            failed = 1;
        } else if (s->status == 200 && s->receive_200) {
            failed = s->receive_200(chunk, k, s->receive_ctx) != 0;
        } else if (!s->discard_200_body || s->status != 200) {
            size_t need = s->body_len + k + 1;
            if (need > s->body_cap) {
                size_t cap = s->body_cap ? s->body_cap : 1024;
                while (cap < need) {
                    cap *= 2;
                }
                char *nb = realloc(s->body, cap);
                /* A fragmented heap may still extend the current allocation
                 * in place by the bytes needed for this chunk and its NUL. */
                if (nb == NULL && cap > need) {
                    cap = need;
                    nb = realloc(s->body, cap);
                }
                if (nb == NULL) {
                    failed = 1;
                    continue;
                }
                s->body = nb;
                s->body_cap = cap;
            }
            memcpy(s->body + s->body_len, chunk, k);
            s->body_len += k;
        }
    }
    if (pad > 0 && skip(c, pad) != 0) {
        if (deliver) {
            pthread_mutex_lock(&c->lock);
            s->busy = 0;
            pthread_cond_broadcast(&c->cond);
            pthread_mutex_unlock(&c->lock);
        }
        return -1;
    }
    window_update(c, deliver && !failed ? s : NULL, len);
    if (deliver) {
        if (failed) {
            stream_cancel(c, s);
        }
        pthread_mutex_lock(&c->lock);
        s->busy = 0;
        if (flags & FL_END_STREAM) {
            stream_end_locked(c, s, 0);
        }
        pthread_cond_broadcast(&c->cond);
        pthread_mutex_unlock(&c->lock);
    }
    return 0;
}

/* A short response takes exactly its fragment size. Geometric growth keeps
 * the copying bounded when a peer splits a block over many small frames. */
static int read_header_fragment(conn_t *c, stream_t *s, size_t len)
{
    if (len == 0) {
        return 0;
    }
    size_t need = s->hlen + len;
    if (need > s->hcap) {
        size_t cap = 2u * s->hcap;
        if (cap < need) {
            cap = need;
        }
        if (cap > MAX_HEADER_BLOCK) {
            cap = MAX_HEADER_BLOCK;
        }
        uint8_t *buf = realloc(s->hblock, cap);
        if (buf == NULL) {
            return -1;
        }
        s->hblock = buf;
        s->hcap = (uint16_t)cap;
    }
    if (read_exact(c, s->hblock + s->hlen, len) != 0) {
        return -1;
    }
    s->hlen = (uint16_t)need;
    return 0;
}

/* A complete header block for stream s (NULL: one the device does not
 * follow any more, dropped: with no dynamic table nothing depends on it). */
static int on_header_block(conn_t *c, stream_t *s, const uint8_t *block, size_t n, int end_stream)
{
    hpack_resp_t h;
    if (hpack_decode(block, n, &h) != 0) {
        free(h.www_authenticate);
        fc_port_log(FC_LOG_WARN, "http2: undecodable header block");
        return -1; /* a compression error ends the connection (RFC 9113, 4.3) */
    }
    pthread_mutex_lock(&c->lock);
    if (s != NULL && s->state == S_OPEN) {
        s->got_frame = 1;
        s->deadline = now_ms() + (long long)s->timeout_s * 1000;
        if (!s->headers_done && h.status >= 200) {
            s->headers_done = 1;
            s->status = h.status;
            s->content_length = h.content_length;
            s->www_authenticate = h.www_authenticate;
            h.www_authenticate = NULL;
        } else if (!s->headers_done && h.status == 0) {
            pthread_mutex_unlock(&c->lock);
            free(h.www_authenticate);
            return -1; /* a response without :status */
        }
        if (h.grpc_status >= 0) {
            s->grpc_status_p1 = h.grpc_status + 1;
        }
        /* 1xx: an interim response, the final one follows. After the final
         * headers, a block is the trailers: only grpc-status is kept. */
        if (end_stream) {
            stream_end_locked(c, s, !s->headers_done);
        }
    }
    pthread_mutex_unlock(&c->lock);
    free(h.www_authenticate);
    return 0;
}

static int on_headers(conn_t *c, uint8_t type, uint32_t sid, uint8_t flags, size_t len)
{
    size_t pad = 0, left = len;
    if (sid == 0) {
        return -1;
    }
    if (type == F_HEADERS) {
        if (c->cont_id != 0) {
            return -1;
        }
        if (flags & FL_PADDED) {
            uint8_t pl;
            if (left < 1 || read_exact(c, &pl, 1) != 0) {
                return -1;
            }
            pad = pl;
            left--;
        }
        if (flags & FL_PRIORITY) {
            if (left < 5 || skip(c, 5) != 0) {
                return -1;
            }
            left -= 5;
        }
        if (pad > left) {
            return -1;
        }
        left -= pad;
    } else if (c->cont_id != sid) {
        return -1; /* CONTINUATION out of place */
    }
    pthread_mutex_lock(&c->lock);
    stream_t *s = find_stream(c, sid);
    if (s != NULL && s->state != S_OPEN) {
        s = NULL;
    }
    if (s != NULL) {
        s->busy = 1;
        if (type == F_HEADERS) {
            s->hlen = 0;
            s->end_with_headers = (flags & FL_END_STREAM) != 0;
        }
    }
    pthread_mutex_unlock(&c->lock);

    int rc = 0;
    if (s != NULL && left <= (size_t)MAX_HEADER_BLOCK - s->hlen) {
        rc = read_header_fragment(c, s, left);
    } else {
        if (s != NULL) {
            fc_port_log(FC_LOG_WARN, "http2: response headers over %d bytes", MAX_HEADER_BLOCK);
        }
        rc = skip(c, left);
        if (s != NULL && rc == 0) {
            /* Too large to keep: the stream fails, the connection goes on. */
            pthread_mutex_lock(&c->lock);
            s->busy = 0;
            pthread_mutex_unlock(&c->lock);
            stream_cancel(c, s);
            s = NULL;
        }
    }
    if (rc == 0 && pad > 0) {
        rc = skip(c, pad);
    }
    if (rc == 0) {
        if (flags & FL_END_HEADERS) {
            c->cont_id = 0;
            if (s != NULL) {
                rc = on_header_block(c, s, s->hblock, s->hlen, s->end_with_headers);
                free(s->hblock);
                s->hblock = NULL;
                s->hlen = s->hcap = 0;
            }
        } else {
            c->cont_id = sid;
        }
    }
    if (s != NULL) {
        pthread_mutex_lock(&c->lock);
        s->busy = 0;
        pthread_cond_broadcast(&c->cond);
        pthread_mutex_unlock(&c->lock);
    }
    return rc;
}

static int on_settings(conn_t *c, uint8_t flags, uint32_t sid, size_t len)
{
    if (sid != 0 || len % 6 != 0 || ((flags & FL_ACK) && len != 0)) {
        return -1;
    }
    if (flags & FL_ACK) {
        return 0;
    }
    while (len > 0) {
        uint8_t p[6];
        if (read_exact(c, p, sizeof(p)) != 0) {
            return -1;
        }
        len -= 6;
        uint16_t id = (uint16_t)(p[0] << 8 | p[1]);
        uint32_t v = (uint32_t)p[2] << 24 | (uint32_t)p[3] << 16 | (uint32_t)p[4] << 8 | p[5];
        pthread_mutex_lock(&c->lock);
        if (id == 4) { /* INITIAL_WINDOW_SIZE: open streams move by the difference */
            if (v > 0x7fffffffu) {
                pthread_mutex_unlock(&c->lock);
                return -1;
            }
            int64_t delta = (int64_t)v - c->peer_init_window;
            c->peer_init_window = v;
            for (stream_t *s = c->streams; s != NULL; s = s->next) {
                s->send_window += delta;
            }
        } else if (id == 5) { /* MAX_FRAME_SIZE */
            if (v < 16384 || v > 16777215) {
                pthread_mutex_unlock(&c->lock);
                return -1;
            }
            c->peer_max_frame = v;
        }
        pthread_cond_broadcast(&c->cond);
        pthread_mutex_unlock(&c->lock);
    }
    return send_frame(c, F_SETTINGS, FL_ACK, 0, NULL, 0);
}

/* Reads one frame and acts on it. Returns 0 when one was read, 1 when none
 * began by `deadline`, -1 when the connection is no longer usable. */
static int read_frame(conn_t *c, long long deadline)
{
    pthread_mutex_lock(&c->lock);
    int dead = c->dead;
    pthread_mutex_unlock(&c->lock);
    if (dead) {
        return -1;
    }
    long long left = deadline - now_ms();
    if (left <= 0) {
        return 1;
    }
    int w = c->tp->wait(c, left < SLICE_MS ? (int)left : SLICE_MS);
    if (w <= 0) {
        return w < 0 ? -1 : 1;
    }
    uint8_t h[9];
    if (read_exact(c, h, sizeof(h)) != 0) {
        return -1;
    }
    size_t len = (size_t)h[0] << 16 | (size_t)h[1] << 8 | h[2];
    uint8_t type = h[3], flags = h[4];
    uint32_t sid = ((uint32_t)h[5] << 24 | (uint32_t)h[6] << 16 | (uint32_t)h[7] << 8 | h[8]) & 0x7fffffffu;
    if (len > OUR_MAX_FRAME) {
        send_u32(c, F_GOAWAY, 0, E_FRAME_SIZE);
        return -1;
    }
    if (c->cont_id != 0 && type != F_CONTINUATION) {
        return -1;
    }
    switch (type) {
    case F_DATA:
        return on_data(c, sid, flags, len);
    case F_HEADERS:
    case F_CONTINUATION:
        return on_headers(c, type, sid, flags, len);
    case F_RST_STREAM: {
        uint8_t p[4];
        if (len != 4 || sid == 0 || read_exact(c, p, 4) != 0) {
            return -1;
        }
        uint32_t code = (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
        pthread_mutex_lock(&c->lock);
        stream_t *s = find_stream(c, sid);
        if (s != NULL && s->state == S_OPEN) {
            /* REFUSED_STREAM: not processed, and may be sent again. */
            s->retryable = code == E_REFUSED;
            stream_end_locked(c, s, 1);
        }
        pthread_mutex_unlock(&c->lock);
        return 0;
    }
    case F_SETTINGS:
        return on_settings(c, flags, sid, len);
    case F_PING: {
        uint8_t p[8];
        if (len != 8 || sid != 0 || read_exact(c, p, 8) != 0) {
            return -1;
        }
        return (flags & FL_ACK) ? 0 : send_frame(c, F_PING, FL_ACK, 0, p, 8);
    }
    case F_GOAWAY: {
        uint8_t p[8];
        if (len < 8 || sid != 0 || read_exact(c, p, 8) != 0 || skip(c, len - 8) != 0) {
            return -1;
        }
        uint32_t last = ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]) & 0x7fffffffu;
        pthread_mutex_lock(&c->lock);
        c->goaway = 1;
        for (stream_t *s = c->streams; s != NULL; s = s->next) {
            if (s->id > last && s->state == S_OPEN) {
                s->retryable = 1; /* never processed: may be sent again */
                stream_end_locked(c, s, 1);
            }
        }
        pthread_cond_broadcast(&c->cond);
        pthread_mutex_unlock(&c->lock);
        return 0;
    }
    case F_WINDOW_UPDATE: {
        uint8_t p[4];
        if (len != 4 || read_exact(c, p, 4) != 0) {
            return -1;
        }
        uint32_t inc = ((uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]) & 0x7fffffffu;
        pthread_mutex_lock(&c->lock);
        if (sid == 0) {
            c->send_window += inc;
        } else {
            stream_t *s = find_stream(c, sid);
            if (s != NULL) {
                s->send_window += inc;
            }
        }
        pthread_cond_broadcast(&c->cond);
        pthread_mutex_unlock(&c->lock);
        return 0;
    }
    case F_PUSH_PROMISE: /* refused in our SETTINGS */
        send_u32(c, F_GOAWAY, 0, E_PROTOCOL);
        return -1;
    default: /* PRIORITY, and frame types this client does not know */
        return skip(c, len);
    }
}

/* ---- streams -------------------------------------------------------------- */

/* With c->lock held: waits until done(s), reading frames while no other
 * thread does. Returns 0, or -1 if the connection is lost or nothing came
 * for s by its deadline (the connection is then given up: a server that
 * went silent is a connection that is gone). */
static int wait_core(conn_t *c, stream_t *s, int (*done)(conn_t *, stream_t *), const long long *fixed_deadline,
                     int (*interrupt_wait)(void))
{
    while (!done(c, s)) {
        if (c->dead) {
            return -1;
        }
        if (interrupt_wait && !s->status && interrupt_wait()) {
            return FC_HTTP_INTERRUPTED;
        }
        /* Request timeouts measure silence on this stream. DATA or HEADERS
         * may have refreshed it while another thread was the reader. */
        long long deadline = fixed_deadline ? *fixed_deadline : s->deadline;
        long long left = deadline - now_ms();
        if (left <= 0) {
            if (fixed_deadline) {
                return 1;
            }
            conn_kill_locked(c, "timeout");
            s->retryable = 0; /* the others on it may go again; this one waited enough */
            return -1;
        }
        if (!c->reading) {
            c->reading = 1;
            pthread_mutex_unlock(&c->lock);
            int rc = read_frame(c, deadline);
            pthread_mutex_lock(&c->lock);
            c->reading = 0;
            pthread_cond_broadcast(&c->cond);
            if (rc < 0) {
                conn_kill_locked(c, "read failed");
            }
            continue;
        }
        if (left > SLICE_MS) {
            left = SLICE_MS;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += (time_t)(left / 1000);
        ts.tv_nsec += (long)(left % 1000) * 1000000;
        if (ts.tv_nsec >= 1000000000) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000;
        }
        pthread_cond_timedwait(&c->cond, &c->lock, &ts);
    }
    return 0;
}

static int wait_until(conn_t *c, stream_t *s, int (*done)(conn_t *, stream_t *))
{
    return wait_core(c, s, done, NULL, NULL);
}

static int is_over(conn_t *c, stream_t *s)
{
    return s->state != S_OPEN;
}

static int can_send(conn_t *c, stream_t *s)
{
    return s->state != S_OPEN || (c->send_window > 0 && s->send_window > 0);
}

/* Builds the request's header block. */
static uint8_t *header_block(const fc_http_target_t *t, const char *method, const char *path,
                             const char *content_type, size_t body_len, int has_body, size_t *out_len)
{
    char authority[160], length[24];
    if (t->port == 443) {
        snprintf(authority, sizeof(authority), "%s", t->host);
    } else {
        snprintf(authority, sizeof(authority), "%s:%d", t->host, t->port);
    }
    snprintf(length, sizeof(length), "%zu", body_len);
    const char *accept = t->accept ? t->accept : "application/json";
    size_t cap = 64 + strlen(method) + strlen(path) + strlen(authority) + strlen(accept) + strlen(length) +
                 (content_type ? strlen(content_type) : 0) + (t->authorization ? strlen(t->authorization) : 0) +
                 32;
    uint8_t *b = malloc(cap);
    if (b == NULL) {
        return NULL;
    }
    size_t n = 0;
    if (strcmp(method, "GET") == 0) {
        b[n++] = 0x82;
    } else if (strcmp(method, "POST") == 0) {
        b[n++] = 0x83;
    } else {
        n += hpack_put_field(b + n, 2, method, 0);
    }
    b[n++] = 0x87; /* :scheme https */
    n += hpack_put_field(b + n, 4, path, 0);
    n += hpack_put_field(b + n, 1, authority, 0);
    n += hpack_put_field(b + n, 58, "flightctl-mcu", 0);
    n += hpack_put_field(b + n, 19, accept, 0);
    if (content_type != NULL) {
        n += hpack_put_field(b + n, 31, content_type, 0);
    }
    if (has_body) {
        n += hpack_put_field(b + n, 28, length, 0);
    }
    if (t->authorization != NULL) {
        n += hpack_put_field(b + n, 23, t->authorization, 1);
    }
    *out_len = n;
    return b;
}

/* Opens a stream: a new stream id and its HEADERS (with CONTINUATION if the
 * block is larger than a frame) go out together, under the transport lock,
 * as ids must reach the server in order and nothing may come between the
 * frames of a header block. */
static int stream_open(conn_t *c, stream_t *s, const uint8_t *hb, size_t hlen, int end_stream)
{
    pthread_mutex_lock(&c->io);
    pthread_mutex_lock(&c->lock);
    if (c->dead || c->goaway || c->next_id > 0x7ffffff0u) {
        int dead = c->dead;
        pthread_mutex_unlock(&c->lock);
        pthread_mutex_unlock(&c->io);
        s->retryable = 1;
        return dead ? -1 : -2;
    }
    s->id = c->next_id;
    c->next_id += 2;
    s->send_window = c->peer_init_window;
    s->deadline = now_ms() + (long long)s->timeout_s * 1000;
    s->next = c->streams;
    c->streams = s;
    uint32_t max = c->peer_max_frame;
    pthread_mutex_unlock(&c->lock);

    int rc = 0;
    size_t off = 0;
    int first = 1;
    do {
        size_t k = hlen - off < max ? hlen - off : max;
        uint8_t flags = (off + k == hlen ? FL_END_HEADERS : 0) | (first && end_stream ? FL_END_STREAM : 0);
        rc = send_frame_locked_io(c, first ? F_HEADERS : F_CONTINUATION, flags, s->id, hb + off, k);
        off += k;
        first = 0;
    } while (rc == 0 && off < hlen);
    pthread_mutex_unlock(&c->io);
    if (rc != 0) {
        pthread_mutex_lock(&c->lock);
        conn_kill_locked(c, "write failed");
        pthread_mutex_unlock(&c->lock);
    }
    return rc;
}

/* Sends the body in DATA frames, as the windows allow. */
static int stream_send_body(conn_t *c, stream_t *s, const char *body, size_t len)
{
    size_t off = 0;
    while (off < len) {
        pthread_mutex_lock(&c->lock);
        if (wait_until(c, s, can_send) != 0 || s->state != S_OPEN) {
            pthread_mutex_unlock(&c->lock);
            return -1;
        }
        int64_t k = (int64_t)(len - off);
        if (k > c->send_window) {
            k = c->send_window;
        }
        if (k > s->send_window) {
            k = s->send_window;
        }
        if (k > (int64_t)c->peer_max_frame) {
            k = c->peer_max_frame;
        }
        c->send_window -= k;
        s->send_window -= k;
        pthread_mutex_unlock(&c->lock);
        int last = off + (size_t)k == len;
        if (send_frame(c, F_DATA, last ? FL_END_STREAM : 0, s->id, body + off, (size_t)k) != 0) {
            return -1;
        }
        off += (size_t)k;
    }
    return 0;
}

/* Takes s off the connection, once the reader is done with it. */
static void stream_close(conn_t *c, stream_t *s)
{
    pthread_mutex_lock(&c->lock);
    long long keep = s->deadline;
    s->deadline = now_ms() + FRAME_TIMEOUT_MS + 1000; /* busy ends within a frame */
    while (s->busy) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_sec += 1;
        pthread_cond_timedwait(&c->cond, &c->lock, &ts);
    }
    s->deadline = keep;
    for (stream_t **pp = &c->streams; *pp != NULL; pp = &(*pp)->next) {
        if (*pp == s) {
            *pp = s->next;
            break;
        }
    }
    pthread_mutex_unlock(&c->lock);
    free(s->hblock);
    s->hblock = NULL;
    s->hlen = s->hcap = 0;
}

/* One request on c. Returns 0 when a response arrived (s->status), -1 on
 * failure; s->retryable tells whether it may be sent again elsewhere. */
static int exchange(conn_t *c, stream_t *s, const fc_http_target_t *t, const char *method, const char *path,
                    const char *content_type, const char *body, size_t body_len)
{
    int has_body = body != NULL && body_len > 0;
    s->discard_200_body = t->discard_200_body;
    s->receive_200 = t->receive_200;
    s->receive_ctx = t->receive_ctx;
    size_t hlen;
    uint8_t *hb = header_block(t, method, path, content_type, body_len, has_body || content_type != NULL, &hlen);
    if (hb == NULL) {
        return -1;
    }
    int rc = stream_open(c, s, hb, hlen, !has_body);
    free(hb);
    if (rc != 0) {
        if (s->id != 0) {
            stream_close(c, s);
        }
        return -1;
    }
    if (has_body && stream_send_body(c, s, body, body_len) != 0) {
        stream_close(c, s);
        return -1;
    }
    pthread_mutex_lock(&c->lock);
    rc = wait_core(c, s, is_over, NULL, t->interrupt_wait);
    int ok = rc == 0 && s->state == S_DONE;
    int reset = !ok && s->state == S_OPEN;
    pthread_mutex_unlock(&c->lock);
    if (reset) {
        /* Given up by this side (timeout): the server must stop too. */
        send_u32(c, F_RST_STREAM, s->id, E_CANCEL);
    }
    stream_close(c, s);
    if (rc == FC_HTTP_INTERRUPTED) {
        return rc;
    }
    return ok ? 0 : -1;
}

/* ---- sessions ------------------------------------------------------------- */

fc_http_session_t *fc_http_session_new(void)
{
    fc_http_session_t *s = calloc(1, sizeof(*s));
    if (s != NULL && pthread_mutex_init(&s->lock, NULL) != 0) {
        fc_port_log(FC_LOG_ERROR, "http2: no mutex left for a session");
        free(s);
        s = NULL;
    }
    return s;
}

/* With s->lock held: the session lets go of its connection; requests still
 * on it finish there, and the last one closes it. */
static void session_drop_locked(fc_http_session_t *s)
{
    conn_t *c = s->conn;
    if (c == NULL) {
        return;
    }
    s->conn = NULL;
    pthread_mutex_lock(&c->lock);
    c->retired = 1;
    pthread_mutex_unlock(&c->lock);
    conn_put(c);
}

void fc_http_session_close(fc_http_session_t *s)
{
    if (s != NULL) {
        pthread_mutex_lock(&s->lock);
        session_drop_locked(s);
        pthread_mutex_unlock(&s->lock);
    }
}

int fc_http_session_close_idle(fc_http_session_t *s)
{
    if (s == NULL) {
        return 1;
    }
    pthread_mutex_lock(&s->lock);
    conn_t *c = s->conn;
    int idle = 1;
    if (c != NULL) {
        pthread_mutex_lock(&c->lock);
        idle = c->refs == 1;
        pthread_mutex_unlock(&c->lock);
        if (idle) {
            session_drop_locked(s);
        }
    }
    pthread_mutex_unlock(&s->lock);
    return idle;
}

void fc_http_session_free(fc_http_session_t *s)
{
    if (s != NULL) {
        fc_http_session_close(s);
        pthread_mutex_destroy(&s->lock);
        free(s);
    }
}

/* A connection for t, with a reference for the caller: the session's when it
 * is still good for t, else a new one (kept by the session, if any). *fresh
 * tells whether it was just opened. */
static conn_t *get_conn_mode(const fc_http_target_t *t, int *fresh, int existing_only)
{
    fc_http_session_t *s = t->session;
    *fresh = 0;
    if (s == NULL && existing_only) {
        return NULL;
    }
    if (s == NULL) {
        *fresh = 1;
        return conn_open(t);
    }
    pthread_mutex_lock(&s->lock);
    conn_t *c = s->conn;
    if (c != NULL) {
        pthread_mutex_lock(&c->lock);
        int good = !c->dead && !c->goaway && !c->retired && c->port == t->port && strcmp(c->host, t->host) == 0 &&
                   c->cert_id == cert_id(t->cert_pem);
        if (good) {
            c->refs++;
        }
        pthread_mutex_unlock(&c->lock);
        if (!good) {
            session_drop_locked(s); /* gone, or a new client certificate */
            c = NULL;
        }
    }
    if (c == NULL && !existing_only) {
        /* Opened under the session's lock: a thread that needs it meanwhile
         * waits for this one instead of opening a second. */
        c = conn_open(t);
        if (c != NULL) {
            c->refs++; /* the session's and the caller's */
            s->conn = c;
            *fresh = 1;
        }
    }
    pthread_mutex_unlock(&s->lock);
    return c;
}

static conn_t *get_conn(const fc_http_target_t *t, int *fresh)
{
    return get_conn_mode(t, fresh, 0);
}

/* A request with its retry: one that the server never took (its connection
 * closed before any answer, a GOAWAY that leaves it out, REFUSED_STREAM) is
 * sent once more, on a new connection. */
static int request(const fc_http_target_t *t, const char *method, const char *path, const char *content_type,
                   const char *body, size_t body_len, stream_t *s)
{
    for (int attempt = 0; attempt < 2; attempt++) {
        int fresh;
        conn_t *c = get_conn(t, &fresh);
        if (c == NULL) {
            return -1;
        }
        s->timeout_s = t->timeout_s > 0 ? t->timeout_s : 30;
        int rc = exchange(c, s, t, method, path, content_type, body, body_len);
        conn_put(c);
        if (rc == 0 || rc == FC_HTTP_INTERRUPTED) {
            return rc;
        }
        if (!s->retryable || s->aborted || attempt == 1) {
            return -1;
        }
        (void)fresh;
        fc_port_log(FC_LOG_DEBUG, "%s %s: the connection went away before an answer, again", method, path);
        stream_t keep = *s;
        memset(s, 0, sizeof(*s));
        s->on_data = keep.on_data;
        s->ctx = keep.ctx;
        free(keep.body);
        free(keep.www_authenticate);
    }
    return -1;
}

int fc_http_request(const fc_http_target_t *t, const char *method, const char *path, const char *content_type,
                    const char *body, size_t body_len, fc_http_response_t *resp)
{
    memset(resp, 0, sizeof(*resp));
    resp->status = -1;
    stream_t s;
    memset(&s, 0, sizeof(s));
    int rc = request(t, method, path, content_type, body, body_len, &s);
    if (rc != 0) {
        free(s.body);
        free(s.www_authenticate);
        return rc;
    }
    resp->status = s.status;
    resp->www_authenticate = s.www_authenticate;
    if (s.body == NULL) {
        s.body = malloc(1);
        if (s.body == NULL) {
            free(resp->www_authenticate);
            resp->www_authenticate = NULL;
            resp->status = -1;
            return -1;
        }
    }
    s.body[s.body_len] = '\0';
    resp->body = s.body;
    resp->body_len = s.body_len;
    return 0;
}

void fc_http_free(fc_http_response_t *resp)
{
    free(resp->body);
    resp->body = NULL;
    free(resp->www_authenticate);
    resp->www_authenticate = NULL;
}

int fc_http_get_stream(const fc_http_target_t *t, const char *path, fc_http_data_cb on_data, void *ctx, int *status,
                       long *total)
{
    stream_t s;
    memset(&s, 0, sizeof(s));
    s.on_data = on_data;
    s.ctx = ctx;
    int rc = request(t, "GET", path, NULL, NULL, 0, &s);
    *status = s.headers_done ? s.status : -1;
    *total = s.headers_done ? s.content_length : -1;
    free(s.body);
    free(s.www_authenticate);
    if (rc != 0 || s.status != 200 || s.aborted) {
        return -1;
    }
    if (s.content_length >= 0 && s.got != s.content_length) {
        fc_port_log(FC_LOG_WARN, "GET %s: %lld bytes of %ld", path, s.got, s.content_length);
        return -1;
    }
    return 0;
}

/* ---- bidirectional streams (gRPC) ---------------------------------------------- */

struct fc_http_stream {
    conn_t *c;
    stream_t s;
    int sent_end; /* END_STREAM sent */
};

#define STREAM_RING 4096

fc_http_stream_t *fc_http_stream_open(const fc_http_target_t *t, const char *path, const char *content_type,
                                      const char *const *headers)
{
    int fresh;
    /* A session's connection only, never a new one for it; without a
     * session, a connection for this stream alone. */
    conn_t *c = get_conn_mode(t, &fresh, t->session != NULL);
    if (c == NULL) {
        return NULL;
    }
    fc_http_stream_t *st = calloc(1, sizeof(*st));
    uint8_t *ring = malloc(STREAM_RING);
    char authority[160];
    snprintf(authority, sizeof(authority), t->port == 443 ? "%s" : "%s:%d", t->host, t->port);
    size_t cap = 96 + strlen(path) + strlen(authority) + strlen(content_type);
    for (const char *const *h = headers; h != NULL && h[0] != NULL; h += 2) {
        cap += strlen(h[0]) + strlen(h[1]) + 12;
    }
    uint8_t *hb = st != NULL && ring != NULL ? malloc(cap) : NULL;
    if (hb == NULL) {
        free(st);
        free(ring);
        conn_put(c);
        return NULL;
    }
    size_t n = 0;
    hb[n++] = 0x83; /* POST */
    hb[n++] = 0x87; /* :scheme https */
    n += hpack_put_field(hb + n, 4, path, 0);
    n += hpack_put_field(hb + n, 1, authority, 0);
    n += hpack_put_field(hb + n, 58, "flightctl-mcu", 0);
    n += hpack_put_field(hb + n, 31, content_type, 0);
    n += hpack_put_named(hb + n, "te", "trailers");
    for (const char *const *h = headers; h != NULL && h[0] != NULL; h += 2) {
        n += hpack_put_named(hb + n, h[0], h[1]);
    }
    st->c = c;
    st->s.ring = ring;
    st->s.ring_cap = STREAM_RING;
    st->s.timeout_s = t->timeout_s > 0 ? t->timeout_s : 30;
    st->s.headers_done = 0;
    int rc = stream_open(c, &st->s, hb, n, 0);
    free(hb);
    if (rc != 0) {
        fc_http_stream_close(st);
        return NULL;
    }
    return st;
}

static int has_room(conn_t *c, stream_t *s)
{
    return can_send(c, s) || c->retired;
}

int fc_http_stream_write(fc_http_stream_t *st, const void *data, size_t len, int end, int timeout_ms)
{
    conn_t *c = st->c;
    stream_t *s = &st->s;
    const uint8_t *p = data;
    size_t off = 0;
    long long deadline = now_ms() + timeout_ms;
    if (st->sent_end) {
        return -1;
    }
    do {
        pthread_mutex_lock(&c->lock);
        int rc = wait_core(c, s, has_room, &deadline, NULL);
        if (rc != 0 || s->state != S_OPEN || c->retired) {
            pthread_mutex_unlock(&c->lock);
            return -1;
        }
        int64_t k = (int64_t)(len - off);
        if (k > c->send_window) {
            k = c->send_window;
        }
        if (k > s->send_window) {
            k = s->send_window;
        }
        if (k > (int64_t)c->peer_max_frame) {
            k = c->peer_max_frame;
        }
        c->send_window -= k;
        s->send_window -= k;
        pthread_mutex_unlock(&c->lock);
        int last = off + (size_t)k == len;
        if (send_frame(c, F_DATA, last && end ? FL_END_STREAM : 0, s->id, p + off, (size_t)k) != 0) {
            return -1;
        }
        off += (size_t)k;
    } while (off < len);
    st->sent_end = end;
    return 0;
}

static int readable(conn_t *c, stream_t *s)
{
    return s->ring_len > 0 || s->state != S_OPEN || c->retired;
}

int fc_http_stream_read(fc_http_stream_t *st, void *buf, size_t cap, int timeout_ms)
{
    conn_t *c = st->c;
    stream_t *s = &st->s;
    pthread_mutex_lock(&c->lock);
    long long deadline = now_ms() + timeout_ms;
    int rc = wait_core(c, s, readable, &deadline, NULL);
    int n = 0;
    if (s->ring_len > 0) {
        size_t k = cap < s->ring_len ? cap : s->ring_len;
        for (size_t i = 0; i < k; i++) {
            ((uint8_t *)buf)[i] = s->ring[(s->ring_head + i) % s->ring_cap];
        }
        s->ring_head = (s->ring_head + k) % s->ring_cap;
        s->ring_len -= k;
        n = (int)k;
    } else if (rc < 0 || s->state != S_OPEN || c->retired) {
        n = -1; /* over: ended, reset, the connection lost or let go */
    }
    pthread_mutex_unlock(&c->lock);
    return n;
}

void fc_http_stream_status(fc_http_stream_t *st, int *http_status, int *grpc_status)
{
    pthread_mutex_lock(&st->c->lock);
    *http_status = st->s.headers_done ? st->s.status : -1;
    *grpc_status = st->s.grpc_status_p1 - 1;
    pthread_mutex_unlock(&st->c->lock);
}

void fc_http_stream_close(fc_http_stream_t *st)
{
    if (st == NULL) {
        return;
    }
    conn_t *c = st->c;
    if (st->s.id != 0) {
        pthread_mutex_lock(&c->lock);
        int open = st->s.state == S_OPEN && !c->dead;
        pthread_mutex_unlock(&c->lock);
        if (open) {
            send_u32(c, F_RST_STREAM, st->s.id, E_CANCEL);
        }
        stream_close(c, &st->s);
    }
    conn_put(c);
    free(st->s.ring);
    free(st->s.www_authenticate);
    free(st);
}
