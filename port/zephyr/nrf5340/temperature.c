/* Zephyr sensor backed by the nRF5340 network core's TEMP peripheral. */
#define DT_DRV_COMPAT flightctl_nrf5340_die_temp
#include <zephyr/drivers/sensor.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#include "temperature.h"

static struct ipc_ept endpoint;
static struct k_spinlock lock;
static int64_t received_at = -1;
static int32_t latest, fetched;
static sensor_trigger_handler_t handler;
static const struct sensor_trigger *trigger;
static int32_t lower, upper;
static int zone;

static void notify(struct k_work *work)
{
    k_spinlock_key_t key = k_spin_lock(&lock);
    sensor_trigger_handler_t callback = handler;
    const struct sensor_trigger *event = trigger;
    k_spin_unlock(&lock, key);
    if (callback) {
        callback(DEVICE_DT_INST_GET(0), event);
    }
}
static K_WORK_DEFINE(notify_work, notify);

static void received(const void *data, size_t len, void *priv)
{
    if (len == sizeof(int32_t)) {
        k_spinlock_key_t key = k_spin_lock(&lock);
        latest = (int32_t)sys_get_le32(data);
        received_at = k_uptime_get();
        if (handler) {
            int next = latest <= lower ? -1 : latest >= upper ? 1 : 0;
            if (trigger->type == SENSOR_TRIG_DATA_READY || next != zone) {
                k_work_submit(&notify_work);
            }
            zone = next;
        }
        k_spin_unlock(&lock, key);
    }
}

static int set_attribute(const struct device *dev, enum sensor_channel chan,
                         enum sensor_attribute attr, const struct sensor_value *value)
{
    if (chan != SENSOR_CHAN_DIE_TEMP ||
        (attr != SENSOR_ATTR_LOWER_THRESH && attr != SENSOR_ATTR_UPPER_THRESH)) {
        return -ENOTSUP;
    }
    int64_t mc = sensor_value_to_milli(value);
    if (mc < INT32_MIN || mc > INT32_MAX) {
        return -EINVAL;
    }
    k_spinlock_key_t key = k_spin_lock(&lock);
    if (attr == SENSOR_ATTR_LOWER_THRESH) {
        lower = (int32_t)mc;
    } else {
        upper = (int32_t)mc;
    }
    k_spin_unlock(&lock, key);
    return 0;
}

static int set_trigger(const struct device *dev, const struct sensor_trigger *event,
                       sensor_trigger_handler_t callback)
{
    if (event->chan != SENSOR_CHAN_DIE_TEMP ||
        (event->type != SENSOR_TRIG_DATA_READY && event->type != SENSOR_TRIG_THRESHOLD)) {
        return -ENOTSUP;
    }
    k_spinlock_key_t key = k_spin_lock(&lock);
    trigger = callback ? event : NULL;
    handler = callback;
    zone = 0;
    k_spin_unlock(&lock, key);
    if (!callback) {
        struct k_work_sync sync;
        k_work_cancel_sync(&notify_work, &sync);
    }
    return 0;
}

static int fetch(const struct device *dev, enum sensor_channel chan)
{
    if (chan != SENSOR_CHAN_ALL && chan != SENSOR_CHAN_DIE_TEMP) {
        return -ENOTSUP;
    }
    k_spinlock_key_t key = k_spin_lock(&lock);
    bool fresh = received_at >= 0 && k_uptime_get() - received_at <= 3 * FC_TEMP_PERIOD_MS;
    fetched = latest;
    k_spin_unlock(&lock, key);
    return fresh ? 0 : -ENODATA;
}

static int get(const struct device *dev, enum sensor_channel chan, struct sensor_value *value)
{
    return chan == SENSOR_CHAN_DIE_TEMP ? sensor_value_from_milli(value, fetched) : -ENOTSUP;
}

static int init(const struct device *dev)
{
    static const struct ipc_ept_cfg config = {.name = FC_TEMP_ENDPOINT, .cb.received = received};
    const struct device *ipc = DEVICE_DT_GET(DT_INST_PHANDLE(0, ipc));
    int rc = ipc_service_open_instance(ipc);
    if (rc && rc != -EALREADY) {
        return rc;
    }
    return ipc_service_register_endpoint(ipc, &endpoint, &config);
}

static DEVICE_API(sensor, api) = {
    .sample_fetch = fetch,
    .channel_get = get,
    .attr_set = set_attribute,
    .trigger_set = set_trigger,
};
SENSOR_DEVICE_DT_INST_DEFINE(0, init, NULL, NULL, NULL, POST_KERNEL, CONFIG_SENSOR_INIT_PRIORITY, &api);
