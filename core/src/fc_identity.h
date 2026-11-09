#pragma once

#include <stddef.h>

#include "mbedtls/pk.h"

typedef struct {
    char name[64];  /* device name derived from the public key */
    char *key_pem;  /* private key, PEM; NULL when it is in hardware */
    char *csr_pem;  /* PKCS#10 CSR with CN=<name>, PEM */
    int hw_key;     /* 1: the key is in hardware (fc_port_hwkey_pk) */
} fc_identity_t;

/* Loads the device key from storage, generating and persisting it on first
 * boot, then derives the device name and CSR. */
int fc_identity_load(fc_identity_t *id);
void fc_identity_free(fc_identity_t *id);

/* Deletes the stored device key: the next fc_identity_load makes a new one,
 * hence a new device name. */
void fc_identity_forget(void);

/* Device name for a public key (the one in a certificate, for instance), as
 * Flight Control computes it: lower(base32hex(sha256(X || Y))) for ECDSA,
 * lower(base32hex(sha256(N))) for RSA. */
int fc_identity_name_of(mbedtls_pk_context *pk, char *name, size_t name_len);
