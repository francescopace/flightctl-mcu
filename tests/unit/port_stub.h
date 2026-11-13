#pragma once

#include "fc_port.h"

/* State of the in-memory port (port_stub.c). */
extern int stub_reboots;
extern int stub_metrics_reads; /* fc_port_metrics calls; it reports the count as "reads" */
extern int stub_verbose; /* 1: print the agent's log lines */
/* OTA and the running image (off unless stub_ota_enabled). */
#define STUB_OTA_MAX 8192
extern int stub_ota_enabled;
extern unsigned char stub_ota[STUB_OTA_MAX]; /* what was written to the slot */
extern size_t stub_ota_len;
extern int stub_ota_commits, stub_ota_aborts;
extern int stub_ota_end_result;      /* what committing the slot returns */
extern const unsigned char *stub_fw; /* the running image, NULL: cannot be read */
extern size_t stub_fw_len;

extern int stub_hwkey_bits;       /* > 0: a "hardware" key store, for RSA keys of this size or FC_HWKEY_ECDSA_P256 */
extern int stub_hwkey_seal_fails; /* 1: sealing fails */
extern int stub_hwkey_seals;      /* keys sealed since stub_reset */
extern int stub_kv_set_fails;     /* 1: fc_port_kv_set fails, as a full or broken flash would */

/* A new ECDSA P-256 key pair in `pk` (initialized here), for a test to sign with. */
struct mbedtls_pk_context;
int stub_ec_key(struct mbedtls_pk_context *pk);

void stub_reset(void);
const char *stub_kv(const char *key);     /* NULL if missing */
const char *stub_file(const char *path);  /* NULL if never written */
