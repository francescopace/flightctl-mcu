/*
 * The apps' MQTT client (fc_mqtt.c) against a real broker: mosquitto in
 * Docker, which tests/mqtt-broker.sh starts. Plain TCP, then TLS: a wrong
 * CA and a wrong password refused; publishing seen from outside; messages
 * from outside on the device's and the fleet's prefixes; one too large
 * dropped; the keep-alive over a quiet minute (TLS); the broker restarted, the
 * client back with its subscriptions.
 *
 * Environment: MQTT_PLAIN and MQTT_TLS (host:port), MQTT_CA and
 * MQTT_WRONG_CA (PEM files), MQTT_USER, MQTT_PASS, MQTT_CONTAINER (for
 * mosquitto_pub/sub inside it, and to restart it). MQTT_VERBOSE=1 prints the
 * client's log.
 */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "fc_hw.h"
#include "fc_mqtt.h"
#include "fc_port.h"
#include "port_stub.h"

static int s_failures;
#define CHECK(c)                                                                                                       \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            fprintf(stderr, "  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);                                             \
            s_failures++;                                                                                              \
        }                                                                                                              \
    } while (0)

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_up, s_ups, s_downs;
typedef struct {
    void *owner;
    int sub;
    char topic[FC_MQTT_MAX_TOPIC + 1];
    char payload[FC_MQTT_MAX_PAYLOAD + 1];
} got_t;
static got_t s_got[16];
static int s_ngot;

static int on_message(void *owner, int sub, fc_mqtt_msg_t *m)
{
    pthread_mutex_lock(&s_lock);
    if (s_ngot < 16) {
        got_t *g = &s_got[s_ngot++];
        size_t tl, pl;
        const char *t = fc_mqtt_msg_topic(m, &tl);
        const char *p = fc_mqtt_msg_payload(m, &pl);
        g->owner = owner;
        g->sub = sub;
        snprintf(g->topic, sizeof(g->topic), "%.*s", (int)tl, t);
        snprintf(g->payload, sizeof(g->payload), "%.*s", (int)pl, p);
    }
    pthread_mutex_unlock(&s_lock);
    fc_mqtt_msg_free(m); /* copied: the app would keep it until its on_event */
    return 0;
}

static void on_link(int up)
{
    pthread_mutex_lock(&s_lock);
    s_up = up;
    if (up) {
        s_ups++;
    } else {
        s_downs++;
    }
    pthread_mutex_unlock(&s_lock);
}

static int get(int *v)
{
    pthread_mutex_lock(&s_lock);
    int x = *v;
    pthread_mutex_unlock(&s_lock);
    return x;
}

static int wait_for(int *v, int want, int seconds)
{
    for (int i = 0; i < seconds * 10 && get(v) < want; i++) {
        usleep(100000);
    }
    return get(v) >= want;
}

static const char *env(const char *name)
{
    const char *v = getenv(name);
    if (v == NULL) {
        fprintf(stderr, "%s is not set\n", name);
        exit(2);
    }
    return v;
}

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        perror(path);
        exit(2);
    }
    static char buf[8192];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

static void configure(const char *broker, const char *pass, const char *ca_file)
{
    char json[512];
    snprintf(json, sizeof(json),
             "{\"broker\": \"%s\", \"username\": \"%s\", \"password\": \"%s\", \"keepAliveSeconds\": 10,"
             " \"fleetPrefix\": \"flightctl/fleet/test/{app}/\"}",
             broker, env("MQTT_USER"), pass);
    fc_port_write_file(FC_MQTT_CONFIG, json, strlen(json));
    if (ca_file != NULL) {
        const char *ca = read_file(ca_file);
        fc_port_write_file(FC_MQTT_CA_FILE, ca, strlen(ca));
    } else {
        fc_port_remove_file(FC_MQTT_CA_FILE);
    }
    CHECK(fc_mqtt_configure("dev1") == 1);
}

/* mosquitto_pub inside the broker's container, on its plain listener. */
static void outside_pub(const char *topic, const char *payload, int qos)
{
    char cmd[1024];
    snprintf(cmd, sizeof(cmd), "docker exec %s mosquitto_pub -h localhost -p 1883 -u '%s' -P '%s' -q %d -t '%s' -m '%s'",
             env("MQTT_CONTAINER"), env("MQTT_USER"), env("MQTT_PASS"), qos, topic, payload);
    CHECK(system(cmd) == 0);
}

static void reset_got(void)
{
    pthread_mutex_lock(&s_lock);
    s_ngot = 0;
    pthread_mutex_unlock(&s_lock);
}

static void round_trip(const char *mode, int *a, int *b, int sa, int sb)
{
    printf("%s: publish, seen from outside\n", mode);
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "docker exec %s mosquitto_sub -h localhost -p 1883 -u '%s' -P '%s' -C 1 -W 10 -v -t 'flightctl/dev1/app1/out'",
             env("MQTT_CONTAINER"), env("MQTT_USER"), env("MQTT_PASS"));
    FILE *sub = popen(cmd, "r");
    CHECK(sub != NULL);
    sleep(2); /* mosquitto_sub subscribed */
    CHECK(fc_mqtt_publish("app1", "out", 3, "hello", 5, 1, 0) == 0);
    char line[256] = "";
    if (sub != NULL) {
        if (fgets(line, sizeof(line), sub) == NULL) {
            line[0] = '\0';
        }
        pclose(sub);
    }
    CHECK(strcmp(line, "flightctl/dev1/app1/out hello\n") == 0);

    printf("%s: messages from outside\n", mode);
    reset_got();
    outside_pub("flightctl/dev1/app1/in/x", "data", 1);
    outside_pub("flightctl/fleet/test/app2/cmd", "go", 0);
    CHECK(wait_for(&s_ngot, 2, 10));
    pthread_mutex_lock(&s_lock);
    CHECK(s_got[0].owner == a && s_got[0].sub == sa && strcmp(s_got[0].topic, "in/x") == 0 &&
          strcmp(s_got[0].payload, "data") == 0);
    CHECK(s_got[1].owner == b && s_got[1].sub == sb && strcmp(s_got[1].topic, "cmd") == 0 &&
          strcmp(s_got[1].payload, "go") == 0);
    pthread_mutex_unlock(&s_lock);

    printf("%s: too large, dropped\n", mode);
    fc_mqtt_stats_t st0, st;
    fc_mqtt_stats(&st0);
    char big[700];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    outside_pub("flightctl/dev1/app1/in/big", big, 1);
    outside_pub("flightctl/dev1/app1/in/after", "small", 1);
    CHECK(wait_for(&s_ngot, 3, 10));
    sleep(1);
    fc_mqtt_stats(&st);
    CHECK(st.dropped == st0.dropped + 1);
    pthread_mutex_lock(&s_lock);
    CHECK(s_ngot == 3 && strcmp(s_got[2].topic, "in/after") == 0);
    pthread_mutex_unlock(&s_lock);

    printf("%s: its own message back\n", mode);
    reset_got();
    CHECK(fc_mqtt_publish("app1", "in/self", 7, "me", 2, 0, 0) == 0);
    CHECK(wait_for(&s_ngot, 1, 10));
    pthread_mutex_lock(&s_lock);
    CHECK(strcmp(s_got[0].topic, "in/self") == 0 && strcmp(s_got[0].payload, "me") == 0);
    pthread_mutex_unlock(&s_lock);
}

int main(void)
{
    stub_reset();
    stub_verbose = getenv("MQTT_VERBOSE") != NULL;
    fc_mqtt_hooks_t hooks = {.on_message = on_message, .on_link = on_link};
    fc_mqtt_set_hooks(&hooks);
    int a, b;
    int sa = fc_mqtt_subscribe(&a, "app1", FC_MQTT_DEVICE, "in/#", 4, 1);
    int sb = fc_mqtt_subscribe(&b, "app2", FC_MQTT_FLEET, "cmd", 3, 0);
    CHECK(sa >= 0 && sb >= 0);
    char url[256];

    /* ---- plain TCP ---- */
    printf("plain: wrong password refused\n");
    snprintf(url, sizeof(url), "mqtt://%s", env("MQTT_PLAIN"));
    configure(url, "wrong", NULL);
    sleep(3);
    CHECK(get(&s_ups) == 0);
    char line[200], line2[200];
    snprintf(line, sizeof(line), "mqtt: %s refused the connection: not authorized", env("MQTT_PLAIN"));
    snprintf(line2, sizeof(line2), "mqtt: %s refused the connection: bad user name or password", env("MQTT_PLAIN"));
    CHECK(stub_log_has(line) || stub_log_has(line2));
    fc_mqtt_stats_t st;
    fc_mqtt_stats(&st);
    CHECK(st.connected == 0);

    printf("plain: connected\n");
    configure(url, env("MQTT_PASS"), NULL);
    CHECK(wait_for(&s_ups, 1, 15));
    sleep(1); /* the SUBACKs */
    round_trip("plain", &a, &b, sa, sb);
    fc_mqtt_stop();
    CHECK(get(&s_up) == 0);

    /* ---- TLS ---- */
    snprintf(url, sizeof(url), "mqtts://%s", env("MQTT_TLS"));
    printf("tls: wrong CA refused\n");
    int ups = get(&s_ups);
    configure(url, env("MQTT_PASS"), env("MQTT_WRONG_CA"));
    sleep(3);
    CHECK(get(&s_ups) == ups);
    CHECK(stub_log_has("mqtt: the broker's certificate:"));

    printf("tls: connected\n");
    configure(url, env("MQTT_PASS"), env("MQTT_CA"));
    CHECK(wait_for(&s_ups, ups + 1, 15));
    sleep(1);
    round_trip("tls", &a, &b, sa, sb);

    printf("tls: a quiet 25 s, kept alive (keepAliveSeconds 10)\n");
    fc_mqtt_stats_t st0;
    fc_mqtt_stats(&st0);
    sleep(25);
    fc_mqtt_stats(&st);
    CHECK(st.connected == 1 && st.connects == st0.connects);

    printf("tls: the broker restarted\n");
    int downs = get(&s_downs);
    ups = get(&s_ups);
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "docker restart %s >/dev/null", env("MQTT_CONTAINER"));
    CHECK(system(cmd) == 0);
    CHECK(wait_for(&s_downs, downs + 1, 20));
    CHECK(wait_for(&s_ups, ups + 1, 40));
    sleep(1);
    reset_got();
    outside_pub("flightctl/fleet/test/app2/cmd", "again", 1);
    CHECK(wait_for(&s_ngot, 1, 10));
    pthread_mutex_lock(&s_lock);
    CHECK(s_ngot >= 1 && strcmp(s_got[0].payload, "again") == 0);
    pthread_mutex_unlock(&s_lock);

    fc_mqtt_stats(&st);
    printf("tls: %lu connections, %lu published, %lu received, %lu dropped, %llu bytes out, %llu in, pool peak %zu\n",
           st.connects, st.published, st.received, st.dropped, st.tx, st.rx, st.pool_peak);
    fc_port_remove_file(FC_MQTT_CONFIG);
    CHECK(fc_mqtt_configure("dev1") == 0);
    if (s_failures != 0) {
        printf("%d check(s) failed\n", s_failures);
        return 1;
    }
    printf("ok\n");
    return 0;
}
