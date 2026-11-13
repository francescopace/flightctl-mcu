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
    .storage_dev = (void *)PARTITION_ID(fc_storage_partition),
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
    char tmp[192];
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
    struct fs_dirent st;
    if (fs_stat(p, &st) != 0) {
        return 0; /* not there: LittleFS would log an error for the unlink */
    }
    int rc = fs_unlink(p);
    return rc == 0 || rc == -ENOENT ? 0 : -1;
}

/* Long enough for a registry's CA, /etc/containers/certs.d/<host:port>/ca.crt */
#define FILE_PATH_MAX 160

int fc_port_write_file(const char *path, const void *data, size_t len)
{
    char p[FILE_PATH_MAX];
    return file_path(FILE_DIR, path, p, sizeof(p)) == 0 ? write_file(p, data, len) : -1;
}

int fc_port_read_file(const char *path, char **out, size_t *len)
{
    char p[FILE_PATH_MAX];
    return file_path(FILE_DIR, path, p, sizeof(p)) == 0 ? read_file(p, out, len) : -1;
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
#ifdef CONFIG_SCHED_THREAD_USAGE_ALL
    /* Cycles: all of them, and those outside the idle threads. */
    k_thread_runtime_stats_t rt;
    if (k_thread_runtime_stats_all_get(&rt) == 0) {
        si->cpu_total = rt.execution_cycles;
        si->cpu_busy = rt.total_cycles;
    }
#endif
}

void fc_port_custom_info(void *obj)
{
    cJSON_AddStringToObject((cJSON *)obj, "deviceClass", "zephyr");
    cJSON_AddStringToObject((cJSON *)obj, "board", CONFIG_BOARD_TARGET);
}

void fc_port_metrics(void *obj)
{
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
    return CONFIG_FC_FW_PLATFORM;
}

#if defined(CONFIG_MCUBOOT_IMG_MANAGER)
/* ---- firmware update: MCUboot's two slots -------------------------------------
 * The image (signed for MCUboot: zephyr.signed.bin) is streamed into the
 * second slot and asked for as a test: MCUboot checks its signature at the
 * next boot, swaps the slots, and swaps them back at the boot after that
 * unless the image was confirmed. */
#include <zephyr/dfu/flash_img.h>
#include <zephyr/dfu/mcuboot.h>

#include "fc_crypto.h"

#define SLOT_RUNNING PARTITION_ID(slot0_partition)
#define SLOT_NEXT    PARTITION_ID(slot1_partition)
typedef struct {
    struct flash_img_context img;
    /* Of what was given, to compare with what the slot holds: a flash driver
     * may report a write it did not do (the RP2040's did, for short ones, in
     * Zephyr 4.3: docs/ZEPHYR.md). */
    fc_sha256_t sha;
    size_t size, written;
} ota_t;

int fc_port_ota_begin(size_t size, void **handle)
{
    const struct flash_area *fa;
    if (flash_area_open(SLOT_NEXT, &fa) != 0) {
        return -1;
    }
    size_t room = fa->fa_size;
    flash_area_close(fa);
    if (size == 0 || size > room) {
        fc_port_log(FC_LOG_ERROR, "ota: an image of %u bytes for a slot of %u", (unsigned)size, (unsigned)room);
        return -1;
    }
    ota_t *o = calloc(1, sizeof(*o));
    if (o == NULL) {
        return -1;
    }
    if (flash_img_init_id(&o->img, SLOT_NEXT) != 0) {
        free(o);
        return -1;
    }
    fc_sha256_start(&o->sha);
    o->size = size;
    *handle = o;
    return 0;
}

int fc_port_ota_write(void *handle, const void *data, size_t len)
{
    ota_t *o = handle;
    if (o->written + len > o->size) {
        return -1;
    }
    int rc = flash_img_buffered_write(&o->img, data, len, false);
    if (rc != 0) {
        fc_port_log(FC_LOG_ERROR, "ota: flash write at %u: %d", (unsigned)o->written, rc);
        return -1;
    }
    fc_sha256_update(&o->sha, data, len);
    o->written += len;
    return 0;
}

/* 0 when the slot holds what was written. */
static int slot_matches(ota_t *o)
{
    uint8_t want[32], got[32], buf[256];
    const struct flash_area *fa;
    if (fc_sha256_finish(&o->sha, want) != 0 || flash_area_open(SLOT_NEXT, &fa) != 0) {
        return -1;
    }
    fc_sha256_t sha;
    fc_sha256_start(&sha);
    int rc = 0;
    for (size_t off = 0; off < o->size && rc == 0; off += sizeof(buf)) {
        size_t n = o->size - off < sizeof(buf) ? o->size - off : sizeof(buf);
        rc = flash_area_read(fa, (off_t)off, buf, n);
        fc_sha256_update(&sha, buf, n);
    }
    if (fc_sha256_finish(&sha, got) != 0) {
        rc = -1;
    }
    flash_area_close(fa);
    return rc == 0 && memcmp(want, got, sizeof(want)) == 0 ? 0 : -1;
}

int fc_port_ota_end(void *handle, int commit)
{
    ota_t *o = handle;
    int rc = -1;
    if (commit && o->written == o->size) {
        struct mcuboot_img_header hdr;
        if (flash_img_buffered_write(&o->img, (const uint8_t *)"", 0, true) != 0) {
            fc_port_log(FC_LOG_ERROR, "ota: cannot write the end of the image");
        } else if (slot_matches(o) != 0) {
            fc_port_log(FC_LOG_ERROR, "ota: the slot does not hold what was written");
        } else if (boot_read_bank_header(SLOT_NEXT, &hdr, sizeof(hdr)) != 0) {
            fc_port_log(FC_LOG_ERROR, "ota: not an image for MCUboot");
        } else if (boot_request_upgrade(BOOT_UPGRADE_TEST) != 0 || mcuboot_swap_type() != BOOT_SWAP_TYPE_TEST) {
            fc_port_log(FC_LOG_ERROR, "ota: MCUboot does not take the request to try the image");
        } else {
            rc = 0;
        }
    }
    fc_sha256_abort(&o->sha);
    free(o);
    return rc;
}

int fc_port_fw_read(size_t off, void *buf, size_t len)
{
    const struct flash_area *fa;
    if (flash_area_open(SLOT_RUNNING, &fa) != 0) {
        return -1;
    }
    int rc = off + len <= fa->fa_size ? flash_area_read(fa, (off_t)off, buf, len) : -1;
    flash_area_close(fa);
    return rc == 0 ? 0 : -1;
}

int fc_port_ota_pending_verify(void)
{
    return !boot_is_img_confirmed();
}

void fc_port_ota_confirm(void)
{
    int rc = boot_write_img_confirmed();
    if (rc != 0 || !boot_is_img_confirmed()) {
        fc_port_log(FC_LOG_ERROR, "ota: cannot confirm the running image (%d): the next boot goes back", rc);
    }
}

/* An image that was not confirmed is swapped back by MCUboot at the next boot. */
void fc_port_ota_rollback(void)
{
    fc_port_reboot();
}
#else
/* No firmware updates without MCUboot (native_sim does not run it). */
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
#endif

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

int fc_port_hwkey_pk(struct mbedtls_pk_context *pk, const struct mbedtls_pk_context *pub)
{
    return -1;
}

void fc_port_hwkey_forget(void) {}
