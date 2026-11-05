#pragma once

#include <stddef.h>

typedef struct {
    const char *host;
    int port;
    int plain;            /* 1: plain HTTP (local registry), 0: TLS 1.3 */
    const char *ca_pem;   /* NUL terminated (TLS only) */
    const char *cert_pem; /* client certificate (chain), NUL terminated; NULL: no client auth */
    const char *key_pem;  /* client private key, NUL terminated */
    const char *accept;   /* Accept header; NULL: application/json */
    int timeout_s;        /* read timeout (no data for this long) */
} fc_http_target_t;

typedef struct {
    int status;   /* HTTP status, or -1 on transport error */
    char *body;   /* NUL terminated, caller frees with fc_http_free */
    size_t body_len;
} fc_http_response_t;

int fc_http_global_init(void);

/* Performs one request on a fresh connection (Connection: close) and buffers
 * the response (max 512 KB). `content_type` and `body` may be NULL.
 * Returns 0 if a response was read. */
int fc_http_request(const fc_http_target_t *t, const char *method,
                    const char *path, const char *content_type,
                    const char *body, size_t body_len,
                    fc_http_response_t *resp);

void fc_http_free(fc_http_response_t *resp);

/* GET with the body streamed to `on_data` (Content-Length bodies only), for
 * downloads larger than RAM. *status gets the HTTP status; on_data is called
 * only for 200 responses and may return non-zero to abort. *total gets the
 * Content-Length. Returns 0 if the whole body was delivered. */
typedef int (*fc_http_data_cb)(const void *data, size_t len, void *ctx);
int fc_http_get_stream(const fc_http_target_t *t, const char *path, fc_http_data_cb on_data,
                       void *ctx, int *status, long *total);
