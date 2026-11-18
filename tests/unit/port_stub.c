/*
 * Port for unit tests: key/value store and files in memory, no OTA, reboots
 * counted instead of performed. Tests can read and reset the state.
 */
#include "port_stub.h"

#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/pk.h"
#include "cJSON.h"
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
int stub_kv_set_fails, stub_kv_get_fails, stub_kv_del_fails;

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
    stub_metrics_reads = 0;
    stub_stack_unused = -1;
    fc_port_hwkey_forget();
    stub_ota_enabled = 0;
    stub_ota_pending_verify = 0;
    stub_ota_len = 0;
    stub_ota_commits = stub_ota_aborts = stub_ota_end_result = 0;
    stub_fw = NULL;
    stub_fw_len = 0;
    stub_hwkey_bits = 0;
    stub_hwkey_seal_fails = 0;
    stub_hwkey_seals = 0;
    stub_kv_set_fails = stub_kv_get_fails = stub_kv_del_fails = 0;
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
    if (stub_kv_get_fails) {
        return -1;
    }
    entry_t *e = find(s_kv, key, 0);
    if (e == NULL) {
        return FC_KV_MISSING;
    }
    *out = malloc(e->len + 1);
    memcpy(*out, e->data, e->len + 1);
    *len = e->len;
    return 0;
}

int fc_port_kv_set(const char *key, const void *data, size_t len)
{
    if (stub_kv_set_fails) {
        return -1;
    }
    return put(s_kv, key, data, len);
}

int fc_port_kv_del(const char *key)
{
    if (stub_kv_del_fails) {
        return -1;
    }
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

int stub_metrics_reads;
long stub_stack_unused = -1;

long stub_heap_largest = -1;
long stub_heap_free = -1;
long stub_heap_free_next[4];
int stub_heap_free_queued;

long fc_port_heap_low(void)
{
    return -1;
}

long fc_port_heap_free(void)
{
    if (stub_heap_free_queued > 0) {
        long v = stub_heap_free_next[0];
        memmove(stub_heap_free_next, stub_heap_free_next + 1, sizeof(stub_heap_free_next) - sizeof(long));
        stub_heap_free_queued--;
        return v;
    }
    return stub_heap_free;
}
long stub_heap_free_now = -2;
long fc_port_heap_free_now(void)
{
    return stub_heap_free_now != -2 ? stub_heap_free_now : fc_port_heap_free();
}
long stub_heap_largest_next[4];
int stub_heap_largest_queued;

long stub_heap_watch_low = -1;

long fc_port_heap_watch(int start)
{
    return stub_heap_watch_low < 0 ? -1 : start ? 0 : stub_heap_watch_low;
}

long fc_port_heap_largest(void)
{
    if (stub_heap_largest_queued > 0) {
        long v = stub_heap_largest_next[0];
        memmove(stub_heap_largest_next, stub_heap_largest_next + 1, sizeof(stub_heap_largest_next) - sizeof(long));
        stub_heap_largest_queued--;
        return v;
    }
    return stub_heap_largest;
}


void fc_port_metrics(void *obj)
{
    char n[16];
    snprintf(n, sizeof(n), "%d", ++stub_metrics_reads);
    cJSON_AddStringToObject((cJSON *)obj, "reads", n);
}

void fc_port_background_compute(void) {}

long fc_port_stack_unused(void)
{
    return stub_stack_unused;
}

int fc_port_write_file(const char *path, const void *data, size_t len)
{
    return put(s_files, path, data, len);
}

int fc_port_read_file(const char *path, char **out, size_t *len)
{
    entry_t *e = find(s_files, path, 0);
    if (e == NULL) {
        return -1;
    }
    *out = malloc(e->len + 1);
    memcpy(*out, e->data, e->len);
    (*out)[e->len] = '\0';
    *len = e->len;
    return 0;
}

int fc_port_remove_file(const char *path)
{
    if (stub_kv_del_fails) {
        return -1;
    }
    entry_t *e = find(s_files, path, 0);
    if (e != NULL) {
        free(e->data);
        e->data = NULL;
    }
    return 0;
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
int stub_ota_pending_verify;

int fc_port_ota_pending_verify(void)
{
    return stub_ota_pending_verify;
}
void fc_port_ota_confirm(void) {}
void fc_port_ota_rollback(void) {}

void fc_port_reboot(void)
{
    stub_reboots++;
}

/* The log's last lines, one per line: stub_log_has. */
static char s_log[4096];
static size_t s_log_len;
static pthread_mutex_t s_log_lock = PTHREAD_MUTEX_INITIALIZER;

void fc_port_log(fc_log_level_t level, const char *fmt, ...)
{
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    size_t n = strlen(line);
    pthread_mutex_lock(&s_log_lock);
    if (s_log_len + n + 2 > sizeof(s_log)) {
        s_log_len = 0;
    }
    memcpy(s_log + s_log_len, line, n);
    s_log_len += n;
    s_log[s_log_len++] = '\n';
    s_log[s_log_len] = 0;
    pthread_mutex_unlock(&s_log_lock);
    if (stub_verbose) {
        fprintf(stderr, "%s\n", line);
    }
}

/* Forgets the log, for a test that counts its lines: full, it starts again
 * from nothing, and a count could miss the lines before. */
void stub_log_clear(void)
{
    pthread_mutex_lock(&s_log_lock);
    s_log_len = 0;
    s_log[0] = 0;
    pthread_mutex_unlock(&s_log_lock);
}

/* How many of the log's last lines are this one. */
int stub_log_count(const char *line)
{
    pthread_mutex_lock(&s_log_lock);
    int n = 0;
    for (const char *p = s_log; (p = strstr(p, line)) != NULL; p++) {
        n += (p == s_log || p[-1] == '\n') && p[strlen(line)] == '\n';
    }
    pthread_mutex_unlock(&s_log_lock);
    return n;
}

int stub_log_has(const char *line)
{
    return stub_log_count(line) > 0;
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
