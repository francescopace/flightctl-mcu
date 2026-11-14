/* Imports every function of the 1.0 interface (docs/drafts/fc_app.h), so it
 * loads only if the device links them all. */
#include "fc_app.h"

FC_APP_ABI()

static volatile int32_t never;

void tick(void)
{
    if (!never) {
        FC_STATE("all linked");
        return;
    }
    char b[8];
    int32_t v;
    fc_log(FC_LOG_INFO, b, 1);
    fc_info(b, 1, b, 8);
    fc_time_ms();
    fc_time_unix_ms();
    fc_sleep_ms(1);
    fc_random(b, 8);
    fc_timer_start(1, 0);
    fc_config_get(b, 1, b, 8);
    fc_kv_get(b, 1, b, 8);
    fc_kv_set(b, 1, b, 8);
    fc_kv_del(b, 1);
    fc_close(0);
    fc_gpio_open(b, 1, 0);
    fc_gpio_read(0);
    fc_gpio_write(0, 0);
    fc_gpio_irq(0, 0, 0);
    fc_led_open(b, 1);
    fc_led_rgb(0, 0, 0, 0);
    fc_i2c_open(b, 1);
    fc_i2c_write(0, 0, b, 1);
    fc_i2c_read(0, 0, b, 1);
    fc_i2c_write_read(0, 0, b, 1, b, 1);
    fc_spi_open(b, 1);
    fc_spi_transfer(0, b, b, 1, 0);
    fc_uart_open(b, 1, 0, 0);
    fc_uart_write(0, b, 1);
    fc_uart_read(0, b, 1, 0);
    fc_adc_open(b, 1);
    fc_adc_read_mv(0);
    fc_pwm_open(b, 1, 0);
    fc_pwm_duty(0, 0);
    fc_sensor_read(b, 1, &v);
    fc_metric(b, 1, 0, 1.0);
    fc_mqtt_publish(b, 1, b, 1, 0, 0);
    fc_mqtt_subscribe(0, b, 1, 0);
    fc_mqtt_topic(b, 8);
    fc_event_data(b, 8);
}
