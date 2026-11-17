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
    char os_version[32];       /* e.g. "v6.0.3" */
    char hostname[64];
    char hw_id[32];            /* MAC or serial, used as default alias */
    int cpu_count;
    uint64_t mem_total;
    uint64_t mem_free;
    uint64_t disk_total;       /* the storage the agent writes to (config, apps) */
    uint64_t disk_free;
    /* Time the CPUs were busy, and all the time, both counted since boot in
     * any one unit (cycles, microseconds): the core takes the difference
     * between two readings. cpu_total 0: the port cannot tell. */
    uint64_t cpu_busy;
    uint64_t cpu_total;
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

/* Add platform-specific string entries to status.systemInfo.customInfo.
 * `obj` is a cJSON object; declared as void* to keep cJSON out of this API.
 * fc_port_custom_info: what stays the same while the device runs (the chip,
 *   the firmware, the security state), in every status.
 * fc_port_metrics: what changes all the time (heap, uptime, RSSI,
 *   temperature). Flight Control logs an event for each change in the
 *   status, so these are read only every metrics_interval_s
 *   (fc_agent_config_t), and the statuses in between repeat them. */
void fc_port_custom_info(void *obj);
void fc_port_metrics(void *obj);

/* Bytes of the calling thread's stack it has never touched since it started,
 * or -1 when the port cannot tell. The agent reports the lowest it saw for
 * each of its threads and the apps' (customInfo stackMinFreeBytes). */
long fc_port_stack_unused(void);

/* The largest block the heap could give now, in bytes, or -1 when the port
 * cannot tell. An app whose memory does not fit says it in its error. */
long fc_port_heap_largest(void);

/* Bytes free now in the heap the agent allocates from (internal RAM where
 * there is PSRAM too), or -1 when the port cannot tell. An app whose memory
 * would leave less than the agent needs is refused, and the largest .wasm
 * follows it (fc_apps.c). */
long fc_port_heap_free(void);

/* Bytes free now in that heap, for the gates that keep a TLS connection
 * from starting without room (the apps' broker, the metrics export), or -1
 * when the port cannot tell. Unlike fc_port_heap_free it is given where the
 * apps' own checks are off (Zephyr without CONFIG_FC_APP_HEAP_CHECK). */
long fc_port_heap_free_now(void);

/* The heap's low-water mark over a stretch of work: fc_port_heap_watch(1)
 * starts watching (0), fc_port_heap_watch(0) stops and returns the fewest
 * bytes of internal RAM free in between; -1 when the port cannot tell. One
 * stretch at a time: a start while another runs returns -1, and that caller
 * does not stop it. Its one caller is the core's meter (fc_heap_hold.h),
 * which serves the metrics export and an app's start from it. */
long fc_port_heap_watch(int start);
/* The fewest bytes of internal RAM free so far: since boot, or since the
 * start of the stretch fc_port_heap_watch is watching (ESP-IDF's monitor);
 * -1 when the port cannot tell. Read on both sides of a piece of work, it
 * says whether that work went lower than anything before it. */
long fc_port_heap_low(void);

/* Writes a config file delivered through spec.config. `path` is the absolute
 * path from the device spec; the port maps it onto its own storage. */
int fc_port_write_file(const char *path, const void *data, size_t len);
/* Reads one back, as fc_port_kv_get does: *out is allocated (NUL terminated)
 * and the caller frees it. Returns -1 if it was never written. */
int fc_port_read_file(const char *path, char **out, size_t *len);
/* Removes one, when the spec no longer has it; 0 also if it was not there. */
int fc_port_remove_file(const char *path);

/* ---- Firmware update (A/B slots with rollback) ----------------------------
 * fc_port_fw_platform: the platform.architecture this firmware matches in a
 *   multi-chip OCI index, e.g. "esp32c3".
 * fc_port_ota_begin/write/end: stream an image into the inactive slot;
 *   end(commit=1) validates it and makes it the next boot image, end(0) aborts;
 *   it returns FC_OTA_WRONG_VARIANT for a valid image built for other security
 *   features (e.g. Secure Boot and flash encryption on a plain device), and
 *   FC_OTA_TOO_OLD for one below the security version the device holds
 *   (anti-rollback).
 * fc_port_ota_pending_verify: 1 while the running image is new and must be
 *   confirmed (fc_port_ota_confirm) or it is rolled back on the next reset;
 *   fc_port_ota_rollback marks it bad and reboots into the previous image.
 * Ports without OTA return -1 from fc_port_ota_begin.
 * fc_port_fw_read: `len` bytes of the running image at `off`, as it was
 *   downloaded (0), for updates that come as a delta from it; -1 when the
 *   port cannot read it, or past its slot: updates are then whole images. */
#define FC_OTA_WRONG_VARIANT (-2)
#define FC_OTA_TOO_OLD (-3)
const char *fc_port_fw_platform(void);
int fc_port_ota_begin(size_t size, void **handle);
int fc_port_ota_write(void *handle, const void *data, size_t len);
int fc_port_ota_end(void *handle, int commit);
int fc_port_fw_read(size_t off, void *buf, size_t len);
int fc_port_ota_pending_verify(void);
void fc_port_ota_confirm(void);
void fc_port_ota_rollback(void);
void fc_port_reboot(void);

/* ---- Device key in hardware (optional) ----------------------------------
 * A port that can keep the device's private key where software cannot read
 * it says which key it takes from fc_port_hwkey_bits: an RSA key size (the
 * ESP32 DS peripheral), or FC_HWKEY_ECDSA_P256 (the ECDSA peripheral); 0
 * means it cannot, and the key stays in storage.
 * The core generates the key, writes the CSR with it, then hands it to
 * fc_port_hwkey_seal, which moves it into the hardware and checks a
 * signature made there (0), or fails (-1: the core keeps a software key).
 * fc_port_hwkey_pk sets up a context that signs through the hardware, as
 * long as a sealed key exists (0), and fc_port_hwkey_forget drops it. `pub`
 * is the key's public half, which the caller has from the CSR or from the
 * certificate: hardware that only signs (the DS peripheral) cannot give it. */
#define FC_HWKEY_ECDSA_P256 256
struct mbedtls_pk_context;
int fc_port_hwkey_bits(void);
int fc_port_hwkey_seal(struct mbedtls_pk_context *pk);
int fc_port_hwkey_pk(struct mbedtls_pk_context *pk, const struct mbedtls_pk_context *pub);
void fc_port_hwkey_forget(void);

void fc_port_log(fc_log_level_t level, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

#ifdef __cplusplus
}
#endif
