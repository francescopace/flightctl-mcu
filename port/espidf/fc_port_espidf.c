/*
 * ESP-IDF port: key/value and config files in a dedicated NVS partition,
 * system info from the SoC, logging through esp_log.
 */
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "soc/soc_caps.h"
#if SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#else
#include <math.h>
#endif
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_partition.h"
#include "esp_chip_info.h"
#include "esp_efuse.h"
#include "esp_flash_encrypt.h"
#include "esp_secure_boot.h"
#include "esp_heap_caps.h"
#include "esp_idf_version.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "fc_port.h"
#include "mbedtls/sha256.h"
#include "nvs.h"
#include "nvs_flash.h"

#define PART "nvs_store"
#define NS   "fc"

static const char *TAG = "fc";
static const char *hwkey_info(void);
static void secure_boot_keys_info(cJSON *o);
static void revoke_older_keys(void);
#if SOC_TEMP_SENSOR_SUPPORTED
static temperature_sensor_handle_t s_tsens;
/* The status thread, the alarm task and WASM apps all read the sensor. On
 * the S2 a read starts a conversion and busy-waits for it, without a lock in
 * the driver: two overlapping reads can leave one spinning forever. */
static pthread_mutex_t s_tsens_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

/* nvs_flash_init() encrypts only the default partition: with NVS encryption
 * the store gets the same keys (nvs_keys partition, made by nvs_flash_init()
 * on first boot), so the device key never sits in flash in clear. */
static esp_err_t store_init(void)
{
#if CONFIG_NVS_ENCRYPTION
    nvs_sec_cfg_t keys;
    esp_err_t err = nvs_flash_read_security_cfg_v2(nvs_flash_get_default_security_scheme(), &keys);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "NVS keys: %s", esp_err_to_name(err));
        return err;
    }
    return nvs_flash_secure_init_partition(PART, &keys);
#else
    return nvs_flash_init_partition(PART);
#endif
}

int fc_port_espidf_init(void)
{
    esp_err_t err = store_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* The device key, its certificate and the apps are in there: after
         * this the device enrolls again under a new name. Say so. */
        ESP_LOGE(TAG, "%s unusable (%s): erasing it, the device identity is lost", PART, esp_err_to_name(err));
        nvs_flash_erase_partition(PART);
        err = store_init();
    }
    /* A revocation cut short by a reset is finished on the next boot. */
    revoke_older_keys();
    cJSON *keys = cJSON_CreateObject();
    secure_boot_keys_info(keys);
    if (cJSON_GetObjectItem(keys, "secureBootKeys") != NULL) {
        ESP_LOGI(TAG, "Secure Boot keys usable: %s (* signs this app)",
                 cJSON_GetObjectItem(keys, "secureBootKeys")->valuestring);
    }
    cJSON_Delete(keys);
#if CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK
    ESP_LOGI(TAG, "security version: this image %u, the chip accepts %u and up",
             (unsigned)esp_app_get_description()->secure_version, (unsigned)esp_efuse_read_secure_version());
#endif
#if SOC_TEMP_SENSOR_SUPPORTED
    temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
    if (temperature_sensor_install(&cfg, &s_tsens) == ESP_OK) {
        temperature_sensor_enable(s_tsens);
    }
#endif
    return err == ESP_OK ? 0 : -1;
}

int fc_port_temperature(float *celsius)
{
#if SOC_TEMP_SENSOR_SUPPORTED
    if (!s_tsens) {
        return -1;
    }
    pthread_mutex_lock(&s_tsens_lock);
    esp_err_t err = temperature_sensor_get_celsius(s_tsens, celsius);
    pthread_mutex_unlock(&s_tsens_lock);
    return err == ESP_OK ? 0 : -1;
#else
    /* No usable internal sensor (classic ESP32): a simulated 34-46 C swing
     * over 10 minutes, so the alarm logic still has something to act on. */
    double t = (double)(esp_timer_get_time() / 1000000) / 600.0;
    *celsius = (float)(40.0 + 6.0 * sin(2 * M_PI * t));
    return 0;
#endif
}

/* NVS keys are limited to 15 characters. */
static void nvs_key(const char *key, char out[16])
{
    if (strlen(key) <= 15) {
        strcpy(out, key);
        return;
    }
    unsigned char h[32];
    mbedtls_sha256((const unsigned char *)key, strlen(key), h, 0);
    snprintf(out, 16, "h_%02x%02x%02x%02x%02x%02x", h[0], h[1], h[2], h[3], h[4], h[5]);
}

int fc_port_kv_get(const char *key, char **out, size_t *len)
{
    nvs_handle_t h;
    char k[16];
    nvs_key(key, k);
    if (nvs_open_from_partition(PART, NS, NVS_READONLY, &h) != ESP_OK) {
        return -1;
    }
    size_t n = 0;
    int rc = -1;
    if (nvs_get_blob(h, k, NULL, &n) == ESP_OK) {
        char *buf = malloc(n + 1);
        if (buf && nvs_get_blob(h, k, buf, &n) == ESP_OK) {
            buf[n] = '\0';
            *out = buf;
            *len = n;
            rc = 0;
        } else {
            free(buf);
        }
    }
    nvs_close(h);
    return rc;
}

int fc_port_kv_set(const char *key, const void *data, size_t len)
{
    nvs_handle_t h;
    char k[16];
    nvs_key(key, k);
    if (nvs_open_from_partition(PART, NS, NVS_READWRITE, &h) != ESP_OK) {
        return -1;
    }
    esp_err_t err = nvs_set_blob(h, k, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "kv_set %s: %s", key, esp_err_to_name(err));
    }
    return err == ESP_OK ? 0 : -1;
}

int fc_port_kv_del(const char *key)
{
    nvs_handle_t h;
    char k[16];
    nvs_key(key, k);
    if (nvs_open_from_partition(PART, NS, NVS_READWRITE, &h) != ESP_OK) {
        return -1;
    }
    esp_err_t err = nvs_erase_key(h, k);
    nvs_commit(h);
    nvs_close(h);
    return err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND ? 0 : -1;
}

void fc_port_sysinfo(fc_sysinfo_t *si)
{
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(si->architecture, sizeof(si->architecture), "xtensa-%s", CONFIG_IDF_TARGET);
#if CONFIG_IDF_TARGET_ARCH_RISCV
    snprintf(si->architecture, sizeof(si->architecture), "riscv32-%s", CONFIG_IDF_TARGET);
#endif
    snprintf(si->operating_system, sizeof(si->operating_system), "esp-idf");
    snprintf(si->os_version, sizeof(si->os_version), "%s", esp_get_idf_version());
    snprintf(si->hostname, sizeof(si->hostname), "%s-%02x%02x%02x", CONFIG_IDF_TARGET, mac[3], mac[4], mac[5]);
    snprintf(si->hw_id, sizeof(si->hw_id), "%02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    si->cpu_count = chip.cores;
    si->mem_total = heap_caps_get_total_size(MALLOC_CAP_DEFAULT);
    si->mem_free = heap_caps_get_free_size(MALLOC_CAP_DEFAULT);
    nvs_stats_t st;
    if (nvs_get_stats(PART, &st) == ESP_OK) {
        si->disk_total = (uint64_t)st.total_entries * 32;
        si->disk_free = (uint64_t)st.free_entries * 32;
    }
}

void fc_port_custom_info(void *obj)
{
    cJSON *o = obj;
    char buf[48];
    fc_sysinfo_t si = {0};
    fc_port_sysinfo(&si);
    cJSON_AddStringToObject(o, "mac", si.hw_id);
    cJSON_AddStringToObject(o, "firmwareVersion", esp_app_get_description()->version);
    snprintf(buf, sizeof(buf), "%lld", (long long)(esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(o, "uptimeSeconds", buf);
    snprintf(buf, sizeof(buf), "%u", (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_DEFAULT));
    cJSON_AddStringToObject(o, "memMinFreeBytes", buf);
    /* With PSRAM the totals above include it; TLS and WiFi care about
     * internal RAM, and about its largest free block. */
    snprintf(buf, sizeof(buf), "%u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddStringToObject(o, "memInternalFreeBytes", buf);
    snprintf(buf, sizeof(buf), "%u", (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddStringToObject(o, "memInternalLargestBlock", buf);
    static const char *const reasons[] = {
        [ESP_RST_UNKNOWN] = "unknown", [ESP_RST_POWERON] = "poweron", [ESP_RST_EXT] = "external",
        [ESP_RST_SW] = "software", [ESP_RST_PANIC] = "panic", [ESP_RST_INT_WDT] = "int_wdt",
        [ESP_RST_TASK_WDT] = "task_wdt", [ESP_RST_WDT] = "wdt", [ESP_RST_DEEPSLEEP] = "deepsleep",
        [ESP_RST_BROWNOUT] = "brownout", [ESP_RST_SDIO] = "sdio", [ESP_RST_USB] = "usb",
    };
    esp_reset_reason_t rr = esp_reset_reason();
    cJSON_AddStringToObject(o, "resetReason",
                            (unsigned)rr < sizeof(reasons) / sizeof(reasons[0]) && reasons[rr] ? reasons[rr] : "other");
    /* What the chip enforces (eFuses), not what the image was built for. */
    cJSON_AddStringToObject(o, "deviceKey", hwkey_info());
    cJSON_AddStringToObject(o, "secureBoot", esp_secure_boot_enabled() ? "true" : "false");
    secure_boot_keys_info(o);
#if CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK
    /* this image's security version / the lowest the chip accepts */
    snprintf(buf, sizeof(buf), "%u/%u", (unsigned)esp_app_get_description()->secure_version,
             (unsigned)esp_efuse_read_secure_version());
    cJSON_AddStringToObject(o, "secureVersion", buf);
#endif
    esp_flash_enc_mode_t fe = esp_get_flash_encryption_mode();
    cJSON_AddStringToObject(o, "flashEncryption", fe == ESP_FLASH_ENC_MODE_RELEASE       ? "release"
                                                   : fe == ESP_FLASH_ENC_MODE_DEVELOPMENT ? "development"
                                                                                          : "off");
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(buf, sizeof(buf), "%d", ap.rssi);
        cJSON_AddStringToObject(o, "wifiRssi", buf);
    }
    float t;
    if (fc_port_temperature(&t) == 0) {
        snprintf(buf, sizeof(buf), "%.1f", t);
        cJSON_AddStringToObject(o, "temperatureC", buf);
#if !SOC_TEMP_SENSOR_SUPPORTED
        cJSON_AddStringToObject(o, "temperatureSource", "simulated");
#endif
    }
}

int fc_port_write_file(const char *path, const void *data, size_t len)
{
    char key[300];
    snprintf(key, sizeof(key), "file:%s", path);
    return fc_port_kv_set(key, data, len);
}

const char *fc_port_fw_platform(void)
{
    return CONFIG_IDF_TARGET;
}

int fc_port_ota_begin(size_t size, void **handle)
{
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    if (next == NULL || size > next->size) {
        ESP_LOGW(TAG, "OTA: no slot for %u bytes", (unsigned)size);
        return -1;
    }
    esp_ota_handle_t h;
    esp_err_t err = esp_ota_begin(next, size, &h);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_ota_begin: %s", esp_err_to_name(err));
        return -1;
    }
    ESP_LOGI(TAG, "OTA: writing %u bytes to %s", (unsigned)size, next->label);
    *handle = (void *)(uintptr_t)h;
    return 0;
}

int fc_port_ota_write(void *handle, const void *data, size_t len)
{
    return esp_ota_write((esp_ota_handle_t)(uintptr_t)handle, data, len) == ESP_OK ? 0 : -1;
}

/* Which security features this firmware is built for, in the image itself:
 * ESP-IDF places .rodata_custom_desc right after esp_app_desc_t. A secure
 * build installed on a plain chip could not open its encrypted store, and a
 * plain build on a secure chip would store keys in clear: the device takes
 * only images built for what it is. Images from before this marker count as
 * plain. */
#define FW_DESC_MAGIC 0x44564346u /* "FCVD" */
#define FW_SECURE_BOOT 1u
#define FW_FLASH_ENC   2u
#define FW_NVS_ENC     4u
#if CONFIG_SECURE_BOOT
#define FW_F1 FW_SECURE_BOOT
#else
#define FW_F1 0u
#endif
#if CONFIG_SECURE_FLASH_ENC_ENABLED
#define FW_F2 FW_FLASH_ENC
#else
#define FW_F2 0u
#endif
#if CONFIG_NVS_ENCRYPTION
#define FW_F3 FW_NVS_ENC
#else
#define FW_F3 0u
#endif
#define FW_FLAGS (FW_F1 | FW_F2 | FW_F3)
typedef struct {
    uint32_t magic;
    uint32_t flags;
} fw_desc_t;
/* Kept by "-u fc_fw_desc" (fc_core's CMakeLists.txt): nothing references it. */
const __attribute__((section(".rodata_custom_desc"))) fw_desc_t fc_fw_desc = {FW_DESC_MAGIC, FW_FLAGS};

static const char *fw_variant(uint32_t flags)
{
    return flags == 0 ? "plain" : "secure";
}

/* The running image as it was downloaded: the slot is read through the flash
 * encryption, so this is the image in clear, signature included. */
int fc_port_fw_read(size_t off, void *buf, size_t len)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    if (p == NULL || off > p->size || len > p->size - off) {
        return -1;
    }
    return esp_partition_read(p, off, buf, len) == ESP_OK ? 0 : -1;
}

int fc_port_ota_end(void *handle, int commit)
{
    esp_ota_handle_t h = (esp_ota_handle_t)(uintptr_t)handle;
    if (!commit) {
        esp_ota_abort(h);
        return 0;
    }
    const esp_partition_t *next = esp_ota_get_next_update_partition(NULL);
    esp_err_t err = esp_ota_end(h); /* checks the image header, segments and hash */
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA end: %s", esp_err_to_name(err));
        return -1;
    }
    fw_desc_t d;
    size_t off = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t) + sizeof(esp_app_desc_t);
    if (esp_partition_read(next, off, &d, sizeof(d)) != ESP_OK) {
        ESP_LOGW(TAG, "OTA: cannot read the image's variant");
        return -1;
    }
    uint32_t flags = d.magic == FW_DESC_MAGIC ? d.flags : 0;
    if (flags != FW_FLAGS) {
        ESP_LOGW(TAG, "OTA: image built for a %s device (0x%x), this one is %s (0x%x)", fw_variant(flags),
                 (unsigned)flags, fw_variant(FW_FLAGS), (unsigned)FW_FLAGS);
        return FC_OTA_WRONG_VARIANT;
    }
    err = esp_ota_set_boot_partition(next);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA set boot: %s", esp_err_to_name(err));
        return err == ESP_ERR_OTA_SMALL_SEC_VER ? FC_OTA_TOO_OLD : -1;
    }
    return 0;
}

int fc_port_ota_pending_verify(void)
{
    esp_ota_img_states_t st;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
           st == ESP_OTA_IMG_PENDING_VERIFY;
}

/* Secure Boot key rotation. The factory bootloader is signed with up to
 * three keys (merge-secure.py --extra-key), so the first boot trusts all
 * three, in order. Releases are signed with one of them; once a release
 * signed with key N is confirmed, the keys before N are of no further use
 * and are revoked for good (esp_ota_revoke_secure_boot_public_key checks
 * first that the bootloader and this app verify without them). A key is
 * thus retired by shipping a release signed with the next one. */
#if CONFIG_SECURE_BOOT_V2_ENABLED && SOC_EFUSE_REVOKE_BOOT_KEY_DIGESTS
static int app_key_index(char *keys, size_t keys_len)
{
    esp_image_sig_public_key_digests_t app;
    esp_secure_boot_key_digests_t chip;
    if (esp_secure_boot_get_signature_blocks_for_running_app(true, &app) != ESP_OK ||
        esp_secure_boot_read_key_digests(&chip) != ESP_OK) {
        return -1;
    }
    int lowest = -1;
    size_t o = 0;
    for (int i = 0; i < SOC_EFUSE_SECURE_BOOT_KEY_DIGESTS; i++) {
        if (chip.key_digests[i] == NULL) {
            continue;
        }
        int signs = 0;
        for (unsigned j = 0; j < app.num_digests; j++) {
            signs |= memcmp(chip.key_digests[i], app.key_digests[j], ESP_SECURE_BOOT_KEY_DIGEST_SHA_256_LEN) == 0;
        }
        if (signs && lowest < 0) {
            lowest = i;
        }
        if (keys != NULL && o + 4 < keys_len) {
            o += (size_t)snprintf(keys + o, keys_len - o, "%s%d%s", o ? "," : "", i, signs ? "*" : "");
        }
    }
    return lowest;
}

static void revoke_older_keys(void)
{
    esp_ota_img_states_t st;
    if (!esp_secure_boot_enabled() ||
        esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) != ESP_OK || st != ESP_OTA_IMG_VALID) {
        return;
    }
    int n = app_key_index(NULL, 0);
    esp_secure_boot_key_digests_t chip;
    if (n <= 0 || esp_secure_boot_read_key_digests(&chip) != ESP_OK) {
        return;
    }
    for (int i = 0; i < n; i++) {
        if (chip.key_digests[i] != NULL) {
            esp_err_t err = esp_ota_revoke_secure_boot_public_key((esp_ota_secure_boot_public_key_index_t)i);
            ESP_LOGW(TAG, "Secure Boot key %d revoked (this release is signed with key %d): %s", i, n,
                     esp_err_to_name(err));
        }
    }
}

/* Revokes Secure Boot key `index` for good, on request (fleet config, see
 * main.c): a spare key that was exposed. Refused when the bootloader or the
 * running app would no longer verify without it, or before the running app
 * is confirmed. Revoking a key already revoked succeeds. */
int fc_port_espidf_revoke_secure_boot_key(int index)
{
    if (!esp_secure_boot_enabled() || index < 0 || index >= SOC_EFUSE_SECURE_BOOT_KEY_DIGESTS) {
        ESP_LOGW(TAG, "Secure Boot key %d: not a key this device can revoke", index);
        return -1;
    }
    esp_err_t err = esp_ota_revoke_secure_boot_public_key((esp_ota_secure_boot_public_key_index_t)index);
    ESP_LOGW(TAG, "Secure Boot key %d revoked on request: %s", index, esp_err_to_name(err));
    return err == ESP_OK ? 0 : -1;
}

static void secure_boot_keys_info(cJSON *o)
{
    char keys[24] = "";
    if (esp_secure_boot_enabled() && app_key_index(keys, sizeof(keys)) >= 0) {
        cJSON_AddStringToObject(o, "secureBootKeys", keys); /* usable slots, * = signs this app */
    }
}
#else
static void revoke_older_keys(void) {}
static void secure_boot_keys_info(cJSON *o) {}
int fc_port_espidf_revoke_secure_boot_key(int index)
{
    ESP_LOGW(TAG, "Secure Boot key %d: this firmware has no Secure Boot", index);
    return -1;
}
#endif

void fc_port_ota_confirm(void)
{
    esp_ota_mark_app_valid_cancel_rollback();
    revoke_older_keys();
}

void fc_port_ota_rollback(void)
{
    esp_ota_mark_app_invalid_rollback_and_reboot();
    esp_restart(); /* no previous image: plain restart */
}

void fc_port_reboot(void)
{
    esp_restart();
}

void fc_port_log(fc_log_level_t level, const char *fmt, ...)
{
    static const esp_log_level_t map[] = {ESP_LOG_ERROR, ESP_LOG_WARN, ESP_LOG_INFO, ESP_LOG_DEBUG};
    char buf[384];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    ESP_LOG_LEVEL(map[level], TAG, "%s", buf);
}

/* ---- Device key in the Digital Signature peripheral ---------------------
 * The RSA private key is encrypted (AES, by esp_ds_encrypt_params) with a
 * key derived from a random HMAC key, which is burned into a free eFuse key
 * block with the DS purpose: that block cannot be read by software, and
 * only the DS peripheral can decrypt the parameters and sign with them. The
 * encrypted parameters stay in the (encrypted) store. */
#if CONFIG_FC_DEVICE_KEY_DS || CONFIG_FC_DEVICE_KEY_ECDSA
#include "esp_efuse_table.h"

/* A free eFuse key block for a key that software must not read, or
 * EFUSE_BLK_KEY_MAX. None once the chip can no longer read-protect a block:
 * the key would be burned and left readable. */
static esp_efuse_block_t hwkey_free_block(bool not_block9)
{
    if (esp_efuse_read_field_bit(ESP_EFUSE_WR_DIS_RD_DIS)) {
        ESP_LOGE(TAG, "device key in hardware: this chip no longer read-protects eFuse key blocks");
        return EFUSE_BLK_KEY_MAX;
    }
    for (int b = EFUSE_BLK_KEY0; b < EFUSE_BLK_KEY_MAX; b++) {
#if SOC_EFUSE_BLOCK9_KEY_PURPOSE_QUIRK
        if (not_block9 && b == EFUSE_BLK9) {
            continue;
        }
#endif
        if (esp_efuse_key_block_unused((esp_efuse_block_t)b)) {
            return (esp_efuse_block_t)b;
        }
    }
    ESP_LOGE(TAG, "device key in hardware: no free eFuse key block left");
    return EFUSE_BLK_KEY_MAX;
}

/* After the key is sealed. Secure Boot normally stops further read
 * protection on its first boot (a read-protected key digest would brick the
 * device); builds with the device key in hardware leave it open until here
 * (CONFIG_SECURE_BOOT_V2_ALLOW_EFUSE_RD_DIS). A later key (re-enrollment)
 * therefore stays in the encrypted store. */
static void hwkey_sealed(void)
{
#if CONFIG_SECURE_BOOT_V2_ENABLED
    esp_err_t err = esp_efuse_write_field_bit(ESP_EFUSE_WR_DIS_RD_DIS);
    ESP_LOGW(TAG, "eFuse read protection closed for the other key blocks: %s", esp_err_to_name(err));
#endif
}
#endif

#if CONFIG_FC_DEVICE_KEY_DS
#include "esp_ds.h"
#include "esp_efuse.h"
#include "esp_random.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform_util.h"
#include "mbedtls/rsa.h"
#include "rsa_sign_alt.h"

#define KV_DS "hwkey.ds"

typedef struct {
    uint32_t block; /* eFuse key block, 0 = BLOCK_KEY0 */
    uint32_t bits;
    esp_ds_data_t data;
} ds_blob_t;

static int esp_rng(void *ctx, unsigned char *buf, size_t len)
{
    esp_fill_random(buf, len);
    return 0;
}

static pthread_mutex_t s_ds_lock = PTHREAD_MUTEX_INITIALIZER;
static ds_blob_t *s_ds;            /* the sealed key in use */
static esp_ds_data_ctx_t s_ds_ctx;
static mbedtls_rsa_context s_ds_rsa; /* holds the padding mode mbedTLS asks for */

int fc_port_hwkey_bits(void)
{
    return CONFIG_FC_DEVICE_KEY_DS_BITS > ESP_DS_SIGNATURE_MAX_BIT_LEN ? ESP_DS_SIGNATURE_MAX_BIT_LEN
                                                                      : CONFIG_FC_DEVICE_KEY_DS_BITS;
}

/* Makes `b` the key that esp_ds_rsa_sign uses. Once per boot: the DS
 * context takes a lock that is never given back, as nothing else uses DS. */
static int ds_activate(ds_blob_t *b)
{
    if (s_ds != NULL) {
        return s_ds == b ? 0 : -1;
    }
    s_ds_ctx.esp_ds_data = &b->data;
    s_ds_ctx.efuse_key_id = (uint8_t)b->block;
    s_ds_ctx.rsa_length_bits = (uint16_t)b->bits;
    if (esp_ds_init_data_ctx(&s_ds_ctx) != ESP_OK) {
        return -1;
    }
    mbedtls_rsa_init(&s_ds_rsa);
    s_ds = b;
    return 0;
}

/* DS operands for an RSA key (little-endian words): the private exponent,
 * the modulus, Rb = 2^(2*bits) mod N and M' = -N^-1 mod 2^32. */
static int ds_operands(mbedtls_rsa_context *rsa, esp_ds_p_data_t *p)
{
    size_t bits = mbedtls_rsa_get_bitlen(rsa);
    mbedtls_mpi n, d, r, t;
    mbedtls_mpi_init(&n);
    mbedtls_mpi_init(&d);
    mbedtls_mpi_init(&r);
    mbedtls_mpi_init(&t);
    int rc = mbedtls_rsa_export(rsa, &n, NULL, NULL, &d, NULL);
    if (rc == 0) {
        rc = mbedtls_mpi_lset(&t, 1);
    }
    if (rc == 0) {
        rc = mbedtls_mpi_shift_l(&t, 2 * bits);
    }
    if (rc == 0) {
        rc = mbedtls_mpi_mod_mpi(&r, &t, &n);
    }
    if (rc == 0) {
        rc = mbedtls_mpi_write_binary_le(&d, (unsigned char *)p->Y, bits / 8);
    }
    if (rc == 0) {
        rc = mbedtls_mpi_write_binary_le(&n, (unsigned char *)p->M, bits / 8);
    }
    if (rc == 0) {
        rc = mbedtls_mpi_write_binary_le(&r, (unsigned char *)p->Rb, bits / 8);
    }
    if (rc == 0) {
        rc = mbedtls_mpi_lset(&t, 1);
    }
    if (rc == 0) {
        rc = mbedtls_mpi_shift_l(&t, 32);
    }
    if (rc == 0) {
        rc = mbedtls_mpi_inv_mod(&r, &n, &t);
    }
    if (rc == 0) {
        uint32_t inv = 0;
        rc = mbedtls_mpi_write_binary_le(&r, (unsigned char *)&inv, sizeof(inv));
        p->M_prime = (uint32_t)(0u - inv);
        p->length = (uint32_t)(bits / 32 - 1);
    }
    mbedtls_mpi_free(&n);
    mbedtls_mpi_free(&d);
    mbedtls_mpi_free(&r);
    mbedtls_mpi_free(&t);
    return rc;
}

int fc_port_hwkey_seal(struct mbedtls_pk_context *pk)
{
    if (mbedtls_pk_get_type(pk) != MBEDTLS_PK_RSA) {
        return -1;
    }
    mbedtls_rsa_context *rsa = mbedtls_pk_rsa(*pk);
    size_t bits = mbedtls_rsa_get_bitlen(rsa);
    if (bits % 32 != 0 || bits > ESP_DS_SIGNATURE_MAX_BIT_LEN) {
        ESP_LOGE(TAG, "DS: RSA-%u not supported", (unsigned)bits);
        return -1;
    }
    esp_efuse_block_t blk = hwkey_free_block(false);
    if (blk == EFUSE_BLK_KEY_MAX) {
        return -1;
    }
    esp_ds_p_data_t *p = calloc(1, sizeof(*p));
    ds_blob_t *b = calloc(1, sizeof(*b));
    uint8_t key[32], iv[ESP_DS_IV_LEN];
    int rc = (p == NULL || b == NULL) ? -1 : ds_operands(rsa, p);
    if (rc == 0) {
        esp_fill_random(key, sizeof(key));
        esp_fill_random(iv, sizeof(iv));
        b->block = (uint32_t)(blk - EFUSE_BLK_KEY0);
        b->bits = (uint32_t)bits;
        rc = esp_ds_encrypt_params(&b->data, iv, p, key) == ESP_OK ? 0 : -1;
    }
    if (rc == 0) {
        /* From here on the HMAC key exists only in the eFuse block. */
        esp_err_t err = esp_efuse_write_key(blk, ESP_EFUSE_KEY_PURPOSE_HMAC_DOWN_DIGITAL_SIGNATURE, key, sizeof(key));
        ESP_LOGW(TAG, "DS: HMAC key burned into eFuse BLOCK_KEY%u: %s", (unsigned)b->block, esp_err_to_name(err));
        rc = err == ESP_OK && esp_efuse_get_key_dis_read(blk) ? 0 : -1;
    }
    mbedtls_platform_zeroize(key, sizeof(key));
    if (p != NULL) {
        mbedtls_platform_zeroize(p, sizeof(*p));
        free(p);
    }
    pthread_mutex_lock(&s_ds_lock);
    if (rc == 0) {
        rc = ds_activate(b);
    }
    pthread_mutex_unlock(&s_ds_lock);
    /* A signature made by the peripheral, checked with the public key,
     * before the software copy of the key is gone. */
    if (rc == 0) {
        mbedtls_pk_context hw;
        mbedtls_pk_init(&hw);
        unsigned char hash[32], sig[ESP_DS_SIGNATURE_MAX_BIT_LEN / 8];
        size_t sig_len = 0;
        esp_fill_random(hash, sizeof(hash));
        rc = fc_port_hwkey_pk(&hw);
        if (rc == 0) {
            rc = mbedtls_pk_sign_ext(MBEDTLS_PK_RSASSA_PSS, &hw, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sizeof(sig),
                                     &sig_len, esp_rng, NULL);
        }
        int signed_ok = rc == 0;
        if (rc == 0) {
            const mbedtls_pk_rsassa_pss_options pss = {MBEDTLS_MD_SHA256, MBEDTLS_RSA_SALT_LEN_ANY};
            rc = mbedtls_pk_verify_ext(MBEDTLS_PK_RSASSA_PSS, &pss, pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig,
                                       sig_len);
        }
        mbedtls_pk_free(&hw);
        if (rc == 0) {
            ESP_LOGI(TAG, "DS: RSA-PSS signature made by the peripheral verified with the public key");
        } else {
            ESP_LOGE(TAG, "DS: %s failed (-0x%04x)", signed_ok ? "signature check" : "signing", (unsigned)-rc);
        }
    }
    if (rc == 0 && fc_port_kv_set(KV_DS, b, sizeof(*b)) != 0) {
        rc = -1;
    }
    if (rc == 0) {
        hwkey_sealed();
    }
    if (rc != 0 && s_ds != b) {
        free(b);
    }
    return rc == 0 ? 0 : -1;
}

int fc_port_hwkey_pk(struct mbedtls_pk_context *pk)
{
    pthread_mutex_lock(&s_ds_lock);
    int rc = 0;
    if (s_ds == NULL) {
        char *blob = NULL;
        size_t len = 0;
        rc = -1;
        if (fc_port_kv_get(KV_DS, &blob, &len) == 0 && len == sizeof(ds_blob_t)) {
            rc = ds_activate((ds_blob_t *)blob);
        }
        if (rc != 0) {
            free(blob);
        }
    }
    pthread_mutex_unlock(&s_ds_lock);
    if (rc != 0) {
        return -1;
    }
    return mbedtls_pk_setup_rsa_alt(pk, &s_ds_rsa, NULL, esp_ds_rsa_sign, esp_ds_get_keylen) == 0 ? 0 : -1;
}

/* The eFuse block stays used: a key block can be burned only once. */
void fc_port_hwkey_forget(void)
{
    fc_port_kv_del(KV_DS);
}

static const char *hwkey_info(void)
{
    return s_ds != NULL ? "ds" : "storage";
}
#elif CONFIG_FC_DEVICE_KEY_ECDSA
/* The device key in the ECDSA peripheral (ESP32-C5, H2, P4): a P-256 key in
 * an eFuse key block that only the peripheral reads. */
#include "ecdsa/ecdsa_alt.h"
#include "esp_random.h"
#include "mbedtls/ecdsa.h"
#include "mbedtls/ecp.h"
#include "mbedtls/pk.h"
#include "mbedtls/platform_util.h"

#define KV_ECDSA "hwkey.ecdsa"

static pthread_mutex_t s_ecdsa_lock = PTHREAD_MUTEX_INITIALIZER;
static int s_ecdsa_blk = -1; /* eFuse block (esp_efuse_block_t) of the key in use */

static int esp_rng(void *ctx, unsigned char *buf, size_t len)
{
    esp_fill_random(buf, len);
    return 0;
}

int fc_port_hwkey_bits(void)
{
    return FC_HWKEY_ECDSA_P256;
}

/* A context that signs with the key in `blk`: it holds no private key. */
static int ecdsa_pk(mbedtls_pk_context *pk, int blk)
{
    if (blk < EFUSE_BLK_KEY0 || blk >= EFUSE_BLK_KEY_MAX ||
        esp_efuse_get_key_purpose((esp_efuse_block_t)blk) != ESP_EFUSE_KEY_PURPOSE_ECDSA_KEY) {
        return -1;
    }
    esp_ecdsa_pk_conf_t conf = {
        .grp_id = MBEDTLS_ECP_DP_SECP256R1,
        .efuse_block = (uint8_t)blk,
#if SOC_ECDSA_SUPPORT_EXPORT_PUBKEY
        .load_pubkey = true,
#endif
    };
    return esp_ecdsa_set_pk_context(pk, &conf) == 0 ? 0 : -1;
}

int fc_port_hwkey_seal(struct mbedtls_pk_context *pk)
{
    if (mbedtls_pk_get_type(pk) != MBEDTLS_PK_ECKEY || mbedtls_pk_get_bitlen(pk) != 256) {
        return -1;
    }
    esp_efuse_block_t blk = hwkey_free_block(true);
    if (blk == EFUSE_BLK_KEY_MAX) {
        return -1;
    }
    uint8_t d[32];
    size_t len = 0;
    int rc = mbedtls_ecp_write_key_ext(mbedtls_pk_ec(*pk), &len, d, sizeof(d)) == 0 && len == sizeof(d) ? 0 : -1;
    if (rc == 0) {
        /* The peripheral takes the scalar little-endian. From here on the
         * key exists in this block, and in `pk` until the caller frees it. */
        for (size_t i = 0; i < sizeof(d) / 2; i++) {
            uint8_t t = d[i];
            d[i] = d[sizeof(d) - 1 - i];
            d[sizeof(d) - 1 - i] = t;
        }
        esp_err_t err = esp_efuse_write_key(blk, ESP_EFUSE_KEY_PURPOSE_ECDSA_KEY, d, sizeof(d));
        ESP_LOGW(TAG, "ECDSA: device key burned into eFuse BLOCK_KEY%u: %s", (unsigned)(blk - EFUSE_BLK_KEY0),
                 esp_err_to_name(err));
        rc = err == ESP_OK && esp_efuse_get_key_dis_read(blk) ? 0 : -1;
    }
    mbedtls_platform_zeroize(d, sizeof(d));
    /* A signature made by the peripheral, checked with the public key,
     * before the software copy of the key is gone. */
    if (rc == 0) {
        mbedtls_pk_context hw;
        mbedtls_pk_init(&hw);
        unsigned char hash[32], sig[MBEDTLS_ECDSA_MAX_LEN];
        size_t sig_len = 0;
        esp_fill_random(hash, sizeof(hash));
        rc = ecdsa_pk(&hw, blk);
        if (rc == 0) {
            rc = mbedtls_pk_sign(&hw, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sizeof(sig), &sig_len, esp_rng, NULL);
        }
        int signed_ok = rc == 0;
        if (rc == 0) {
            rc = mbedtls_pk_verify(pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sig_len);
        }
        mbedtls_pk_free(&hw);
        if (rc == 0) {
            ESP_LOGI(TAG, "ECDSA: signature made by the peripheral verified with the public key");
        } else {
            ESP_LOGE(TAG, "ECDSA: %s failed (-0x%04x)", signed_ok ? "signature check" : "signing", (unsigned)-rc);
        }
    }
    uint32_t stored = (uint32_t)blk;
    if (rc == 0 && fc_port_kv_set(KV_ECDSA, &stored, sizeof(stored)) != 0) {
        rc = -1;
    }
    if (rc == 0) {
        pthread_mutex_lock(&s_ecdsa_lock);
        s_ecdsa_blk = (int)blk;
        pthread_mutex_unlock(&s_ecdsa_lock);
        hwkey_sealed();
    }
    return rc == 0 ? 0 : -1;
}

int fc_port_hwkey_pk(struct mbedtls_pk_context *pk)
{
    pthread_mutex_lock(&s_ecdsa_lock);
    if (s_ecdsa_blk < 0) {
        char *v = NULL;
        size_t len = 0;
        uint32_t stored;
        if (fc_port_kv_get(KV_ECDSA, &v, &len) == 0 && len == sizeof(stored)) {
            memcpy(&stored, v, sizeof(stored));
            s_ecdsa_blk = (int)stored;
        }
        free(v);
    }
    int blk = s_ecdsa_blk;
    pthread_mutex_unlock(&s_ecdsa_lock);
    return ecdsa_pk(pk, blk);
}

/* The eFuse block stays used: a key block can be burned only once. */
void fc_port_hwkey_forget(void)
{
    fc_port_kv_del(KV_ECDSA);
    pthread_mutex_lock(&s_ecdsa_lock);
    s_ecdsa_blk = -1;
    pthread_mutex_unlock(&s_ecdsa_lock);
}

static const char *hwkey_info(void)
{
    return s_ecdsa_blk >= 0 ? "ecdsa" : "storage";
}
#else
int fc_port_hwkey_bits(void)
{
    return 0;
}

int fc_port_hwkey_seal(struct mbedtls_pk_context *pk)
{
    return -1;
}

int fc_port_hwkey_pk(struct mbedtls_pk_context *pk)
{
    return -1;
}

void fc_port_hwkey_forget(void) {}

static const char *hwkey_info(void)
{
    return "storage";
}
#endif
