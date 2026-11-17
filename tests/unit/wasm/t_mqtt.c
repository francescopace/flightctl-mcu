/* MQTT both ways (test_apps.c, test_fc_mqtt): subscribes under its own and
 * the fleet's prefix, publishes "up" when the link comes, answers each
 * message on "ack" with its topic and payload, and closes its first
 * subscription on "close". Its state says what it saw. */
#include "fc_app.h"

FC_APP_ABI()

static int32_t sub = -1;
static char st[64];
static int32_t n;

static void put(const char *s, int32_t len)
{
    volatile char *out = st; /* not a memcpy: there is no C library here */
    for (int32_t i = 0; i < len && n < (int32_t)sizeof(st) - 1; i++) {
        out[n++] = s[i];
    }
}

static void put_int(int32_t v)
{
    char b[12];
    put(b, fc_fmt_int(b, v));
}

int32_t init(void)
{
    char d[8];
    int32_t outside = fc_event_data(d, sizeof(d)); /* no message being handled */
    sub = fc_mqtt_subscribe(FC_MQTT_DEVICE, FC_STR("in/#"), 1);
    n = 0;
    if (sub < 0) {
        put(FC_STR("nosub "));
        put_int(sub);
        fc_set_state(st, n);
        return 0;
    }
    int32_t fleet = fc_mqtt_subscribe(FC_MQTT_FLEET, FC_STR("cmd"), 0);
    int32_t bad = fc_mqtt_subscribe(FC_MQTT_DEVICE, FC_STR("a/#/b"), 0);
    put(FC_STR("init "));
    put_int(outside);
    put(fleet >= 0 ? " ok " : " no ", 4);
    put_int(bad);
    fc_set_state(st, n);
    return 0;
}

void on_event(int32_t type, int32_t h, int32_t value)
{
    n = 0;
    if (type == FC_EV_NET) {
        put(FC_STR("net "));
        put_int(value);
        if (value) {
            put(FC_STR(" "));
            put_int(fc_mqtt_publish(FC_STR("up"), "hi", 2, 0, 0));
        }
    } else if (type == FC_EV_MQTT) {
        char t[FC_MQTT_MAX_TOPIC], p[64];
        int32_t tl = fc_mqtt_topic(t, sizeof(t));
        int32_t small = fc_event_data(p, 1);
        int32_t pl = fc_event_data(p, sizeof(p));
        if (tl < 0 || pl < 0) {
            put(FC_STR("err "));
            put_int(tl < 0 ? tl : pl);
            fc_set_state(st, n);
            return;
        }
        if (pl == 5 && p[0] == 'c' && p[1] == 'l' && p[2] == 'o' && p[3] == 's' && p[4] == 'e') {
            fc_close(sub);
        }
        put(t, tl);
        put(FC_STR(" "));
        put(p, pl);
        fc_mqtt_publish(FC_STR("ack"), st, n, 1, 0);
        n = 0;
        put(FC_STR("msg "));
        put(t, tl);
        put(FC_STR(" "));
        put(p, pl);
        put(FC_STR(" "));
        put_int(value);
        put(FC_STR(" "));
        put_int(small);
    } else {
        return;
    }
    fc_set_state(st, n);
}
