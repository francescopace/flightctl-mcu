#pragma once

#include <stddef.h>

/* An HTTP/2 connection kept open across requests. Threads may share one:
 * their requests go over the same connection at once, each on its stream. */
typedef struct fc_http_session fc_http_session_t;

/* Always TLS 1.3 and HTTP/2 (ALPN h2): a server that offers neither is
 * refused, and so is a target without a CA. */
typedef struct {
    const char *host;
    int port;
    const char *ca_pem;   /* NUL terminated */
    const char *cert_pem; /* client certificate (chain), NUL terminated; NULL: no client auth */
    const char *key_pem;  /* client private key, NUL terminated */
    int hw_key;           /* 1: the client key is in hardware, key_pem unused */
    const char *accept;   /* Accept header; NULL: application/json */
    const char *authorization; /* Authorization header's value; NULL: none */
    int timeout_s;        /* no frame for the request for this long: it fails, with its connection */
    fc_http_session_t *session; /* NULL: a connection for this request alone */
} fc_http_target_t;

typedef struct {
    int status;   /* HTTP status, or -1 on transport error */
    char *body;   /* NUL terminated, caller frees with fc_http_free */
    size_t body_len;
    char *www_authenticate; /* the WWW-Authenticate header of a 401, or NULL */
} fc_http_response_t;

int fc_http_global_init(void);

fc_http_session_t *fc_http_session_new(void);
/* Lets go of the connection: requests still on it finish, and the last one
 * closes it. The session can still be used. */
void fc_http_session_close(fc_http_session_t *s);
/* Closes it and frees it. */
void fc_http_session_free(fc_http_session_t *s);

/* Performs one request and buffers the response (max 512 KB). With
 * t->session the connection is the session's, opened again when it is gone
 * or the client certificate changed; without, it is closed afterwards. A
 * request the server never took (its connection lost before any answer, a
 * GOAWAY that leaves it out, REFUSED_STREAM) is sent once more on a new
 * connection. `content_type` and `body` may be NULL. Returns 0 if a
 * response was read. */
int fc_http_request(const fc_http_target_t *t, const char *method,
                    const char *path, const char *content_type,
                    const char *body, size_t body_len,
                    fc_http_response_t *resp);

void fc_http_free(fc_http_response_t *resp);

/* GET with the body streamed to `on_data`, for downloads larger than RAM,
 * on the session's connection if t->session is set. on_data runs on the
 * thread that reads the connection: a session used for streams is one
 * thread's. *status gets the HTTP status; on_data is called only for 200
 * responses and may return non-zero to abort (the stream is reset, the
 * connection kept). *total gets the Content-Length, or -1. Returns 0 if
 * the whole body was delivered. */
typedef int (*fc_http_data_cb)(const void *data, size_t len, void *ctx);
int fc_http_get_stream(const fc_http_target_t *t, const char *path, fc_http_data_cb on_data,
                       void *ctx, int *status, long *total);

/* Bytes sent/received on the wire since boot (TLS and HTTP overhead included). */
void fc_http_traffic(unsigned long long *tx, unsigned long long *rx);

/* TLS handshakes since boot. */
unsigned long fc_http_handshakes(void);
