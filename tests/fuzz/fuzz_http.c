/*
 * Fuzzes the HTTP/2 client as the server would: the input is what the server
 * sends (frames, HPACK header blocks, Huffman strings), read through the real
 * request path; what the client writes is dropped. The input is also decoded
 * as one header block on its own.
 */
#include "../../core/src/fc_http.c"

typedef struct {
    const uint8_t *data;
    size_t size, off;
} mem_t;

static int mem_read(conn_t *c, unsigned char *buf, size_t len)
{
    mem_t *m = c->tctx;
    size_t n = m->size - m->off < len ? m->size - m->off : len;
    memcpy(buf, m->data + m->off, n);
    m->off += n;
    return (int)n; /* 0 at the end: the server closed */
}

static int mem_write(conn_t *c, const unsigned char *buf, size_t len)
{
    return (int)len;
}

static int mem_wait(conn_t *c, int timeout_ms)
{
    return 1; /* data, or the end, which a read reports */
}

static void mem_close(conn_t *c) {}

static const transport_t MEM = {mem_read, mem_write, mem_wait, mem_close};
static mem_t s_mem;

static conn_t *mem_open(const fc_http_target_t *t)
{
    return conn_new(&MEM, &s_mem);
}

static int sink(const void *data, size_t len, void *ctx)
{
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    hpack_resp_t h;
    hpack_decode(data, size, &h);
    free(h.www_authenticate);

    s_open_hook = mem_open;
    fc_http_target_t t = {.host = "fuzz", .port = 443, .ca_pem = "x", .timeout_s = 1};
    s_mem = (mem_t){data, size, 0};
    fc_http_response_t resp;
    if (fc_http_request(&t, "PUT", "/fuzz", "application/json", "{}", 2, &resp) == 0) {
        fc_http_free(&resp);
    }
    s_mem = (mem_t){data, size, 0};
    int status;
    long total;
    fc_http_get_stream(&t, "/blob", sink, NULL, &status, &total);
    return 0;
}

void fc_port_log(fc_log_level_t level, const char *fmt, ...) {}
long fc_port_heap_largest(void)
{
    return -1;
}
long fc_port_heap_watch(int start)
{
    return -1;
}
int fc_port_hwkey_pk(struct mbedtls_pk_context *pk, const struct mbedtls_pk_context *pub)
{
    return -1;
}
