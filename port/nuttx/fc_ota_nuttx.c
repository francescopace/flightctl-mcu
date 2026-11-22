#include <nuttx/config.h>

#include "fc_ota_arm.h"
#if defined(FC_ARM_PLATFORM) || defined(CONFIG_ESP32S3_APP_FORMAT_MCUBOOT)
#include <nuttx/wqueue.h>
#include "fc_port.h"
static struct work_s trial_deadline;

static void expire_trial(void *arg)
{
    fc_port_reboot();
}

/* Wi-Fi and NTP run before the core's management confirmation timer. */
void fc_nuttx_start_trial(void)
{
    if (fc_port_ota_pending_verify() && work_available(&trial_deadline) &&
        work_queue(LPWORK, &trial_deadline, expire_trial, NULL, SEC2TICK(180)) < 0) {
        fc_port_reboot();
    }
}

void fc_nuttx_cancel_trial(void)
{
    work_cancel_sync(LPWORK, &trial_deadline);
}
#endif

#ifdef CONFIG_ESP32S3_APP_FORMAT_MCUBOOT
#include <bootutil/bootutil_public.h>
#include <bootutil/image.h>
#include <nuttx/mtd/mtd.h>
#include <nuttx/wqueue.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp32s3_spiflash_mtd.h"
#include "fc_port.h"

#define PRIMARY CONFIG_ESP32S3_OTA_PRIMARY_SLOT_OFFSET
#define SECONDARY CONFIG_ESP32S3_OTA_SECONDARY_SLOT_OFFSET
#define SLOT_SIZE CONFIG_ESP32S3_OTA_SLOT_SIZE
/* Scratch swap records up to 512 sectors, three writes per sector.
 * Reserve four erase sectors, including the flags and magic at the end. */
#define IMAGE_LIMIT (SLOT_SIZE - 0x4000)
#define SECTOR 4096

static pthread_mutex_t update_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    size_t size;
    size_t written;
} ota_t;

static int read_flash(size_t off, void *buf, size_t len)
{
    struct mtd_dev_s *flash = esp32s3_spiflash_mtd();
    return MTD_READ(flash, off, len, buf) == (ssize_t)len ? 0 : -1;
}

const char *fc_port_fw_platform(void)
{
#if defined(CONFIG_ARCH_CHIP_ESP32S3WROOM1N16R8)
    return "nuttx-esp32s3-n16r8";
#elif defined(CONFIG_ARCH_CHIP_ESP32S3WROOM1N8R8)
    return "nuttx-esp32s3-n8r8";
#elif defined(CONFIG_ARCH_CHIP_ESP32S3WROOM1N4)
    return "nuttx-esp32s3-n4r2";
#else
#error Unsupported MCUboot slot layout
#endif
}

static int header_at(size_t off, struct image_header *hdr)
{
    return read_flash(off, hdr, sizeof(*hdr)) == 0 && hdr->ih_magic == IMAGE_MAGIC &&
           hdr->ih_hdr_size == CONFIG_ESP32S3_APP_MCUBOOT_HEADER_SIZE && hdr->ih_img_size != 0 &&
           hdr->ih_img_size <= IMAGE_LIMIT - hdr->ih_hdr_size ? 0 : -1;
}

void fc_nuttx_fw_info(cJSON *obj)
{
    struct image_header hdr;
    if (header_at(PRIMARY, &hdr) == 0) {
        char version[48];
        snprintf(version, sizeof(version), "%u.%u.%u+%lu", hdr.ih_ver.iv_major,
                 hdr.ih_ver.iv_minor, hdr.ih_ver.iv_revision, (unsigned long)hdr.ih_ver.iv_build_num);
        cJSON_AddStringToObject(obj, "firmwareVersion", version);
    }
}

int fc_port_ota_begin(size_t size, void **handle)
{
    if (size < sizeof(struct image_header) || size > IMAGE_LIMIT || pthread_mutex_trylock(&update_lock)) {
        return -1;
    }
    ota_t *o = calloc(1, sizeof(*o));
    struct mtd_dev_s *flash = esp32s3_spiflash_mtd();
    /* An unconfirmed image still needs the secondary slot to revert.
     * Clear the trailer before writing any candidate bytes. */
    if (!o || boot_swap_type_multi(0) != BOOT_SWAP_TYPE_NONE ||
        MTD_ERASE(flash, (SECONDARY + IMAGE_LIMIT) / SECTOR, (SLOT_SIZE - IMAGE_LIMIT) / SECTOR) !=
            (SLOT_SIZE - IMAGE_LIMIT) / SECTOR) {
        free(o);
        pthread_mutex_unlock(&update_lock);
        return -1;
    }
    o->size = size;
    *handle = o;
    return 0;
}

int fc_port_ota_write(void *handle, const void *data, size_t len)
{
    ota_t *o = handle;
    const uint8_t *src = data;
    if (len > o->size - o->written) {
        return -1;
    }
    struct mtd_dev_s *flash = esp32s3_spiflash_mtd();
    while (len) {
        size_t addr = SECONDARY + o->written;
        size_t in_sector = addr % SECTOR;
        if (!in_sector && MTD_ERASE(flash, addr / SECTOR, 1) != 1) {
            return -1;
        }
        uint8_t check[512];
        size_t n = SECTOR - in_sector;
        if (n > len) n = len;
        if (n > sizeof(check)) n = sizeof(check);
        if (MTD_WRITE(flash, addr, n, src) != (ssize_t)n || read_flash(addr, check, n) ||
            memcmp(src, check, n) != 0) {
            return -1;
        }
        o->written += n;
        src += n;
        len -= n;
    }
    return 0;
}

/* The build adds one protected TLV naming the board. Check it even for a
 * signed single-platform manifest, which has no OCI platform selector. */
static int candidate_matches(const struct image_header *hdr, size_t size)
{
    size_t off = hdr->ih_hdr_size + hdr->ih_img_size;
    const char *platform = fc_port_fw_platform();
    size_t len = strlen(platform);
    struct image_tlv_info block;
    struct image_tlv entry;
    char value[32];
    if (len > sizeof(value) || hdr->ih_flags != 0 ||
        hdr->ih_protect_tlv_size != sizeof(block) + sizeof(entry) + len ||
        off + hdr->ih_protect_tlv_size + sizeof(block) > size ||
        read_flash(SECONDARY + off, &block, sizeof(block)) || block.it_magic != IMAGE_TLV_PROT_INFO_MAGIC ||
        block.it_tlv_tot != hdr->ih_protect_tlv_size ||
        read_flash(SECONDARY + off + sizeof(block), &entry, sizeof(entry)) ||
        entry.it_type != 0xa0 || entry.it_len != len ||
        read_flash(SECONDARY + off + sizeof(block) + sizeof(entry), value, len) ||
        memcmp(platform, value, len)) {
        return 0;
    }
    off += hdr->ih_protect_tlv_size;
    return read_flash(SECONDARY + off, &block, sizeof(block)) == 0 && block.it_magic == IMAGE_TLV_INFO_MAGIC &&
           block.it_tlv_tot >= sizeof(block) && off + block.it_tlv_tot == size;
}

int fc_port_ota_end(void *handle, int commit)
{
    ota_t *o = handle;
    int rc = -1;
    struct image_header hdr;
    if (commit && o->written == o->size && header_at(SECONDARY, &hdr) == 0) {
        if (!candidate_matches(&hdr, o->size)) {
            rc = FC_OTA_WRONG_VARIANT;
        } else if (boot_set_pending_multi(0, 0) == 0 && boot_swap_type_multi(0) == BOOT_SWAP_TYPE_TEST) {
            rc = 0;
        }
    }
    free(o);
    pthread_mutex_unlock(&update_lock);
    return rc;
}

int fc_port_fw_read(size_t off, void *buf, size_t len)
{
    return off <= IMAGE_LIMIT && len <= IMAGE_LIMIT - off ? read_flash(PRIMARY + off, buf, len) : -1;
}

int fc_port_ota_pending_verify(void)
{
    return boot_swap_type_multi(0) == BOOT_SWAP_TYPE_REVERT;
}

void fc_port_ota_confirm(void)
{
    fc_nuttx_cancel_trial();
    if (boot_set_confirmed_multi(0) || boot_swap_type_multi(0) != BOOT_SWAP_TYPE_NONE) {
        fc_port_log(FC_LOG_ERROR, "OTA confirmation failed; rebooting to the previous image");
        fc_port_reboot();
    }
}

void fc_port_ota_rollback(void)
{
    fc_port_reboot();
}
#endif
