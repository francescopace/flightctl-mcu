/* An emulated SPI device that sends back what it is sent. */
#define DT_DRV_COMPAT flightctl_test_spi_loopback

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/emul.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/drivers/spi_emul.h>

static int lb_io(const struct emul *target, const struct spi_config *config, const struct spi_buf_set *tx,
                 const struct spi_buf_set *rx)
{
    if (tx != NULL && rx != NULL && tx->count > 0 && rx->count > 0 && rx->buffers[0].buf != NULL) {
        size_t n = MIN(tx->buffers[0].len, rx->buffers[0].len);
        memcpy(rx->buffers[0].buf, tx->buffers[0].buf, n);
    }
    return 0;
}

static struct spi_emul_api lb_api = {.io = lb_io};

static int lb_emul_init(const struct emul *target, const struct device *parent) { return 0; }

DEVICE_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, 99, NULL);
EMUL_DT_INST_DEFINE(0, lb_emul_init, NULL, NULL, &lb_api, NULL);
