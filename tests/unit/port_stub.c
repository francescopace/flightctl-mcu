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
#include "psa/crypto.h"

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

/* "Hardware" key store: the sealed key is a PSA key that cannot be exported,
 * out of the identity's storage, and signs through a PK context that wraps
 * it, as the keys of the ESP32's peripherals do. */
static psa_key_id_t s_hw = PSA_KEY_ID_NULL;

int stub_ec_key(struct mbedtls_pk_context *pk)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_EXPORT);
    psa_key_id_t id = PSA_KEY_ID_NULL;
    mbedtls_pk_init(pk);
    if (psa_crypto_init() != PSA_SUCCESS || psa_generate_key(&attr, &id) != PSA_SUCCESS) {
        return -1;
    }
    int rc = mbedtls_pk_copy_from_psa(id, pk);
    psa_destroy_key(id);
    return rc;
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
    psa_key_type_t type = mbedtls_pk_get_key_type(pk);
    int wanted = stub_hwkey_bits == FC_HWKEY_ECDSA_P256 ? PSA_KEY_TYPE_IS_ECC(type) : PSA_KEY_TYPE_IS_RSA(type);
    if (!wanted) {
        return -1;
    }
    fc_port_hwkey_forget();
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    if (mbedtls_pk_get_psa_attributes(pk, PSA_KEY_USAGE_SIGN_HASH, &attr) != 0 ||
        mbedtls_pk_import_into_psa(pk, &attr, &s_hw) != 0) {
        return -1;
    }
    stub_hwkey_seals++;
    return 0;
}

int fc_port_hwkey_pk(struct mbedtls_pk_context *pk, const struct mbedtls_pk_context *pub)
{
    return s_hw == PSA_KEY_ID_NULL ? -1 : mbedtls_pk_wrap_psa(pk, s_hw);
}

void fc_port_hwkey_forget(void)
{
    psa_destroy_key(s_hw);
    s_hw = PSA_KEY_ID_NULL;
}
