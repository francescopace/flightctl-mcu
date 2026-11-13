/*
 * Host (Linux/macOS) port: a "fake device" used for development and CI.
 * Key/value pairs and config files live under the data directory.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

#include "cJSON.h"
#include "fc_port.h"

#ifdef __APPLE__
#include <mach/mach.h>
#include <sys/sysctl.h>
#endif

static char s_data_dir[512] = "build/host-data";
static int s_debug;

void fc_port_host_init(const char *data_dir, int debug)
{
    snprintf(s_data_dir, sizeof(s_data_dir), "%s", data_dir);
    mkdir(s_data_dir, 0700);
    char files[600];
    snprintf(files, sizeof(files), "%s/files", s_data_dir);
    mkdir(files, 0700);
    s_debug = debug;
}

static void kv_path(const char *key, char *out, size_t len)
{
    snprintf(out, len, "%s/%s", s_data_dir, key);
}

int fc_port_kv_get(const char *key, char **out, size_t *len)
{
    char p[600];
    kv_path(key, p, sizeof(p));
    FILE *f = fopen(p, "rb");
    if (f == NULL) {
        return -1;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (buf == NULL || fread(buf, 1, (size_t)n, f) != (size_t)n) {
        free(buf);
        fclose(f);
        return -1;
    }
    fclose(f);
    buf[n] = '\0';
    *out = buf;
    *len = (size_t)n;
    return 0;
}

int fc_port_kv_set(const char *key, const void *data, size_t len)
{
    char p[600], tmp[610];
    kv_path(key, p, sizeof(p));
    snprintf(tmp, sizeof(tmp), "%s.tmp", p);
    FILE *f = fopen(tmp, "wb");
    if (f == NULL) {
        return -1;
    }
    int ok = fwrite(data, 1, len, f) == len;
    ok = (fclose(f) == 0) && ok;
    chmod(tmp, 0600);
    return ok && rename(tmp, p) == 0 ? 0 : -1;
}

int fc_port_kv_del(const char *key)
{
    char p[600];
    kv_path(key, p, sizeof(p));
    return unlink(p) == 0 || errno == ENOENT ? 0 : -1;
}

/* Bytes in the files under `dir`. */
static uint64_t du(const char *dir, int depth)
{
    uint64_t total = 0;
    DIR *d = opendir(dir);
    if (d == NULL) {
        return 0;
    }
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        char p[1024];
        struct stat st;
        snprintf(p, sizeof(p), "%s/%s", dir, e->d_name);
        if (lstat(p, &st) != 0) {
            continue;
        }
        if (S_ISREG(st.st_mode)) {
            total += (uint64_t)st.st_size;
        } else if (S_ISDIR(st.st_mode) && depth < 8) {
            total += du(p, depth + 1);
        }
    }
    closedir(d);
    return total;
}

void fc_port_sysinfo(fc_sysinfo_t *si)
{
    struct utsname u;
    uname(&u);
    snprintf(si->architecture, sizeof(si->architecture), "%.31s", u.machine);
    snprintf(si->operating_system, sizeof(si->operating_system), "%.31s", u.sysname);
    snprintf(si->os_version, sizeof(si->os_version), "%.31s", u.release);
    for (char *c = si->operating_system; *c; c++) {
        if (*c >= 'A' && *c <= 'Z') {
            *c += 'a' - 'A';
        }
    }
    snprintf(si->hostname, sizeof(si->hostname), "host-%.58s", u.nodename);
    for (char *c = si->hostname; *c; c++) {
        if (*c == '.') {
            *c = '\0';
            break;
        }
    }
    si->cpu_count = (int)sysconf(_SC_NPROCESSORS_ONLN);
#ifdef __APPLE__
    uint64_t mem = 0;
    size_t sz = sizeof(mem);
    sysctlbyname("hw.memsize", &mem, &sz, NULL, 0);
    si->mem_total = mem;
    vm_statistics64_data_t vm;
    mach_msg_type_number_t count = HOST_VM_INFO64_COUNT;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&vm, &count) == KERN_SUCCESS) {
        si->mem_free = (uint64_t)(vm.free_count + vm.inactive_count) * (uint64_t)sysconf(_SC_PAGESIZE);
    }
#else
    si->mem_total = (uint64_t)sysconf(_SC_PHYS_PAGES) * (uint64_t)sysconf(_SC_PAGESIZE);
    si->mem_free = (uint64_t)sysconf(_SC_AVPHYS_PAGES) * (uint64_t)sysconf(_SC_PAGESIZE);
    /* MemFree leaves out the page cache, which the kernel gives back. */
    FILE *mi = fopen("/proc/meminfo", "r");
    if (mi != NULL) {
        char line[128];
        unsigned long long kb;
        while (fgets(line, sizeof(line), mi) != NULL) {
            if (sscanf(line, "MemAvailable: %llu kB", &kb) == 1) {
                si->mem_free = kb * 1024;
            }
        }
        fclose(mi);
    }
#endif
    /* The simulated device's storage: what its data directory holds, out of
     * FC_HOST_DISK_BYTES (default 640 KB, the ESP-IDF firmware's partition),
     * not the host's file system. */
    const char *quota = getenv("FC_HOST_DISK_BYTES");
    si->disk_total = quota != NULL ? strtoull(quota, NULL, 10) : 640 * 1024;
    uint64_t used = du(s_data_dir, 0);
    si->disk_free = used < si->disk_total ? si->disk_total - used : 0;
    /* The process stands for the device's CPU: its CPU time against the clock. */
    struct rusage ru;
    struct timespec ts;
    if (getrusage(RUSAGE_SELF, &ru) == 0 && clock_gettime(CLOCK_MONOTONIC, &ts) == 0) {
        si->cpu_busy = (uint64_t)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000 +
                       (uint64_t)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec);
        si->cpu_total = (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
    }
}

void fc_port_custom_info(void *obj)
{
    cJSON_AddStringToObject((cJSON *)obj, "deviceClass", "host-simulator");
}

void fc_port_metrics(void *obj) {}

/* The absolute path flattened into a single file name under files/. */
static void file_key(const char *path, char key[300])
{
    char name[256];
    size_t j = 0;
    for (const char *c = path; *c && j < sizeof(name) - 1; c++) {
        name[j++] = (*c == '/') ? '_' : *c;
    }
    name[j] = '\0';
    snprintf(key, 300, "files/%s", name);
}

int fc_port_write_file(const char *path, const void *data, size_t len)
{
    char key[300];
    file_key(path, key);
    return fc_port_kv_set(key, data, len);
}

int fc_port_read_file(const char *path, char **out, size_t *len)
{
    char key[300];
    file_key(path, key);
    return fc_port_kv_get(key, out, len);
}

/* OTA on the host: the image lands in <data_dir>/ota-next.bin and "reboot"
 * exits the process (the supervisor restarts it). FC_FW_PLATFORM selects the
 * chip to fetch from a multi-chip index, to test downloads without hardware. */
const char *fc_port_fw_platform(void)
{
    const char *p = getenv("FC_FW_PLATFORM");
    return p && *p ? p : "host";
}

int fc_port_ota_begin(size_t size, void **handle)
{
    char path[600];
    snprintf(path, sizeof(path), "%s/ota-next.bin.part", s_data_dir);
    FILE *f = fopen(path, "wb");
    *handle = f;
    return f ? 0 : -1;
}

int fc_port_ota_write(void *handle, const void *data, size_t len)
{
    return fwrite(data, 1, len, (FILE *)handle) == len ? 0 : -1;
}

int fc_port_ota_end(void *handle, int commit)
{
    char part[600], path[600];
    snprintf(part, sizeof(part), "%s/ota-next.bin.part", s_data_dir);
    snprintf(path, sizeof(path), "%s/ota-next.bin", s_data_dir);
    int rc = fclose((FILE *)handle) == 0 ? 0 : -1;
    if (!commit || rc != 0) {
        unlink(part);
        return commit ? -1 : 0;
    }
    return rename(part, path) == 0 ? 0 : -1;
}

/* The "running image" is the file FC_FW_IMAGE names, if any: with it, the
 * host takes firmware updates as deltas, like a device. */
int fc_port_fw_read(size_t off, void *buf, size_t len)
{
    const char *path = getenv("FC_FW_IMAGE");
    FILE *f = path && *path ? fopen(path, "rb") : NULL;
    if (f == NULL) {
        return -1;
    }
    int ok = fseek(f, (long)off, SEEK_SET) == 0 && fread(buf, 1, len, f) == len;
    fclose(f);
    return ok ? 0 : -1;
}

int fc_port_ota_pending_verify(void)
{
    return 0;
}

void fc_port_ota_confirm(void)
{
}

void fc_port_ota_rollback(void)
{
    fc_port_reboot();
}

void fc_port_reboot(void)
{
    fc_port_log(FC_LOG_INFO, "reboot requested: exiting");
    exit(0);
}

void fc_port_log(fc_log_level_t level, const char *fmt, ...)
{
    if (level == FC_LOG_DEBUG && !s_debug) {
        return;
    }
    static const char *names[] = {"E", "W", "I", "D"};
    char ts[32];
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    strftime(ts, sizeof(ts), "%H:%M:%S", &tm);
    fprintf(stderr, "%s %s fc: ", ts, names[level]);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* No hardware key store on the host: the device key stays in storage. */
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
