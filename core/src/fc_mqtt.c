/*
 * MQTT 3.1.1 client for the apps (fc_mqtt.h): one connection for the whole
 * device, QoS 0 and 1, clean sessions, on mbedTLS and BSD sockets like
 * fc_http.c, so the same code runs on POSIX hosts, ESP-IDF and Zephyr.
 *
 * Written here rather than taken from a library: ESP-IDF's esp-mqtt and
 * Zephyr's MQTT library are each one port's, and what the apps need
 * (CONNECT, PUBLISH both ways, SUBSCRIBE, PING) is a few hundred lines.
 *
 * Threads. The client's thread connects, reads and keeps the link alive; the
 * apps' threads publish and subscribe on the same connection. One packet is
 * written or read at a time, under s_io; waiting for data is not under it.
 * s_lock keeps the config, the subscriptions and the counters; it is never
 * held while taking s_io (the order is s_io, then s_lock). The pool has a
 * lock of its own, taken last.
 *
 * Received messages. The topic and the packet id are read into a small
 * buffer, the payload straight into a block of the receive pool, one per
 * subscription it matches (each app gets its own copy, with its topic
 * relative to its own prefix). A message the device cannot hold (larger than
 * FC_MQTT_MAX_PAYLOAD, a topic too long, the pool full or the app's share of
 * it used) is dropped, counted and, at QoS 1, still acknowledged: with a
 * clean session the broker would not send it again anyway.
 */
#include "fc_mqtt.h"

#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>

#include "cJSON.h"
#include "fc_crypto.h"
#include "fc_http.h"
#include "fc_hw.h"
#include "fc_port.h"
#include "fc_tls_pool.h"
#include "mbedtls/error.h"
#include "mbedtls/net_sockets.h"
#include "mbedtls/ssl.h"
#include "mbedtls/x509_crt.h"

#define FULL_TOPIC_MAX   (FC_MQTT_MAX_PREFIX + FC_MQTT_MAX_TOPIC)
/* A PUBLISH as the apps send it, and the CONNECT. */
#define TX_CAP           (5 + 2 + FULL_TOPIC_MAX + 2 + FC_MQTT_MAX_PAYLOAD)
/* What is read before a payload: a PUBLISH's topic and packet id. */
#define RX_CAP           (FULL_TOPIC_MAX + 4)
#define CONNECT_TIMEOUT_S 15
#define READ_TIMEOUT_MS  10000 /* the rest of a packet that began */
#define FIRST_READ_MS    50    /* a packet's first byte, once the socket has something */
#define SEND_TIMEOUT_S   10
#define WAIT_SLICE_MS    500   /* how soon a stop or a keep-alive is seen */
#define BACKOFF_MIN_MS   2000
#define BACKOFF_MAX_MS   120000
#define DROP_LOG_MS      60000 /* one line about dropped messages a minute at most */

#ifndef FC_MQTT_STACK_SIZE
#if defined(ESP_PLATFORM) || (defined(__ZEPHYR__) && !defined(CONFIG_ARCH_POSIX))
/* The TLS handshake (ECDHE, X.509): as the agent's status thread. */
#define FC_MQTT_STACK_SIZE (12 * 1024)
#else
#define FC_MQTT_STACK_SIZE (256 * 1024)
#endif
#endif

/* Where mbedTLS's receive buffer is the whole record (16 KB) for every
 * connection, this one takes a block of the TLS pool for as long as it is
 * open; with ESP-IDF's dynamic buffers it is sized to each record, and an
 * MQTT record is small. */
#if defined(CONFIG_MBEDTLS_DYNAMIC_BUFFER)
#define NEEDS_TLS_BLOCK 0
#else
#define NEEDS_TLS_BLOCK 1
#endif

enum {
    P_CONNECT = 1, P_CONNACK = 2, P_PUBLISH = 3, P_PUBACK = 4, P_SUBSCRIBE = 8, P_SUBACK = 9,
    P_UNSUBSCRIBE = 10, P_UNSUBACK = 11, P_PINGREQ = 12, P_PINGRESP = 13, P_DISCONNECT = 14,
};

static long long now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static void log_mbedtls(const char *what, int rc)
{
    char buf[100];
    if (rc == PSA_ERROR_INSUFFICIENT_MEMORY) {
        snprintf(buf, sizeof(buf), "out of memory");
    } else {
        mbedtls_strerror(rc, buf, sizeof(buf));
    }
    fc_port_log(FC_LOG_WARN, "mqtt: %s: -0x%04x %s", what, -rc, buf);
}

/* ---- the config (FC_MQTT_CONFIG) ------------------------------------------ */

typedef struct {
    char host[128];
    int port;
    int tls;
    char user[96];
    char pass[128];
    char client_id[128];
    int keepalive_s;
    /* Prefixes with {device} put in, {app} still to come; "" for none. */
    char dev_prefix[FC_MQTT_MAX_PREFIX + 1];
    char fleet_prefix[FC_MQTT_MAX_PREFIX + 1];
    char *ca_pem; /* NUL terminated, NULL for plain TCP */
} cfg_t;

static void cfg_free(cfg_t *c)
{
    if (c != NULL) {
        free(c->ca_pem);
        free(c);
    }
}

static int cfg_same(const cfg_t *a, const cfg_t *b)
{
    if (a == NULL || b == NULL) {
        return a == b;
    }
    if (strcmp(a->host, b->host) != 0 || a->port != b->port || a->tls != b->tls || strcmp(a->user, b->user) != 0 ||
        strcmp(a->pass, b->pass) != 0 || strcmp(a->client_id, b->client_id) != 0 ||
        a->keepalive_s != b->keepalive_s || strcmp(a->dev_prefix, b->dev_prefix) != 0 ||
        strcmp(a->fleet_prefix, b->fleet_prefix) != 0) {
        return 0;
    }
    return (a->ca_pem == NULL && b->ca_pem == NULL) ||
           (a->ca_pem != NULL && b->ca_pem != NULL && strcmp(a->ca_pem, b->ca_pem) == 0);
}

/* "mqtts://host:port", "mqtt://host:port" or "host:port" (TLS); IPv6 in
 * brackets. 0, or -1. */
static int parse_broker(const char *url, char *host, size_t hcap, int *port, int *tls)
{
    const char *p = url;
    *tls = 1;
    *port = 8883;
    if (strncmp(p, "mqtts://", 8) == 0) {
        p += 8;
    } else if (strncmp(p, "mqtt://", 7) == 0) {
        p += 7;
        *tls = 0;
        *port = 1883;
    } else if (strstr(p, "://") != NULL) {
        return -1;
    }
    const char *h = p, *he;
    if (*p == '[') {
        h = p + 1;
        he = strchr(h, ']');
        if (he == NULL) {
            return -1;
        }
        p = he + 1;
    } else {
        he = p;
        while (*he != '\0' && *he != ':' && *he != '/') {
            he++;
        }
        p = he;
    }
    size_t hl = (size_t)(he - h);
    if (hl == 0 || hl >= hcap) {
        return -1;
    }
    memcpy(host, h, hl);
    host[hl] = '\0';
    if (*p == ':') {
        char *end;
        long v = strtol(p + 1, &end, 10);
        if (end == p + 1 || v < 1 || v > 65535) {
            return -1;
        }
        *port = (int)v;
        p = end;
    }
    if (*p == '/') {
        p++;
    }
    return *p == '\0' ? 0 : -1;
}

/* Puts `with` in place of each `name` in `tmpl`: the length, or -1 if it
 * does not fit in cap - 1. */
static int replace(const char *tmpl, const char *name, const char *with, char *out, size_t cap)
{
    size_t nl = strlen(name), wl = strlen(with), o = 0;
    for (const char *p = tmpl; *p != '\0';) {
        if (strncmp(p, name, nl) == 0) {
            if (o + wl >= cap) {
                return -1;
            }
            memcpy(out + o, with, wl);
            o += wl;
            p += nl;
        } else {
            if (o + 1 >= cap) {
                return -1;
            }
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
    return (int)o;
}

/* A prefix template from the config, with {device} put in and a '/' at its
 * end: 0, or -1 (logged). */
static int prefix_template(const char *field, const char *tmpl, const char *device, char *out, size_t cap)
{
    if (tmpl[0] == '\0' || tmpl[0] == '$' || strpbrk(tmpl, "+#") != NULL) {
        fc_port_log(FC_LOG_WARN, "mqtt: %s \"%s\": empty, or with a wildcard or a leading '$'", field, tmpl);
        return -1;
    }
    int n = replace(tmpl, "{device}", device, out, cap - 1);
    if (n <= 0) {
        fc_port_log(FC_LOG_WARN, "mqtt: %s too long", field);
        return -1;
    }
    if (out[n - 1] != '/') {
        out[n] = '/';
        out[n + 1] = '\0';
    }
    if (strpbrk(out, "+#") != NULL) {
        fc_port_log(FC_LOG_WARN, "mqtt: the device's name makes %s a wildcard", field);
        return -1;
    }
    return 0;
}

static const char *json_str(const cJSON *o, const char *name, const char *def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, name);
    return cJSON_IsString(v) && v->valuestring != NULL ? v->valuestring : def;
}

static int copy_str(char *out, size_t cap, const char *s, const char *field)
{
    size_t n = strlen(s);
    if (n >= cap) {
        fc_port_log(FC_LOG_WARN, "mqtt: %s longer than %u characters", field, (unsigned)(cap - 1));
        return -1;
    }
    memcpy(out, s, n + 1);
    return 0;
}

/* The config from mqtt.json's text and the CA's (NULL: none), for `device`.
 * NULL, logged, when it cannot be used. */
static cfg_t *cfg_parse(const char *json, const char *ca_pem, const char *device)
{
    cJSON *o = cJSON_Parse(json);
    cfg_t *c = calloc(1, sizeof(*c));
    int ok = 0;
    if (c == NULL || !cJSON_IsObject(o)) {
        fc_port_log(FC_LOG_WARN, "mqtt: %s is not a JSON object", FC_MQTT_CONFIG);
        goto out;
    }
    const char *broker = json_str(o, "broker", NULL);
    if (broker == NULL || parse_broker(broker, c->host, sizeof(c->host), &c->port, &c->tls) != 0) {
        fc_port_log(FC_LOG_WARN, "mqtt: no usable \"broker\" (mqtts://host:port or mqtt://host:port)");
        goto out;
    }
    if (copy_str(c->user, sizeof(c->user), json_str(o, "username", ""), "username") != 0 ||
        copy_str(c->pass, sizeof(c->pass), json_str(o, "password", ""), "password") != 0) {
        goto out;
    }
    if (c->pass[0] != '\0' && c->user[0] == '\0') {
        fc_port_log(FC_LOG_WARN, "mqtt: a password without a username");
        goto out;
    }
    if (replace(json_str(o, "clientId", "{device}"), "{device}", device, c->client_id, sizeof(c->client_id)) <= 0) {
        fc_port_log(FC_LOG_WARN, "mqtt: clientId empty or too long");
        goto out;
    }
    const cJSON *ka = cJSON_GetObjectItemCaseSensitive(o, "keepAliveSeconds");
    c->keepalive_s = 60;
    if (ka != NULL) {
        if (!cJSON_IsNumber(ka) || ka->valuedouble < 10 || ka->valuedouble > 3600) {
            fc_port_log(FC_LOG_WARN, "mqtt: keepAliveSeconds must be 10 to 3600");
            goto out;
        }
        c->keepalive_s = (int)ka->valuedouble;
    }
    if (prefix_template("devicePrefix", json_str(o, "devicePrefix", "flightctl/{device}/{app}/"), device,
                        c->dev_prefix, sizeof(c->dev_prefix)) != 0) {
        goto out;
    }
    const char *fleet = json_str(o, "fleetPrefix", NULL);
    if (fleet != NULL &&
        prefix_template("fleetPrefix", fleet, device, c->fleet_prefix, sizeof(c->fleet_prefix)) != 0) {
        goto out;
    }
    if (strstr(c->dev_prefix, "{app}") == NULL) {
        fc_port_log(FC_LOG_WARN, "mqtt: devicePrefix has no {app}: the apps share their topics");
    }
    if (c->tls) {
        if (ca_pem == NULL || ca_pem[0] == '\0') {
            fc_port_log(FC_LOG_WARN, "mqtt: %s needs its CA in %s", broker, FC_MQTT_CA_FILE);
            goto out;
        }
        if ((c->ca_pem = strdup(ca_pem)) == NULL) {
            goto out;
        }
    }
    ok = 1;
out:
    cJSON_Delete(o);
    if (!ok) {
        cfg_free(c);
        return NULL;
    }
    return c;
}

/* ---- topics ------------------------------------------------------------------ */

/* A relative topic to publish on: 1..FC_MQTT_MAX_TOPIC, no wildcard, no NUL. */
static int topic_ok(const char *t, size_t n)
{
    if (n == 0 || n > FC_MQTT_MAX_TOPIC) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        if (t[i] == '+' || t[i] == '#' || t[i] == '\0') {
            return 0;
        }
    }
    return 1;
}

/* A relative filter: '+' a whole level, '#' the whole last one. The prefix
 * before it ends with '/', so the first level starts a level. */
static int filter_ok(const char *t, size_t n)
{
    if (n == 0 || n > FC_MQTT_MAX_TOPIC) {
        return 0;
    }
    for (size_t i = 0; i < n; i++) {
        char ch = t[i];
        int starts = i == 0 || t[i - 1] == '/';
        int ends = i + 1 == n || t[i + 1] == '/';
        if (ch == '\0' || ((ch == '+' || ch == '#') && !(starts && ends)) || (ch == '#' && i + 1 != n)) {
            return 0;
        }
    }
    return 1;
}

/* Whether `topic` (n bytes) matches the filter (MQTT 3.1.1, 4.7). */
static int topic_matches(const char *filter, const char *topic, size_t n)
{
    const char *f = filter, *t = topic, *te = topic + n;
    if (n > 0 && topic[0] == '$' && (*f == '+' || *f == '#')) {
        return 0;
    }
    for (;;) {
        if (*f == '#') {
            return 1;
        }
        if (*f == '+') {
            while (t < te && *t != '/') {
                t++;
            }
            f++;
        } else {
            while (*f != '\0' && *f != '/' && t < te && *t != '/') {
                if (*f++ != *t++) {
                    return 0;
                }
            }
            if ((*f == '\0' || *f == '/') != (t == te || *t == '/')) {
                return 0;
            }
        }
        if (*f == '\0' || t == te) {
            /* "a/#" matches "a" too. */
            return *f == '\0' ? t == te : (t == te && f[0] == '/' && f[1] == '#' && f[2] == '\0');
        }
        f++; /* both at a '/' */
        t++;
    }
}

/* ---- the receive pool ---------------------------------------------------------- */

struct fc_mqtt_msg {
    uint16_t size;  /* the block's, this header included, a multiple of 4 */
    uint8_t used;
    uint8_t owner;  /* index in s_owners */
    uint16_t tlen, plen;
    /* then the topic, then the payload */
};
#define MSG_HDR sizeof(struct fc_mqtt_msg)

_Static_assert(FC_MQTT_POOL_SIZE % 4 == 0 && FC_MQTT_POOL_SIZE <= 65532, "FC_MQTT_POOL_SIZE");

typedef struct {
    void *owner;
    size_t bytes;
} pool_owner_t;

static pthread_mutex_t s_pool_lock = PTHREAD_MUTEX_INITIALIZER;
/* Allocated with the first broker, kept: the owners' table, then the blocks. */
static pool_owner_t *s_owners;
static unsigned char *s_pool;
static size_t s_pool_used, s_pool_peak;

static int pool_init(void)
{
    pthread_mutex_lock(&s_pool_lock);
    if (s_pool == NULL && (s_owners = calloc(1, sizeof(pool_owner_t) * FC_MQTT_MAX_SUBS + FC_MQTT_POOL_SIZE)) != NULL) {
        s_pool = (unsigned char *)(s_owners + FC_MQTT_MAX_SUBS);
        struct fc_mqtt_msg *b = (struct fc_mqtt_msg *)s_pool;
        b->size = FC_MQTT_POOL_SIZE;
    }
    int rc = s_pool != NULL ? 0 : -1;
    pthread_mutex_unlock(&s_pool_lock);
    return rc;
}

/* A block for a message of `owner`, or NULL: the pool full, or the owner
 * already holding half of it. */
static struct fc_mqtt_msg *pool_alloc(void *owner, size_t tlen, size_t plen)
{
    size_t need = (MSG_HDR + tlen + plen + 3) & ~(size_t)3;
    struct fc_mqtt_msg *got = NULL;
    pthread_mutex_lock(&s_pool_lock);
    int oi = -1;
    for (int i = 0; s_pool != NULL && i < FC_MQTT_MAX_SUBS; i++) {
        if (s_owners[i].owner == owner && s_owners[i].bytes > 0) {
            oi = i;
            break;
        }
        if (oi < 0 && s_owners[i].bytes == 0) {
            oi = i;
        }
    }
    if (oi < 0 || s_owners[oi].bytes + need > FC_MQTT_POOL_SIZE / 2) {
        goto out;
    }
    for (size_t off = 0; off < FC_MQTT_POOL_SIZE;) {
        struct fc_mqtt_msg *b = (struct fc_mqtt_msg *)(s_pool + off);
        if (!b->used) {
            /* Free blocks are joined here, as they are walked. */
            while (off + b->size < FC_MQTT_POOL_SIZE) {
                struct fc_mqtt_msg *n = (struct fc_mqtt_msg *)(s_pool + off + b->size);
                if (n->used) {
                    break;
                }
                b->size = (uint16_t)(b->size + n->size);
            }
            if (b->size >= need) {
                if (b->size - need >= MSG_HDR + 16) {
                    struct fc_mqtt_msg *rest = (struct fc_mqtt_msg *)(s_pool + off + need);
                    memset(rest, 0, MSG_HDR);
                    rest->size = (uint16_t)(b->size - need);
                    b->size = (uint16_t)need;
                }
                b->used = 1;
                b->owner = (uint8_t)oi;
                b->tlen = (uint16_t)tlen;
                b->plen = (uint16_t)plen;
                s_owners[oi].owner = owner;
                s_owners[oi].bytes += b->size;
                s_pool_used += b->size;
                if (s_pool_used > s_pool_peak) {
                    s_pool_peak = s_pool_used;
                }
                got = b;
                break;
            }
        }
        off += b->size;
    }
out:
    pthread_mutex_unlock(&s_pool_lock);
    return got;
}

void fc_mqtt_msg_free(fc_mqtt_msg_t *m)
{
    if (m == NULL) {
        return;
    }
    pthread_mutex_lock(&s_pool_lock);
    if (m->used) {
        m->used = 0;
        s_owners[m->owner].bytes -= m->size;
        s_pool_used -= m->size;
    }
    pthread_mutex_unlock(&s_pool_lock);
}

const char *fc_mqtt_msg_topic(const fc_mqtt_msg_t *m, size_t *len)
{
    *len = m->tlen;
    return (const char *)m + MSG_HDR;
}

const void *fc_mqtt_msg_payload(const fc_mqtt_msg_t *m, size_t *len)
{
    *len = m->plen;
    return (const unsigned char *)m + MSG_HDR + m->tlen;
}

/* ---- state -------------------------------------------------------------------- */

typedef struct {
    void *owner; /* NULL: free */
    unsigned gen;
    int scope, qos;
    char *app;
    char *rel;    /* the relative filter */
    char *filter; /* with its prefix; NULL while that prefix is not known */
    size_t prefix_len;
    uint16_t pid; /* its SUBSCRIBE, until the SUBACK */
} sub_t;

typedef struct conn {
    int tls;
    mbedtls_net_context net;
    mbedtls_ssl_context ssl;
    mbedtls_ssl_config conf;
    mbedtls_x509_crt ca;
    int broken;        /* __atomic: a write failed, the client's thread closes it */
    int quick;         /* under s_io: a packet's first read, which does not wait */
    long long last_tx; /* under s_io */
    long long ping_at; /* a PINGREQ waits for its answer since then, 0 none */
    unsigned char tx[TX_CAP];
    unsigned char rx[RX_CAP];
} conn_t;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t s_cond = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t s_io = PTHREAD_MUTEX_INITIALIZER;

/* The client's state, allocated at the first subscription or broker and
 * kept: on the ESP32-S3 with Zephyr there is no static RAM to spare. */
typedef struct {
    char dev_prefix[FC_MQTT_MAX_PREFIX + 1];   /* "" unconfigured */
    char fleet_prefix[FC_MQTT_MAX_PREFIX + 1]; /* "" none */
    sub_t subs[FC_MQTT_MAX_SUBS];
    cfg_t *cfg;      /* the running config, the thread reads it */
    int up;          /* CONNACK accepted, not lost since */
    int stop;
    int running;     /* the thread exists (agent's thread only) */
    int extra_block; /* the TLS pool's second block is ours (agent's thread) */
    pthread_t thread;
    pthread_attr_t attr;
    uint16_t next_pid; /* under s_io */
    unsigned long connects, published, received, dropped, dropped_logged;
    long long drop_log_at;
    unsigned long long tx, rx; /* bytes on the wire */
} state_t;

static fc_mqtt_hooks_t s_hooks;           /* set before the thread */
static state_t *s_st;                     /* under s_lock but where said, NULL until needed */
static conn_t *s_conn;                    /* under s_io: the connection, once up */


/* Tests: a TCP connection of their own (fills net->fd), 0 or -1. */
static int (*s_connect_hook)(mbedtls_net_context *net, const char *host, int port);

static void count_wire(unsigned long long *c, int n)
{
    if (n > 0) {
        pthread_mutex_lock(&s_lock);
        *c += (unsigned long long)n;
        pthread_mutex_unlock(&s_lock);
    }
}

static void count_drop(const char *why)
{
    long long now = now_ms();
    pthread_mutex_lock(&s_lock);
    s_st->dropped++;
    unsigned long n = s_st->dropped - s_st->dropped_logged;
    int log = s_st->drop_log_at == 0 || now - s_st->drop_log_at >= DROP_LOG_MS;
    if (log) {
        s_st->drop_log_at = now;
        s_st->dropped_logged = s_st->dropped;
    }
    pthread_mutex_unlock(&s_lock);
    if (log) {
        fc_port_log(FC_LOG_WARN, "mqtt: %lu message(s) dropped, the last: %s", n, why);
    }
}

/* ---- the transport ---------------------------------------------------------------- */

static int bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    int rc = mbedtls_net_send(ctx, buf, len);
    count_wire(&s_st->tx, rc);
    return rc;
}

static int bio_recv_timeout(void *ctx, unsigned char *buf, size_t len, uint32_t timeout)
{
    int rc = mbedtls_net_recv_timeout(ctx, buf, len, timeout);
    count_wire(&s_st->rx, rc);
    return rc;
}

/* The TLS connection's transport: as bio_*, and during a packet's first
 * read a wait of FIRST_READ_MS that ends in WANT_READ. */
static int tls_bio_send(void *ctx, const unsigned char *buf, size_t len)
{
    return bio_send(&((struct conn *)ctx)->net, buf, len);
}

static int tls_bio_recv(void *ctx, unsigned char *buf, size_t len, uint32_t timeout)
{
    struct conn *c = ctx;
    if (c->quick) {
        int rc = bio_recv_timeout(&c->net, buf, len, FIRST_READ_MS);
        return rc == MBEDTLS_ERR_SSL_TIMEOUT ? MBEDTLS_ERR_SSL_WANT_READ : rc;
    }
    return bio_recv_timeout(&c->net, buf, len, timeout);
}

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

/* Writes it all: 0, or -1 (the connection is broken). Under s_io. */
static int tp_write(conn_t *c, const unsigned char *b, size_t n)
{
    while (n > 0) {
        int rc = c->tls ? mbedtls_ssl_write(&c->ssl, b, n) : bio_send(&c->net, b, n);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE) {
            continue;
        }
        if (rc <= 0) {
            __atomic_store_n(&c->broken, 1, __ATOMIC_RELAXED);
            return -1;
        }
        b += rc;
        n -= (size_t)rc;
    }
    c->last_tx = now_ms();
    return 0;
}

/* Whether anything is there to read within timeout_ms: 1, 0, -1. */
static int tp_wait(conn_t *c, int timeout_ms)
{
    if (c->tls) {
        pthread_mutex_lock(&s_io);
        int now = mbedtls_ssl_get_bytes_avail(&c->ssl) > 0 || mbedtls_ssl_check_pending(&c->ssl);
        pthread_mutex_unlock(&s_io);
        if (now) {
            return 1;
        }
    }
    return fd_readable(c->net.fd, timeout_ms);
}

/* What is there now, without waiting: > 0 bytes, 0 nothing (a TLS record
 * that carried no data: a session ticket), -1 closed or broken. Under s_io. */
static int tp_read_some(conn_t *c, unsigned char *b, size_t n)
{
    for (;;) {
        int rc;
        if (c->tls) {
            /* A record without data (a TLS 1.3 session ticket, a key
             * update) would otherwise hold s_io until the next one comes:
             * the transport waits FIRST_READ_MS and answers WANT_READ, after
             * which mbedTLS goes on with what it read (a timeout would end
             * the connection). */
            c->quick = 1;
            rc = mbedtls_ssl_read(&c->ssl, b, n);
            c->quick = 0;
            if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE ||
                rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
                if (mbedtls_ssl_get_bytes_avail(&c->ssl) == 0 && !mbedtls_ssl_check_pending(&c->ssl) &&
                    fd_readable(c->net.fd, 0) == 0) {
                    return 0;
                }
                continue;
            }
        } else {
            if (fd_readable(c->net.fd, 0) == 0) {
                return 0;
            }
            rc = bio_recv_timeout(&c->net, b, n, 0);
        }
        if (rc < 0 && rc != MBEDTLS_ERR_SSL_PEER_CLOSE_NOTIFY && rc != MBEDTLS_ERR_NET_CONN_RESET) {
            log_mbedtls("read", rc);
        }
        return rc > 0 ? rc : -1;
    }
}

/* Reads exactly n bytes, the rest of a packet: 0, or -1 (closed, broken, or
 * nothing for READ_TIMEOUT_MS). Under s_io. */
static int tp_read(conn_t *c, unsigned char *b, size_t n)
{
    while (n > 0) {
        int rc = c->tls ? mbedtls_ssl_read(&c->ssl, b, n) : bio_recv_timeout(&c->net, b, n, READ_TIMEOUT_MS);
        if (rc == MBEDTLS_ERR_SSL_WANT_READ || rc == MBEDTLS_ERR_SSL_WANT_WRITE ||
            rc == MBEDTLS_ERR_SSL_RECEIVED_NEW_SESSION_TICKET) {
            continue;
        }
        if (rc <= 0) {
            return -1;
        }
        b += rc;
        n -= (size_t)rc;
    }
    return 0;
}

/* Reads and forgets n bytes. Under s_io. */
static int tp_skip(conn_t *c, size_t n)
{
    while (n > 0) {
        size_t k = n < RX_CAP ? n : RX_CAP;
        if (tp_read(c, c->rx, k) != 0) {
            return -1;
        }
        n -= k;
    }
    return 0;
}

static void conn_free(conn_t *c)
{
    if (c == NULL) {
        return;
    }
    if (c->tls) {
        mbedtls_ssl_free(&c->ssl);
        mbedtls_ssl_config_free(&c->conf);
        mbedtls_x509_crt_free(&c->ca);
    }
    mbedtls_net_free(&c->net);
    free(c);
}

static int tls_start(conn_t *c, const cfg_t *cfg)
{
    int rc;
    mbedtls_ssl_init(&c->ssl);
    mbedtls_ssl_config_init(&c->conf);
    mbedtls_x509_crt_init(&c->ca);
    if (fc_http_global_init() != 0) {
        return -1;
    }
    if ((rc = mbedtls_x509_crt_parse(&c->ca, (const unsigned char *)cfg->ca_pem, strlen(cfg->ca_pem) + 1)) != 0) {
        log_mbedtls("the broker's CA", rc);
        return -1;
    }
    if ((rc = mbedtls_ssl_config_defaults(&c->conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM,
                                          MBEDTLS_SSL_PRESET_DEFAULT)) != 0) {
        log_mbedtls("ssl_config_defaults", rc);
        return -1;
    }
    mbedtls_ssl_conf_authmode(&c->conf, MBEDTLS_SSL_VERIFY_REQUIRED);
    mbedtls_ssl_conf_ca_chain(&c->conf, &c->ca, NULL);
    mbedtls_ssl_conf_read_timeout(&c->conf, CONNECT_TIMEOUT_S * 1000);
#if defined(MBEDTLS_SSL_SESSION_TICKETS)
    /* Never resumed: each connection is a full handshake, as fc_http.c's. */
    mbedtls_ssl_conf_session_tickets(&c->conf, MBEDTLS_SSL_SESSION_TICKETS_DISABLED);
#endif
    if ((rc = mbedtls_ssl_setup(&c->ssl, &c->conf)) != 0 ||
        (rc = mbedtls_ssl_set_hostname(&c->ssl, cfg->host)) != 0) {
        log_mbedtls("ssl_setup", rc);
        return -1;
    }
    mbedtls_ssl_set_bio(&c->ssl, c, tls_bio_send, NULL, tls_bio_recv);
    while ((rc = mbedtls_ssl_handshake(&c->ssl)) != 0) {
        if (rc != MBEDTLS_ERR_SSL_WANT_READ && rc != MBEDTLS_ERR_SSL_WANT_WRITE) {
            log_mbedtls("handshake", rc);
            uint32_t flags = mbedtls_ssl_get_verify_result(&c->ssl);
            if (flags != 0 && flags != UINT32_MAX) {
                char vbuf[200];
                mbedtls_x509_crt_verify_info(vbuf, sizeof(vbuf), "  ", flags);
                fc_port_log(FC_LOG_WARN, "mqtt: the broker's certificate:\n%s", vbuf);
            }
            return -1;
        }
    }
    mbedtls_ssl_conf_read_timeout(&c->conf, READ_TIMEOUT_MS);
    return 0;
}

/* ---- packets ---------------------------------------------------------------------- */

static size_t put_len(unsigned char *p, size_t v)
{
    size_t n = 0;
    do {
        unsigned char b = (unsigned char)(v % 128);
        v /= 128;
        p[n++] = (unsigned char)(b | (v > 0 ? 0x80 : 0));
    } while (v > 0);
    return n;
}

static size_t put_u16(unsigned char *p, unsigned v)
{
    p[0] = (unsigned char)(v >> 8);
    p[1] = (unsigned char)v;
    return 2;
}

static size_t put_str(unsigned char *p, const char *s, size_t n)
{
    put_u16(p, (unsigned)n);
    memcpy(p + 2, s, n);
    return n + 2;
}

/* A packet whose body is already at out + 5: moves it behind its header.
 * Returns the packet's length. */
static size_t finish(unsigned char *out, unsigned char first, size_t body)
{
    unsigned char h[5];
    h[0] = first;
    size_t hl = 1 + put_len(h + 1, body);
    memmove(out + hl, out + 5, body);
    memcpy(out, h, hl);
    return hl + body;
}

static size_t enc_connect(unsigned char *out, const cfg_t *c)
{
    unsigned char *p = out + 5;
    p += put_str(p, "MQTT", 4);
    *p++ = 4; /* 3.1.1 */
    unsigned char flags = 0x02; /* clean session */
    if (c->user[0] != '\0') {
        flags |= 0x80;
        if (c->pass[0] != '\0') {
            flags |= 0x40;
        }
    }
    *p++ = flags;
    p += put_u16(p, (unsigned)c->keepalive_s);
    p += put_str(p, c->client_id, strlen(c->client_id));
    if (flags & 0x80) {
        p += put_str(p, c->user, strlen(c->user));
    }
    if (flags & 0x40) {
        p += put_str(p, c->pass, strlen(c->pass));
    }
    return finish(out, P_CONNECT << 4, (size_t)(p - out - 5));
}

static size_t enc_publish(unsigned char *out, const char *prefix, size_t pl, const char *topic, size_t tl,
                          const void *payload, size_t plen, int qos, int retain, uint16_t pid)
{
    unsigned char *p = out + 5;
    p += put_u16(p, (unsigned)(pl + tl));
    memcpy(p, prefix, pl);
    memcpy(p + pl, topic, tl);
    p += pl + tl;
    if (qos > 0) {
        p += put_u16(p, pid);
    }
    memcpy(p, payload, plen);
    p += plen;
    return finish(out, (unsigned char)(P_PUBLISH << 4 | qos << 1 | (retain ? 1 : 0)), (size_t)(p - out - 5));
}

static size_t enc_subscribe(unsigned char *out, int unsub, uint16_t pid, const char *filter, int qos)
{
    unsigned char *p = out + 5;
    p += put_u16(p, pid);
    p += put_str(p, filter, strlen(filter));
    if (!unsub) {
        *p++ = (unsigned char)qos;
    }
    return finish(out, (unsigned char)((unsub ? P_UNSUBSCRIBE : P_SUBSCRIBE) << 4 | 0x02), (size_t)(p - out - 5));
}

static uint16_t next_pid(void)
{
    if (++s_st->next_pid == 0) {
        s_st->next_pid = 1;
    }
    return s_st->next_pid;
}

/* (UN)SUBSCRIBE for one filter, if connected; returns its packet id, 0 if not sent. */
static uint16_t send_subscribe(int unsub, const char *filter, int qos)
{
    uint16_t pid = 0;
    pthread_mutex_lock(&s_io);
    conn_t *c = s_conn;
    if (c != NULL) {
        pid = next_pid();
        if (tp_write(c, c->tx, enc_subscribe(c->tx, unsub, pid, filter, qos)) != 0) {
            pid = 0;
        }
    }
    pthread_mutex_unlock(&s_io);
    return pid;
}

/* ---- subscriptions ------------------------------------------------------------------ */

/* s_st, allocated if it was not; NULL without memory. Under s_lock. */
static state_t *state(void)
{
    if (s_st == NULL) {
        s_st = calloc(1, sizeof(*s_st));
    }
    return s_st;
}

static void sub_clear(sub_t *s)
{
    free(s->app);
    free(s->rel);
    free(s->filter);
    unsigned gen = s->gen;
    memset(s, 0, sizeof(*s));
    s->gen = gen;
}

/* The full filter of s under the current prefixes (s->filter, s->prefix_len),
 * NULL when its prefix is unknown. -1: too long. Under s_lock. */
static int sub_expand(sub_t *s)
{
    free(s->filter);
    s->filter = NULL;
    const char *tmpl = s->scope == FC_MQTT_FLEET ? s_st->fleet_prefix : s_st->dev_prefix;
    if (tmpl[0] == '\0') {
        return 0;
    }
    char prefix[FC_MQTT_MAX_PREFIX + 1];
    int pl = replace(tmpl, "{app}", s->app, prefix, sizeof(prefix));
    if (pl < 0 || strpbrk(prefix, "+#") != NULL) {
        return -1;
    }
    size_t rl = strlen(s->rel);
    if ((s->filter = malloc((size_t)pl + rl + 1)) == NULL) {
        return -1;
    }
    memcpy(s->filter, prefix, (size_t)pl);
    memcpy(s->filter + pl, s->rel, rl + 1);
    s->prefix_len = (size_t)pl;
    return 0;
}

/* The highest QoS asked for `filter` by any subscription, -1 if none has it.
 * Under s_lock. */
static int filter_qos(const char *filter)
{
    int q = -1;
    for (int i = 0; i < FC_MQTT_MAX_SUBS; i++) {
        if (s_st->subs[i].owner != NULL && s_st->subs[i].filter != NULL && strcmp(s_st->subs[i].filter, filter) == 0 &&
            s_st->subs[i].qos > q) {
            q = s_st->subs[i].qos;
        }
    }
    return q;
}

static int sub_handle(int i)
{
    return (int)((s_st->subs[i].gen % 0x100000u) * FC_MQTT_MAX_SUBS) + i;
}

int fc_mqtt_subscribe(void *owner, const char *app, int scope, const char *topic, size_t tlen, int qos)
{
    if (owner == NULL || app == NULL || (scope != FC_MQTT_DEVICE && scope != FC_MQTT_FLEET) || qos < 0 || qos > 1 ||
        topic == NULL || !filter_ok(topic, tlen)) {
        return FC_HW_EINVAL;
    }
    char *a = strdup(app), *rel = malloc(tlen + 1);
    if (a == NULL || rel == NULL) {
        free(a);
        free(rel);
        return FC_HW_ENOSPC;
    }
    memcpy(rel, topic, tlen);
    rel[tlen] = '\0';
    int rc = FC_HW_ENOSPC, send_qos = -1;
    char *send = NULL;
    pthread_mutex_lock(&s_lock);
    if (state() == NULL) {
        rc = FC_HW_ENOSPC;
    } else if (scope == FC_MQTT_FLEET && s_st->dev_prefix[0] != '\0' && s_st->fleet_prefix[0] == '\0') {
        rc = FC_HW_ENOENT;
    } else {
        for (int i = 0; i < FC_MQTT_MAX_SUBS; i++) {
            sub_t *s = &s_st->subs[i];
            if (s->owner != NULL) {
                continue;
            }
            s->owner = owner;
            s->gen++;
            s->scope = scope;
            s->qos = qos;
            s->app = a;
            s->rel = rel;
            a = rel = NULL;
            if (sub_expand(s) != 0) {
                sub_clear(s);
                rc = FC_HW_EINVAL;
                break;
            }
            rc = sub_handle(i);
            if (s_st->up && s->filter != NULL) {
                send = strdup(s->filter);
                send_qos = filter_qos(s->filter);
            }
            break;
        }
    }
    pthread_mutex_unlock(&s_lock);
    free(a);
    free(rel);
    if (send != NULL) {
        uint16_t pid = send_subscribe(0, send, send_qos);
        pthread_mutex_lock(&s_lock);
        int i = rc % FC_MQTT_MAX_SUBS;
        if (s_st->subs[i].owner == owner && sub_handle(i) == rc) {
            s_st->subs[i].pid = pid;
        }
        pthread_mutex_unlock(&s_lock);
        free(send);
    }
    return rc;
}

/* Closes subscription i; under s_lock. Returns the filter to unsubscribe
 * from (the caller frees it), NULL if none: another one has it too, or it
 * is not on the broker. */
static char *sub_close(int i)
{
    sub_t *s = &s_st->subs[i];
    char *filter = s->filter;
    s->filter = NULL;
    sub_clear(s);
    if (filter != NULL && (!s_st->up || filter_qos(filter) >= 0)) {
        free(filter);
        filter = NULL;
    }
    return filter;
}

int fc_mqtt_unsubscribe(void *owner, int sub)
{
    if (sub < 0) {
        return FC_HW_EINVAL;
    }
    int i = sub % FC_MQTT_MAX_SUBS;
    pthread_mutex_lock(&s_lock);
    if (owner == NULL || s_st == NULL || s_st->subs[i].owner != owner || sub_handle(i) != sub) {
        pthread_mutex_unlock(&s_lock);
        return FC_HW_EINVAL;
    }
    char *f = sub_close(i);
    pthread_mutex_unlock(&s_lock);
    if (f != NULL) {
        send_subscribe(1, f, 0);
        free(f);
    }
    return 0;
}

void fc_mqtt_drop_owner(void *owner)
{
    for (int i = 0; owner != NULL && i < FC_MQTT_MAX_SUBS; i++) {
        pthread_mutex_lock(&s_lock);
        char *f = s_st != NULL && s_st->subs[i].owner == owner ? sub_close(i) : NULL;
        pthread_mutex_unlock(&s_lock);
        if (f != NULL) {
            send_subscribe(1, f, 0);
            free(f);
        }
    }
}

/* After CONNACK: every filter once, at the highest QoS asked for it. */
static void resubscribe_all(void)
{
    for (int i = 0; i < FC_MQTT_MAX_SUBS; i++) {
        char *f = NULL;
        int q = 0;
        pthread_mutex_lock(&s_lock);
        sub_t *s = &s_st->subs[i];
        if (s->owner != NULL && s->filter != NULL) {
            int first = 1;
            for (int j = 0; j < i; j++) {
                if (s_st->subs[j].owner != NULL && s_st->subs[j].filter != NULL && strcmp(s_st->subs[j].filter, s->filter) == 0) {
                    first = 0;
                }
            }
            if (first) {
                f = strdup(s->filter);
                q = filter_qos(s->filter);
            }
        }
        pthread_mutex_unlock(&s_lock);
        if (f != NULL) {
            uint16_t pid = send_subscribe(0, f, q);
            pthread_mutex_lock(&s_lock);
            for (int j = 0; j < FC_MQTT_MAX_SUBS; j++) {
                if (s_st->subs[j].owner != NULL && s_st->subs[j].filter != NULL && strcmp(s_st->subs[j].filter, f) == 0) {
                    s_st->subs[j].pid = pid;
                }
            }
            pthread_mutex_unlock(&s_lock);
            free(f);
        }
    }
}

/* ---- publishing ---------------------------------------------------------------------- */

int fc_mqtt_publish(const char *app, const char *topic, size_t tlen, const void *payload, size_t plen, int qos,
                    int retain)
{
    if (app == NULL || topic == NULL || !topic_ok(topic, tlen) || plen > FC_MQTT_MAX_PAYLOAD ||
        (payload == NULL && plen > 0) || qos < 0 || qos > 1 || retain < 0 || retain > 1) {
        return FC_HW_EINVAL;
    }
    char prefix[FC_MQTT_MAX_PREFIX + 1];
    pthread_mutex_lock(&s_lock);
    int up = s_st != NULL && s_st->up;
    int pl = s_st != NULL && s_st->dev_prefix[0] != '\0' ? replace(s_st->dev_prefix, "{app}", app, prefix, sizeof(prefix)) : -1;
    pthread_mutex_unlock(&s_lock);
    if (!up) {
        return FC_HW_EAGAIN;
    }
    if (pl < 0 || strpbrk(prefix, "+#") != NULL) {
        return FC_HW_EINVAL;
    }
    int rc = FC_HW_EAGAIN;
    pthread_mutex_lock(&s_io);
    conn_t *c = s_conn;
    if (c != NULL && !__atomic_load_n(&c->broken, __ATOMIC_RELAXED)) {
        uint16_t pid = qos > 0 ? next_pid() : 0;
        size_t n = enc_publish(c->tx, prefix, (size_t)pl, topic, tlen, payload, plen, qos, retain, pid);
        rc = tp_write(c, c->tx, n) == 0 ? 0 : FC_HW_EAGAIN;
    }
    pthread_mutex_unlock(&s_io);
    if (rc == 0) {
        pthread_mutex_lock(&s_lock);
        s_st->published++;
        pthread_mutex_unlock(&s_lock);
    }
    return rc;
}

/* ---- receiving ------------------------------------------------------------------------ */

typedef struct {
    void *owner;
    int sub;
    struct fc_mqtt_msg *m;
} delivery_t;

/* A PUBLISH whose first byte and length were read: its blocks in d[], one
 * per subscription it matches that the pool had room for. Under s_io.
 * Returns how many, or -1 (the connection is broken). */
static int rx_publish(conn_t *c, unsigned char first, size_t rem, delivery_t *d)
{
    int qos = (first >> 1) & 3;
    if (qos > 1 || rem < 2) {
        return -1; /* never asked for: QoS 2 */
    }
    if (tp_read(c, c->rx, 2) != 0) {
        return -1;
    }
    size_t tl = (size_t)c->rx[0] << 8 | c->rx[1];
    size_t head = tl + (qos ? 2 : 0);
    if (head > rem - 2) {
        return -1;
    }
    size_t plen = rem - 2 - head;
    unsigned char pid[2] = {0, 0};
    int n = 0;
    if (tl > FULL_TOPIC_MAX) {
        count_drop("a topic too long");
        if (tp_skip(c, tl) != 0 || (qos && tp_read(c, pid, 2) != 0) || tp_skip(c, plen) != 0) {
            return -1;
        }
        goto ack;
    }
    if (tp_read(c, c->rx, head) != 0) {
        return -1;
    }
    if (qos) {
        pid[0] = c->rx[tl];
        pid[1] = c->rx[tl + 1];
    }
    const char *topic = (const char *)c->rx;
    if (plen > FC_MQTT_MAX_PAYLOAD) {
        count_drop("a payload too large");
    } else {
        /* The subscriptions it matches, then a block for each. */
        struct {
            void *owner;
            int sub;
            size_t pl;
        } to[FC_MQTT_MAX_SUBS];
        int k = 0;
        pthread_mutex_lock(&s_lock);
        for (int i = 0; i < FC_MQTT_MAX_SUBS; i++) {
            sub_t *s = &s_st->subs[i];
            if (s->owner != NULL && s->filter != NULL && topic_matches(s->filter, topic, tl) &&
                tl - s->prefix_len <= FC_MQTT_MAX_TOPIC) {
                to[k].owner = s->owner;
                to[k].sub = sub_handle(i);
                to[k].pl = s->prefix_len;
                k++;
            }
        }
        pthread_mutex_unlock(&s_lock);
        for (int i = 0; i < k; i++) {
            size_t rl = tl - to[i].pl;
            struct fc_mqtt_msg *m = pool_alloc(to[i].owner, rl, plen);
            if (m == NULL) {
                count_drop("the receive pool full");
                continue;
            }
            memcpy((char *)m + MSG_HDR, topic + to[i].pl, rl);
            d[n].owner = to[i].owner;
            d[n].sub = to[i].sub;
            d[n].m = m;
            n++;
        }
    }
    if (n > 0) {
        size_t len;
        void *first_payload = (void *)fc_mqtt_msg_payload(d[0].m, &len);
        if (tp_read(c, first_payload, plen) != 0) {
            for (int i = 0; i < n; i++) {
                fc_mqtt_msg_free(d[i].m);
            }
            return -1;
        }
        for (int i = 1; i < n; i++) {
            memcpy((void *)fc_mqtt_msg_payload(d[i].m, &len), first_payload, plen);
        }
    } else if (tp_skip(c, plen) != 0) {
        return -1;
    }
ack:
    if (qos == 1) {
        unsigned char ack[4] = {P_PUBACK << 4, 2, pid[0], pid[1]};
        if (tp_write(c, ack, sizeof(ack)) != 0) {
            for (int i = 0; i < n; i++) {
                fc_mqtt_msg_free(d[i].m);
            }
            return -1;
        }
    }
    return n;
}

/* Reads one packet if one has begun: 1 read, 0 none, -1 the link is broken. */
static int rx_packet(conn_t *c)
{
    delivery_t d[FC_MQTT_MAX_SUBS];
    int n = 0, rc = 1;
    unsigned char first;
    pthread_mutex_lock(&s_io);
    int got = tp_read_some(c, &first, 1);
    if (got <= 0) {
        pthread_mutex_unlock(&s_io);
        return got;
    }
    size_t rem = 0;
    for (int i = 0, mult = 1;; i++, mult *= 128) {
        unsigned char b;
        if (i == 4 || tp_read(c, &b, 1) != 0) {
            rc = -1;
            goto out;
        }
        rem += (size_t)(b & 0x7f) * (size_t)mult;
        if (!(b & 0x80)) {
            break;
        }
    }
    switch (first >> 4) {
    case P_PUBLISH:
        n = rx_publish(c, first, rem, d);
        if (n < 0) {
            n = 0;
            rc = -1;
        }
        break;
    case P_SUBACK:
        if (rem < 3 || rem > RX_CAP || tp_read(c, c->rx, rem) != 0) {
            rc = -1;
            break;
        }
        if (c->rx[2] == 0x80) {
            uint16_t pid = (uint16_t)(c->rx[0] << 8 | c->rx[1]);
            pthread_mutex_lock(&s_lock);
            for (int i = 0; i < FC_MQTT_MAX_SUBS; i++) {
                if (s_st->subs[i].owner != NULL && s_st->subs[i].pid == pid && s_st->subs[i].filter != NULL) {
                    fc_port_log(FC_LOG_WARN, "mqtt: the broker refused the subscription to %s", s_st->subs[i].filter);
                    break;
                }
            }
            pthread_mutex_unlock(&s_lock);
        }
        break;
    case P_PINGRESP:
        c->ping_at = 0;
        /* fall through */
    case P_PUBACK:
    case P_UNSUBACK:
        if (tp_skip(c, rem) != 0) {
            rc = -1;
        }
        break;
    default:
        fc_port_log(FC_LOG_WARN, "mqtt: unexpected packet type %d from the broker", first >> 4);
        rc = -1;
    }
out:
    pthread_mutex_unlock(&s_io);
    for (int i = 0; i < n; i++) {
        if (s_hooks.on_message == NULL || s_hooks.on_message(d[i].owner, d[i].sub, d[i].m) != 0) {
            fc_mqtt_msg_free(d[i].m);
            count_drop("the app's queue full");
        } else {
            pthread_mutex_lock(&s_lock);
            s_st->received++;
            pthread_mutex_unlock(&s_lock);
        }
    }
    return rc;
}

/* ---- the client's thread ----------------------------------------------------------------- */

static int stopping(void)
{
    pthread_mutex_lock(&s_lock);
    int s = s_st->stop;
    pthread_mutex_unlock(&s_lock);
    return s;
}

/* Waits up to ms, or until a stop. */
static void pause_ms(long long ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += (time_t)(ms / 1000);
    ts.tv_nsec += (long)(ms % 1000) * 1000000;
    if (ts.tv_nsec >= 1000000000) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&s_lock);
    if (!s_st->stop) {
        pthread_cond_timedwait(&s_cond, &s_lock, &ts);
    }
    pthread_mutex_unlock(&s_lock);
}

static const char *const CONNACK_WHY[] = {
    "accepted", "protocol version refused", "client id refused", "server unavailable",
    "bad user name or password", "not authorized",
};

/* TCP, TLS, CONNECT and its CONNACK: the connection, or NULL (logged). */
static conn_t *conn_open(const cfg_t *cfg)
{
    conn_t *c = calloc(1, sizeof(*c));
    if (c == NULL) {
        fc_port_log(FC_LOG_WARN, "mqtt: no memory for the connection");
        return NULL;
    }
    c->tls = cfg->tls;
    mbedtls_net_init(&c->net);
    char port[8];
    snprintf(port, sizeof(port), "%d", cfg->port);
    int rc = s_connect_hook ? s_connect_hook(&c->net, cfg->host, cfg->port)
                            : fc_net_connect_within(&c->net, cfg->host, port, CONNECT_TIMEOUT_S);
    if (rc != 0) {
        if (c->tls) { /* conn_free frees them */
            mbedtls_ssl_init(&c->ssl);
            mbedtls_ssl_config_init(&c->conf);
            mbedtls_x509_crt_init(&c->ca);
        }
        log_mbedtls("connect", rc);
        conn_free(c);
        return NULL;
    }
    struct timeval tv = {.tv_sec = SEND_TIMEOUT_S};
    setsockopt(c->net.fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (c->tls && tls_start(c, cfg) != 0) {
        conn_free(c);
        return NULL;
    }
    unsigned char ack[4];
    if (tp_write(c, c->tx, enc_connect(c->tx, cfg)) != 0 || tp_read(c, ack, sizeof(ack)) != 0) {
        fc_port_log(FC_LOG_WARN, "mqtt: %s:%d did not answer the CONNECT", cfg->host, cfg->port);
        conn_free(c);
        return NULL;
    }
    if (ack[0] != P_CONNACK << 4 || ack[1] != 2 || ack[3] != 0) {
        fc_port_log(FC_LOG_WARN, "mqtt: %s:%d refused the connection: %s", cfg->host, cfg->port,
                    ack[0] == P_CONNACK << 4 && ack[3] < 6 ? CONNACK_WHY[ack[3]] : "not an MQTT broker");
        conn_free(c);
        return NULL;
    }
    return c;
}

static void conn_close(conn_t *c, int bye)
{
    if (bye && !__atomic_load_n(&c->broken, __ATOMIC_RELAXED)) {
        static const unsigned char disc[2] = {P_DISCONNECT << 4, 0};
        tp_write(c, disc, sizeof(disc));
    }
    if (c->tls) {
        mbedtls_ssl_close_notify(&c->ssl);
    }
    conn_free(c);
}

static long long jitter(long long ms)
{
    uint16_t r = 0;
    fc_random(&r, sizeof(r));
    return ms / 2 + (long long)(r % (unsigned)(ms / 2 + 1));
}

/* Reads and keeps the link alive until it breaks or the client stops. */
static void run(conn_t *c, int keepalive_s)
{
    long long ka = (long long)keepalive_s * 1000;
    while (!stopping() && !__atomic_load_n(&c->broken, __ATOMIC_RELAXED)) {
        int r = tp_wait(c, WAIT_SLICE_MS);
        if (r < 0) {
            break;
        }
        if (r > 0) {
            int rc;
            while ((rc = rx_packet(c)) > 0) {
            }
            if (rc < 0) {
                break;
            }
        }
        long long now = now_ms();
        pthread_mutex_lock(&s_io);
        int dead = c->ping_at != 0 && now - c->ping_at > ka;
        if (!dead && c->ping_at == 0 && now - c->last_tx >= ka * 3 / 4) {
            static const unsigned char ping[2] = {P_PINGREQ << 4, 0};
            if (tp_write(c, ping, sizeof(ping)) == 0) {
                c->ping_at = now;
            }
        }
        pthread_mutex_unlock(&s_io);
        if (dead) {
            fc_port_log(FC_LOG_WARN, "mqtt: no answer to the keep-alive");
            break;
        }
    }
}

static void *mqtt_thread(void *arg)
{
    (void)arg;
    long long backoff = BACKOFF_MIN_MS, next_try = 0;
    while (!stopping()) {
        long long now = now_ms();
        if (now < next_try) {
            pause_ms(next_try - now < 1000 ? next_try - now : 1000);
            continue;
        }
        if (s_hooks.may_connect != NULL && !s_hooks.may_connect()) {
            pause_ms(1000);
            continue;
        }
        pthread_mutex_lock(&s_lock);
        const cfg_t *cfg = s_st->cfg; /* replaced only while this thread is stopped */
        pthread_mutex_unlock(&s_lock);
        conn_t *c = conn_open(cfg);
        if (c == NULL) {
            next_try = now_ms() + jitter(backoff);
            backoff = backoff * 2 > BACKOFF_MAX_MS ? BACKOFF_MAX_MS : backoff * 2;
            continue;
        }
        backoff = BACKOFF_MIN_MS;
        fc_port_log(FC_LOG_INFO, "mqtt: connected to %s:%d%s", cfg->host, cfg->port, cfg->tls ? "" : " (no TLS)");
        pthread_mutex_lock(&s_io);
        s_conn = c;
        pthread_mutex_unlock(&s_io);
        pthread_mutex_lock(&s_lock);
        s_st->up = 1;
        s_st->connects++;
        pthread_mutex_unlock(&s_lock);
        if (s_hooks.on_link != NULL) {
            s_hooks.on_link(1);
        }
        resubscribe_all();
        run(c, cfg->keepalive_s);
        int bye = stopping();
        pthread_mutex_lock(&s_lock);
        s_st->up = 0;
        pthread_mutex_unlock(&s_lock);
        pthread_mutex_lock(&s_io);
        s_conn = NULL;
        conn_close(c, bye);
        pthread_mutex_unlock(&s_io);
        if (!bye) {
            fc_port_log(FC_LOG_WARN, "mqtt: the link to %s:%d is lost", cfg->host, cfg->port);
            next_try = now_ms() + jitter(BACKOFF_MIN_MS);
        }
        if (s_hooks.on_link != NULL) {
            s_hooks.on_link(0);
        }
    }
    return NULL;
}

/* ---- control ------------------------------------------------------------------------ */

void fc_mqtt_set_hooks(const fc_mqtt_hooks_t *h)
{
    if (h != NULL) {
        s_hooks = *h;
    } else {
        memset(&s_hooks, 0, sizeof(s_hooks));
    }
}

void fc_mqtt_stop(void)
{
    if (s_st == NULL || !s_st->running) {
        return;
    }
    pthread_mutex_lock(&s_lock);
    s_st->stop = 1;
    pthread_cond_broadcast(&s_cond);
    pthread_mutex_unlock(&s_lock);
    pthread_join(s_st->thread, NULL);
    pthread_attr_destroy(&s_st->attr);
    s_st->running = 0;
    pthread_mutex_lock(&s_lock);
    s_st->stop = 0;
    pthread_mutex_unlock(&s_lock);
}

/* New prefixes for every subscription; under s_lock. */
static void set_prefixes(const char *dev, const char *fleet)
{
    if (s_st == NULL) {
        return;
    }
    snprintf(s_st->dev_prefix, sizeof(s_st->dev_prefix), "%s", dev);
    snprintf(s_st->fleet_prefix, sizeof(s_st->fleet_prefix), "%s", fleet);
    for (int i = 0; i < FC_MQTT_MAX_SUBS; i++) {
        if (s_st->subs[i].owner != NULL && sub_expand(&s_st->subs[i]) != 0) {
            fc_port_log(FC_LOG_WARN, "mqtt: app %s's subscription %s does not fit under the new prefix",
                        s_st->subs[i].app, s_st->subs[i].rel);
        }
    }
}

static void unconfigure(void)
{
    if (s_st == NULL) {
        return; /* never configured */
    }
    fc_mqtt_stop();
    pthread_mutex_lock(&s_lock);
    cfg_t *old = s_st->cfg;
    s_st->cfg = NULL;
    set_prefixes("", "");
    pthread_mutex_unlock(&s_lock);
    cfg_free(old);
    if (s_st->extra_block) {
        fc_tls_pool_extra(0);
        s_st->extra_block = 0;
    }
}

int fc_mqtt_configure(const char *device)
{
    char *json = NULL, *ca = NULL;
    size_t len;
    if (fc_port_read_file(FC_MQTT_CONFIG, &json, &len) != 0) {
        unconfigure();
        return 0;
    }
    if (device == NULL || device[0] == '\0') {
        fc_port_log(FC_LOG_INFO, "mqtt: waiting for the device's name to connect");
        free(json);
        unconfigure();
        return 0;
    }
    if (fc_port_read_file(FC_MQTT_CA_FILE, &ca, &len) != 0) {
        ca = NULL;
    }
    cfg_t *cfg = cfg_parse(json, ca, device);
    free(json);
    free(ca);
    if (cfg == NULL) {
        unconfigure();
        return -1;
    }
    pthread_mutex_lock(&s_lock);
    int have_state = state() != NULL;
    int same = have_state && s_st->running && cfg_same(cfg, s_st->cfg);
    pthread_mutex_unlock(&s_lock);
    if (same) {
        cfg_free(cfg);
        return 1;
    }
    fc_mqtt_stop();
    if (!have_state || pool_init() != 0) {
        fc_port_log(FC_LOG_WARN, "mqtt: no memory for the receive pool (%d bytes)", FC_MQTT_POOL_SIZE);
        cfg_free(cfg);
        unconfigure();
        return -2;
    }
    if (cfg->tls && NEEDS_TLS_BLOCK && !s_st->extra_block) {
        fc_tls_pool_extra(1);
        s_st->extra_block = 1;
    } else if (!cfg->tls && s_st->extra_block) {
        fc_tls_pool_extra(0);
        s_st->extra_block = 0;
    }
    pthread_mutex_lock(&s_lock);
    cfg_t *old = s_st->cfg;
    s_st->cfg = cfg;
    set_prefixes(cfg->dev_prefix, cfg->fleet_prefix);
    pthread_mutex_unlock(&s_lock);
    cfg_free(old);
    pthread_attr_init(&s_st->attr);
    if (pthread_attr_setstacksize(&s_st->attr, FC_MQTT_STACK_SIZE) != 0 ||
        pthread_create(&s_st->thread, &s_st->attr, mqtt_thread, NULL) != 0) {
        pthread_attr_destroy(&s_st->attr);
        fc_port_log(FC_LOG_WARN, "mqtt: cannot start the client's thread (a stack of %u KB): tried again later",
                    (unsigned)(FC_MQTT_STACK_SIZE / 1024));
        return -2;
    }
    s_st->running = 1;
    if (!cfg->tls) {
        fc_port_log(FC_LOG_WARN, "mqtt: %s:%d without TLS, as the fleet asks", cfg->host, cfg->port);
    }
    return 1;
}

void fc_mqtt_stats(fc_mqtt_stats_t *st)
{
    memset(st, 0, sizeof(*st));
    pthread_mutex_lock(&s_lock);
    if (s_st != NULL) {
        st->configured = s_st->cfg != NULL;
        st->connected = s_st->up;
        st->connects = s_st->connects;
        st->published = s_st->published;
        st->received = s_st->received;
        st->dropped = s_st->dropped;
        st->tx = s_st->tx;
        st->rx = s_st->rx;
    }
    pthread_mutex_unlock(&s_lock);
    pthread_mutex_lock(&s_pool_lock);
    st->pool_used = s_pool_used;
    st->pool_peak = s_pool_peak;
    pthread_mutex_unlock(&s_pool_lock);
}
