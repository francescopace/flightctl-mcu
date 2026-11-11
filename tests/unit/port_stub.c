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
    stub_ota_enabled = 0;
    stub_ota_len = 0;
    stub_ota_commits = stub_ota_aborts = stub_ota_end_result = 0;
    stub_fw = NULL;
    stub_fw_len = 0;
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

/* No OTA unless a test turns it on: then the slot is stub_ota (what was
 * written to it) and the running image is stub_fw. */
int stub_ota_enabled;
unsigned char stub_ota[STUB_OTA_MAX];
size_t stub_ota_len;
int stub_ota_commits, stub_ota_aborts, stub_ota_end_result;
const unsigned char *stub_fw;
size_t stub_fw_len;

int fc_port_ota_begin(size_t size, void **handle)
{
    if (!stub_ota_enabled || size > STUB_OTA_MAX) {
        return -1;
    }
    stub_ota_len = 0;
    *handle = stub_ota;
    return 0;
}
int fc_port_ota_write(void *handle, const void *data, size_t len)
{
    if (!stub_ota_enabled || len > STUB_OTA_MAX - stub_ota_len) {
        return -1;
    }
    memcpy(stub_ota + stub_ota_len, data, len);
    stub_ota_len += len;
    return 0;
}
int fc_port_ota_end(void *handle, int commit)
{
    if (!stub_ota_enabled) {
        return -1;
    }
    if (!commit) {
        stub_ota_aborts++;
        return 0;
    }
    if (stub_ota_end_result == 0) {
        stub_ota_commits++;
    }
    return stub_ota_end_result;
}
int fc_port_fw_read(size_t off, void *buf, size_t len)
{
    if (stub_fw == NULL || off > stub_fw_len || len > stub_fw_len - off) {
        return -1;
    }
    memcpy(buf, stub_fw + off, len);
    return 0;
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
static unsigned char s_hw_ec[256]; /* an ECDSA key "in hardware", as DER at the end */
static size_t s_hw_ec_len;

/* Blinding only, in a test. */
static int stub_rng(void *ctx, unsigned char *buf, size_t len)
{
    (void)ctx;
    for (size_t i = 0; i < len; i++) {
        buf[i] = (unsigned char)rand();
    }
    return 0;
}

int fc_port_hwkey_bits(void)
{
    return stub_hwkey_bits;
}

int fc_port_hwkey_seal(struct mbedtls_pk_context *pk)
{
    if (stub_hwkey_seal_fails) {
        return -1;
    }
    if (stub_hwkey_bits == FC_HWKEY_ECDSA_P256 && mbedtls_pk_get_type(pk) == MBEDTLS_PK_ECKEY) {
        fc_port_hwkey_forget();
        int len = mbedtls_pk_write_key_der(pk, s_hw_ec, sizeof(s_hw_ec));
        if (len <= 0) {
            return -1;
        }
        s_hw_ec_len = (size_t)len;
        s_hw_sealed = 1;
        stub_hwkey_seals++;
        return 0;
    }
    if (stub_hwkey_bits == FC_HWKEY_ECDSA_P256 || mbedtls_pk_get_type(pk) != MBEDTLS_PK_RSA) {
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
    if (s_hw_ec_len > 0) {
        return mbedtls_pk_parse_key(pk, s_hw_ec + sizeof(s_hw_ec) - s_hw_ec_len, s_hw_ec_len, NULL, 0, stub_rng, NULL);
    }
    return mbedtls_pk_setup_rsa_alt(pk, mbedtls_pk_rsa(s_hw), NULL, hw_sign, hw_len);
}

void fc_port_hwkey_forget(void)
{
    mbedtls_pk_free(&s_hw);
    mbedtls_pk_init(&s_hw);
    s_hw_sealed = 0;
    s_hw_ec_len = 0;
}
