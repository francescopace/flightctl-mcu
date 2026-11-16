/*
 * Fuzzes what the remote console does with the server's bytes: gRPC framing,
 * the StreamResponse protobuf, the remote command channels and the line
 * editor of a terminal session. Commands run (help, status, apps); reboot is
 * counted, not done.
 */
#include "../../core/src/fc_console.c"

#include <stdlib.h>

struct fc_http_stream {
    int unused;
};

fc_http_stream_t *fc_http_stream_open(const fc_http_target_t *t, const char *path, const char *content_type,
                                      const char *const *headers)
{
    static fc_http_stream_t st;
    return &st;
}
int fc_http_stream_write(fc_http_stream_t *st, const void *data, size_t len, int end, int timeout_ms) { return 0; }
int fc_http_stream_read(fc_http_stream_t *st, void *buf, size_t cap, int timeout_ms) { return -1; }
void fc_http_stream_status(fc_http_stream_t *st, int *http_status, int *grpc_status) {}
void fc_http_stream_close(fc_http_stream_t *st) {}
void fc_apps_status(cJSON *apps, cJSON *summary, cJSON *custom_info) {}
void fc_port_host_init(const char *data_dir, int debug);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static int inited;
    if (!inited) {
        fc_port_host_init("/tmp/fc-fuzz-console", 0);
        inited = 1;
    }
    static fc_http_stream_t st;
    session_t *s = calloc(1, sizeof(*s));
    s->st = &st;
    s->tty = size > 0 && (data[0] & 1);
    on_bytes(s, data, size);
    free(s->command);
    free(s);
    return 0;
}
