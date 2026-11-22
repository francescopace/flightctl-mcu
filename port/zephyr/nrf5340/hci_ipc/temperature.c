#include <zephyr/device.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/ipc/ipc_service.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>

#include "../temperature.h"

static const struct device *const sensor = DEVICE_DT_GET(DT_NODELABEL(temp));
static struct ipc_ept endpoint;
static void send_sample(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(sample_work, send_sample);

static void send_sample(struct k_work *work)
{
    struct sensor_value sample;
    if (sensor_sample_fetch_chan(sensor, SENSOR_CHAN_DIE_TEMP) == 0 &&
        sensor_channel_get(sensor, SENSOR_CHAN_DIE_TEMP, &sample) == 0) {
        uint8_t message[4];
        sys_put_le32((uint32_t)sensor_value_to_milli(&sample), message);
        ipc_service_send(&endpoint, message, sizeof(message));
    }
    k_work_schedule(&sample_work, K_MSEC(FC_TEMP_PERIOD_MS));
}

static void bound(void *priv)
{
    k_work_reschedule(&sample_work, K_NO_WAIT);
}

static int temperature_init(void)
{
    static const struct ipc_ept_cfg config = {.name = FC_TEMP_ENDPOINT, .cb.bound = bound};
    const struct device *ipc = DEVICE_DT_GET(DT_CHOSEN(zephyr_bt_hci_ipc));
    if (!device_is_ready(sensor)) {
        return -ENODEV;
    }
    int rc = ipc_service_open_instance(ipc);
    if (rc && rc != -EALREADY) {
        return rc;
    }
    return ipc_service_register_endpoint(ipc, &endpoint, &config);
}
SYS_INIT(temperature_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
