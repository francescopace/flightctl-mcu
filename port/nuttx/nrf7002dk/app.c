#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/drivers/rpmsgdev.h>
#include <nuttx/fs/fs.h>
#include <nuttx/mtd/mtd.h>
#include <nuttx/spi/spi.h>
#include <arch/board/board.h>
#include <errno.h>
#include <stdint.h>
#include <sys/mount.h>
#include "nrf53_gpio.h"
#include "nrf53_spi.h"

/* Leave MCUboot's secondary slot and the Zephyr filesystem intact. */
#define DATA_OFFSET 0x200000
#define DATA_SIZE   0x100000

void devurandom_register(void)
{
    rpmsgdev_register("netcore", "/dev/urandom", "/dev/urandom", 0);
}

void nrf53_spi4select(struct spi_dev_s *bus, uint32_t devid, bool selected)
{
    if (devid == SPIDEV_FLASH(0)) nrf53_gpio_write(BOARD_FLASH_CS_PIN, !selected);
}

uint8_t nrf53_spi4status(struct spi_dev_s *bus, uint32_t devid)
{
    return devid == SPIDEV_FLASH(0) ? SPI_STATUS_PRESENT : 0;
}

static struct mtd_dev_s *ota_flash;

struct mtd_dev_s *fc_nrf_ota_flash(void)
{
    return ota_flash;
}

int fc_nrf_mount_storage(void)
{
    int rc = rpmsgdev_register("netcore", "/dev/die_temp", "/dev/die_temp", 0);
    if (rc < 0) return rc;
    nrf53_gpio_config(BOARD_FLASH_CS_PIN);
    struct spi_dev_s *bus = nrf53_spibus_initialize(4);
    if (!bus) return -ENODEV;
    SPI_LOCK(bus, true);
    SPI_SETMODE(bus, SPIDEV_MODE0);
    SPI_SETFREQUENCY(bus, 8000000);
    SPI_SELECT(bus, SPIDEV_FLASH(0), true);
    SPI_SEND(bus, 0xab); /* Release from deep power-down. */
    SPI_SELECT(bus, SPIDEV_FLASH(0), false);
    up_udelay(100);
    SPI_LOCK(bus, false);
    struct mtd_dev_s *flash = mx25l_initialize_spi(bus);
    if (!flash) return -ENODEV;
    struct mtd_geometry_s geo = {0};
    if (MTD_IOCTL(flash, MTDIOC_GEOMETRY, (unsigned long)&geo) < 0 ||
        geo.blocksize != 256 || geo.erasesize != 4096 || geo.neraseblocks != 2048) return -EINVAL;
    ota_flash = flash;
    struct mtd_dev_s *part = mtd_partition(flash, DATA_OFFSET / geo.blocksize,
                                           DATA_SIZE / geo.blocksize);
    if (!part) return -ENOMEM;
    if ((rc = register_mtddriver("/dev/fcdata", part, 0600, NULL)) < 0) return rc;
    if (mount("/dev/fcdata", "/data", "littlefs", 0, NULL) == 0) return 0;
    uint8_t data[256];
    for (size_t page = 0; page < DATA_SIZE / sizeof(data); page++) {
        if (MTD_BREAD(part, page, 1, data) != 1) return -EIO;
        for (size_t i = 0; i < sizeof(data); i++) {
            if (data[i] != 0xff) return -EINVAL;
        }
    }
    return mount("/dev/fcdata", "/data", "littlefs", 0, "forceformat");
}
