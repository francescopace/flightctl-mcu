/*
 * Fuzzes the HTTP response reader (status line, headers, Content-Length and
 * chunked framing) through the real read loop: the input is served from a
 * file descriptor as if it came from the server.
 */
#include "../../core/src/fc_http.c"

#include <unistd.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static FILE *f;
    if (f == NULL) {
        f = tmpfile();
    }
    int fd = fileno(f);
    if (ftruncate(fd, 0) != 0 || pwrite(fd, data, size, 0) != (ssize_t)size || lseek(fd, 0, SEEK_SET) != 0) {
        return 0;
    }
    conn_t c;
    memset(&c, 0, sizeof(c));
    c.plain = 1;
    c.net.fd = fd;
    fc_http_target_t t = {.host = "fuzz", .port = 80, .plain = 1, .timeout_s = 1};
    fc_http_response_t resp = {0};
    int keep, got_any;
    if (read_response(&c, &t, "GET", "/fuzz", &resp, &keep, &got_any) == 0) {
        fc_http_free(&resp);
    }
    return 0;
}

void fc_port_log(fc_log_level_t level, const char *fmt, ...) {}
int fc_port_hwkey_pk(struct mbedtls_pk_context *pk, const struct mbedtls_pk_context *pub)
{
    return -1;
}
