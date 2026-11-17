/*
 * The apps' MQTT client (fc_mqtt.c): mqtt.json, topics and filters, the
 * packets, the receive pool, the TLS pool's second block, and the client
 * against a fake broker on a socketpair (plain TCP; tests/mqtt-broker.sh
 * runs it against mosquitto, with TLS).
 */
#include "../../core/src/fc_mqtt.c"

#include <sys/socket.h>
#include <unistd.h>

#include "port_stub.h"
#include "test.h"

#define FC_STR(lit) lit, (sizeof(lit) - 1)

/* ---- mqtt.json --------------------------------------------------------------- */

static cfg_t *parse(const char *json, const char *ca)
{
    return cfg_parse(json, ca, "dev1");
}

static void test_config(void)
{
    char host[64];
    int port, tls;
    CHECK_INT(parse_broker("mqtts://b.example.com", host, sizeof(host), &port, &tls), 0);
    CHECK_STR(host, "b.example.com");
    CHECK_INT(port, 8883);
    CHECK_INT(tls, 1);
    CHECK_INT(parse_broker("mqtt://10.0.0.1:1884/", host, sizeof(host), &port, &tls), 0);
    CHECK_STR(host, "10.0.0.1");
    CHECK_INT(port, 1884);
    CHECK_INT(tls, 0);
    CHECK_INT(parse_broker("b:9000", host, sizeof(host), &port, &tls), 0);
    CHECK_INT(port, 9000);
    CHECK_INT(tls, 1);
    CHECK_INT(parse_broker("mqtts://[fd00::1]:8884", host, sizeof(host), &port, &tls), 0);
    CHECK_STR(host, "fd00::1");
    CHECK_INT(port, 8884);
    CHECK_INT(parse_broker("ws://b:80", host, sizeof(host), &port, &tls), -1);
    CHECK_INT(parse_broker("mqtt://b:0", host, sizeof(host), &port, &tls), -1);
    CHECK_INT(parse_broker("mqtt://b:x", host, sizeof(host), &port, &tls), -1);
    CHECK_INT(parse_broker("mqtt://", host, sizeof(host), &port, &tls), -1);
    CHECK_INT(parse_broker("mqtt://b/path", host, sizeof(host), &port, &tls), -1);

    /* The defaults. */
    cfg_t *c = parse("{\"broker\": \"mqtts://b:8883\"}", "CA");
    CHECK(c != NULL);
    if (c != NULL) {
        CHECK_STR(c->client_id, "dev1");
        CHECK_INT(c->keepalive_s, 60);
        CHECK_STR(c->dev_prefix, "flightctl/dev1/{app}/");
        CHECK_STR(c->fleet_prefix, "");
        CHECK_STR(c->ca_pem, "CA");
        CHECK_STR(c->user, "");
    }
    cfg_free(c);
    c = parse("{\"broker\": \"mqtt://b\", \"username\": \"u\", \"password\": \"p\", \"clientId\": \"mcu-{device}\","
              " \"keepAliveSeconds\": 30, \"devicePrefix\": \"plant/{device}/{app}\","
              " \"fleetPrefix\": \"plant/fleet/pumps/{app}/\"}",
              NULL);
    CHECK(c != NULL);
    if (c != NULL) {
        CHECK_STR(c->client_id, "mcu-dev1");
        CHECK_INT(c->keepalive_s, 30);
        CHECK_STR(c->dev_prefix, "plant/dev1/{app}/"); /* a '/' added */
        CHECK_STR(c->fleet_prefix, "plant/fleet/pumps/{app}/");
        CHECK(c->ca_pem == NULL);
    }
    cfg_free(c);
    /* What cannot be used. */
    CHECK(parse("[]", "CA") == NULL);
    CHECK(parse("{}", "CA") == NULL);
    CHECK(parse("{\"broker\": \"mqtts://b\"}", NULL) == NULL); /* TLS without a CA */
    CHECK(stub_log_has("mqtt: mqtts://b needs its CA in /etc/fc/mqtt-ca.crt"));
    CHECK(parse("{\"broker\": \"mqtt://b\", \"password\": \"p\"}", NULL) == NULL);
    CHECK(parse("{\"broker\": \"mqtt://b\", \"keepAliveSeconds\": 5}", NULL) == NULL);
    CHECK(parse("{\"broker\": \"mqtt://b\", \"devicePrefix\": \"a/+/{app}\"}", NULL) == NULL);
    CHECK(parse("{\"broker\": \"mqtt://b\", \"fleetPrefix\": \"a/#\"}", NULL) == NULL);
    CHECK(parse("{\"broker\": \"mqtt://b\", \"devicePrefix\": \"$SYS/{app}\"}", NULL) == NULL);
    CHECK(parse("{\"broker\": \"mqtt://b\", \"devicePrefix\": \"\"}", NULL) == NULL);
    CHECK(parse("{\"broker\": \"mqtt://b\", \"clientId\": \"\"}", NULL) == NULL);
    char long_prefix[300];
    memset(long_prefix, 'a', sizeof(long_prefix));
    long_prefix[sizeof(long_prefix) - 1] = '\0';
    char json[400];
    snprintf(json, sizeof(json), "{\"broker\": \"mqtt://b\", \"devicePrefix\": \"%s\"}", long_prefix);
    CHECK(parse(json, NULL) == NULL);
    /* Without {app}, it works, with a warning. */
    c = parse("{\"broker\": \"mqtt://b\", \"devicePrefix\": \"d/{device}\"}", NULL);
    CHECK(c != NULL);
    CHECK(stub_log_has("mqtt: devicePrefix has no {app}: the apps share their topics"));
    cfg_free(c);
}

/* ---- topics -------------------------------------------------------------------- */

static int matches(const char *f, const char *t)
{
    return topic_matches(f, t, strlen(t));
}

static void test_topics(void)
{
    CHECK(topic_ok("a/b", 3));
    CHECK(!topic_ok("", 0));
    CHECK(!topic_ok("a/+", 3));
    CHECK(!topic_ok("a/#", 3));
    CHECK(!topic_ok("a\0b", 3));
    char big[FC_MQTT_MAX_TOPIC + 1];
    memset(big, 'a', sizeof(big));
    CHECK(topic_ok(big, FC_MQTT_MAX_TOPIC));
    CHECK(!topic_ok(big, FC_MQTT_MAX_TOPIC + 1));

    CHECK(filter_ok("#", 1));
    CHECK(filter_ok("+", 1));
    CHECK(filter_ok("a/+/c", 5));
    CHECK(filter_ok("a/#", 3));
    CHECK(filter_ok("+/+", 3));
    CHECK(!filter_ok("a#", 2));
    CHECK(!filter_ok("a/#/b", 5));
    CHECK(!filter_ok("a+/b", 4));
    CHECK(!filter_ok("a/b+", 4));
    CHECK(!filter_ok("", 0));

    CHECK(matches("p/a/b", "p/a/b"));
    CHECK(!matches("p/a/b", "p/a/bc"));
    CHECK(!matches("p/a/bc", "p/a/b"));
    CHECK(!matches("p/a", "p/a/b"));
    CHECK(matches("p/+/c", "p/b/c"));
    CHECK(!matches("p/+/c", "p/b/d"));
    CHECK(!matches("p/+", "p/b/c"));
    CHECK(matches("p/+", "p/"));
    CHECK(matches("p/#", "p/b/c"));
    CHECK(matches("p/#", "p"));
    CHECK(matches("p/a/#", "p/a/"));
    CHECK(!matches("p/a/#", "p/ab"));
    CHECK(matches("p/+/+", "p//"));
    CHECK(!matches("+/x", "$SYS/x"));
    CHECK(matches("$SYS/x", "$SYS/x"));
}

/* ---- packets ------------------------------------------------------------------------- */

static void test_packets(void)
{
    unsigned char out[TX_CAP];
    unsigned char lenb[4];
    CHECK_INT(put_len(lenb, 0), 1);
    CHECK_INT(put_len(lenb, 127), 1);
    CHECK_INT(put_len(lenb, 128), 2);
    CHECK_INT(lenb[0], 0x80);
    CHECK_INT(lenb[1], 0x01);
    CHECK_INT(put_len(lenb, 16383), 2);
    CHECK_INT(put_len(lenb, 16384), 3);

    size_t n = enc_publish(out, "p/", 2, "t", 1, "hi", 2, 1, 1, 0x1234);
    static const unsigned char pub[] = {0x33, 9, 0, 3, 'p', '/', 't', 0x12, 0x34, 'h', 'i'};
    CHECK_INT(n, sizeof(pub));
    CHECK(memcmp(out, pub, sizeof(pub)) == 0);
    n = enc_publish(out, "p/", 2, "t", 1, "", 0, 0, 0, 0);
    static const unsigned char pub0[] = {0x30, 5, 0, 3, 'p', '/', 't'};
    CHECK_INT(n, sizeof(pub0));
    CHECK(memcmp(out, pub0, sizeof(pub0)) == 0);
    /* The largest: a two-byte length. */
    char topic[FC_MQTT_MAX_TOPIC], payload[FC_MQTT_MAX_PAYLOAD], prefix[FC_MQTT_MAX_PREFIX];
    memset(topic, 't', sizeof(topic));
    memset(payload, 'x', sizeof(payload));
    memset(prefix, 'p', sizeof(prefix));
    n = enc_publish(out, prefix, sizeof(prefix), topic, sizeof(topic), payload, sizeof(payload), 1, 0, 1);
    CHECK(n <= TX_CAP);
    CHECK_INT(n, 1 + 2 + 2 + FULL_TOPIC_MAX + 2 + FC_MQTT_MAX_PAYLOAD);

    n = enc_subscribe(out, 0, 5, "a/#", 1);
    static const unsigned char sub[] = {0x82, 8, 0, 5, 0, 3, 'a', '/', '#', 1};
    CHECK_INT(n, sizeof(sub));
    CHECK(memcmp(out, sub, sizeof(sub)) == 0);
    n = enc_subscribe(out, 1, 6, "a", 0);
    static const unsigned char unsub[] = {0xa2, 5, 0, 6, 0, 1, 'a'};
    CHECK_INT(n, sizeof(unsub));
    CHECK(memcmp(out, unsub, sizeof(unsub)) == 0);

    cfg_t c;
    memset(&c, 0, sizeof(c));
    strcpy(c.client_id, "d");
    strcpy(c.user, "u");
    strcpy(c.pass, "p");
    c.keepalive_s = 60;
    n = enc_connect(out, &c);
    static const unsigned char conn[] = {0x10, 19, 0, 4, 'M', 'Q', 'T', 'T', 4, 0xc2, 0, 60,
                                         0,    1,  'd', 0, 1, 'u', 0, 1, 'p'};
    CHECK_INT(n, sizeof(conn));
    CHECK(memcmp(out, conn, sizeof(conn)) == 0);
}

/* ---- the receive pool ------------------------------------------------------------------ */

static void test_pool(void)
{
    CHECK_INT(pool_init(), 0);
    int a, b;
    /* 8 + 4 + 500 = 512: an owner holds four, half the pool. */
    struct fc_mqtt_msg *m[8];
    for (int i = 0; i < 4; i++) {
        m[i] = pool_alloc(&a, 4, 500);
        CHECK(m[i] != NULL);
    }
    CHECK(pool_alloc(&a, 4, 500) == NULL);
    CHECK(pool_alloc(&a, 0, 1) == NULL);
    for (int i = 4; i < 8; i++) {
        m[i] = pool_alloc(&b, 4, 500);
        CHECK(m[i] != NULL);
    }
    CHECK(pool_alloc(&b, 0, 1) == NULL);
    fc_mqtt_stats_t st;
    fc_mqtt_stats(&st);
    CHECK_INT(st.pool_used, FC_MQTT_POOL_SIZE);
    /* Freed blocks join again: two neighbours make room for a larger one. */
    fc_mqtt_msg_free(m[1]);
    fc_mqtt_msg_free(m[2]);
    struct fc_mqtt_msg *big = pool_alloc(&a, 64, 900);
    CHECK(big != NULL);
    CHECK_INT(big != NULL ? big->plen : 0, 900);
    fc_mqtt_msg_free(big);
    fc_mqtt_msg_free(m[0]);
    fc_mqtt_msg_free(m[3]);
    for (int i = 4; i < 8; i++) {
        fc_mqtt_msg_free(m[i]);
    }
    fc_mqtt_msg_free(NULL);
    fc_mqtt_stats(&st);
    CHECK_INT(st.pool_used, 0);
    CHECK_INT(st.pool_peak, FC_MQTT_POOL_SIZE);
    /* All free again: one block of the whole pool. */
    struct fc_mqtt_msg *whole = (struct fc_mqtt_msg *)s_pool;
    CHECK(pool_alloc(&a, 0, FC_MQTT_POOL_SIZE / 2 - MSG_HDR) == whole);
    CHECK_INT(whole->size, FC_MQTT_POOL_SIZE / 2);
    fc_mqtt_msg_free(whole);
    /* Many small ones from many owners. */
    int owners[FC_MQTT_MAX_SUBS + 1];
    struct fc_mqtt_msg *s[FC_MQTT_MAX_SUBS + 1];
    for (int i = 0; i < FC_MQTT_MAX_SUBS; i++) {
        s[i] = pool_alloc(&owners[i], 3, 5);
        CHECK(s[i] != NULL);
    }
    CHECK(pool_alloc(&owners[FC_MQTT_MAX_SUBS], 3, 5) == NULL); /* no owner left */
    for (int i = 0; i < FC_MQTT_MAX_SUBS; i++) {
        fc_mqtt_msg_free(s[i]);
    }
    fc_mqtt_stats(&st);
    CHECK_INT(st.pool_used, 0);
}

/* ---- the TLS pool's second block --------------------------------------------------------- */

static void test_tls_extra(void)
{
    CHECK_INT(fc_tls_pool_init(), 0);
    int now, peak;
    void *one = fc_tls_pool_take(1, FC_TLS_POOL_MIN);
    CHECK(one != NULL);
    CHECK(fc_tls_pool_take(1, FC_TLS_POOL_MIN) == NULL); /* one block only */
    CHECK_INT(fc_tls_pool_extra(1), 0);
    void *two = fc_tls_pool_take(1, FC_TLS_POOL_MIN);
    CHECK(two != NULL && two != one);
    fc_tls_pool_usage(&now, &peak);
    CHECK_INT(now, 2);
    /* Given back while in use: it goes when it is free. */
    CHECK_INT(fc_tls_pool_extra(0), 0);
    CHECK_INT(fc_tls_pool_give(two), 1);
    CHECK_INT(fc_tls_pool_give(one), 1);
    one = fc_tls_pool_take(1, FC_TLS_POOL_MIN);
    CHECK(one != NULL);
    CHECK(fc_tls_pool_take(1, FC_TLS_POOL_MIN) == NULL);
    CHECK_INT(fc_tls_pool_give(one), 1);
    /* Reserved and given back while free. */
    CHECK_INT(fc_tls_pool_extra(1), 0);
    CHECK_INT(fc_tls_pool_extra(1), 0); /* twice is once */
    CHECK_INT(fc_tls_pool_extra(0), 0);
    one = fc_tls_pool_take(1, FC_TLS_POOL_MIN);
    CHECK(fc_tls_pool_take(1, FC_TLS_POOL_MIN) == NULL);
    fc_tls_pool_give(one);
    fc_tls_pool_usage(&now, &peak);
    CHECK_INT(now, 0);
}

/* ---- a fake broker ------------------------------------------------------------------------ */

static pthread_mutex_t f_lock = PTHREAD_MUTEX_INITIALIZER;
static int f_fd = -1, f_connects, f_links_up, f_links_down, f_refuse;
typedef struct {
    void *owner;
    int sub;
    char topic[FC_MQTT_MAX_TOPIC + 1];
    char payload[FC_MQTT_MAX_PAYLOAD + 1];
    size_t plen;
    fc_mqtt_msg_t *m;
} got_t;
static got_t f_got[32];
static int f_ngot;

static int fake_connect(mbedtls_net_context *net, const char *host, int port)
{
    int sv[2];
    if (strcmp(host, "broker.test") != 0 || port != 1883 || socketpair(AF_UNIX, SOCK_STREAM, 0, sv) != 0) {
        return MBEDTLS_ERR_NET_CONNECT_FAILED;
    }
    net->fd = sv[0];
    pthread_mutex_lock(&f_lock);
    if (f_fd >= 0) {
        close(f_fd);
    }
    f_fd = sv[1];
    f_connects++;
    pthread_mutex_unlock(&f_lock);
    return 0;
}

static int f_on_message(void *owner, int sub, fc_mqtt_msg_t *m)
{
    pthread_mutex_lock(&f_lock);
    if (f_refuse || f_ngot == 32) {
        pthread_mutex_unlock(&f_lock);
        return -1;
    }
    got_t *g = &f_got[f_ngot++];
    g->owner = owner;
    g->sub = sub;
    size_t tl, pl;
    const char *t = fc_mqtt_msg_topic(m, &tl);
    const void *p = fc_mqtt_msg_payload(m, &pl);
    memcpy(g->topic, t, tl);
    g->topic[tl] = '\0';
    memcpy(g->payload, p, pl);
    g->payload[pl] = '\0';
    g->plen = pl;
    g->m = m;
    pthread_mutex_unlock(&f_lock);
    return 0;
}

static void f_on_link(int up)
{
    pthread_mutex_lock(&f_lock);
    if (up) {
        f_links_up++;
    } else {
        f_links_down++;
    }
    pthread_mutex_unlock(&f_lock);
}

static int locked(int *v)
{
    pthread_mutex_lock(&f_lock);
    int x = *v;
    pthread_mutex_unlock(&f_lock);
    return x;
}

static int wait_for(int *v, int want, int ms)
{
    for (int i = 0; i < ms / 10; i++) {
        if (locked(v) >= want) {
            return 1;
        }
        usleep(10000);
    }
    return locked(v) >= want;
}

static int f_read(unsigned char *b, size_t n)
{
    int fd = locked(&f_fd);
    while (n > 0) {
        if (fd_readable(fd, 3000) != 1) {
            return -1;
        }
        ssize_t r = read(fd, b, n);
        if (r <= 0) {
            return -1;
        }
        b += r;
        n -= (size_t)r;
    }
    return 0;
}

/* One packet from the client: its first byte, its body in b. -1 none in 3 s. */
static int f_packet(unsigned char *b, size_t cap, size_t *len)
{
    unsigned char first, x;
    if (f_read(&first, 1) != 0) {
        return -1;
    }
    size_t rem = 0, mult = 1;
    do {
        if (f_read(&x, 1) != 0) {
            return -1;
        }
        rem += (x & 0x7fu) * mult;
        mult *= 128;
    } while (x & 0x80);
    if (rem > cap || f_read(b, rem) != 0) {
        return -1;
    }
    *len = rem;
    return first;
}

static void f_send(const void *b, size_t n)
{
    CHECK_INT(write(locked(&f_fd), b, n), (long)n);
}

/* A PUBLISH from the broker. */
static void f_publish(const char *topic, const char *payload, size_t plen, int qos, uint16_t pid)
{
    unsigned char out[2048];
    size_t n = enc_publish(out, "", 0, topic, strlen(topic), payload, plen, qos, 0, pid);
    f_send(out, n);
}

/* Expects a SUBSCRIBE (or UNSUBSCRIBE) for `filter`, answers it. */
static void f_expect_sub(int unsub, const char *filter, int qos)
{
    unsigned char b[512];
    size_t n = 0;
    int t = f_packet(b, sizeof(b), &n);
    CHECK_INT(t, unsub ? 0xa2 : 0x82);
    if (t < 0 || n < 5) {
        return;
    }
    size_t fl = (size_t)b[2] << 8 | b[3];
    CHECK_INT(fl, strlen(filter));
    CHECK(fl == strlen(filter) && memcmp(b + 4, filter, fl) == 0);
    if (!unsub) {
        CHECK_INT(b[4 + fl], qos);
    }
    unsigned char ack[5] = {unsub ? 0xb0 : 0x90, unsub ? 2 : 3, b[0], b[1], (unsigned char)qos};
    f_send(ack, unsub ? 4 : 5);
}

static void f_accept(int code)
{
    unsigned char b[512];
    size_t n = 0;
    CHECK_INT(f_packet(b, sizeof(b), &n), 0x10);
    unsigned char ack[4] = {0x20, 2, 0, (unsigned char)code};
    f_send(ack, sizeof(ack));
}

static got_t *got(int i)
{
    static got_t none;
    return i < locked(&f_ngot) ? &f_got[i] : &none;
}

static void test_client(void)
{
    stub_reset();
    s_connect_hook = fake_connect;
    fc_mqtt_hooks_t hooks = {.on_message = f_on_message, .on_link = f_on_link};
    fc_mqtt_set_hooks(&hooks);
    int a, b, c; /* the apps */

    /* No mqtt.json: nothing runs, publishing waits, subscribing is kept. */
    CHECK_INT(fc_mqtt_configure("dev1"), 0);
    CHECK_INT(fc_mqtt_publish("app1", FC_STR("out"), "x", 1, 0, 0), FC_HW_EAGAIN);
    int sa = fc_mqtt_subscribe(&a, "app1", FC_MQTT_DEVICE, FC_STR("in/#"), 1);
    int sb = fc_mqtt_subscribe(&b, "app2", FC_MQTT_FLEET, FC_STR("cmd"), 0);
    CHECK(sa >= 0 && sb >= 0 && sa != sb);
    CHECK_INT(fc_mqtt_subscribe(&a, "app1", FC_MQTT_DEVICE, FC_STR("in/#/x"), 0), FC_HW_EINVAL);
    CHECK_INT(fc_mqtt_subscribe(&a, "app1", FC_MQTT_DEVICE, FC_STR("in"), 2), FC_HW_EINVAL);
    CHECK_INT(fc_mqtt_subscribe(&a, "app1", 2, FC_STR("in"), 0), FC_HW_EINVAL);

    /* A config, but no device name yet. */
    const char *json = "{\"broker\": \"mqtt://broker.test\", \"username\": \"u\", \"password\": \"p\","
                       " \"fleetPrefix\": \"flightctl/fleet/f1/{app}/\"}";
    fc_port_write_file(FC_MQTT_CONFIG, json, strlen(json));
    CHECK_INT(fc_mqtt_configure(NULL), 0);
    CHECK_INT(locked(&f_connects), 0);

    /* Connecting: CONNECT, then the subscriptions taken before. */
    CHECK_INT(fc_mqtt_configure("dev1"), 1);
    CHECK(stub_log_has("mqtt: broker.test:1883 without TLS, as the fleet asks"));
    CHECK(wait_for(&f_connects, 1, 3000));
    unsigned char pk[1024];
    size_t n = 0;
    CHECK_INT(f_packet(pk, sizeof(pk), &n), 0x10);
    static const unsigned char want_conn[] = {0, 4, 'M', 'Q', 'T', 'T', 4, 0xc2, 0, 60, 0, 4, 'd', 'e', 'v', '1',
                                              0, 1, 'u', 0, 1, 'p'};
    CHECK_INT(n, sizeof(want_conn));
    CHECK(n == sizeof(want_conn) && memcmp(pk, want_conn, n) == 0);
    unsigned char connack[4] = {0x20, 2, 0, 0};
    f_send(connack, sizeof(connack));
    CHECK(wait_for(&f_links_up, 1, 3000));
    f_expect_sub(0, "flightctl/dev1/app1/in/#", 1);
    f_expect_sub(0, "flightctl/fleet/f1/app2/cmd", 0);
    fc_mqtt_stats_t st;
    fc_mqtt_stats(&st);
    CHECK_INT(st.connected, 1);
    CHECK_INT(st.connects, 1);

    /* Publishing: under the app's prefix, QoS 1 with a packet id. */
    CHECK_INT(fc_mqtt_publish("app1", FC_STR("out"), "hi", 2, 1, 0), 0);
    CHECK_INT(f_packet(pk, sizeof(pk), &n), 0x32);
    CHECK_INT(n, 2 + 23 + 2 + 2);
    CHECK(memcmp(pk + 2, "flightctl/dev1/app1/out", 23) == 0 && memcmp(pk + 27, "hi", 2) == 0);
    unsigned char puback[4] = {0x40, 2, pk[25], pk[26]};
    f_send(puback, sizeof(puback));
    CHECK_INT(fc_mqtt_publish("app1", FC_STR("st"), "", 0, 0, 1), 0);
    CHECK_INT(f_packet(pk, sizeof(pk), &n), 0x31); /* retained, QoS 0 */
    CHECK_INT(fc_mqtt_publish("app1", FC_STR("a/+"), "x", 1, 0, 0), FC_HW_EINVAL);
    CHECK_INT(fc_mqtt_publish("app1", FC_STR("a"), "x", 1, 2, 0), FC_HW_EINVAL);
    char big[FC_MQTT_MAX_PAYLOAD + 100];
    memset(big, 'x', sizeof(big));
    CHECK_INT(fc_mqtt_publish("app1", FC_STR("a"), big, FC_MQTT_MAX_PAYLOAD + 1, 0, 0), FC_HW_EINVAL);
    CHECK_INT(fc_mqtt_publish("app1", FC_STR("a"), big, FC_MQTT_MAX_PAYLOAD, 0, 0), 0);
    CHECK_INT(f_packet(pk, sizeof(pk), &n), 0x30);
    CHECK_INT(fc_mqtt_publish("a+b", FC_STR("a"), "x", 1, 0, 0), FC_HW_EINVAL); /* the app's name a wildcard */

    /* Receiving: to the subscription it matches, its topic relative, QoS 1 acknowledged. */
    f_publish("flightctl/dev1/app1/in/x", "data", 4, 1, 7);
    CHECK_INT(f_packet(pk, sizeof(pk), &n), 0x40);
    CHECK(n == 2 && pk[0] == 0 && pk[1] == 7);
    f_publish("flightctl/fleet/f1/app2/cmd", "go", 2, 0, 0);
    f_publish("flightctl/dev1/app9/in/x", "not ours", 8, 0, 0);
    f_publish("flightctl/dev1/app1/in", "a/# matches its parent", 22, 0, 0);
    CHECK(wait_for(&f_ngot, 3, 3000));
    CHECK(got(0)->owner == &a && got(0)->sub == sa);
    CHECK_STR(got(0)->topic, "in/x");
    CHECK_STR(got(0)->payload, "data");
    CHECK(got(1)->owner == &b && got(1)->sub == sb);
    CHECK_STR(got(1)->topic, "cmd");
    CHECK_STR(got(1)->payload, "go");
    CHECK_STR(got(2)->topic, "in");
    usleep(100000);
    CHECK_INT(locked(&f_ngot), 3);

    /* Too large: dropped, still acknowledged. */
    f_publish("flightctl/dev1/app1/in/big", big, FC_MQTT_MAX_PAYLOAD + 1, 1, 8);
    CHECK_INT(f_packet(pk, sizeof(pk), &n), 0x40);
    CHECK(n == 2 && pk[1] == 8);
    fc_mqtt_stats(&st);
    CHECK_INT(st.dropped, 1);
    CHECK(stub_log_has("mqtt: 1 message(s) dropped, the last: a payload too large"));
    /* An app that refuses it: dropped, its block back in the pool. */
    pthread_mutex_lock(&f_lock);
    f_refuse = 1;
    pthread_mutex_unlock(&f_lock);
    f_publish("flightctl/dev1/app1/in/r", "r", 1, 1, 9);
    CHECK_INT(f_packet(pk, sizeof(pk), &n), 0x40);
    pthread_mutex_lock(&f_lock);
    f_refuse = 0;
    pthread_mutex_unlock(&f_lock);
    fc_mqtt_stats(&st);
    CHECK_INT(st.dropped, 2);
    for (int i = 0; i < 3; i++) {
        fc_mqtt_msg_free(got(i)->m);
    }
    fc_mqtt_stats(&st);
    CHECK_INT(st.pool_used, 0);
    CHECK_INT(st.received, 3);

    /* An app that does not read: it fills its half of the pool, the other still receives. */
    pthread_mutex_lock(&f_lock);
    f_ngot = 0;
    pthread_mutex_unlock(&f_lock);
    for (int i = 0; i < 5; i++) {
        f_publish("flightctl/dev1/app1/in/full", big, 490, 0, 0); /* 8 + 7 + 490: 4 fit */
    }
    f_publish("flightctl/fleet/f1/app2/cmd", "still", 5, 0, 0);
    CHECK(wait_for(&f_ngot, 5, 3000));
    usleep(100000);
    CHECK_INT(locked(&f_ngot), 5);
    CHECK(got(4)->owner == &b);
    fc_mqtt_stats(&st);
    CHECK_INT(st.dropped, 3);
    for (int i = 0; i < 5; i++) {
        fc_mqtt_msg_free(got(i)->m);
    }
    pthread_mutex_lock(&f_lock);
    f_ngot = 0;
    pthread_mutex_unlock(&f_lock);

    /* Two apps on one filter: each its own copy; the filter goes when the last one does. */
    int sc = fc_mqtt_subscribe(&c, "app2", FC_MQTT_FLEET, FC_STR("cmd"), 1);
    CHECK(sc >= 0);
    f_expect_sub(0, "flightctl/fleet/f1/app2/cmd", 1); /* the highest QoS asked */
    f_publish("flightctl/fleet/f1/app2/cmd", "both", 4, 0, 0);
    CHECK(wait_for(&f_ngot, 2, 3000));
    CHECK(got(0)->m != got(1)->m);
    CHECK((got(0)->owner == &b && got(1)->owner == &c) || (got(0)->owner == &c && got(1)->owner == &b));
    CHECK_STR(got(1)->payload, "both");
    fc_mqtt_msg_free(got(0)->m);
    fc_mqtt_msg_free(got(1)->m);
    pthread_mutex_lock(&f_lock);
    f_ngot = 0;
    pthread_mutex_unlock(&f_lock);
    CHECK_INT(fc_mqtt_unsubscribe(&b, sc), FC_HW_EINVAL); /* not b's */
    CHECK_INT(fc_mqtt_unsubscribe(&c, sc), 0);
    CHECK_INT(fc_mqtt_unsubscribe(&c, sc), FC_HW_EINVAL); /* closed already */
    CHECK_INT(fc_mqtt_publish("app1", FC_STR("p"), "1", 1, 0, 0), 0);
    CHECK_INT(f_packet(pk, sizeof(pk), &n), 0x30); /* no UNSUBSCRIBE: b still has it */
    fc_mqtt_drop_owner(&b);
    f_expect_sub(1, "flightctl/fleet/f1/app2/cmd", 0);

    /* The link lost: the client connects again and subscribes again. */
    pthread_mutex_lock(&f_lock);
    close(f_fd);
    f_fd = -1;
    pthread_mutex_unlock(&f_lock);
    CHECK(wait_for(&f_links_down, 1, 3000));
    CHECK_INT(fc_mqtt_publish("app1", FC_STR("p"), "1", 1, 0, 0), FC_HW_EAGAIN);
    CHECK(wait_for(&f_connects, 2, 5000));
    f_accept(0);
    CHECK(wait_for(&f_links_up, 2, 3000));
    f_expect_sub(0, "flightctl/dev1/app1/in/#", 1);
    f_publish("flightctl/dev1/app1/in/again", "x", 1, 0, 0);
    CHECK(wait_for(&f_ngot, 1, 3000));
    CHECK_STR(got(0)->topic, "in/again");
    fc_mqtt_msg_free(got(0)->m);

    /* The same config again: nothing changes. */
    CHECK_INT(fc_mqtt_configure("dev1"), 1);
    CHECK_INT(locked(&f_connects), 2);

    /* A new prefix: the client connects again, the subscription under it. */
    json = "{\"broker\": \"mqtt://broker.test\", \"username\": \"u\", \"password\": \"p\","
           " \"devicePrefix\": \"plant/{device}/{app}\"}";
    fc_port_write_file(FC_MQTT_CONFIG, json, strlen(json));
    CHECK_INT(fc_mqtt_configure("dev1"), 1);
    CHECK_INT(f_packet(pk, sizeof(pk), &n), 0xe0); /* DISCONNECT */
    CHECK(wait_for(&f_connects, 3, 3000));
    f_accept(0);
    f_expect_sub(0, "plant/dev1/app1/in/#", 1);
    /* No fleetPrefix now. */
    CHECK_INT(fc_mqtt_subscribe(&b, "app2", FC_MQTT_FLEET, FC_STR("cmd"), 0), FC_HW_ENOENT);

    /* Refused by the broker: logged, tried again later. */
    pthread_mutex_lock(&f_lock);
    close(f_fd);
    f_fd = -1;
    pthread_mutex_unlock(&f_lock);
    CHECK(wait_for(&f_connects, 4, 5000));
    f_accept(4);
    CHECK(wait_for(&f_links_down, 3, 3000));
    usleep(100000);
    CHECK(stub_log_has("mqtt: broker.test:1883 refused the connection: bad user name or password"));
    fc_mqtt_stats(&st);
    CHECK_INT(st.connected, 0);

    /* Stopped: no more connections, the subscriptions kept. */
    fc_port_remove_file(FC_MQTT_CONFIG);
    CHECK_INT(fc_mqtt_configure("dev1"), 0);
    fc_mqtt_stats(&st);
    CHECK_INT(st.configured, 0);
    CHECK_INT(fc_mqtt_unsubscribe(&a, sa), 0);
    fc_mqtt_set_hooks(NULL);
    s_connect_hook = NULL;
}

int main(void)
{
    RUN(test_config);
    RUN(test_topics);
    RUN(test_packets);
    RUN(test_pool);
    RUN(test_tls_extra);
    RUN(test_client);
    return TEST_RESULT();
}
