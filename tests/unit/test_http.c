/*
 * HTTP/2 client: HPACK and Huffman (RFC 7541's examples), then whole
 * exchanges against a scripted server on a socket pair: flow control over
 * many bodies, two threads on one connection, GOAWAY, resets, a lost
 * connection, CONTINUATION both ways, and the TLS buffer pool.
 */
#include "../../core/src/fc_http.c"

#include <poll.h>
#include <stdint.h>
#include <unistd.h>

#include "port_stub.h"
#include "test.h"

/* ---- HPACK and Huffman ----------------------------------------------------- */

static size_t unhex(const char *hex, uint8_t *out)
{
    size_t n = 0;
    for (const char *p = hex; *p;) {
        if (*p == ' ') {
            p++;
            continue;
        }
        unsigned v;
        sscanf(p, "%2x", &v);
        out[n++] = (uint8_t)v;
        p += 2;
    }
    return n;
}

static void huffman_decodes_rfc7541_examples(void)
{
    uint8_t in[64];
    char out[64];
    size_t n = unhex("f1e3 c2e5 f23a 6ba0 ab90 f4ff", in); /* C.4.1 */
    CHECK_INT(huff_decode(in, n, out, sizeof(out)), 15);
    CHECK_STR(out, "www.example.com");
    n = unhex("a8eb 1064 9cbf", in); /* C.4.2 */
    CHECK_INT(huff_decode(in, n, out, sizeof(out)), 8);
    CHECK_STR(out, "no-cache");
    n = unhex("25a8 49e9 5ba9 7d7f", in); /* C.4.3 */
    CHECK_INT(huff_decode(in, n, out, sizeof(out)), 10);
    CHECK_STR(out, "custom-key");
}

static void huffman_refuses_eos_and_bad_padding(void)
{
    uint8_t eos[] = {0xff, 0xff, 0xff, 0xff}; /* EOS (30 ones) in the data */
    char out[16];
    CHECK_INT(huff_decode(eos, sizeof(eos), out, sizeof(out)), -1);
    uint8_t zero_pad[] = {0x00}; /* '0' (00000) then 000: padding must be ones */
    CHECK_INT(huff_decode(zero_pad, 1, out, sizeof(out)), -1);
    uint8_t one_pad[] = {0x07}; /* '0' then 111 */
    CHECK_INT(huff_decode(one_pad, 1, out, sizeof(out)), 1);
    CHECK_STR(out, "0");
    uint8_t long_pad[] = {0x07, 0xff}; /* 11 bits of padding */
    CHECK_INT(huff_decode(long_pad, 2, out, sizeof(out)), -1);
    uint8_t three[] = {0x00, 0x00}; /* "000" and 1 bit: no room for it in 2 */
    CHECK_INT(huff_decode(three, 2, out, 2), -1);
}

static void hpack_integers(void)
{
    uint8_t b[8];
    uint32_t v;
    CHECK_INT(hpack_put_int(b, 0, 5, 10), 1); /* C.1.1 */
    CHECK_INT(b[0], 10);
    CHECK_INT(hpack_put_int(b, 0, 5, 1337), 3); /* C.1.2 */
    CHECK_INT(b[0], 0x1f);
    CHECK_INT(b[1], 0x9a);
    CHECK_INT(b[2], 0x0a);
    CHECK_INT(hpack_int(b, 3, 5, &v), 3);
    CHECK_INT(v, 1337);
    CHECK_INT(hpack_int(b, 2, 5, &v), -1); /* cut short */
    uint8_t huge[] = {0x1f, 0xff, 0xff, 0xff, 0xff, 0x0f};
    CHECK_INT(hpack_int(huge, sizeof(huge), 5, &v), -1);
}

static void hpack_decodes_a_response_with_huffman(void)
{
    uint8_t b[128];
    /* C.6.1: :status 302, cache-control, date, location; incremental
     * indexing literals with Huffman values. */
    size_t n = unhex("4882 6402 5885 aec3 771a 4b61 96d0 7abe 9410 54d4 44a8 2005 9504 0b81 66e0 82a6"
                     " 2d1b ff6e 919d 29ad 1718 63c7 8f0b 97c8 e9ae 82ae 43d3",
                     b);
    hpack_resp_t h;
    CHECK_INT(hpack_decode(b, n, &h), 0);
    CHECK_INT(h.status, 302);
    free(h.www_authenticate);
    /* C.6.2 refers to the dynamic table, which the device announced at 0. */
    n = unhex("4883 640e ffc1 c0bf", b);
    CHECK_INT(hpack_decode(b, n, &h), -1);
    free(h.www_authenticate);
    /* Indexed :status 204, a size update to 0, a never-indexed literal. */
    n = unhex("20 89 1f2e 05 6261 7369 63", b);
    CHECK_INT(hpack_decode(b, n, &h), 0);
    CHECK_INT(h.status, 204);
    CHECK_STR(h.www_authenticate, "basic");
    free(h.www_authenticate);
    uint8_t bad_index[] = {0xbe}; /* 62: past the static table */
    CHECK_INT(hpack_decode(bad_index, 1, &h), -1);
    uint8_t short_str[] = {0x08, 0x05, 'a'}; /* :status with 5 bytes promised, 1 there */
    CHECK_INT(hpack_decode(short_str, sizeof(short_str), &h), -1);
}

/* ---- a scripted server ------------------------------------------------------ */

static int fd_read(conn_t *c, unsigned char *buf, size_t len)
{
    ssize_t n = recv((int)(intptr_t)c->tctx, buf, len, 0);
    return n < 0 ? -1 : (int)n;
}

static int fd_write(conn_t *c, const unsigned char *buf, size_t len)
{
    ssize_t n = send((int)(intptr_t)c->tctx, buf, len, 0);
    return n <= 0 ? -1 : (int)n;
}

static int fd_wait(conn_t *c, int timeout_ms)
{
    struct pollfd p = {.fd = (int)(intptr_t)c->tctx, .events = POLLIN};
    int rc = poll(&p, 1, timeout_ms);
    return rc < 0 ? -1 : rc > 0 ? 1 : 0;
}

static void fd_close(conn_t *c)
{
    close((int)(intptr_t)c->tctx);
}

static const transport_t FD = {fd_read, fd_write, fd_wait, fd_close};

typedef void (*script_t)(int fd);
static script_t s_script;
static pthread_t s_servers[8];
static int s_nservers, s_opens;

static void *server_main(void *arg)
{
    int fd = (int)(intptr_t)arg;
    s_script(fd);
    close(fd);
    return NULL;
}

static conn_t *test_open(const fc_http_target_t *t)
{
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        return NULL;
    }
    s_opens++;
    pthread_create(&s_servers[s_nservers++], NULL, server_main, (void *)(intptr_t)sv[1]);
    return conn_new(&FD, (void *)(intptr_t)sv[0]);
}

static void serve(script_t script)
{
    s_script = script;
    s_nservers = s_opens = 0;
    s_open_hook = test_open;
}

static void join_servers(void)
{
    for (int i = 0; i < s_nservers; i++) {
        pthread_join(s_servers[i], NULL);
    }
    s_nservers = 0;
}

static int rd(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n > 0) {
        ssize_t k = recv(fd, p, n, 0);
        if (k <= 0) {
            return -1;
        }
        p += k;
        n -= (size_t)k;
    }
    return 0;
}

typedef struct {
    uint8_t type, flags;
    uint32_t sid;
    size_t len;
    uint8_t *payload; /* malloc'd */
} frame_t;

static int srv_frame(int fd, frame_t *f)
{
    uint8_t h[9];
    if (rd(fd, h, 9) != 0) {
        return -1;
    }
    f->len = (size_t)h[0] << 16 | (size_t)h[1] << 8 | h[2];
    f->type = h[3];
    f->flags = h[4];
    f->sid = ((uint32_t)h[5] << 24 | (uint32_t)h[6] << 16 | (uint32_t)h[7] << 8 | h[8]) & 0x7fffffff;
    f->payload = malloc(f->len + 1);
    return rd(fd, f->payload, f->len);
}

static void srv_send(int fd, uint8_t type, uint8_t flags, uint32_t sid, const void *p, size_t len)
{
    uint8_t h[9] = {(uint8_t)(len >> 16), (uint8_t)(len >> 8), (uint8_t)len, type, flags,
                    (uint8_t)(sid >> 24), (uint8_t)(sid >> 16), (uint8_t)(sid >> 8), (uint8_t)sid};
    send(fd, h, 9, 0);
    if (len) {
        send(fd, p, len, 0);
    }
}

static void srv_u32(int fd, uint8_t type, uint32_t sid, uint32_t v)
{
    uint8_t p[4] = {(uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v};
    srv_send(fd, type, 0, sid, p, 4);
}

/* The client's preface and SETTINGS, then the server's (empty) SETTINGS. */
static int srv_hello(int fd)
{
    char pre[24];
    frame_t f;
    if (rd(fd, pre, 24) != 0 || memcmp(pre, PREFACE, 24) != 0 || srv_frame(fd, &f) != 0) {
        return -1;
    }
    int ok = f.type == F_SETTINGS && f.len == 24 && f.payload[5] == 0; /* HEADER_TABLE_SIZE 0 */
    free(f.payload);
    srv_send(fd, F_SETTINGS, 0, 0, NULL, 0);
    return ok ? 0 : -1;
}

/* A request as the server sees it. */
typedef struct {
    uint32_t sid;
    char method[8], path[128], authority[64], auth[32768];
    int auth_never_indexed;
    size_t body_len;
    char body[4096];
    int continuations;
    uint32_t window_conn, window_stream; /* WINDOW_UPDATE increments seen meanwhile */
    int settings_acks, ping_acks;
} req_t;

/* Decodes the client's header block: indexed fields and literals without
 * Huffman, as the client writes them. */
static void decode_request(const uint8_t *p, size_t n, req_t *r)
{
    size_t off = 0;
    while (off < n) {
        uint8_t b = p[off];
        if (b & 0x80) {
            uint32_t idx;
            off += (size_t)hpack_int(p + off, n - off, 7, &idx);
            if (idx == 2 || idx == 3) {
                strcpy(r->method, HPACK_STATIC[idx][1]);
            }
            continue;
        }
        uint32_t idx, len;
        int never = (b & 0xf0) == 0x10;
        off += (size_t)hpack_int(p + off, n - off, 4, &idx);
        off += (size_t)hpack_int(p + off, n - off, 7, &len);
        char *dst = idx == 2 ? r->method : idx == 4 ? r->path : idx == 1 ? r->authority : idx == 23 ? r->auth : NULL;
        size_t cap = idx == 2 ? sizeof(r->method) : idx == 23 ? sizeof(r->auth) : 128;
        if (dst != NULL && len < cap) {
            memcpy(dst, p + off, len);
            dst[len] = '\0';
        }
        if (idx == 23) {
            r->auth_never_indexed = never;
        }
        off += len;
    }
}

/* Reads frames until a whole request (headers and body) has arrived. */
static int srv_request(int fd, req_t *r)
{
    memset(r, 0, sizeof(*r));
    uint8_t block[40000];
    size_t blen = 0;
    int headers_done = 0, ended = 0;
    while (!(headers_done && ended)) {
        frame_t f;
        if (srv_frame(fd, &f) != 0) {
            return -1;
        }
        if (f.type == F_HEADERS || f.type == F_CONTINUATION) {
            r->sid = f.sid;
            r->continuations += f.type == F_CONTINUATION;
            memcpy(block + blen, f.payload, f.len);
            blen += f.len;
            if (f.type == F_HEADERS && (f.flags & FL_END_STREAM)) {
                ended = 1;
            }
            if (f.flags & FL_END_HEADERS) {
                headers_done = 1;
                decode_request(block, blen, r);
            }
        } else if (f.type == F_DATA) {
            memcpy(r->body + r->body_len, f.payload, f.len);
            r->body_len += f.len;
            ended = (f.flags & FL_END_STREAM) != 0;
        } else if (f.type == F_WINDOW_UPDATE) {
            uint32_t v = (uint32_t)f.payload[0] << 24 | (uint32_t)f.payload[1] << 16 |
                         (uint32_t)f.payload[2] << 8 | f.payload[3];
            *(f.sid ? &r->window_stream : &r->window_conn) += v;
        } else if (f.type == F_SETTINGS && (f.flags & FL_ACK)) {
            r->settings_acks++;
        } else if (f.type == F_PING && (f.flags & FL_ACK)) {
            r->ping_acks++;
        }
        free(f.payload);
    }
    return 0;
}

/* Response headers: indexed :status 200, or a literal :status. */
static void srv_headers(int fd, uint32_t sid, int status, const char *extra_name, const char *extra_value,
                        int end_stream)
{
    uint8_t b[512];
    size_t n = 0;
    if (status == 200) {
        b[n++] = 0x88;
    } else {
        char s[8];
        snprintf(s, sizeof(s), "%d", status);
        n += hpack_put_field(b + n, 8, s, 0);
    }
    if (extra_name != NULL) {
        b[n++] = 0x00; /* literal, new name */
        n += hpack_put_int(b + n, 0, 7, (uint32_t)strlen(extra_name));
        memcpy(b + n, extra_name, strlen(extra_name));
        n += strlen(extra_name);
        n += hpack_put_int(b + n, 0, 7, (uint32_t)strlen(extra_value));
        memcpy(b + n, extra_value, strlen(extra_value));
        n += strlen(extra_value);
    }
    srv_send(fd, F_HEADERS, FL_END_HEADERS | (end_stream ? FL_END_STREAM : 0), sid, b, n);
}

/* Waits for the client to close (GOAWAY, then the end of the stream). */
static void srv_drain(int fd)
{
    frame_t f;
    while (srv_frame(fd, &f) == 0) {
        free(f.payload);
    }
}

static fc_http_target_t target(fc_http_session_t *s)
{
    fc_http_target_t t = {.host = "srv", .port = 443, .ca_pem = "x", .timeout_s = 5, .session = s};
    return t;
}

/* ---- exchanges --------------------------------------------------------------- */

static req_t s_req;

static void script_get(int fd)
{
    if (srv_hello(fd) == 0 && srv_request(fd, &s_req) == 0) {
        srv_headers(fd, s_req.sid, 200, "content-length", "5", 0);
        srv_send(fd, F_DATA, FL_END_STREAM, s_req.sid, "hello", 5);
    }
    srv_drain(fd);
}

static void a_get_returns_status_and_body(void)
{
    serve(script_get);
    fc_http_target_t t = target(NULL);
    t.port = 8443;
    fc_http_response_t r;
    CHECK_INT(fc_http_request(&t, "GET", "/api/v1/x", NULL, NULL, 0, &r), 0);
    CHECK_INT(r.status, 200);
    CHECK_STR(r.body, "hello");
    CHECK_INT(r.body_len, 5);
    fc_http_free(&r);
    join_servers();
    CHECK_STR(s_req.method, "GET");
    CHECK_STR(s_req.path, "/api/v1/x");
    CHECK_STR(s_req.authority, "srv:8443");
    CHECK_INT(s_req.body_len, 0);
}

static void script_put(int fd)
{
    if (srv_hello(fd) == 0 && srv_request(fd, &s_req) == 0) {
        srv_headers(fd, s_req.sid, 401, "www-authenticate", "Bearer realm=\"r\"", 1);
    }
    srv_drain(fd);
}

static void a_put_sends_its_body_and_reads_a_challenge(void)
{
    serve(script_put);
    fc_http_target_t t = target(NULL);
    t.authorization = "Basic dXNlcjpwYXNz";
    fc_http_response_t r;
    CHECK_INT(fc_http_request(&t, "PUT", "/s", "application/json", "{\"a\":1}", 7, &r), 0);
    CHECK_INT(r.status, 401);
    CHECK_STR(r.body, "");
    CHECK_STR(r.www_authenticate, "Bearer realm=\"r\"");
    fc_http_free(&r);
    join_servers();
    CHECK_STR(s_req.method, "PUT");
    CHECK_STR(s_req.authority, "srv");
    CHECK_INT(s_req.body_len, 7);
    CHECK(memcmp(s_req.body, "{\"a\":1}", 7) == 0);
    CHECK_STR(s_req.auth, "Basic dXNlcjpwYXNz");
    CHECK_INT(s_req.auth_never_indexed, 1);
}

/* Bodies of 100 KB, three times on one connection: past the 64 KB the
 * windows start with, only as the client acknowledges what it read. */
#define BIG (100 * 1024)
static uint32_t s_conn_acked;

static void script_flow(int fd)
{
    static uint8_t chunk[16384];
    memset(chunk, 'x', sizeof(chunk));
    int64_t conn_win = DEFAULT_WINDOW;
    if (srv_hello(fd) != 0) {
        return;
    }
    for (int i = 0; i < 3; i++) {
        req_t r;
        if (srv_request(fd, &r) != 0) {
            return;
        }
        conn_win += r.window_conn;
        s_conn_acked += r.window_conn;
        int64_t stream_win = DEFAULT_WINDOW;
        srv_headers(fd, r.sid, 200, NULL, NULL, 0);
        size_t sent = 0;
        while (sent < BIG) {
            size_t k = BIG - sent < sizeof(chunk) ? BIG - sent : sizeof(chunk);
            while ((int64_t)k > conn_win || (int64_t)k > stream_win) {
                frame_t f; /* wait for the client's WINDOW_UPDATE */
                if (srv_frame(fd, &f) != 0) {
                    return;
                }
                if (f.type == F_WINDOW_UPDATE) {
                    uint32_t v = (uint32_t)f.payload[0] << 24 | (uint32_t)f.payload[1] << 16 |
                                 (uint32_t)f.payload[2] << 8 | f.payload[3];
                    if (f.sid == 0) {
                        conn_win += v;
                        s_conn_acked += v;
                    } else if (f.sid == r.sid) {
                        stream_win += v;
                    }
                }
                free(f.payload);
            }
            srv_send(fd, F_DATA, sent + k == BIG ? FL_END_STREAM : 0, r.sid, chunk, k);
            conn_win -= (int64_t)k;
            stream_win -= (int64_t)k;
            sent += k;
        }
    }
    srv_drain(fd);
}

static void flow_control_keeps_a_kept_connection_going(void)
{
    serve(script_flow);
    s_conn_acked = 0;
    fc_http_session_t *s = fc_http_session_new();
    fc_http_target_t t = target(s);
    for (int i = 0; i < 3; i++) {
        fc_http_response_t r;
        CHECK_INT(fc_http_request(&t, "GET", "/big", NULL, NULL, 0, &r), 0);
        CHECK_INT(r.body_len, BIG);
        fc_http_free(&r);
    }
    CHECK_INT(s_opens, 1);
    fc_http_session_free(s);
    join_servers();
    CHECK(s_conn_acked >= 3 * BIG - DEFAULT_WINDOW);
}

/* Two threads on one connection: a long-poll that the server answers last,
 * and a request that comes later and is answered first. */
static void script_two(int fd)
{
    req_t a, b;
    if (srv_hello(fd) != 0 || srv_request(fd, &a) != 0 || srv_request(fd, &b) != 0) {
        return;
    }
    /* A PING and SETTINGS in between, which the client answers. */
    srv_send(fd, F_PING, 0, 0, "12345678", 8);
    uint8_t set[6] = {0, 4, 0, 1, 0, 0}; /* INITIAL_WINDOW_SIZE 65536 */
    srv_send(fd, F_SETTINGS, 0, 0, set, 6);
    srv_headers(fd, b.sid, 200, NULL, NULL, 0);
    srv_send(fd, F_DATA, FL_END_STREAM, b.sid, "second", 6);
    usleep(200 * 1000);
    srv_headers(fd, a.sid, 204, NULL, NULL, 1);
    int pings = 0, acks = 0;
    frame_t f;
    while (srv_frame(fd, &f) == 0) {
        pings += f.type == F_PING && (f.flags & FL_ACK) && memcmp(f.payload, "12345678", 8) == 0;
        acks += f.type == F_SETTINGS && (f.flags & FL_ACK);
        free(f.payload);
    }
    s_req.ping_acks = pings;
    s_req.settings_acks = acks + a.settings_acks + b.settings_acks;
}

typedef struct {
    fc_http_target_t t;
    const char *path;
    int rc, status;
    long long done_at;
} job_t;

static void *job(void *arg)
{
    job_t *j = arg;
    fc_http_response_t r;
    j->rc = fc_http_request(&j->t, "GET", j->path, NULL, NULL, 0, &r);
    j->status = r.status;
    j->done_at = now_ms();
    fc_http_free(&r);
    return NULL;
}

static void two_threads_share_one_connection(void)
{
    serve(script_two);
    fc_http_session_t *s = fc_http_session_new();
    job_t a = {.t = target(s), .path = "/poll"}, b = {.t = target(s), .path = "/status"};
    pthread_t ta, tb;
    pthread_create(&ta, NULL, job, &a);
    usleep(100 * 1000);
    pthread_create(&tb, NULL, job, &b);
    pthread_join(ta, NULL);
    pthread_join(tb, NULL);
    CHECK_INT(a.rc, 0);
    CHECK_INT(a.status, 204);
    CHECK_INT(b.rc, 0);
    CHECK_INT(b.status, 200);
    CHECK(b.done_at < a.done_at);
    CHECK_INT(s_opens, 1);
    fc_http_session_free(s);
    join_servers();
    CHECK_INT(s_req.ping_acks, 1);
    CHECK(s_req.settings_acks >= 2);
}

/* GOAWAY that leaves the request out: sent again, on a new connection. */
static void script_goaway(int fd)
{
    req_t r;
    if (srv_hello(fd) != 0 || srv_request(fd, &r) != 0) {
        return;
    }
    if (s_opens == 1) {
        uint8_t p[8] = {0}; /* last stream 0: nothing processed */
        srv_send(fd, F_GOAWAY, 0, 0, p, 8);
    } else {
        srv_headers(fd, r.sid, 200, NULL, NULL, 1);
    }
    srv_drain(fd);
}

static void goaway_sends_the_request_again(void)
{
    serve(script_goaway);
    fc_http_session_t *s = fc_http_session_new();
    fc_http_target_t t = target(s);
    fc_http_response_t r;
    CHECK_INT(fc_http_request(&t, "GET", "/x", NULL, NULL, 0, &r), 0);
    CHECK_INT(r.status, 200);
    fc_http_free(&r);
    CHECK_INT(s_opens, 2);
    fc_http_session_free(s);
    join_servers();
}

/* A kept connection that the server closed meanwhile: the next request goes
 * on a new one. One answered with a reset is not sent again. */
static _Atomic int s_step; /* set by the test, read by the server thread */

static void script_closes(int fd)
{
    req_t r;
    if (srv_hello(fd) != 0 || srv_request(fd, &r) != 0) {
        return;
    }
    if (s_opens == 1) {
        srv_headers(fd, r.sid, 200, NULL, NULL, 1);
        while (s_step == 0) {
            usleep(10 * 1000);
        }
        return; /* closed: the second request finds it gone */
    }
    srv_u32(fd, F_RST_STREAM, r.sid, E_PROTOCOL);
    srv_drain(fd);
}

static void a_lost_connection_is_retried_a_reset_is_not(void)
{
    serve(script_closes);
    s_step = 0;
    fc_http_session_t *s = fc_http_session_new();
    fc_http_target_t t = target(s);
    fc_http_response_t r;
    CHECK_INT(fc_http_request(&t, "GET", "/1", NULL, NULL, 0, &r), 0);
    fc_http_free(&r);
    s_step = 1;
    usleep(100 * 1000);
    CHECK(fc_http_request(&t, "GET", "/2", NULL, NULL, 0, &r) != 0); /* reset on the new connection */
    CHECK_INT(r.status, -1);
    CHECK_INT(s_opens, 2);
    fc_http_session_free(s);
    join_servers();
}

/* Headers in HEADERS and CONTINUATION; a request header block (a long
 * token) over several frames. */
static void script_continuation(int fd)
{
    if (srv_hello(fd) != 0 || srv_request(fd, &s_req) != 0) {
        return;
    }
    uint8_t b[64];
    size_t n = hpack_put_field(b, 8, "200", 0);
    size_t m = n + hpack_put_field(b + n, 28, "2", 0);
    srv_send(fd, F_HEADERS, 0, s_req.sid, b, 3);
    srv_send(fd, F_CONTINUATION, 0, s_req.sid, b + 3, n - 3);
    srv_send(fd, F_CONTINUATION, FL_END_HEADERS, s_req.sid, b + n, m - n);
    /* Padded DATA: 1 byte of length, the data, 4 bytes of padding. */
    uint8_t d[] = {4, 'o', 'k', 0, 0, 0, 0};
    srv_send(fd, F_DATA, FL_PADDED | FL_END_STREAM, s_req.sid, d, sizeof(d));
    srv_drain(fd);
}

static void header_blocks_span_frames(void)
{
    serve(script_continuation);
    static char token[20001];
    memset(token, 'T', sizeof(token) - 1);
    fc_http_target_t t = target(NULL);
    t.authorization = token;
    fc_http_response_t r;
    CHECK_INT(fc_http_request(&t, "GET", "/c", NULL, NULL, 0, &r), 0);
    CHECK_INT(r.status, 200);
    CHECK_STR(r.body, "ok");
    fc_http_free(&r);
    join_servers();
    CHECK_INT(s_req.continuations, 1);
    CHECK_INT(strlen(s_req.auth), 20000);
}

/* A streamed GET: the body handed on as it comes, without Content-Length;
 * a callback that stops it resets the stream and keeps the connection. */
static size_t s_streamed;

static int sink(const void *data, size_t len, void *ctx)
{
    s_streamed += len;
    return ctx != NULL && s_streamed > 1000 ? 1 : 0;
}

static void script_stream(int fd)
{
    static uint8_t chunk[3000];
    if (srv_hello(fd) != 0) {
        return;
    }
    for (int i = 0; i < 2; i++) {
        req_t r;
        if (srv_request(fd, &r) != 0) {
            return;
        }
        srv_headers(fd, r.sid, 200, NULL, NULL, 0);
        srv_send(fd, F_DATA, 0, r.sid, chunk, 1500);
        srv_send(fd, F_DATA, FL_END_STREAM, r.sid, chunk, 1500);
    }
    srv_drain(fd);
}

static void a_streamed_get_and_its_abort(void)
{
    serve(script_stream);
    fc_http_session_t *s = fc_http_session_new();
    fc_http_target_t t = target(s);
    int status;
    long total;
    s_streamed = 0;
    CHECK_INT(fc_http_get_stream(&t, "/blob", sink, NULL, &status, &total), 0);
    CHECK_INT(status, 200);
    CHECK_INT(total, -1);
    CHECK_INT(s_streamed, 3000);
    s_streamed = 0;
    CHECK(fc_http_get_stream(&t, "/blob", sink, (void *)1, &status, &total) != 0);
    CHECK(s_streamed > 1000 && s_streamed < 3000); /* stopped where the sink said */
    CHECK_INT(s_opens, 1);
    fc_http_session_free(s);
    join_servers();
}

/* A server that never answers: the request ends at its timeout. */
static void script_silent(int fd)
{
    req_t r;
    if (srv_hello(fd) == 0) {
        srv_request(fd, &r);
    }
    srv_drain(fd);
}

static void a_silent_server_times_out(void)
{
    serve(script_silent);
    fc_http_target_t t = target(NULL);
    t.timeout_s = 1;
    fc_http_response_t r;
    long long t0 = now_ms();
    CHECK(fc_http_request(&t, "GET", "/never", NULL, NULL, 0, &r) != 0);
    long long took = now_ms() - t0;
    CHECK(took >= 900 && took < 4000);
    CHECK_INT(s_opens, 1); /* timed out: not sent again */
    join_servers();
}

/* A frame larger than the device allows ends the connection. */
static void script_oversize(int fd)
{
    req_t r;
    if (srv_hello(fd) == 0 && srv_request(fd, &r) == 0) {
        srv_headers(fd, r.sid, 200, NULL, NULL, 0);
        static uint8_t big[OUR_MAX_FRAME + 1];
        srv_send(fd, F_DATA, FL_END_STREAM, r.sid, big, sizeof(big));
    }
    srv_drain(fd);
}

static void an_oversized_frame_is_refused(void)
{
    serve(script_oversize);
    fc_http_target_t t = target(NULL);
    fc_http_response_t r;
    CHECK(fc_http_request(&t, "GET", "/o", NULL, NULL, 0, &r) != 0);
    join_servers();
}

static void a_new_certificate_opens_a_new_connection(void)
{
    serve(script_get);
    fc_http_session_t *s = fc_http_session_new();
    fc_http_target_t t = target(s);
    t.cert_pem = "cert A";
    fc_http_response_t r;
    CHECK_INT(fc_http_request(&t, "GET", "/a", NULL, NULL, 0, &r), 0);
    fc_http_free(&r);
    t.cert_pem = "cert B";
    CHECK_INT(fc_http_request(&t, "GET", "/b", NULL, NULL, 0, &r), 0);
    fc_http_free(&r);
    CHECK_INT(s_opens, 2);
    fc_http_session_free(s);
    join_servers();
    CHECK(cert_id("cert A") != cert_id("cert B"));
    CHECK(cert_id(NULL) == cert_id(""));
}

/* ---- the TLS buffer pool ------------------------------------------------------ */

static void the_pool_serves_receive_buffers_only(void)
{
    CHECK_INT(fc_tls_pool_init(), 0);
    int now, peak;
    void *a = fc_tls_calloc(1, 16384 + 333);
    void *b = fc_tls_calloc(16384 + 333, 1);
    void *c = fc_tls_calloc(1, 16384 + 333); /* a third: from the heap */
    void *small = fc_tls_calloc(1, 4096);
    CHECK_INT(fc_tls_pool_give(a), 1);
    CHECK_INT(fc_tls_pool_give(b), 1);
    CHECK_INT(fc_tls_pool_give(c), 0); /* the heap's */
    fc_tls_pool_usage(&now, &peak);
    CHECK_INT(now, 0);
    /* Take them again, and see where they come from. */
    a = fc_tls_pool_take(1, 16384 + 333);
    b = fc_tls_pool_take(1, 16384 + 333);
    CHECK(a != NULL && b != NULL && a != b);
    CHECK(fc_tls_pool_take(1, 16384 + 333) == NULL); /* both slots taken */
    CHECK(fc_tls_pool_take(1, 4096) == NULL);         /* too small for a slot */
    CHECK(fc_tls_pool_take(1, FC_TLS_POOL_SLOT_SIZE + 1) == NULL);
    CHECK(fc_tls_pool_take(SIZE_MAX, 2) == NULL);
    fc_tls_pool_usage(&now, &peak);
    CHECK_INT(now, 2);
    CHECK_INT(peak, 2);
    CHECK_INT(((unsigned char *)a)[100], 0);
    fc_tls_free(a);
    fc_tls_free(b);
    fc_tls_free(c);
    fc_tls_free(small);
    fc_tls_free(NULL);
    CHECK_INT(fc_tls_pool_give(&now), 0);
    fc_tls_pool_usage(&now, &peak);
    CHECK_INT(now, 0);
}

int main(void)
{
    signal(SIGPIPE, SIG_IGN); /* a write to a closed test server must fail, not kill */
    RUN(huffman_decodes_rfc7541_examples);
    RUN(huffman_refuses_eos_and_bad_padding);
    RUN(hpack_integers);
    RUN(hpack_decodes_a_response_with_huffman);
    RUN(a_get_returns_status_and_body);
    RUN(a_put_sends_its_body_and_reads_a_challenge);
    RUN(flow_control_keeps_a_kept_connection_going);
    RUN(two_threads_share_one_connection);
    RUN(goaway_sends_the_request_again);
    RUN(a_lost_connection_is_retried_a_reset_is_not);
    RUN(header_blocks_span_frames);
    RUN(a_streamed_get_and_its_abort);
    RUN(a_silent_server_times_out);
    RUN(an_oversized_frame_is_refused);
    RUN(a_new_certificate_opens_a_new_connection);
    RUN(the_pool_serves_receive_buffers_only);
    return TEST_RESULT();
}
