/*
 * The device's log in RAM (fc_log.h): a byte ring of entries, each a header
 * (the text's length, the level with two flags, the uptime) and the text,
 * without its NUL.
 */
#include "fc_log.h"

#include <pthread.h>
#include <string.h>
#include <time.h>

#define HDR       7    /* length (2), level and flags (1), uptime (4) */
#define F_SYSTEM  0x80 /* the system's line */
#define F_PREV    0x40 /* the previous boot's */
#define CLOCK_SET 1577836800 /* 2020-01-01: before it, the clock was not set */
/* Changes with the layout: a ring of another firmware's is not read. */
#define MAGIC (0x46434c00u ^ (uint32_t)FC_LOG_RING ^ ((uint32_t)HDR << 24))

/* What a restart keeps (FC_LOG_KEEP): the ports place it in RAM that is not
 * cleared at boot (".noinit", in ESP-IDF's and Zephyr's linker scripts). */
typedef struct {
    uint32_t magic;
    uint32_t head, used;         /* the oldest entry's offset; bytes in use */
    uint32_t head_seq, end_seq;  /* the oldest entry's number; the next one's */
    int64_t boot_epoch;          /* the time at this boot's uptime 0, or 0 */
    uint8_t ring[FC_LOG_RING];
} log_mem_t;

#ifdef FC_LOG_KEEP
static log_mem_t s_mem __attribute__((section(".noinit.fc_log")));
#else
static log_mem_t s_mem;
#endif

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_ready;          /* cleared at boot: s_mem looked at once */
static int64_t s_prev_epoch; /* the previous boot's time at its uptime 0, or 0 */

uint32_t fc_log_uptime(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint32_t)ts.tv_sec;
}

static void put_bytes(uint32_t at, const void *src, size_t n)
{
    const uint8_t *p = src;
    for (size_t i = 0; i < n; i++) {
        s_mem.ring[(at + i) % FC_LOG_RING] = p[i];
    }
}

static void get_bytes(uint32_t at, void *dst, size_t n)
{
    uint8_t *p = dst;
    for (size_t i = 0; i < n; i++) {
        p[i] = s_mem.ring[(at + i) % FC_LOG_RING];
    }
}

static uint32_t entry_size(uint32_t at)
{
    uint8_t h[2];
    get_bytes(at, h, 2);
    return HDR + ((uint32_t)h[0] | (uint32_t)h[1] << 8);
}

static uint8_t entry_flags(uint32_t at)
{
    return s_mem.ring[(at + 2) % FC_LOG_RING];
}

/* The ring as a restart left it: its numbers in range, and its entries
 * adding up to what it says it holds. */
static int ring_checks_out(void)
{
    if (s_mem.magic != MAGIC || s_mem.head >= FC_LOG_RING || s_mem.used > FC_LOG_RING ||
        s_mem.end_seq - s_mem.head_seq > FC_LOG_RING / HDR) {
        return 0;
    }
    uint32_t at = s_mem.head, left = s_mem.used, n = 0;
    while (left > 0) {
        uint32_t size = entry_size(at);
        if (size > HDR + FC_LOG_TEXT_MAX || size > left || (entry_flags(at) & 0x3f) > FC_LOG_DEBUG) {
            return 0;
        }
        at = (at + size) % FC_LOG_RING;
        left -= size;
        n++;
    }
    return n == s_mem.end_seq - s_mem.head_seq;
}

/* Under the lock, once per boot: the previous boot's lines are kept (the
 * one before it goes), else the ring starts empty. */
static void ready_locked(void)
{
    if (s_ready) {
        return;
    }
    s_ready = 1;
    s_prev_epoch = 0;
    if (!ring_checks_out()) {
        memset(&s_mem, 0, sizeof(s_mem));
        s_mem.magic = MAGIC;
        return;
    }
    while (s_mem.used > 0 && (entry_flags(s_mem.head) & F_PREV)) {
        uint32_t n = entry_size(s_mem.head);
        s_mem.head = (s_mem.head + n) % FC_LOG_RING;
        s_mem.used -= n;
        s_mem.head_seq++;
    }
    for (uint32_t at = s_mem.head, left = s_mem.used; left > 0;) {
        s_mem.ring[(at + 2) % FC_LOG_RING] |= F_PREV;
        uint32_t n = entry_size(at);
        at = (at + n) % FC_LOG_RING;
        left -= n;
    }
    s_prev_epoch = s_mem.boot_epoch;
    s_mem.boot_epoch = 0;
}

/* read_clock: 0 from a fatal error's handler, where the wall clock is not
 * read (on ESP-IDF time() takes a lock of the C library: in a panic it may be
 * held, and taking it lowers the interrupt level, which lets the interrupt
 * watchdog that caused the panic in again). The line keeps its uptime. */
static void put_locked(uint8_t flags, const char *text, int read_clock)
{
    size_t len = strlen(text);
    while (len > 0 && (text[len - 1] == '\n' || text[len - 1] == '\r')) {
        len--;
    }
    if (len > FC_LOG_TEXT_MAX) {
        len = FC_LOG_TEXT_MAX;
    }
    uint32_t up = fc_log_uptime();
    time_t now = read_clock ? time(NULL) : 0;
    if (now >= CLOCK_SET) {
        s_mem.boot_epoch = (int64_t)now - up;
    }
    uint8_t h[HDR] = {(uint8_t)len, (uint8_t)(len >> 8), flags,
                      (uint8_t)up, (uint8_t)(up >> 8), (uint8_t)(up >> 16), (uint8_t)(up >> 24)};
    /* The indices move only once the bytes are there: a restart in the
     * middle leaves the ring as it was before this line. */
    while (s_mem.used + HDR + len > FC_LOG_RING) {
        uint32_t n = entry_size(s_mem.head);
        s_mem.head = (s_mem.head + n) % FC_LOG_RING;
        s_mem.used -= n;
        s_mem.head_seq++;
    }
    uint32_t at = (s_mem.head + s_mem.used) % FC_LOG_RING;
    put_bytes(at, h, HDR);
    put_bytes(at + HDR, text, len);
    s_mem.used += HDR + (uint32_t)len;
    s_mem.end_seq++;
}

void fc_log_put(fc_log_level_t level, const char *text)
{
    pthread_mutex_lock(&s_lock);
    ready_locked();
    put_locked((uint8_t)level, text, 1);
    pthread_mutex_unlock(&s_lock);
}

void fc_log_put_system(fc_log_level_t level, const char *text)
{
    pthread_mutex_lock(&s_lock);
    ready_locked();
    put_locked((uint8_t)level | F_SYSTEM, text, 1);
    pthread_mutex_unlock(&s_lock);
}

void fc_log_put_fatal(const char *text)
{
    if (s_ready) {
        put_locked((uint8_t)FC_LOG_ERROR | F_SYSTEM, text, 0);
    }
}

int fc_log_next(uint32_t *seq, fc_log_entry_t *e)
{
    pthread_mutex_lock(&s_lock);
    ready_locked();
    /* Numbers wrap: what counts is the distance from the oldest. */
    uint32_t want = *seq - s_mem.head_seq > s_mem.end_seq - s_mem.head_seq ? s_mem.head_seq : *seq;
    if (want == s_mem.end_seq) {
        pthread_mutex_unlock(&s_lock);
        return -1;
    }
    uint32_t at = s_mem.head;
    for (uint32_t s = s_mem.head_seq; s != want; s++) {
        at = (at + entry_size(at)) % FC_LOG_RING;
    }
    uint8_t h[HDR];
    get_bytes(at, h, HDR);
    size_t len = (size_t)h[0] | (size_t)h[1] << 8;
    e->seq = want;
    e->level = h[2] & 0x3f;
    e->system = (h[2] & F_SYSTEM) != 0;
    e->previous = (h[2] & F_PREV) != 0;
    e->up_s = (uint32_t)h[3] | (uint32_t)h[4] << 8 | (uint32_t)h[5] << 16 | (uint32_t)h[6] << 24;
    int64_t epoch = e->previous ? s_prev_epoch : s_mem.boot_epoch;
    if (!e->previous) {
        time_t now = time(NULL);
        epoch = now >= CLOCK_SET ? (int64_t)now - fc_log_uptime() : 0;
    }
    e->when = epoch > 0 ? epoch + e->up_s : 0;
    get_bytes(at + HDR, e->text, len);
    e->text[len] = '\0';
    *seq = want + 1;
    pthread_mutex_unlock(&s_lock);
    return 0;
}

uint32_t fc_log_end(void)
{
    pthread_mutex_lock(&s_lock);
    ready_locked();
    uint32_t end = s_mem.end_seq;
    pthread_mutex_unlock(&s_lock);
    return end;
}

void fc_log_restart(void)
{
    pthread_mutex_lock(&s_lock);
    s_ready = 0;
    pthread_mutex_unlock(&s_lock);
}
