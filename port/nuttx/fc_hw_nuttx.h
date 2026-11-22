#pragma once

#include "fc_hw.h"

extern const fc_hw_t fc_nuttx_hardware;
int fc_nuttx_sensor(const char *name, int32_t *value);
int fc_nuttx_subscribe(int id, int mode, int32_t low, int32_t high, void (*cb)(void *), void *ctx);
void fc_nuttx_unsubscribe(int id);
void fc_nuttx_boot_reason(void);
int fc_nuttx_restart_was_crash(void);

int fc_nuttx_i2c_open(const fc_hw_bus_t *bus);
int fc_nuttx_i2c_transfer(int h, int addr, const uint8_t *w, size_t wl, uint8_t *r, size_t rl, int ms);
void fc_nuttx_i2c_close(int h);
int fc_nuttx_spi_open(const fc_hw_bus_t *bus);
int fc_nuttx_spi_transfer(int h, const uint8_t *tx, uint8_t *rx, size_t len, int keep, int ms);
void fc_nuttx_spi_release(int h);
void fc_nuttx_spi_close(int h);
int fc_nuttx_uart_open(const fc_hw_bus_t *bus, int baud, int format, void (*cb)(void *, int), void *ctx);
int fc_nuttx_uart_read(int h, uint8_t *buf, size_t len, int ms);
int fc_nuttx_uart_write(int h, const uint8_t *buf, size_t len, int ms);
void fc_nuttx_uart_close(int h);
