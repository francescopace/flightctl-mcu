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
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/printk-hooks.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/sys_heap.h>
#include <zephyr/version.h>

#include "cJSON.h"
#include "fc_fatal.h"
#include "fc_log.h"
#include "fc_port.h"

#if defined(CONFIG_XTENSA) && defined(CONFIG_XTENSA_ENABLE_BACKTRACE)
/* Zephyr's own Xtensa backtrace (arch/xtensa/include, added by CMakeLists.txt). */
#include <xtensa_asm2_context.h>
#include <xtensa_backtrace.h>
#include <xtensa_stack.h>
#if XCHAL_HAVE_WINDOWED
#define FC_XTENSA_BACKTRACE 1
#endif
#endif

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
static char s_fatal_text[272]; /* for customInfo: empty unless the last restart was a fatal error */

/* ---- Zephyr's own lines into the device's log ----------------------------------
 * What Zephyr prints (printk, and its minimal log: "E: ...", a fatal error's
 * report, the WiFi driver's) passes here a character at a time: printed as
 * before, and each line kept (fc_log_put_system). The agent's lines ("fc:")
 * are kept by fc_port_log. A line ended in an interrupt or an exception, as a
 * fault's report is, waits in s_isr until a thread keeps it, or until the
 * fatal error's handler does, before the restart. */
static printk_hook_fn_t s_printk_out;
static struct k_spinlock s_pk_lock;
static char s_pk_line[160];
static size_t s_pk_len;
static char s_isr[384]; /* lines, each ending with a newline */
static size_t s_isr_len;

static const char *fatal_reason(uint32_t reason);

/* A line of Zephyr's log, "E (12) tag: text" or "E: text" (the minimal
 * log's, a fault's report), kept as "tag: text"; a bare printk (the
 * simulated LED, the boot banner) is only printed. Its buffer is static, as
 * the stack of a thread that prints may be small: only one caller at a time
 * (s_pk_mutex, or the fatal error's handler). */
static void keep_zephyr_line(const char *line, void (*put)(fc_log_level_t, const char *))
{
    static char text[sizeof(s_pk_line) + 8];
    fc_log_level_t level = FC_LOG_INFO;
    int tagged = line[0] != '\0' && strchr("EWID", line[0]) != NULL;
    const char *close = strchr(line, ')');
    if (tagged && line[1] == ' ' && line[2] == '(' && close != NULL && close[1] == ' ') {
        if (strncmp(close + 2, "fc: ", 4) == 0) {
            return; /* the agent's: kept already */
        }
        snprintf(text, sizeof(text), "%s", close + 2);
    } else if (tagged && line[1] == ':' && line[2] == ' ') {
        snprintf(text, sizeof(text), "zephyr: %s", line + 3);
    } else {
        return;
    }
    level = line[0] == 'E' ? FC_LOG_ERROR : line[0] == 'W' ? FC_LOG_WARN : FC_LOG_INFO;
    put(level, text);
}

static void put_fatal(fc_log_level_t level, const char *text)
{
    (void)level;
    fc_log_put_fatal(text);
}

/* The lines waiting in s_isr, kept by a thread (or, with put_fatal, by the
 * fatal error's handler). */
static void keep_isr_lines(void (*put)(fc_log_level_t, const char *))
{
    static char lines[sizeof(s_isr)];
    k_spinlock_key_t key = k_spin_lock(&s_pk_lock);
    size_t n = s_isr_len;
    memcpy(lines, s_isr, n);
    s_isr_len = 0;
    k_spin_unlock(&s_pk_lock, key);
    for (size_t at = 0; at < n;) {
        char *nl = memchr(lines + at, '\n', n - at);
        if (nl == NULL) {
            break;
        }
        *nl = '\0';
        keep_zephyr_line(lines + at, put);
        at = (size_t)(nl - lines) + 1;
    }
}

static K_MUTEX_DEFINE(s_pk_mutex);

static int printk_keep(int c)
{
    static char line[sizeof(s_pk_line)];
    int r = s_printk_out != NULL ? s_printk_out(c) : c;
    if (c != '\n' && s_pk_len < sizeof(s_pk_line) - 1) {
        k_spinlock_key_t key = k_spin_lock(&s_pk_lock);
        if (c != '\r' && s_pk_len < sizeof(s_pk_line) - 1) {
            s_pk_line[s_pk_len++] = (char)c;
        }
        k_spin_unlock(&s_pk_lock, key);
        return r;
    }
    /* A line ends: a thread that can take the mutex keeps it now, anyone
     * else (an interrupt, an exception) leaves it in s_isr. */
    int now = !k_is_in_isr() && k_mutex_lock(&s_pk_mutex, K_NO_WAIT) == 0;
    k_spinlock_key_t key = k_spin_lock(&s_pk_lock);
    s_pk_line[s_pk_len] = '\0';
    if (now) {
        memcpy(line, s_pk_line, s_pk_len + 1);
    } else if (s_isr_len + s_pk_len + 1 <= sizeof(s_isr)) {
        memcpy(s_isr + s_isr_len, s_pk_line, s_pk_len);
        s_isr_len += s_pk_len;
        s_isr[s_isr_len++] = '\n';
    }
    s_pk_len = 0;
    if (c != '\n' && c != '\r') {
        s_pk_line[s_pk_len++] = (char)c; /* a line too long: the rest is the next */
    }
    int pending = s_isr_len > 0;
    k_spin_unlock(&s_pk_lock, key);
    if (now) {
        if (pending) {
            keep_isr_lines(fc_log_put_system);
        }
        keep_zephyr_line(line, fc_log_put_system);
        k_mutex_unlock(&s_pk_mutex);
    }
    return r;
}

int fc_port_zephyr_init(void)
{
    s_printk_out = __printk_get_hook();
    __printk_hook_install(printk_keep);
    report_last_fatal();
    int rc = fs_mount(&s_mnt); /* formats a blank partition */
    if (rc != 0) {
        fc_port_log(FC_LOG_ERROR, "cannot mount the storage (%d)", rc);
        return -1;
    }
    /* Made at the first boot; asked again, LittleFS logs an error. */
    struct fs_dirent st;
    if (fs_stat(KV_DIR, &st) != 0) {
        fs_mkdir(KV_DIR);
    }
    if (fs_stat(FILE_DIR, &st) != 0) {
        fs_mkdir(FILE_DIR);
    }
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

static int remove_file(const char *p)
{
    struct fs_dirent st;
    if (fs_stat(p, &st) != 0) {
        return 0; /* not there: LittleFS would log an error for the unlink */
    }
    int rc = fs_unlink(p);
    return rc == 0 || rc == -ENOENT ? 0 : -1;
}

int fc_port_kv_del(const char *key)
{
    char p[96];
    return file_path(KV_DIR, key, p, sizeof(p)) == 0 ? remove_file(p) : -1;
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

int fc_port_remove_file(const char *path)
{
    char p[FILE_PATH_MAX];
    return file_path(FILE_DIR, path, p, sizeof(p)) == 0 ? remove_file(p) : -1;
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
    /* The fatal error the last restart came from, as ESP-IDF's resetReason
     * says "panic": the console may have been gone before it got out. */
    if (s_fatal_text[0] != '\0') {
        cJSON_AddStringToObject((cJSON *)obj, "resetReason", "panic");
        cJSON_AddStringToObject((cJSON *)obj, "lastFatalError", s_fatal_text);
    }
#if defined(CONFIG_MCUBOOT_IMG_MANAGER)
    /* The running image's: no update below it is installed. */
    char num[12];
    snprintf(num, sizeof(num), "%u", (unsigned)fc_zephyr_security_counter(PARTITION_ID(slot0_partition)));
    cJSON_AddStringToObject((cJSON *)obj, "secureVersion", num);
#endif
}

long fc_port_heap_watch(int start)
{
    return -1;
}

long fc_port_heap_largest(void)
{
    return -1; /* sys_heap does not tell */
}

long fc_port_heap_low(void)
{
    return -1;
}

long fc_port_heap_free(void)
{
    /* Not yet: the C heap (fc_heap_stats) is where TLS and cJSON allocate,
     * but an app's memory and the module's pieces are split between it and
     * the PSRAM heap, and the floor and the .wasm limit (fc_apps.c) have not
     * been measured with Zephyr. */
    return -1;
}


void fc_port_metrics(void *obj)
{
    size_t total, free_now, free_min;
    char num[24];
    fc_heap_stats(&total, &free_now, &free_min);
    snprintf(num, sizeof(num), "%u", (unsigned)free_min);
    cJSON_AddStringToObject((cJSON *)obj, "memMinFreeBytes", num);
#if K_HEAP_MEM_POOL_SIZE > 0 && defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
    /* The kernel heap, apart from the C heap above: the threads' stacks
     * (the agent's and each app's) come from it, and the WiFi driver's. */
    extern struct k_heap _system_heap;
    struct sys_memory_stats st = {0};
    if (sys_heap_runtime_stats_get(&_system_heap.heap, &st) == 0) {
        snprintf(num, sizeof(num), "%u", (unsigned)st.free_bytes);
        cJSON_AddStringToObject((cJSON *)obj, "memKernelFreeBytes", num);
        snprintf(num, sizeof(num), "%u", (unsigned)(st.free_bytes + st.allocated_bytes - st.max_allocated_bytes));
        cJSON_AddStringToObject((cJSON *)obj, "memKernelMinFreeBytes", num);
    }
#endif
}

long fc_port_stack_unused(void)
{
#if defined(CONFIG_INIT_STACKS) && defined(CONFIG_THREAD_STACK_INFO)
    size_t unused;
    if (k_thread_stack_space_get(k_current_get(), &unused) == 0) {
        return (long)unused;
    }
#endif
    return -1;
}

void fc_port_reboot(void)
{
    sys_reboot(SYS_REBOOT_COLD);
}

/* Zephyr's default is to stop for ever on a fatal error (a fault, a stack
 * overflow): right on a desk with a debugger, not for a device no one is
 * near. Start again instead, as the ESP-IDF firmware does. What happened is
 * kept in RAM that a restart does not clear, in the record and the text the
 * ESP-IDF port uses too (fc_fatal.h), and said at the next boot: the console
 * may be gone before a message gets out. */
static __noinit fc_fatal_t s_fatal;

#ifdef FC_XTENSA_BACKTRACE
/* A return address as a pc: the window bits replaced with the code region's,
 * then back into the call (xtensa_backtrace.c, where it is static). */
static uint32_t xtensa_return_pc(uint32_t pc, uint32_t mask, int cause)
{
    if (pc & 0x80000000u) {
        pc = (pc & 0x3fffffffu) | (cause != EXCCAUSE_INSTR_PROHIBITED ? mask : 0x40000000u);
    }
    return pc - 3;
}

/* The faulting instruction and the return addresses, innermost first, as
 * Zephyr prints them in its backtrace (xtensa_backtrace_print). The esf is
 * the interrupted stack, whose first word points to the saved registers. A
 * k_oops() or k_panic() faults inside xtensa_arch_except: its caller is the
 * first return address. */
static void xtensa_fatal_addrs(const struct arch_esf *esf)
{
    _xtensa_irq_stack_frame_raw_t *frame = (void *)esf;
    if (!xtensa_is_frame_pointer_valid(frame)) {
        return;
    }
    const _xtensa_irq_bsa_t *bsa = frame->ptr_to_bsa;
    s_fatal.pc = bsa->pc;
    fc_fatal_seal(&s_fatal);
    struct xtensa_backtrace_frame_t fr = {.cause = (int)bsa->exccause, .mask = 0x40000000u};
    xtensa_backtrace_get_start(&fr.pc, &fr.sp, &fr.next_pc, (int *)esf);
    if (fr.cause != EXCCAUSE_INSTR_PROHIBITED) {
        fr.mask = fr.pc & 0xc0000000u;
    }
    /* Sealed one address at a time: a fault while walking the stack still
     * leaves the record. */
    while (fr.next_pc != 0 && s_fatal.naddrs < FC_FATAL_ADDRS) {
        bool ok = xtensa_backtrace_get_next_frame(&fr);
        s_fatal.addrs[s_fatal.naddrs++] = xtensa_return_pc(fr.pc, fr.mask, fr.cause);
        fc_fatal_seal(&s_fatal);
        if (!ok) {
            break;
        }
    }
}
#endif

void k_sys_fatal_error_handler(unsigned int reason, const struct arch_esf *esf)
{
    memset(&s_fatal, 0, sizeof(s_fatal));
    s_fatal.code = reason;
    s_fatal.uptime_ms = k_uptime_get_32();
    fc_fatal_copy(s_fatal.reason, sizeof(s_fatal.reason), fatal_reason(reason));
    fc_fatal_copy(s_fatal.thread, sizeof(s_fatal.thread), k_thread_name_get(k_current_get()));
    fc_fatal_copy(s_fatal.firmware, sizeof(s_fatal.firmware), CONFIG_FC_FW_VERSION);
#ifdef CONFIG_CPU_CORTEX_M
    s_fatal.kind = FC_FATAL_LR;
    if (esf != NULL) {
        s_fatal.pc = esf->basic.pc;
        s_fatal.addrs[s_fatal.naddrs++] = esf->basic.lr;
    }
#elif defined(FC_XTENSA_BACKTRACE)
    s_fatal.kind = FC_FATAL_BACKTRACE;
#endif
    fc_fatal_seal(&s_fatal);
#ifdef FC_XTENSA_BACKTRACE
    if (esf != NULL) {
        xtensa_fatal_addrs(esf);
    }
#endif
    /* The last lines of this boot's log: the report Zephyr printed, then
     * what this handler knows, as the ESP-IDF port writes it. */
    keep_isr_lines(put_fatal);
    /* In s_fatal_text, which this boot no longer needs: the S3's RAM is
     * short of another buffer. */
    memcpy(s_fatal_text, "fatal: ", 7);
    fc_fatal_format(&s_fatal, NULL, s_fatal_text + 7, sizeof(s_fatal_text) - 7);
    fc_log_put_fatal(s_fatal_text);
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

static int s_restart_was_crash;

int fc_zephyr_restart_was_crash(void)
{
    return s_restart_was_crash;
}

static void report_last_fatal(void)
{
    /* Believed only if it checks out: after a power cut this RAM holds
     * anything. */
    if (!fc_fatal_valid(&s_fatal)) {
        return;
    }
    s_restart_was_crash = 1;
    fc_fatal_format(&s_fatal, CONFIG_FC_FW_VERSION, s_fatal_text, sizeof(s_fatal_text));
    s_fatal.magic = 0;
    fc_port_log(FC_LOG_ERROR, "restarted after a %s", s_fatal_text);
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
    fc_log_put(level, line);
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

/* An image's security counter (tools: SECURE_VERSION, port/zephyr/build.sh),
 * from its protected TLVs; 0 when it has none. MCUboot installs no image with
 * a lower one than the running image's (sysbuild/mcuboot.conf): the device
 * says so before it restarts, as the ESP-IDF firmware does. */
#define TLV_PROT_MAGIC 0x6908
#define TLV_SEC_CNT    0x50

uint32_t fc_zephyr_security_counter(uint8_t slot)
{
    const struct flash_area *fa;
    uint32_t cnt = 0;
    if (flash_area_open(slot, &fa) != 0) {
        return 0;
    }
    uint8_t hdr[16], tlv[4];
    if (flash_area_read(fa, 0, hdr, sizeof(hdr)) == 0 && sys_get_le32(hdr) == 0x96f3b83dU) {
        off_t off = (off_t)sys_get_le16(hdr + 8) + (off_t)sys_get_le32(hdr + 12);
        if (flash_area_read(fa, off, tlv, sizeof(tlv)) == 0 && sys_get_le16(tlv) == TLV_PROT_MAGIC) {
            off_t end = off + sys_get_le16(tlv + 2);
            for (off += 4; off + 4 <= end; off += 4 + sys_get_le16(tlv + 2)) {
                if (flash_area_read(fa, off, tlv, sizeof(tlv)) != 0) {
                    break;
                }
                uint8_t v[4];
                if (sys_get_le16(tlv) == TLV_SEC_CNT && sys_get_le16(tlv + 2) == 4 &&
                    flash_area_read(fa, off + 4, v, sizeof(v)) == 0) {
                    cnt = sys_get_le32(v);
                    break;
                }
            }
        }
    }
    flash_area_close(fa);
    return cnt;
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
        } else if (fc_zephyr_security_counter(SLOT_NEXT) < fc_zephyr_security_counter(SLOT_RUNNING)) {
            fc_port_log(FC_LOG_ERROR, "ota: security version %u, below this device's %u",
                        (unsigned)fc_zephyr_security_counter(SLOT_NEXT),
                        (unsigned)fc_zephyr_security_counter(SLOT_RUNNING));
            rc = FC_OTA_TOO_OLD;
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
