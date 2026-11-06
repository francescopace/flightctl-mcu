/*
 * Minimal HTTP/1.1 client on mbedTLS + BSD sockets (mbedtls_net), so the
 * same code runs on POSIX hosts and on ESP-IDF (lwIP). TLS 1.3 with mutual
 * auth for Flight Control, plain HTTP for the local PoC registry.
 *
 * One request per connection ("Connection: close"). fc_http_request reads
 * until the peer closes, then parses headers and decodes chunked bodies;
 * fc_http_get_stream hands the body to a callback as it arrives.
 */
#include "fc_http.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "fc_port.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"
#include "psa/crypto.h"

#define FC_HTTP_MAX_RESPONSE (512 * 1024)
#define FC_HTTP_MAX_HEADER   4096

/* Raw bytes on the wire (TCP payload, TLS included), for traffic figures. */
static volatile unsigned long long s_tx_bytes, s_rx_bytes;

void fc_http_traffic(unsigned long long *tx, unsigned long long *rx)
{
    *tx = s_tx_bytes;
    *rx = s_rx_bytes;
}

static int count_send(void *ctx, const unsigned char *buf, size_t len)
{
    int rc = mbedtls_net_send(ctx, buf, len);
    if (rc > 0) {
        s_tx_bytes += (unsigned long long)rc;
    }
    return rc;
}

static int count_recv_timeout(void *ctx, unsigned char *buf, size_t len, uint32_t timeout)
{
    int rc = mbedtls_net_recv_timeout(ctx, buf, len, timeout);
    if (rc > 0) {
        s_rx_bytes += (unsigned long long)rc;
    }
    return rc;
}

static mbedtls_entropy_context s_entropy;
static mbedtls_ctr_drbg_context s_drbg;
static int s_inited;

int fc_http_global_init(void)
{
    if (s_inited) {
        return 0;
    }
    /* TLS 1.3 in mbedTLS 3.x runs on PSA crypto. */
    if (psa_crypto_init() != PSA_SUCCESS) {
        fc_port_log(FC_LOG_ERROR, "psa_crypto_init failed");
        return -1;
    }
    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_drbg);
    const char *pers = "flightctl-mcu";
    int rc = mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &s_entropy,
                                   (const unsigned char *)pers, strlen(pers));
    if (rc != 0) {
        fc_port_log(FC_LOG_ERROR, "ctr_drbg_seed: -0x%04x", -rc);
        return -1;
    }
    s_inited = 1;
    return 0;
}

mbedtls_ctr_drbg_context *fc_http_drbg(void)
{
    return &s_drbg;
}

static void log_mbedtls(const char *what, int rc)
{
    char buf[128];
    mbedtls_strerror(rc, buf, sizeof(buf));
    fc_port_log(FC_LOG_WARN, "%s: -0x%04x %s", what, -rc, buf);
}

/* ---- connection (TLS or plain) ------------------------------------------ */

typedef struct {
    int plain;
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca, own;
    mbedtls_pk_context key;
} conn_t;

static void conn_close(conn_t *c)
{
    if (!c->plain) {
        mbedtls_ssl_close_notify(&c->ssl);
    }
    mbedtls_net_free(&c->net);
    mbedtls_ssl_free(&c->ssl);
    mbedtls_ssl_config_free(&c->conf);
    mbedtls_x509_crt_free(&c->ca);
    mbedtls_x509_crt_free(&c->own);
    mbedtls_pk_free(&c->key);
}

static int conn_open(conn_t *c, const fc_http_target_t *t)
{
    memset(c, 0, sizeof(*c));
    c->plain = t->plain;
    mbedtls_net_init(&c->net);
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_x509_crt_init(&c->ca);
    mbedtls_x509_crt_init(&c->own);
    mbedtls_pk_init(&c->key);
    if (fc_http_global_init() != 0) {
        return -1;
    }
    char port[8];
    snprintf(port, sizeof(port), "%d", t->port);
    int rc;

    if (!c->plain) {
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
        mbedtls_ssl_conf_rng(&c->conf, mbedtls_ctr_drbg_random, &s_drbg);
        mbedtls_ssl_conf_read_timeout(&c->conf, (uint32_t)t->timeout_s * 1000);
        if (t->cert_pem != NULL) {
            if ((rc = mbedtls_x509_crt_parse(&c->own, (const unsigned char *)t->cert_pem,
                                             strlen(t->cert_pem) + 1)) != 0) {
                log_mbedtls("parse client cert", rc);
                return -1;
            }
            if ((rc = mbedtls_pk_parse_key(&c->key, (const unsigned char *)t->key_pem, strlen(t->key_pem) + 1,
                                           NULL, 0, mbedtls_ctr_drbg_random, &s_drbg)) != 0) {
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
    }

    if ((rc = mbedtls_net_connect(&c->net, t->host, port, MBEDTLS_NET_PROTO_TCP)) != 0) {
        log_mbedtls("connect", rc);
        return -1;
    }
    if (c->plain) {
        return 0;
    }
    mbedtls_ssl_set_bio(&c->ssl, &c->net, count_send, NULL, count_recv_timeout);
    while ((rc = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            log_mbedtls("handshake", rc);
            uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);
            if (flags != 0) {
                char vbuf[256];
                mbedtls_x509_crt_verify_info(vbuf, sizeof(vbuf), "  ", flags);
                fc_port_log(FC_LOG_WARN, "certificate verification:\n%s", vbuf);
            }
            return -1;
        }
    }
    return 0;
}

static int conn_write_all(conn_t *c, const void *data, size_t len)
{
    const unsigned char *p = data;
    size_t off = 0;
    while (off < len) {
        int rc = c->plain ? count_send(&c->net, p + off, len - off)
                          : mbedtls_ssl_write(&c->ssl, p + off, len - off);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (rc < 0) {
            log_mbedtls("write", rc);
            return -1;
        }
        off += (size_t)rc;
    }
    return 0;
}

/* Returns bytes read, 0 on orderly close, <0 on error. */
static int conn_read(conn_t *c, void *buf, size_t len, int timeout_s)
{
    for (;;) {
        int rc = c->plain ? count_recv_timeout(&c->net, buf, len, (uint32_t)timeout_s * 1000)
                          : mbedtls_ssl_read(&c->ssl, buf, len);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE ||
            rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            continue;
        }
        if (rc == MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY || rc == MBEDTLS_ERR_NET_CONN_RESET) {
            return 0;
        }
        return rc;
    }
}

static int send_request(conn_t *c, const fc_http_target_t *t, const char *method, const char *path,
                        const char *content_type, const char *body, size_t body_len)
{
    char head[768];
    int n = snprintf(head, sizeof(head),
                     "%s %s HTTP/1.1\r\n"
                     "Host: %s:%d\r\n"
                     "User-Agent: flightctl-mcu\r\n"
                     "Accept: %s\r\n"
                     "Connection: close\r\n"
                     "%s%s%s"
                     "Content-Length: %zu\r\n\r\n",
                     method, path, t->host, t->port, t->accept ? t->accept : "application/json",
                     content_type ? "Content-Type: " : "", content_type ? content_type : "",
                     content_type ? "\r\n" : "", body ? body_len : 0);
    if (n < 0 || (size_t)n >= sizeof(head)) {
        return -1;
    }
    if (conn_write_all(c, head, (size_t)n) != 0) {
        return -1;
    }
    return body && body_len ? conn_write_all(c, body, body_len) : 0;
}

/* ---- buffered request ----------------------------------------------------- */

/* Decodes a chunked body in place. Returns the decoded length or -1. */
static long dechunk(char *body, size_t len)
{
    char *in = body, *out = body, *end = body + len;
    for (;;) {
        char *eol = strstr(in, "\r\n");
        if (eol == NULL || eol >= end) {
            return -1;
        }
        unsigned long n = strtoul(in, NULL, 16);
        in = eol + 2;
        if (n == 0) {
            break;
        }
        if (in + n > end) {
            return -1;
        }
        memmove(out, in, n);
        out += n;
        in += n + 2; /* skip CRLF after chunk */
    }
    *out = '\0';
    return out - body;
}

/* Parses the status line and the headers we care about; *hdr_end must point
 * at the "\r\n\r\n" terminator, which is overwritten with NUL. */
static int parse_headers(char *raw, char *hdr_end, int *status, int *chunked, long *content_length)
{
    if (strncmp(raw, "HTTP/1.", 7) != 0) {
        return -1;
    }
    *hdr_end = '\0';
    *status = atoi(raw + 9);
    *chunked = 0;
    *content_length = -1;
    for (char *line = strstr(raw, "\r\n"); line != NULL; line = strstr(line, "\r\n")) {
        line += 2;
        if (strncasecmp(line, "Transfer-Encoding:", 18) == 0 && strstr(line, "chunked")) {
            *chunked = 1;
        } else if (strncasecmp(line, "Content-Length:", 15) == 0) {
            *content_length = atol(line + 15);
        }
    }
    return 0;
}

static int parse_response(char *raw, size_t len, fc_http_response_t *resp)
{
    char *hdr_end = strstr(raw, "\r\n\r\n");
    int chunked;
    long content_length;
    if (hdr_end == NULL || parse_headers(raw, hdr_end, &resp->status, &chunked, &content_length) != 0) {
        return -1;
    }
    char *body = hdr_end + 4;
    size_t body_len = len - (body - raw);
    if (chunked) {
        long n = dechunk(body, body_len);
        if (n < 0) {
            return -1;
        }
        body_len = (size_t)n;
    } else if (content_length >= 0 && (size_t)content_length < body_len) {
        body_len = (size_t)content_length;
    }

    resp->body = malloc(body_len + 1);
    if (resp->body == NULL) {
        return -1;
    }
    memcpy(resp->body, body, body_len);
    resp->body[body_len] = '\0';
    resp->body_len = body_len;
    return 0;
}

int fc_http_request(const fc_http_target_t *t, const char *method,
                    const char *path, const char *content_type,
                    const char *body, size_t body_len,
                    fc_http_response_t *resp)
{
    memset(resp, 0, sizeof(*resp));
    resp->status = -1;
    conn_t c;
    char *raw = NULL;
    size_t raw_len = 0;
    int ret = -1;

    if (conn_open(&c, t) != 0 || send_request(&c, t, method, path, content_type, body, body_len) != 0) {
        goto out;
    }
    size_t cap = 4096;
    raw = malloc(cap);
    if (raw == NULL) {
        goto out;
    }
    for (;;) {
        if (raw_len + 1024 > cap) {
            if (cap >= FC_HTTP_MAX_RESPONSE) {
                fc_port_log(FC_LOG_WARN, "response too large");
                goto out;
            }
            cap *= 2;
            char *n = realloc(raw, cap);
            if (n == NULL) {
                goto out;
            }
            raw = n;
        }
        int rc = conn_read(&c, raw + raw_len, cap - raw_len - 1, t->timeout_s);
        if (rc > 0) {
            raw_len += (size_t)rc;
            continue;
        }
        if (rc == 0) {
            break;
        }
        if ((rc == MBEDTLS_ERR_SSL_TIMEOUT || rc == MBEDTLS_ERR_NET_RECV_FAILED) && raw_len == 0) {
            fc_port_log(FC_LOG_DEBUG, "%s %s: read timeout", method, path);
        } else {
            log_mbedtls("read", rc);
        }
        goto out;
    }
    raw[raw_len] = '\0';
    if (parse_response(raw, raw_len, resp) != 0) {
        fc_port_log(FC_LOG_WARN, "%s %s: malformed response (%zu bytes)", method, path, raw_len);
        goto out;
    }
    ret = 0;

out:
    free(raw);
    conn_close(&c);
    return ret;
}

void fc_http_free(fc_http_response_t *resp)
{
    free(resp->body);
    resp->body = NULL;
}

/* ---- streamed GET --------------------------------------------------------- */

int fc_http_get_stream(const fc_http_target_t *t, const char *path, fc_http_data_cb on_data,
                       void *ctx, int *status, long *total)
{
    *status = -1;
    *total = -1;
    conn_t c;
    int ret = -1;
    char *buf = malloc(FC_HTTP_MAX_HEADER + 1);
    if (buf == NULL) {
        return -1;
    }
    if (conn_open(&c, t) != 0 || send_request(&c, t, "GET", path, NULL, NULL, 0) != 0) {
        goto out;
    }

    /* Headers: read until the blank line; whatever follows is body. */
    size_t have = 0;
    char *hdr_end = NULL;
    while (hdr_end == NULL) {
        if (have >= FC_HTTP_MAX_HEADER) {
            fc_port_log(FC_LOG_WARN, "GET %s: headers too large", path);
            goto out;
        }
        int rc = conn_read(&c, buf + have, FC_HTTP_MAX_HEADER - have, t->timeout_s);
        if (rc <= 0) {
            log_mbedtls("read headers", rc);
            goto out;
        }
        have += (size_t)rc;
        buf[have] = '\0';
        hdr_end = strstr(buf, "\r\n\r\n");
    }
    int chunked;
    if (parse_headers(buf, hdr_end, status, &chunked, total) != 0) {
        goto out;
    }
    if (*status != 200) {
        goto out;
    }
    if (chunked || *total < 0) {
        fc_port_log(FC_LOG_WARN, "GET %s: streaming needs Content-Length", path);
        goto out;
    }

    long got = 0;
    size_t extra = have - (size_t)(hdr_end + 4 - buf);
    if (extra > 0) {
        if (on_data(hdr_end + 4, extra, ctx) != 0) {
            goto out;
        }
        got += (long)extra;
    }
    while (got < *total) {
        size_t want = (size_t)(*total - got) < FC_HTTP_MAX_HEADER ? (size_t)(*total - got) : FC_HTTP_MAX_HEADER;
        int rc = conn_read(&c, buf, want, t->timeout_s);
        if (rc <= 0) {
            fc_port_log(FC_LOG_WARN, "GET %s: connection lost at %ld/%ld", path, got, *total);
            goto out;
        }
        if (on_data(buf, (size_t)rc, ctx) != 0) {
            goto out;
        }
        got += rc;
    }
    ret = 0;

out:
    free(buf);
    conn_close(&c);
    return ret;
}
