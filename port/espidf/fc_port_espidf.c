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
#include "esp_chip_info.h"
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
#if SOC_TEMP_SENSOR_SUPPORTED
static temperature_sensor_handle_t s_tsens;
/* The status thread, the alarm task and WASM apps all read the sensor. On
 * the S2 a read starts a conversion and busy-waits for it, without a lock in
 * the driver: two overlapping reads can leave one spinning forever. */
static pthread_mutex_t s_tsens_lock = PTHREAD_MUTEX_INITIALIZER;
#endif

int fc_port_espidf_init(void)
{
    esp_err_t err = nvs_flash_init_partition(PART);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase_partition(PART);
        err = nvs_flash_init_partition(PART);
    }
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
    cJSON_AddStringToObject(o, "deviceClass", "mcu");
    cJSON_AddStringToObject(o, "chip", CONFIG_IDF_TARGET);
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
        [ESP_RST_BROWNOUT] = "brownout", [ESP_RST_SDIO] = "sdio",
    };
    esp_reset_reason_t rr = esp_reset_reason();
    cJSON_AddStringToObject(o, "resetReason",
                            (unsigned)rr < sizeof(reasons) / sizeof(reasons[0]) && reasons[rr] ? reasons[rr] : "other");
    wifi_ap_record_t ap;
    if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
        snprintf(buf, sizeof(buf), "%d", ap.rssi);
        cJSON_AddStringToObject(o, "wifiRssi", buf);
    }
    float t;
    if (fc_port_temperature(&t) == 0) {
        snprintf(buf, sizeof(buf), "%.1f", t);
        cJSON_AddStringToObject(o, "temperatureC", buf);
#if SOC_TEMP_SENSOR_SUPPORTED
        cJSON_AddStringToObject(o, "temperatureSource", "internal");
#else
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

int fc_port_ota_end(void *handle, int commit)
{
    esp_ota_handle_t h = (esp_ota_handle_t)(uintptr_t)handle;
    if (!commit) {
        esp_ota_abort(h);
        return 0;
    }
    esp_err_t err = esp_ota_end(h); /* checks the image header, segments and hash */
    if (err == ESP_OK) {
        err = esp_ota_set_boot_partition(esp_ota_get_next_update_partition(NULL));
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "OTA end: %s", esp_err_to_name(err));
        return -1;
    }
    return 0;
}

int fc_port_ota_pending_verify(void)
{
    esp_ota_img_states_t st;
    return esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
           st == ESP_OTA_IMG_PENDING_VERIFY;
}

void fc_port_ota_confirm(void)
{
    esp_ota_mark_app_valid_cancel_rollback();
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
