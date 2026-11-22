#include <nuttx/config.h>
#include "fc_ota_arm.h"
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <malloc.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/boardctl.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/mount.h>
#include <sys/utsname.h>
#include <unistd.h>
#include <netutils/netlib.h>

#include "cJSON.h"
#include "fc_log.h"
#include "fc_port.h"
#include "fc_hw_nuttx.h"
#include <nuttx/arch.h>
#include <nuttx/fs/fs.h>
#include <nuttx/sched.h>
#include <nuttx/mm/mm.h>
#include <nuttx/kmalloc.h>

void fc_nuttx_cpu_ticks(uint64_t *total, uint64_t *busy);
long fc_nuttx_watch_heap(int start);
int esp_wifi_sta_get_rssi(int *rssi);

#ifndef CONFIG_ARCH_BOARD_NUCLEO_F767ZI
static pthread_mutex_t writer = PTHREAD_MUTEX_INITIALIZER;

static int make_dir(const char *path)
{
    struct stat st;
    return mkdir(path, 0700) == 0 ||
           (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : -1;
}

#ifdef CONFIG_FLIGHTCTL_NRF7002DK
int fc_nrf_mount_storage(void);
#endif

int fc_nuttx_storage_init(void)
{
#ifdef CONFIG_FLIGHTCTL_NRF7002DK
    if (fc_nrf_mount_storage() < 0) return -1;
#endif
#ifdef CONFIG_ARCH_BOARD_RASPBERRYPI_PICO_2_W
    if (mount("/dev/rpflash", "/data", "littlefs", 0, NULL) < 0) {
        const uint8_t *storage = (const uint8_t *)0x10304000u;
        for (size_t i = 0; i < 0xfc000; i++) {
            if (storage[i] != 0xff) {
                return -1;
            }
        }
        if (mount("/dev/rpflash", "/data", "littlefs", 0, "forceformat") < 0) {
            return -1;
        }
    }
#endif
    struct statfs fs;
    /* Avoid writing to the pseudo filesystem if mounting flash failed. */
    return statfs("/data", &fs) == 0 && fs.f_blocks > 0 &&
           make_dir("/data/keys") == 0 && make_dir("/data/files") == 0 ? 0 : -1;
}

static int storage_path(char path[PATH_MAX], const char *name, int config)
{
    if (config && *name++ != '/') {
        return -1;
    }
    for (const char *p = name;;) {
        const char *slash = strchr(p, '/');
        size_t len = slash ? (size_t)(slash - p) : strlen(p);
        if (!len || len > NAME_MAX || (len == 1 && p[0] == '.') ||
            (len == 2 && memcmp(p, "..", 2) == 0) || (slash && !config)) {
            return -1;
        }
        if (!slash) {
            break;
        }
        p = slash + 1;
    }
    int n = snprintf(path, PATH_MAX, "/data/%s/%s", config ? "files" : "keys", name);
    return n >= 0 && n < PATH_MAX ? 0 : -1;
}

static int load(const char *name, int config, char **out, size_t *len)
{
    char path[PATH_MAX];
    if (storage_path(path, name, config) != 0) {
        return -1;
    }
    /* The NuttX file interface preserves LittleFS's error on every target.
     * The classic ESP32's stdio can leave an earlier errno on a failed open. */
    struct file f;
    int rc = file_open(&f, path, O_RDONLY);
    if (rc < 0) {
        return rc == -ENOENT ? FC_KV_MISSING : -1;
    }
    struct stat st;
    char *buf = NULL;
    if (file_fstat(&f, &st) == 0 && S_ISREG(st.st_mode) && st.st_size >= 0 &&
        (uintmax_t)st.st_size < SIZE_MAX) {
        buf = malloc((size_t)st.st_size + 1);
    }
    size_t n = 0;
    while (buf && n < (size_t)st.st_size) {
        ssize_t count = file_read(&f, buf + n, (size_t)st.st_size - n);
        if (count <= 0) {
            break;
        }
        n += (size_t)count;
    }
    int ok = buf && n == (size_t)st.st_size;
    file_close(&f);
    if (!ok) {
        free(buf);
        return -1;
    }
    *len = st.st_size;
    buf[*len] = 0;
    *out = buf;
    return 0;
}

static int save(const char *name, int config, const void *data, size_t len)
{
    char path[PATH_MAX];
    if (storage_path(path, name, config) != 0) {
        return -1;
    }
    pthread_mutex_lock(&writer);
    int rc = -1;
    for (char *p = path + strlen("/data/files/"); config && (p = strchr(p, '/')); p++) {
        *p = 0;
        int err = make_dir(path);
        *p = '/';
        if (err) {
            goto done;
        }
    }
    /* LittleFS commits a file at sync/close and a rename atomically. */
    const char *tmp = "/data/.fc-write";
    FILE *f = fopen(tmp, "wb");
    if (f) {
        int ok = fwrite(data, 1, len, f) == len && fflush(f) == 0 && fsync(fileno(f)) == 0;
        if (fclose(f) == 0 && ok) {
            rc = rename(tmp, path);
        }
        if (rc != 0) {
            unlink(tmp);
        }
    }
done:
    pthread_mutex_unlock(&writer);
    return rc;
}

static int remove_entry(const char *name, int config)
{
    char path[PATH_MAX];
    if (storage_path(path, name, config) != 0) {
        return -1;
    }
    pthread_mutex_lock(&writer);
    int rc = nx_unlink(path);
    pthread_mutex_unlock(&writer);
    return rc == 0 || rc == -ENOENT ? 0 : -1;
}

int fc_port_kv_get(const char *key, char **out, size_t *len) { return load(key, 0, out, len); }
int fc_port_kv_set(const char *key, const void *data, size_t len) { return save(key, 0, data, len); }
int fc_port_kv_del(const char *key) { return remove_entry(key, 0); }
int fc_port_read_file(const char *path, char **out, size_t *len) { return load(path, 1, out, len) == 0 ? 0 : -1; }
int fc_port_write_file(const char *path, const void *data, size_t len) { return save(path, 1, data, len); }
int fc_port_remove_file(const char *path) { return remove_entry(path, 1); }

#endif

void fc_port_sysinfo(fc_sysinfo_t *si)
{
    struct utsname u;
    memset(si, 0, sizeof(*si));
    snprintf(si->architecture, sizeof(si->architecture), "%s-%s", CONFIG_ARCH, CONFIG_ARCH_CHIP);
    strcpy(si->operating_system, "nuttx");
    if (uname(&u) == 0) {
        snprintf(si->os_version, sizeof(si->os_version), "%.31s", u.release);
    }
#ifndef CONFIG_FLIGHTCTL_SERIAL_NETWORK
    uint8_t mac[6];
#ifdef CONFIG_STM32_ETHMAC
    const char *iface = "eth0";
#else
    const char *iface = "wlan0";
#endif
    if (netlib_getmacaddr(iface, mac) == 0) {
        snprintf(si->hw_id, sizeof(si->hw_id), "%02x%02x%02x%02x%02x%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        snprintf(si->hostname, sizeof(si->hostname), CONFIG_ARCH_CHIP "-%s", si->hw_id + 6);
    }
#else
    snprintf(si->hostname, sizeof(si->hostname), "%s", CONFIG_ARCH_BOARD);
#endif
    si->cpu_count = CONFIG_SMP_NCPUS;
    fc_nuttx_cpu_ticks(&si->cpu_total, &si->cpu_busy);
    struct mallinfo heap = mallinfo();
    si->mem_total = heap.arena;
    si->mem_free = heap.fordblks;
    struct statfs fs;
    if (statfs("/data", &fs) == 0) {
        si->disk_total = (uint64_t)fs.f_blocks * fs.f_bsize;
        si->disk_free = (uint64_t)fs.f_bavail * fs.f_bsize;
    }
}

#if defined(CONFIG_ESP32S3_APP_FORMAT_MCUBOOT) || defined(FC_ARM_PLATFORM)
void fc_nuttx_fw_info(cJSON *obj);
#endif

void fc_port_custom_info(void *obj)
{
    cJSON_AddStringToObject(obj, "chip", CONFIG_ARCH_CHIP);
#if defined(CONFIG_ESP32S3_APP_FORMAT_MCUBOOT) || defined(FC_ARM_PLATFORM)
    fc_nuttx_fw_info(obj);
#endif
}
static void metric(cJSON *obj, const char *name, long value)
{
    char text[32];
    snprintf(text, sizeof(text), "%ld", value);
    cJSON_AddStringToObject(obj, name, text);
}

void fc_port_metrics(void *obj)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0) metric(obj, "uptimeSeconds", ts.tv_sec);
    metric(obj, "memMinFreeBytes", fc_port_heap_low());
    struct mallinfo internal = kmm_mallinfo();
    metric(obj, "memInternalFreeBytes", internal.fordblks);
    metric(obj, "memInternalLargestBlock", internal.mxordblk);
#ifndef CONFIG_ARCH_ARM
    int rssi;
    if (esp_wifi_sta_get_rssi(&rssi) == 0) metric(obj, "wifiRssi", rssi);
#endif
#if defined(CONFIG_ESPRESSIF_TEMP) || defined(CONFIG_ARCH_BOARD_RASPBERRYPI_PICO_2_W)
    int32_t mc;
    if (fc_nuttx_sensor("die_temp", &mc) == 0) {
        char text[24];
        snprintf(text, sizeof(text), "%.1f", (double)mc / 1000);
        cJSON_AddStringToObject(obj, "temperatureC", text);
    }
#endif
}
long fc_port_heap_largest(void) { return mallinfo().mxordblk; }
long fc_port_heap_free(void) { return mallinfo().fordblks; }
long fc_port_heap_free_now(void) { return fc_port_heap_free(); }
long fc_port_heap_low(void) { struct mallinfo m = mallinfo(); return m.arena - m.usmblks; }
long fc_port_heap_watch(int start) { return fc_nuttx_watch_heap(start); }
long fc_port_stack_unused(void)
{
    struct tcb_s *self = nxsched_self();
    size_t used = up_check_tcbstack(self, self->adj_stack_size);
    return self->adj_stack_size - used;
}

#if !defined(CONFIG_ESP32S3_APP_FORMAT_MCUBOOT) && !defined(FC_ARM_PLATFORM)
const char *fc_port_fw_platform(void) { return "nuttx-" CONFIG_ARCH_CHIP; }
int fc_port_ota_begin(size_t size, void **handle) { return -1; }
int fc_port_ota_write(void *handle, const void *data, size_t len) { return -1; }
int fc_port_ota_end(void *handle, int commit) { return -1; }
int fc_port_fw_read(size_t off, void *buf, size_t len) { return -1; }
int fc_port_ota_pending_verify(void) { return 0; }
void fc_port_ota_confirm(void) {}
void fc_port_ota_rollback(void) {}
#endif
void fc_port_reboot(void)
{
#if defined(CONFIG_ARCH_BOARD_NUCLEO_F767ZI) || defined(CONFIG_FLIGHTCTL_NRF7002DK)
    up_systemreset();
#else
    boardctl(BOARDIOC_RESET, 0);
#endif
}

int fc_port_hwkey_bits(void) { return 0; }
int fc_port_hwkey_seal(struct mbedtls_pk_context *pk) { return -1; }
int fc_port_hwkey_pk(struct mbedtls_pk_context *pk, const struct mbedtls_pk_context *pub) { return -1; }
void fc_port_hwkey_forget(void) {}

void fc_port_log(fc_log_level_t level, const char *fmt, ...)
{
    if (level == FC_LOG_DEBUG) {
        return;
    }
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    fc_log_put(level, line);
    printf("fc: %s\n", line);
}
