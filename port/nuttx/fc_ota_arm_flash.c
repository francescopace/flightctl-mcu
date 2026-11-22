#include "fc_ota_arm.h"
#ifdef FC_ARM_PLATFORM
#include <errno.h>
#include <nuttx/mtd/mtd.h>
#include <nuttx/progmem.h>
#include <string.h>
#if defined(CONFIG_ARCH_BOARD_RASPBERRYPI_PICO_2_W)
#include "rp23xx_flash_mtd.h"
#define SECONDARY_OFFSET (0x190000u - CONFIG_RP23XX_FLASH_MTD_OFFSET)
_Static_assert(CONFIG_RP23XX_FLASH_MTD_OFFSET == 0x18f000 &&
               CONFIG_RP23XX_FLASH_MTD_SIZE == 0x271000, "unexpected Pico flash layout");
#define secondary_mtd rp23xx_flash_mtd_get
#elif defined(CONFIG_FLIGHTCTL_NRF7002DK)
#define SECONDARY_OFFSET 0u
struct mtd_dev_s *fc_nrf_ota_flash(void);
#define secondary_mtd fc_nrf_ota_flash
#endif

int fc_arm_secondary_read(size_t off, void *dst, size_t len)
{
    if (off > FC_SLOT_SIZE || len > FC_SLOT_SIZE - off) return -1;
#ifdef CONFIG_ARCH_BOARD_NUCLEO_F767ZI
    memcpy(dst, (const void *)(FC_PRIMARY + FC_SLOT_SIZE + off), len);
    return 0;
#else
    struct mtd_dev_s *dev = secondary_mtd();
    return dev && MTD_READ(dev, SECONDARY_OFFSET + off, len, dst) == (ssize_t)len ? 0 : -1;
#endif
}

int fc_arm_secondary_erase(size_t off)
{
    if (off >= FC_SLOT_SIZE || off % FC_ERASE_SIZE) return -1;
#ifdef CONFIG_ARCH_BOARD_NUCLEO_F767ZI
    size_t addr = FC_PRIMARY + FC_SLOT_SIZE + off;
    ssize_t page = up_progmem_getpage(addr);
    return page >= 0 && up_progmem_getaddress(page) == addr &&
           up_progmem_pagesize(page) == FC_ERASE_SIZE &&
           up_progmem_eraseblock(page) == FC_ERASE_SIZE ? 0 : -1;
#else
    struct mtd_dev_s *dev = secondary_mtd();
    return dev && MTD_ERASE(dev, (SECONDARY_OFFSET + off) / FC_ERASE_SIZE, 1) == 1 ? 0 : -1;
#endif
}

int fc_arm_secondary_program(size_t off, const uint8_t page[FC_PAGE_SIZE])
{
    if (off >= FC_SLOT_SIZE || off % FC_PAGE_SIZE) return -1;
#ifdef CONFIG_ARCH_BOARD_NUCLEO_F767ZI
    ssize_t n = up_progmem_write(FC_PRIMARY + FC_SLOT_SIZE + off, page, FC_PAGE_SIZE);
    if (n != FC_PAGE_SIZE) return -1;
#else
    struct mtd_dev_s *dev = secondary_mtd();
    if (!dev || MTD_BWRITE(dev, (SECONDARY_OFFSET + off) / FC_PAGE_SIZE, 1, page) != 1) return -1;
#endif
    uint8_t check[FC_PAGE_SIZE];
    return fc_arm_secondary_read(off, check, sizeof(check)) || memcmp(page, check, sizeof(check)) ? -1 : 0;
}

int fc_arm_confirm_flag(size_t off)
{
    if (off != FC_SLOT_SIZE - 24) return -1;
    size_t addr = FC_PRIMARY + off;
#if defined(CONFIG_ARCH_BOARD_RASPBERRYPI_PICO_2_W)
    uint8_t page[FC_PAGE_SIZE];
    size_t base = addr & ~(FC_PAGE_SIZE - 1);
    memcpy(page, (const void *)base, sizeof(page));
    page[addr - base] = 1;
    struct mtd_dev_s *dev = secondary_mtd();
    return dev && MTD_BWRITE(dev, (base - 0x10000000u - CONFIG_RP23XX_FLASH_MTD_OFFSET) /
                             FC_PAGE_SIZE, 1, page) == 1 && *(volatile uint8_t *)addr == 1 ? 0 : -1;
#elif defined(CONFIG_FLIGHTCTL_NRF7002DK)
    const uint32_t flag = 0xffffff01;
    return up_progmem_write(addr, &flag, sizeof(flag)) == sizeof(flag) ? 0 : -1;
#else
    const uint8_t flag = 1;
    return up_progmem_write(addr, &flag, sizeof(flag)) == sizeof(flag) ? 0 : -1;
#endif
}
#endif
