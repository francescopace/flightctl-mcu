/*
 * "mqttdemo" WASM app (docs/APPS.md#mqtt): the chip's temperature out over
 * MQTT, commands in.
 *
 * Every period_ms (the app's config, 10000 by default) it publishes, under
 * its device prefix (flightctl/<device>/mqttdemo/ by default):
 *   temp   {"t_mc": 41250, "up_s": 3600}   QoS 0
 * It subscribes to cmd/# on its device prefix (QoS 1) and to cmd on the
 * fleet's (QoS 0, when mqtt.json has a fleetPrefix), and answers each
 * command on
 *   ack    "<topic> <payload>"             QoS 1
 * "led on" and "led off" turn led0 on and off when it is granted. Its state
 * in Flight Control: "online", "offline" (no broker yet, or the link lost),
 * or why it cannot run.
 *
 * Grants: "mqtt", and "led0" for the LED: FC_ALLOW: "mqtt led0".
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fc_app.h"

FC_APP_ABI()

static int32_t period_ms = 10000;
static int32_t led = -1, timer = -1;
static int32_t sent, waiting; /* published, and not (FC_EAGAIN) since the last state */

static void read_config(void)
{
    char v[16];
    int32_t n = fc_config_get(FC_STR("period_ms"), v, sizeof(v) - 1);
    if (n > 0) {
        v[n] = '\0';
        int32_t ms = (int32_t)strtol(v, NULL, 10);
        period_ms = ms < 1000 ? 1000 : ms;
    }
    if (timer >= 0) {
        fc_close(timer);
    }
    timer = fc_timer_start(period_ms, 1);
}

static void publish_temp(void)
{
    int32_t t;
    if (fc_sensor_read(FC_STR("die_temp"), &t) < 0) {
        t = 0; /* the classic ESP32 has no sensor: the uptime alone */
    }
    char p[64];
    int32_t n = snprintf(p, sizeof(p), "{\"t_mc\": %d, \"up_s\": %d}", t, (int32_t)(fc_time_ms() / 1000));
    int32_t rc = fc_mqtt_publish(FC_STR("temp"), p, n, 0, 0);
    if (rc == 0) {
        sent++;
    } else if (rc == FC_EAGAIN) {
        waiting++;
    } else {
        printf("publish: error %d\n", rc);
    }
}

static void command(int32_t len)
{
    char topic[FC_MQTT_MAX_TOPIC + 1], payload[96];
    int32_t tl = fc_mqtt_topic(topic, sizeof(topic) - 1);
    int32_t pl = fc_event_data(payload, sizeof(payload) - 1);
    if (tl < 0 || pl < 0) {
        return;
    }
    topic[tl] = '\0';
    payload[pl] = '\0';
    printf("command on %s (%d bytes): %s\n", topic, len, payload);
    if (led >= 0 && strcmp(payload, "led on") == 0) {
        fc_led_rgb(led, 0, 32, 0);
    } else if (led >= 0 && strcmp(payload, "led off") == 0) {
        fc_led_rgb(led, 0, 0, 0);
    }
    char ack[FC_MQTT_MAX_TOPIC + 100];
    int32_t n = snprintf(ack, sizeof(ack), "%s %s", topic, payload);
    fc_mqtt_publish(FC_STR("ack"), ack, n, 1, 0);
}

int32_t init(void)
{
    int32_t rc = fc_mqtt_subscribe(FC_MQTT_DEVICE, FC_STR("cmd/#"), 1);
    if (rc == FC_EPERM) {
        FC_STATE("needs mqtt in FC_ALLOW");
        return 0; /* idle */
    }
    if (rc == FC_ENOTSUP) {
        FC_STATE("no MQTT in this firmware");
        return 0;
    }
    if (rc < 0) {
        printf("subscribe cmd/#: error %d\n", rc);
        return 1;
    }
    rc = fc_mqtt_subscribe(FC_MQTT_FLEET, FC_STR("cmd"), 0);
    if (rc < 0 && rc != FC_ENOENT) {
        printf("subscribe to the fleet's cmd: error %d\n", rc);
    }
    led = fc_led_open(FC_STR("led0")); /* FC_EPERM without led0 in FC_ALLOW: no LED */
    FC_STATE("offline");
    read_config();
    return timer >= 0 ? 0 : 1;
}

void on_event(int32_t type, int32_t h, int32_t value)
{
    (void)h;
    switch (type) {
    case FC_EV_TIMER:
        publish_temp();
        break;
    case FC_EV_MQTT:
        command(value);
        break;
    case FC_EV_NET:
        if (value) {
            FC_STATE("online");
        } else {
            char s[48];
            fc_set_state(s, snprintf(s, sizeof(s), "offline (%d sent)", sent));
        }
        break;
    case FC_EV_CONFIG:
        read_config();
        break;
    default:
        break;
    }
}
