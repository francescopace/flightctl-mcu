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
    int connect_timeout_s; /* >0: the TCP connect gives up after this long; 0: the system's (a minute or more) */
    int (*interrupt_wait)(void); /* Optional lock-free check while waiting for response headers. */
    int discard_200_body; /* fc_http_request drains an HTTP 200 body and returns an empty string. */
} fc_http_target_t;

/* The caller interrupted its own request. Other streams keep their connection. */
#define FC_HTTP_INTERRUPTED (-2)

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
/* Release the TLS heap only if no request or console is using it. */
int fc_http_session_close_idle(fc_http_session_t *s);
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

/* A bidirectional stream (gRPC) on the session's connection, never on a
 * new one: NULL if t->session has no connection open, or it cannot be
 * opened. Without t->session, on a connection of its own, opened now and
 * closed with the stream. POST path with content_type and the extra headers, {name, value,
 * ..., NULL} (names lower case); the response headers are not waited for.
 * The stream ends when the session lets go of its connection
 * (fc_http_session_close: a download's turn, a new certificate). */
typedef struct fc_http_stream fc_http_stream_t;
fc_http_stream_t *fc_http_stream_open(const fc_http_target_t *t, const char *path, const char *content_type,
                                      const char *const *headers);
/* Sends data, END_STREAM with it if `end`. Waits up to timeout_ms for the
 * peer's window; 0, or -1 (the stream is over, or no room in time). */
int fc_http_stream_write(fc_http_stream_t *st, const void *data, size_t len, int end, int timeout_ms);
/* What came: >0 bytes, 0 nothing within timeout_ms, -1 the stream is over
 * (ended, reset, the connection lost or let go) and nothing is left. While
 * it waits, the thread reads the connection's frames for every stream on
 * it, as a request does. More than 4 KB not read ends the stream. A wait
 * that runs out leaves the connection alone. */
int fc_http_stream_read(fc_http_stream_t *st, void *buf, size_t cap, int timeout_ms);
/* The response's HTTP status (-1 none yet) and grpc-status (-1 none yet). */
void fc_http_stream_status(fc_http_stream_t *st, int *http_status, int *grpc_status);
/* Resets it if still open, and frees it. */
void fc_http_stream_close(fc_http_stream_t *st);

/* Bytes sent/received on the wire since boot (TLS and HTTP overhead included). */
void fc_http_traffic(unsigned long long *tx, unsigned long long *rx);

/* TLS handshakes since boot. */
unsigned long fc_http_handshakes(void);

/* mbedtls_net_connect (TCP), given up after timeout_s; on Zephyr the port's
 * own, bounded already. For a client of its own (fc_mqtt.c). */
struct mbedtls_net_context;
int fc_net_connect_within(struct mbedtls_net_context *ctx, const char *host, const char *port, int timeout_s);
