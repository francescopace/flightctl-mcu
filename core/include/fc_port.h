/*
 * Platform abstraction for the flightctl-mcu agent.
 *
 * The core uses POSIX (pthread, sockets via mbedtls_net, time) plus mbedTLS
 * and cJSON. Everything that differs between platforms is behind this
 * header: persistent storage, system information, applying config files and
 * firmware updates.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char architecture[32];     /* e.g. "arm64", "xtensa-esp32s3" */
    char operating_system[32]; /* e.g. "darwin", "esp-idf" */
    char os_version[32];       /* e.g. "v5.5.6" */
    char hostname[64];
    char hw_id[32];            /* MAC or serial, used as default alias */
    int cpu_count;
    uint64_t mem_total;
    uint64_t mem_free;
    uint64_t disk_total;
    uint64_t disk_free;
} fc_sysinfo_t;

typedef enum {
    FC_LOG_ERROR = 0,
    FC_LOG_WARN,
    FC_LOG_INFO,
    FC_LOG_DEBUG,
} fc_log_level_t;

/* Persistent key/value storage. fc_port_kv_get allocates *out (NUL terminated,
 * caller frees). Return 0 on success, -1 if missing or on error. */
int fc_port_kv_get(const char *key, char **out, size_t *len);
int fc_port_kv_set(const char *key, const void *data, size_t len);
int fc_port_kv_del(const char *key);

void fc_port_sysinfo(fc_sysinfo_t *si);

/* Adds platform-specific string entries to status.systemInfo.customInfo.
 * `obj` is a cJSON object; declared as void* to keep cJSON out of this API. */
void fc_port_custom_info(void *obj);

/* Writes a config file delivered through spec.config. `path` is the absolute
 * path from the device spec; the port maps it onto its own storage. */
int fc_port_write_file(const char *path, const void *data, size_t len);

/* ---- Firmware update (A/B slots with rollback) ----------------------------
 * fc_port_fw_platform: the platform.architecture this firmware matches in a
 *   multi-chip OCI index, e.g. "esp32c3".
 * fc_port_ota_begin/write/end: stream an image into the inactive slot;
 *   end(commit=1) validates it and makes it the next boot image, end(0) aborts.
 * fc_port_ota_pending_verify: 1 while the running image is new and must be
 *   confirmed (fc_port_ota_confirm) or it is rolled back on the next reset;
 *   fc_port_ota_rollback marks it bad and reboots into the previous image.
 * Ports without OTA return -1 from fc_port_ota_begin. */
const char *fc_port_fw_platform(void);
int fc_port_ota_begin(size_t size, void **handle);
int fc_port_ota_write(void *handle, const void *data, size_t len);
int fc_port_ota_end(void *handle, int commit);
int fc_port_ota_pending_verify(void);
void fc_port_ota_confirm(void);
void fc_port_ota_rollback(void);
void fc_port_reboot(void);

void fc_port_log(fc_log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif
