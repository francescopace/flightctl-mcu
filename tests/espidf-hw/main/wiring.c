/* App-facing ESP-IDF drivers, with the other end of each wire in esp-emu. */
#include <stdio.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hw.h"

static void fail(const char *reason)
{
    printf("WIRING FAIL: %s\n", reason);
    abort();
}

#define CHECK(expr) do { if (!(expr)) fail(#expr); } while (0)

static const fc_hw_t *hw;

/* The fixture links the app hardware without the agent's internal die sensor. */
int fc_port_temperature(float *celsius)
{
    return FC_HW_ENOTSUP;
}

static uint8_t sht31_crc(const uint8_t *word)
{
    uint8_t crc = 0xff;
    for (int i = 0; i < 2; i++) {
        crc ^= word[i];
        for (int bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x31 : crc << 1;
        }
    }
    return crc;
}

static void check_i2c(void)
{
    fc_hw_bus_t pins = {.sda = 8, .scl = 9, .hz = 100000};
    int bus = hw->i2c_open(&pins);
    CHECK(bus >= 0);
    const uint8_t measure[] = {0x24, 0x00}; /* SHT31 high-repeatability single shot */
    uint8_t data[6];
    CHECK(hw->i2c_transfer(bus, 0x44, measure, sizeof(measure), NULL, 0, 500) == 0);
    vTaskDelay(pdMS_TO_TICKS(20)); /* Allow the sensor to finish the conversion. */
    CHECK(hw->i2c_transfer(bus, 0x44, NULL, 0, data, sizeof(data), 500) == 0);
    CHECK(sht31_crc(data) == data[2] && sht31_crc(data + 3) == data[5]);
    CHECK((data[0] * 256 + data[1]) == 25278); /* 22.5 C */
    CHECK((data[3] * 256 + data[4]) == 29491); /* 45 %RH */
    hw->i2c_close(bus);
    puts("WIRING I2C OK");
}

static void check_spi(void)
{
    fc_hw_bus_t pins = {.sck = 12, .mosi = 11, .miso = 13, .cs = 10, .hz = 1000000};
    int dev = hw->spi_open(&pins);
    CHECK(dev >= 0);
    uint8_t data[2];
    CHECK(hw->spi_transfer(dev, NULL, data, sizeof(data), 0, 500) == 0);
    if (data[0] != 0x0c || data[1] != 0x80) { /* TMP123 at 25 C */
        printf("TMP123 returned 0x%02x%02x, expected 0x0c80\n", data[0], data[1]);
        fail("TMP123 sample");
    }
    hw->spi_close(dev);
    puts("WIRING SPI OK");
}

static void uart_ready(void *sem, int n)
{
    if (n > 0) {
        xSemaphoreGive(sem);
    }
}

static void check_uart(void)
{
    fc_hw_bus_t pins = {.tx = 17, .rx = 18, .de = -1};
    SemaphoreHandle_t ready = xSemaphoreCreateBinary();
    CHECK(ready);
    int port = hw->uart_open(&pins, 9600, FC_HW_UART_8N1, uart_ready, ready);
    CHECK(port >= 0);
    const uint8_t query[] = {0xff, 1, 0x86, 0, 0, 0, 0, 0, 0x79}; /* MH-Z19B read CO2 */
    uint8_t data[9];
    CHECK(hw->uart_write(port, query, sizeof(query), 500) == sizeof(query));
    CHECK(xSemaphoreTake(ready, portMAX_DELAY) == pdTRUE);
    size_t received = 0;
    while (received < sizeof(data)) {
        int n = hw->uart_read(port, data + received, sizeof(data) - received, 500);
        CHECK(n > 0);
        received += n;
    }
    uint8_t sum = 0;
    for (size_t i = 1; i < sizeof(data); ++i) {
        sum += data[i];
    }
    CHECK(data[0] == 0xff && data[1] == 0x86 && data[2] * 256 + data[3] == 400 && sum == 0);
    hw->uart_close(port);
    vSemaphoreDelete(ready);
    puts("WIRING UART OK");
}

static void gpio_edge(void *sem)
{
    xSemaphoreGive(sem);
}

static void check_gpio_pwm(void)
{
    SemaphoreHandle_t edges = xSemaphoreCreateCounting(4, 0);
    CHECK(edges);
    CHECK(hw->gpio_setup(5, FC_HW_GPIO_IN_PULLUP) == 0);
    CHECK(hw->gpio_irq(5, 3, gpio_edge, edges) == 0);
    int pwm = hw->pwm_open(6, 50);
    CHECK(pwm >= 0);
    /* The host checks each pulse on the servo before sending the next edge. */
    CHECK(hw->pwm_duty(pwm, 3277) == 0);
    puts("WIRING press");
    CHECK(xSemaphoreTake(edges, portMAX_DELAY) == pdTRUE && hw->gpio_read(5) == 0);
    CHECK(hw->pwm_duty(pwm, 6554) == 0);
    puts("WIRING release");
    CHECK(xSemaphoreTake(edges, portMAX_DELAY) == pdTRUE && hw->gpio_read(5) == 1);
    CHECK(hw->gpio_irq(5, 0, NULL, NULL) == 0);
    hw->gpio_release(5);
    hw->pwm_close(pwm);
    vSemaphoreDelete(edges);
}

void app_main(void)
{
    hw = hw_espidf();
    check_i2c();
    check_spi();
    check_uart();
    check_gpio_pwm();
    puts("WIRING PASS");
}
