/*
 * Port for unit tests: key/value store and files in memory, no OTA, reboots
 * counted instead of performed. Tests can read and reset the state.
 */
#include "port_stub.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/pk.h"
#include "mbedtls/rsa.h"

#define MAX_ENTRIES 64

typedef struct {
    char key[64];
    char *data;
    size_t len;
} entry_t;

static entry_t s_kv[MAX_ENTRIES], s_files[MAX_ENTRIES];
int stub_reboots;
int stub_verbose;
int stub_hwkey_bits;
int stub_hwkey_seal_fails;
int stub_hwkey_seals;

static entry_t *find(entry_t *t, const char *key, int create)
{
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (t[i].data != NULL && strcmp(t[i].key, key) == 0) {
            return &t[i];
        }
    }
    if (!create) {
        return NULL;
    }
    for (int i = 0; i < MAX_ENTRIES; i++) {
        if (t[i].data == NULL) {
            snprintf(t[i].key, sizeof(t[i].key), "%s", key);
            return &t[i];
        }
    }
    return NULL;
}

static int put(entry_t *t, const char *key, const void *data, size_t len)
{
    entry_t *e = find(t, key, 1);
    if (e == NULL) {
        return -1;
    }
    free(e->data);
    e->data = malloc(len + 1);
    memcpy(e->data, data, len);
    e->data[len] = '\0';
    e->len = len;
    return 0;
}

void stub_reset(void)
{
    for (int i = 0; i < MAX_ENTRIES; i++) {
        free(s_kv[i].data);
        free(s_files[i].data);
        s_kv[i].data = s_files[i].data = NULL;
    }
    stub_reboots = 0;
    fc_port_hwkey_forget();
    stub_hwkey_bits = 0;
    stub_hwkey_seal_fails = 0;
    stub_hwkey_seals = 0;
}

const char *stub_kv(const char *key)
{
    entry_t *e = find(s_kv, key, 0);
    return e ? e->data : NULL;
}

const char *stub_file(const char *path)
{
    entry_t *e = find(s_files, path, 0);
    return e ? e->data : NULL;
}

int fc_port_kv_get(const char *key, char **out, size_t *len)
{
    entry_t *e = find(s_kv, key, 0);
    if (e == NULL) {
        return -1;
    }
    *out = malloc(e->len + 1);
    memcpy(*out, e->data, e->len + 1);
    *len = e->len;
    return 0;
}

int fc_port_kv_set(const char *key, const void *data, size_t len)
{
    return put(s_kv, key, data, len);
}

int fc_port_kv_del(const char *key)
{
    entry_t *e = find(s_kv, key, 0);
    if (e != NULL) {
        free(e->data);
        e->data = NULL;
    }
    return 0;
}

void fc_port_sysinfo(fc_sysinfo_t *si)
{
    memset(si, 0, sizeof(*si));
    snprintf(si->architecture, sizeof(si->architecture), "test");
    snprintf(si->operating_system, sizeof(si->operating_system), "test");
    snprintf(si->os_version, sizeof(si->os_version), "1");
    snprintf(si->hostname, sizeof(si->hostname), "unit");
    si->mem_total = 100;
    si->mem_free = 50;
}

void fc_port_custom_info(void *obj) {}

int fc_port_write_file(const char *path, const void *data, size_t len)
{
    return put(s_files, path, data, len);
}

const char *fc_port_fw_platform(void)
{
    return "test";
}

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
int fc_port_ota_pending_verify(void)
{
    return 0;
}
void fc_port_ota_confirm(void) {}
void fc_port_ota_rollback(void) {}

void fc_port_reboot(void)
{
    stub_reboots++;
}

void fc_port_log(fc_log_level_t level, const char *fmt, ...)
{
    if (!stub_verbose) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
}

/* "Hardware" key store: the sealed key is kept here, out of the identity's
 * storage, and signs through an RSA_ALT context as the DS peripheral does. */
static mbedtls_pk_context s_hw;
static int s_hw_sealed;

int fc_port_hwkey_bits(void)
{
    return stub_hwkey_bits;
}

int fc_port_hwkey_seal(struct mbedtls_pk_context *pk)
{
    if (stub_hwkey_seal_fails || mbedtls_pk_get_type(pk) != MBEDTLS_PK_RSA) {
        return -1;
    }
    fc_port_hwkey_forget();
    mbedtls_pk_setup(&s_hw, mbedtls_pk_info_from_type(MBEDTLS_PK_RSA));
    if (mbedtls_rsa_copy(mbedtls_pk_rsa(s_hw), mbedtls_pk_rsa(*pk)) != 0) {
        return -1;
    }
    s_hw_sealed = 1;
    stub_hwkey_seals++;
    return 0;
}

static int hw_sign(void *ctx, int (*f_rng)(void *, unsigned char *, size_t), void *p_rng, mbedtls_md_type_t md_alg,
                   unsigned int hashlen, const unsigned char *hash, unsigned char *sig)
{
    return mbedtls_rsa_pkcs1_sign((mbedtls_rsa_context *)ctx, f_rng, p_rng, md_alg, hashlen, hash, sig);
}

static size_t hw_len(void *ctx)
{
    return mbedtls_rsa_get_len((mbedtls_rsa_context *)ctx);
}

int fc_port_hwkey_pk(struct mbedtls_pk_context *pk)
{
    if (!s_hw_sealed) {
        return -1;
    }
    return mbedtls_pk_setup_rsa_alt(pk, mbedtls_pk_rsa(s_hw), NULL, hw_sign, hw_len);
}

void fc_port_hwkey_forget(void)
{
    mbedtls_pk_free(&s_hw);
    mbedtls_pk_init(&s_hw);
    s_hw_sealed = 0;
}
