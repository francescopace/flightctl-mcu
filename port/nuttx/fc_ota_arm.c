#include "fc_ota_arm.h"
#ifdef FC_ARM_PLATFORM
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "cJSON.h"
#include "fc_port.h"

/* MCUboot move needs one spare erase sector as well as swap status. The
 * scratch layout on the F767 needs only its trailer in the image slot. */
#define IMAGE_LIMIT (FC_SLOT_SIZE - 0x4000u)
#define TRAILER_MAGIC_OFF (FC_SLOT_SIZE - 16u)
#define IMAGE_OK_OFF (TRAILER_MAGIC_OFF - 8u)
#define COPY_DONE_OFF (IMAGE_OK_OFF - 8u)

static const uint8_t boot_magic[16] = {
    0x77,0xc2,0x95,0xf3,0x60,0xd2,0xef,0x7f,0x35,0x52,0x50,0x0f,0x2c,0xb6,0x79,0x80
};
static pthread_mutex_t update_lock = PTHREAD_MUTEX_INITIALIZER;

typedef struct {
    size_t size, written;
    uint8_t page[FC_PAGE_SIZE];
} arm_update_t;

static uint16_t le16(const uint8_t *p)
{
    return p[0] | (uint16_t)p[1] << 8;
}

static uint32_t le32(const uint8_t *p)
{
    return le16(p) | (uint32_t)le16(p + 2) << 16;
}

const char *fc_port_fw_platform(void) { return FC_ARM_PLATFORM; }

int fc_port_fw_read(size_t off, void *buf, size_t len)
{
    if (off > IMAGE_LIMIT || len > IMAGE_LIMIT - off) return -1;
    memcpy(buf, (const void *)(FC_PRIMARY + off), len);
    return 0;
}

void fc_nuttx_fw_info(cJSON *obj)
{
    const uint8_t *h = (const uint8_t *)FC_PRIMARY;
    if (le32(h) == 0x96f3b83d && le16(h + 8) == FC_HEADER_SIZE) {
        char version[48];
        snprintf(version, sizeof(version), "%u.%u.%u+%lu", h[20], h[21],
                 le16(h + 22), (unsigned long)le32(h + 24));
        cJSON_AddStringToObject(obj, "firmwareVersion", version);
    }
}

int fc_port_ota_pending_verify(void)
{
    const uint8_t *base = (const uint8_t *)FC_PRIMARY;
    return base[COPY_DONE_OFF] == 1 && base[IMAGE_OK_OFF] == 0xff &&
           !memcmp(base + TRAILER_MAGIC_OFF, boot_magic, sizeof(boot_magic));
}

int fc_port_ota_begin(size_t size, void **handle)
{
    if (!handle || size < FC_HEADER_SIZE || size > IMAGE_LIMIT || pthread_mutex_trylock(&update_lock)) return -1;
    arm_update_t *u = calloc(1, sizeof(*u));
    uint8_t magic[16];
    if (!u || fc_port_ota_pending_verify() ||
        fc_arm_secondary_read(TRAILER_MAGIC_OFF, magic, sizeof(magic)) ||
        !memcmp(magic, boot_magic, sizeof(magic)) ||
        fc_arm_secondary_erase(FC_SLOT_SIZE - FC_ERASE_SIZE)) {
        free(u);
        pthread_mutex_unlock(&update_lock);
        return -1;
    }
    u->size = size;
    *handle = u;
    return 0;
}

int fc_port_ota_write(void *handle, const void *data, size_t len)
{
    arm_update_t *u = handle;
    const uint8_t *src = data;
    if (len > u->size - u->written) return -1;
    while (len) {
        size_t pos = u->written % FC_PAGE_SIZE;
        if (!pos) {
            if (u->written % FC_ERASE_SIZE == 0 && u->written != FC_SLOT_SIZE - FC_ERASE_SIZE &&
                fc_arm_secondary_erase(u->written)) return -1;
            memset(u->page, 0xff, sizeof(u->page));
        }
        size_t n = FC_PAGE_SIZE - pos;
        if (n > len) n = len;
        memcpy(u->page + pos, src, n);
        u->written += n;
        src += n;
        len -= n;
        if ((pos + n == FC_PAGE_SIZE || u->written == u->size) &&
            fc_arm_secondary_program(u->written - n - pos, u->page)) return -1;
    }
    return 0;
}

/* sign-arm.sh emits a protected security counter followed by the board
 * name. MCUboot authenticates both, and the complete image, at trial boot. */
static int candidate(size_t size)
{
    uint8_t hdr[32], tlv[16 + sizeof(FC_ARM_PLATFORM) - 1], info[4];
    if (fc_arm_secondary_read(0, hdr, sizeof(hdr)) || le32(hdr) != 0x96f3b83d ||
        le16(hdr + 8) != FC_HEADER_SIZE || le16(hdr + 10) != sizeof(tlv) ||
        le32(hdr + 16) || !le32(hdr + 12) || le32(hdr + 12) > size - FC_HEADER_SIZE) return -1;
    size_t off = FC_HEADER_SIZE + le32(hdr + 12);
    if (size - off < sizeof(tlv) + sizeof(info) || fc_arm_secondary_read(off, tlv, sizeof(tlv)) ||
        le16(tlv) != 0x6908 || le16(tlv + 2) != sizeof(tlv) || le16(tlv + 4) != 0x50 ||
        le16(tlv + 6) != 4 || le16(tlv + 12) != 0xa0 ||
        le16(tlv + 14) != sizeof(FC_ARM_PLATFORM) - 1 ||
        memcmp(tlv + 16, FC_ARM_PLATFORM, sizeof(FC_ARM_PLATFORM) - 1)) return FC_OTA_WRONG_VARIANT;
    off += sizeof(tlv);
    return fc_arm_secondary_read(off, info, sizeof(info)) || le16(info) != 0x6907 ||
           le16(info + 2) <= sizeof(info) || le16(info + 2) != size - off ? -1 : 0;
}

int fc_port_ota_end(void *handle, int commit)
{
    arm_update_t *u = handle;
    int rc = -1;
    if (commit && u->written == u->size && (rc = candidate(u->size)) == 0) {
        /* Only magic is written: image_ok stays erased for a reversible trial. */
        memset(u->page, 0xff, sizeof(u->page));
        memcpy(u->page + FC_PAGE_SIZE - sizeof(boot_magic), boot_magic, sizeof(boot_magic));
        rc = fc_arm_secondary_program(FC_SLOT_SIZE - FC_PAGE_SIZE, u->page);
    }
    free(u);
    pthread_mutex_unlock(&update_lock);
    return rc;
}

void fc_nuttx_cancel_trial(void);
void fc_port_ota_confirm(void)
{
    if (!fc_port_ota_pending_verify() || fc_arm_confirm_flag(IMAGE_OK_OFF) || fc_port_ota_pending_verify()) {
        fc_port_log(FC_LOG_ERROR, "could not confirm trial firmware");
        fc_port_reboot();
        return;
    }
    fc_nuttx_cancel_trial();
}
void fc_port_ota_rollback(void) { fc_port_reboot(); }
#endif
