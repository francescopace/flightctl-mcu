/* STM32F767 single-bank layout: two adjacent 32 KiB sectors for agent data.
 * Keep the adjacent MCUboot and factory sectors outside the MTD device. */
#include <nuttx/config.h>
#include <nuttx/mtd/configdata.h>
#include <nuttx/mtd/mtd.h>
#include <nuttx/progmem.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>
#include "fc_port.h"

#define BASE 0x08010000u
#define SECTOR 0x8000u
#define SIZE (2 * SECTOR)
static pthread_mutex_t storage_lock = PTHREAD_MUTEX_INITIALIZER;

static int erase(struct mtd_dev_s *dev, off_t start, size_t count)
{
    if (start < 0 || start > 2 || count > 2 - (size_t)start) return -EINVAL;
    for (size_t i = 0; i < count; i++) {
        if (up_progmem_eraseblock(2 + start + i) != SECTOR) return -EIO;
    }
    return (int)count;
}

static ssize_t read_bytes(struct mtd_dev_s *dev, off_t pos, size_t n, uint8_t *data)
{
    if (pos < 0 || pos > SIZE || n > SIZE - pos) return -EINVAL;
    memcpy(data, (const void *)(BASE + pos), n);
    return n;
}

static ssize_t write_bytes(struct mtd_dev_s *dev, off_t pos, size_t n, const uint8_t *data)
{
    if (pos < 0 || pos > SIZE || n > SIZE - pos) return -EINVAL;
    return up_progmem_write(BASE + pos, data, n);
}

static int flash_ioctl(struct mtd_dev_s *dev, int cmd, unsigned long arg)
{
    if (cmd == MTDIOC_ERASESTATE) {
        *(uint8_t *)arg = 0xff;
        return 0;
    }
    if (cmd == MTDIOC_GEOMETRY) {
        struct mtd_geometry_s *geometry = (void *)arg;
        *geometry = (struct mtd_geometry_s){ .blocksize = 1, .erasesize = SECTOR, .neraseblocks = 2 };
        return 0;
    }
    return -ENOTTY;
}

static struct mtd_dev_s storage = {
    .erase = erase, .read = read_bytes, .write = write_bytes,
    .bread = read_bytes, .bwrite = write_bytes, .ioctl = flash_ioctl,
};

int fc_nuttx_storage_init(void)
{
    if (up_progmem_getaddress(2) != BASE || up_progmem_getaddress(4) != BASE + SIZE ||
        up_progmem_pagesize(2) != SECTOR || up_progmem_pagesize(3) != SECTOR) return -1;
    return mtdconfig_register(&storage);
}

static int record(struct config_data_s *item, const char *name, int file)
{
    if (!name || !*name || (file ? *name != '/' : strchr(name, '/') != NULL)) return -1;
    int n = snprintf(item->name, sizeof(item->name), "%c%s", file ? 'F' : 'K', name);
    return n > 0 && n < sizeof(item->name) ? open("/dev/config", O_RDWR) : -1;
}

static int load(const char *name, int file, char **out, size_t *len)
{
    struct config_data_s item = {0};
    pthread_mutex_lock(&storage_lock);
    int fd = record(&item, name, file);
    int rc = -1;
    if (fd >= 0) {
        if (ioctl(fd, CFGDIOC_GETCONFIG, (unsigned long)&item) < 0) {
            if (errno == ENOENT) rc = FC_KV_MISSING;
        } else if (item.len && item.len < SECTOR) {
            /* A prefix byte distinguishes an empty value from a deleted key. */
            size_t expected = item.len;
            char *buf = malloc(expected);
            item.configdata = (uint8_t *)buf;
            if (buf && ioctl(fd, CFGDIOC_GETCONFIG, (unsigned long)&item) == 0 &&
                item.len == expected && buf[0] == 1) {
                *len = expected - 1;
                memmove(buf, buf + 1, *len);
                buf[*len] = 0;
                *out = buf;
                rc = 0;
            } else {
                free(buf);
            }
        }
        close(fd);
    }
    pthread_mutex_unlock(&storage_lock);
    return rc;
}

static int update(const char *name, int file, const void *data, size_t len, int remove)
{
    if (len >= SECTOR) return -1;
    struct config_data_s item = {0};
    pthread_mutex_lock(&storage_lock);
    int fd = record(&item, name, file);
    int rc = -1;
    if (fd >= 0) {
        if (remove) {
            rc = ioctl(fd, CFGDIOC_DELCONFIG, (unsigned long)&item);
            if (rc < 0 && errno == ENOENT) rc = 0;
        } else {
            item.configdata = malloc(len + 1);
            if (item.configdata) {
                item.configdata[0] = 1;
                if (len) memcpy(item.configdata + 1, data, len);
                item.len = len + 1;
                rc = ioctl(fd, CFGDIOC_SETCONFIG, (unsigned long)&item);
                free(item.configdata);
            }
        }
        close(fd);
    }
    pthread_mutex_unlock(&storage_lock);
    return rc < 0 ? -1 : 0;
}

int fc_port_kv_get(const char *k, char **out, size_t *n) { return load(k, 0, out, n); }
int fc_port_kv_set(const char *k, const void *data, size_t n) { return update(k, 0, data, n, 0); }
int fc_port_kv_del(const char *k) { return update(k, 0, NULL, 0, 1); }
int fc_port_read_file(const char *p, char **out, size_t *n) { return load(p, 1, out, n) ? -1 : 0; }
int fc_port_write_file(const char *p, const void *data, size_t n) { return update(p, 1, data, n, 0); }
int fc_port_remove_file(const char *p) { return update(p, 1, NULL, 0, 1); }
