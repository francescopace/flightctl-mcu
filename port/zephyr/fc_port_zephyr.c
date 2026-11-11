/*
 * Zephyr port. Key/value pairs and config files are files in a LittleFS
 * file system on the flash partition "fc_storage": values range from a few
 * bytes to a WASM module of tens of KB, more than a settings store takes.
 */
#include "fc_port_zephyr.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/drivers/hwinfo.h>
#include <zephyr/fatal.h>
#include <zephyr/fs/fs.h>
#include <zephyr/fs/littlefs.h>
#include <zephyr/kernel.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/version.h>

#include "cJSON.h"
#include "fc_port.h"

#define MNT      "/lfs"
#define KV_DIR   MNT "/kv"
#define FILE_DIR MNT "/files"

FS_LITTLEFS_DECLARE_DEFAULT_CONFIG(s_lfs);
static struct fs_mount_t s_mnt = {
    .type = FS_LITTLEFS,
    .fs_data = &s_lfs,
    .storage_dev = (void *)FIXED_PARTITION_ID(fc_storage_partition),
    .mnt_point = MNT,
};

static void report_last_fatal(void);

int fc_port_zephyr_init(void)
{
    report_last_fatal();
    int rc = fs_mount(&s_mnt); /* formats a blank partition */
    if (rc != 0) {
        fc_port_log(FC_LOG_ERROR, "cannot mount the storage (%d)", rc);
        return -1;
    }
    fs_mkdir(KV_DIR);   /* -EEXIST after the first boot */
    fs_mkdir(FILE_DIR);
    return 0;
}

/* ---- files ------------------------------------------------------------------ */

static int read_file(const char *path, char **out, size_t *len)
{
    struct fs_dirent st;
    if (fs_stat(path, &st) != 0 || st.type != FS_DIR_ENTRY_FILE) {
        return -1;
    }
    struct fs_file_t f;
    fs_file_t_init(&f);
    if (fs_open(&f, path, FS_O_READ) != 0) {
        return -1;
    }
    char *buf = malloc(st.size + 1);
    ssize_t n = buf != NULL ? fs_read(&f, buf, st.size) : -1;
    fs_close(&f);
    if (n < 0 || (size_t)n != st.size) {
        free(buf);
        return -1;
    }
    buf[n] = '\0';
    *out = buf;
    *len = (size_t)n;
    return 0;
}

/* Through a second file and a rename, which LittleFS makes atomic: a power
 * cut leaves the old value or the new one. */
static int write_file(const char *path, const void *data, size_t len)
{
    char tmp[128];
    if (snprintf(tmp, sizeof(tmp), "%s~", path) >= (int)sizeof(tmp)) {
        return -1;
    }
    struct fs_file_t f;
    fs_file_t_init(&f);
    if (fs_open(&f, tmp, FS_O_CREATE | FS_O_WRITE | FS_O_TRUNC) != 0) {
        return -1;
    }
    ssize_t n = fs_write(&f, data, len);
    int ok = fs_close(&f) == 0 && n >= 0 && (size_t)n == len;
    if (!ok || fs_rename(tmp, path) != 0) {
        fs_unlink(tmp);
        return -1;
    }
    return 0;
}

/* A key, or a path with its slashes flattened, as a file name in `dir`. */
static int file_path(const char *dir, const char *name, char *out, size_t len)
{
    int n = snprintf(out, len, "%s/", dir);
    for (const char *c = name; *c != '\0'; c++, n++) {
        if (n >= (int)len - 1) {
            return -1;
        }
        out[n] = *c == '/' ? '_' : *c;
    }
    out[n] = '\0';
    return 0;
}

int fc_port_kv_get(const char *key, char **out, size_t *len)
{
    char p[96];
    return file_path(KV_DIR, key, p, sizeof(p)) == 0 ? read_file(p, out, len) : -1;
}

int fc_port_kv_set(const char *key, const void *data, size_t len)
{
    char p[96];
    return file_path(KV_DIR, key, p, sizeof(p)) == 0 ? write_file(p, data, len) : -1;
}

int fc_port_kv_del(const char *key)
{
    char p[96];
    if (file_path(KV_DIR, key, p, sizeof(p)) != 0) {
        return -1;
    }
    int rc = fs_unlink(p);
    return rc == 0 || rc == -ENOENT ? 0 : -1;
}

int fc_port_write_file(const char *path, const void *data, size_t len)
{
    char p[96];
    return file_path(FILE_DIR, path, p, sizeof(p)) == 0 ? write_file(p, data, len) : -1;
}

/* ---- system ----------------------------------------------------------------- */

/* The kernel heap, which thread stacks come from: its size and what is free. */
void fc_kernel_heap_stats(size_t *total, size_t *free_now)
{
    extern struct k_heap _system_heap;
    struct sys_memory_stats st = {0};
    sys_heap_runtime_stats_get(&_system_heap.heap, &st);
    *total = st.free_bytes + st.allocated_bytes;
    *free_now = st.free_bytes;
}

#ifdef CONFIG_COMMON_LIBC_MALLOC
/* With Zephyr's own malloc, nothing says how much of the heap is free. */
void fc_heap_stats(size_t *total, size_t *free_now, size_t *free_min)
{
    *total = *free_now = *free_min = 0;
}
#endif

void fc_port_sysinfo(fc_sysinfo_t *si)
{
    memset(si, 0, sizeof(*si));
    snprintf(si->architecture, sizeof(si->architecture), "%s", CONFIG_ARCH);
    snprintf(si->operating_system, sizeof(si->operating_system), "zephyr");
    snprintf(si->os_version, sizeof(si->os_version), "%s", KERNEL_VERSION_STRING);
    /* The chip's unique id where the board has one; else a name per board. */
    uint8_t id[8];
    ssize_t n = hwinfo_get_device_id(id, sizeof(id));
    if (n > 0) {
        for (ssize_t i = n > 6 ? n - 6 : 0, j = 0; i < n; i++, j += 2) {
            snprintf(si->hw_id + j, sizeof(si->hw_id) - (size_t)j, "%02x", id[i]);
        }
    } else {
        snprintf(si->hw_id, sizeof(si->hw_id), "%s", CONFIG_BOARD);
    }
    snprintf(si->hostname, sizeof(si->hostname), "zephyr-%s", si->hw_id);
    si->cpu_count = CONFIG_MP_MAX_NUM_CPUS;
    size_t total, free_now, free_min;
    fc_heap_stats(&total, &free_now, &free_min);
    si->mem_total = total;
    si->mem_free = free_now;
    struct fs_statvfs vfs;
    if (fs_statvfs(MNT, &vfs) == 0) {
        si->disk_total = (uint64_t)vfs.f_blocks * vfs.f_frsize;
        si->disk_free = (uint64_t)vfs.f_bfree * vfs.f_frsize;
    }
}

void fc_port_custom_info(void *obj)
{
    cJSON_AddStringToObject((cJSON *)obj, "deviceClass", "zephyr");
    cJSON_AddStringToObject((cJSON *)obj, "board", CONFIG_BOARD_TARGET);
    size_t total, free_now, free_min;
    char num[24];
    fc_heap_stats(&total, &free_now, &free_min);
    snprintf(num, sizeof(num), "%u", (unsigned)free_min);
    cJSON_AddStringToObject((cJSON *)obj, "memMinFreeBytes", num);
}

void fc_port_reboot(void)
{
    sys_reboot(SYS_REBOOT_COLD);
}

/* Zephyr's default is to stop for ever on a fatal error (a fault, a stack
 * overflow): right on a desk with a debugger, not for a device no one is
 * near. Start again instead, as the ESP-IDF firmware does. What happened is
 * kept in RAM that a restart does not clear, and said at the next boot: the
 * console may be gone before a message gets out. */
#define FATAL_MAGIC 0x46544c21u

static __noinit struct {
    uint32_t magic;
    uint32_t reason;
    uint32_t pc, lr;
    uint32_t uptime_ms;
    char thread[16];
} s_fatal;

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    const char *name = k_thread_name_get(k_current_get());
    s_fatal.magic = FATAL_MAGIC;
    s_fatal.reason = reason;
    s_fatal.uptime_ms = k_uptime_get_32();
    s_fatal.pc = s_fatal.lr = 0;
#ifdef CONFIG_CPU_CORTEX_M
    if (esf != NULL) {
        s_fatal.pc = esf->basic.pc;
        s_fatal.lr = esf->basic.lr;
    }
#endif
    snprintf(s_fatal.thread, sizeof(s_fatal.thread), "%s", name != NULL ? name : "");
    sys_reboot(SYS_REBOOT_COLD);
    CODE_UNREACHABLE;
}

/* Zephyr's reasons (K_ERR_*): the ones seen in practice by name. */
static const char *fatal_reason(uint32_t reason)
{
    switch (reason) {
    case K_ERR_CPU_EXCEPTION:
        return "CPU exception";
    case K_ERR_STACK_CHK_FAIL:
        return "stack overflow";
    case K_ERR_KERNEL_OOPS:
        return "kernel oops";
    case K_ERR_KERNEL_PANIC:
        return "kernel panic";
    default:
        return "fatal error";
    }
}

static void report_last_fatal(void)
{
    if (s_fatal.magic != FATAL_MAGIC) {
        return;
    }
    s_fatal.magic = 0;
    s_fatal.thread[sizeof(s_fatal.thread) - 1] = '\0';
    fc_port_log(FC_LOG_ERROR, "restarted after a %s (%u) in thread \"%s\", %u ms after boot, pc 0x%08x lr 0x%08x",
                fatal_reason(s_fatal.reason), (unsigned)s_fatal.reason, s_fatal.thread, (unsigned)s_fatal.uptime_ms,
                (unsigned)s_fatal.pc, (unsigned)s_fatal.lr);
}

void fc_port_log(fc_log_level_t level, const char *fmt, ...)
{
    static const char tags[] = "EWID";
#ifndef CONFIG_FC_LOG_DEBUG
    if (level > FC_LOG_INFO) {
        return;
    }
#endif
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    printk("%c (%u) fc: %s\n", tags[level], (unsigned)k_uptime_get_32(), line);
}

/* ---- firmware --------------------------------------------------------------- */

const char *fc_port_fw_platform(void)
{
    return CONFIG_BOARD;
}

/* No firmware updates yet: they need MCUboot, which native_sim does not run. */
int fc_port_ota_begin(size_t size, void **handle)
{
    return -1;
}

int fc_port_ota_write(void *handle, const void *data, size_t len)
{
    return -1;
}

int fc_port_ota_end(void *handle, int commit)
{
    return -1;
}

int fc_port_fw_read(size_t off, void *buf, size_t len)
{
    return -1;
}

int fc_port_ota_pending_verify(void)
{
    return 0;
}

void fc_port_ota_confirm(void) {}

void fc_port_ota_rollback(void) {}

/* ---- device key in hardware: none ------------------------------------------- */

struct mbedtls_pk_context;

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
