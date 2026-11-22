/* Feed the application-core driver the wire format used by the network core. */
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/ipc/ipc_service_backend.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/ztest.h>

#include "fc_hw.h"

void fc_hw_zephyr_fill(fc_hw_t *hw);

static const struct ipc_ept_cfg *endpoint;

static int register_endpoint(const struct device *dev, void **token, const struct ipc_ept_cfg *cfg)
{
    endpoint = cfg;
    return 0;
}

static const struct ipc_service_backend backend = {.register_endpoint = register_endpoint};
DEVICE_DT_DEFINE(DT_NODELABEL(temp_ipc), NULL, NULL, NULL, NULL, POST_KERNEL,
                 CONFIG_IPC_SERVICE_REG_BACKEND_PRIORITY, &backend);

static void send_sample(int32_t mc)
{
    uint8_t data[4];
    sys_put_le32((uint32_t)mc, data);
    endpoint->cb.received(data, sizeof(data), endpoint->priv);
    k_work_queue_drain(&k_sys_work_q, false);
}

static void count_event(void *ctx)
{
    (*(int *)ctx)++;
}

ZTEST(fc_hw, test_nrf_temperature)
{
    fc_hw_t hw = {0};
    fc_hw_zephyr_fill(&hw);
    zassert_not_null(endpoint);
    zassert_equal(strcmp(endpoint->name, "fc-die-temp"), 0);
    int id = hw.dt_find(FC_HW_KIND_SENSOR, "core-temperature");
    int32_t mc = 12345;
    int events = 0;
    zassert_true(id >= 0);
    zassert_equal(hw.sensor_read("die_temp", &mc), FC_HW_EIO);
    zassert_equal(mc, 12345);

    int token = hw.sensor_subscribe(-1, FC_HW_SENSOR_READY, 0, 0, count_event, &events);
    zassert_equal(token, id);
    send_sample(-250);
    zassert_equal(events, 1);
    zassert_ok(hw.sensor_read("die_temp", &mc));
    zassert_equal(mc, -250);
    hw.sensor_unsubscribe(token);

    token = hw.sensor_subscribe(id, FC_HW_SENSOR_THRESHOLD, 250, 1250, count_event, &events);
    zassert_equal(token, id);
    send_sample(750);
    send_sample(1250);
    send_sample(1500);
    send_sample(1000);
    send_sample(250);
    zassert_equal(events, 4);
    zassert_ok(hw.sensor_read_dt(id, &mc));
    zassert_equal(mc, 250);
    hw.sensor_unsubscribe(token);
    send_sample(750);
    zassert_equal(events, 4);
    k_sleep(K_SECONDS(4));
    zassert_equal(hw.sensor_read("die_temp", &mc), FC_HW_EIO);
    zassert_equal(mc, 250);
}
