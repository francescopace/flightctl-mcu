#pragma once

typedef struct {
    char name[64];  /* device name derived from the public key */
    char *key_pem;  /* private key, PEM */
    char *csr_pem;  /* PKCS#10 CSR with CN=<name>, PEM */
} fc_identity_t;

/* Loads the device key from storage, generating and persisting it on first
 * boot, then derives the device name and CSR. */
int fc_identity_load(fc_identity_t *id);
void fc_identity_free(fc_identity_t *id);
