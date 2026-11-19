/* Sample locally even without MQTT. When a consumer acknowledges a sample
 * on the device's "ack" topic, advance the persistent queue by one. */
#include "fc_app.h"

FC_APP_ABI()

static int32_t timer, ack, online;
static char sensor[32];
static int32_t sensor_len;

static void publish_head(void)
{
    struct {
        uint32_t id;
        int32_t temperature;
    } packet;
    if (online && ack >= 0 &&
        fc_queue_peek(FC_STR("samples"), &packet.temperature, sizeof(packet.temperature), &packet.id) == 4) {
        fc_mqtt_publish(FC_STR("sample"), &packet, sizeof(packet), 1, 0);
    }
}

static void sample(void)
{
    int32_t value;
    if (fc_sensor_read(sensor, sensor_len, &value) == 0) {
        int local = fc_local_send(FC_STR("localalarm"), value);
        int rc = fc_queue_push(FC_STR("samples"), &value, sizeof(value));
        if (rc == FC_ENOSPC) {
            FC_STATE("sample queue full");
        } else if (rc < 0) {
            FC_STATE("sample storage error");
        } else if (local == 0) {
            FC_STATE("queued and sent locally");
        } else {
            FC_STATE("queued");
        }
    } else {
        FC_STATE("sensor unavailable");
    }
    publish_head();
}

int32_t init(void)
{
    sensor_len = fc_config_get(FC_STR("sensor"), sensor, sizeof(sensor));
    if (sensor_len == FC_ENOENT) {
        const char name[] = "die_temp";
        for (unsigned i = 0; i < sizeof(name) - 1; i++) sensor[i] = name[i];
        sensor_len = sizeof(name) - 1;
    }
    if (sensor_len <= 0) return FC_EINVAL;
    int ms = fc_config_int(FC_STR("sample_ms"), 60000);
    timer = fc_timer_start(ms < 1000 ? 1000 : ms, 1);
    if (timer < 0) return timer;
    ack = fc_mqtt_subscribe(FC_MQTT_DEVICE, FC_STR("ack"), 1);
    sample();
    return 0;
}

void on_event(int32_t type, int32_t handle, int32_t value)
{
    if (type == FC_EV_TIMER && handle == timer) {
        sample();
    } else if (type == FC_EV_NET) {
        online = value;
        publish_head();
    } else if (type == FC_EV_MQTT && handle == ack) {
        uint32_t id;
        if (fc_event_data(&id, sizeof(id)) == sizeof(id) && fc_queue_ack(FC_STR("samples"), id) == 0) {
            publish_head();
        }
    }
}
