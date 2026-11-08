/*
 * Device identity: ECDSA P-256 key, device name and PKCS#10 CSR.
 *
 * The device name must match what the Flight Control service derives from
 * the public key (pkg/crypto/key.go, hashECDSAKey + identity.generateDeviceName):
 *   lower(base32hex_nopad(sha256(trimLeftZeros(X) || trimLeftZeros(Y))))
 */
#include "fc_identity.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fc_port.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/ecp.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"
#include "mbedtls/x509_csr.h"

#define KV_DEVICE_KEY "device.key"

mbedtls_ctr_drbg_context *fc_http_drbg(void);

static void base32hex_lower(const unsigned char *in, size_t len, char *out)
{
    static const char alphabet[] = "0123456789abcdefghijklmnopqrstuv";
    uint32_t buffer = 0;
    int bits = 0;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        buffer = (buffer << 8) | in[i];
        bits += 8;
        while (bits >= 5) {
            out[o++] = alphabet[(buffer >> (bits - 5)) & 0x1f];
            bits -= 5;
        }
    }
    if (bits > 0) {
        out[o++] = alphabet[(buffer << (5 - bits)) & 0x1f];
    }
    out[o] = '\0';
}

int fc_identity_name_of(mbedtls_pk_context *pk, char *name, size_t name_len)
{
    unsigned char raw[65];
    size_t olen = 0;
    if (!mbedtls_pk_can_do(pk, MBEDTLS_PK_ECKEY)) {
        return -1;
    }
    int rc = mbedtls_ecp_write_public_key(mbedtls_pk_ec(*pk), MBEDTLS_ECP_PF_UNCOMPRESSED,
                                          &olen, raw, sizeof(raw));
    if (rc != 0 || olen != 65) {
        return -1;
    }

    const unsigned char *x = raw + 1, *y = raw + 33;
    size_t xl = 32, yl = 32;
    while (xl > 0 && *x == 0) { x++; xl--; }
    while (yl > 0 && *y == 0) { y++; yl--; }

    unsigned char hash[32];
    mbedtls_sha256_context sha;
    mbedtls_sha256_init(&sha);
    mbedtls_sha256_starts(&sha, 0);
    mbedtls_sha256_update(&sha, x, xl);
    mbedtls_sha256_update(&sha, y, yl);
    mbedtls_sha256_finish(&sha, hash);
    mbedtls_sha256_free(&sha);

    if (name_len < 53) {
        return -1;
    }
    base32hex_lower(hash, sizeof(hash), name);
    return 0;
}

int fc_identity_load(fc_identity_t *id)
{
    memset(id, 0, sizeof(*id));
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int rc;

    char *pem = NULL;
    size_t pem_len = 0;
    if (fc_port_kv_get(KV_DEVICE_KEY, &pem, &pem_len) == 0) {
        rc = mbedtls_pk_parse_key(&pk, (unsigned char *)pem, pem_len + 1, NULL, 0,
                                  mbedtls_ctr_drbg_random, fc_http_drbg());
        if (rc != 0) {
            fc_port_log(FC_LOG_ERROR, "stored device key is invalid (-0x%04x)", -rc);
            free(pem);
            goto fail;
        }
    } else {
        /* First boot, or the storage was erased: either way a new device. */
        fc_port_log(FC_LOG_WARN, "no device key stored: generating one (ECDSA P-256), this is a new device");
        if ((rc = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY))) != 0 ||
            (rc = mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk),
                                      mbedtls_ctr_drbg_random, fc_http_drbg())) != 0) {
            fc_port_log(FC_LOG_ERROR, "key generation failed (-0x%04x)", -rc);
            goto fail;
        }
        pem = malloc(512);
        if (pem == NULL || mbedtls_pk_write_key_pem(&pk, (unsigned char *)pem, 512) != 0) {
            free(pem);
            goto fail;
        }
        pem_len = strlen(pem);
        if (fc_port_kv_set(KV_DEVICE_KEY, pem, pem_len) != 0) {
            fc_port_log(FC_LOG_ERROR, "cannot persist device key");
            free(pem);
            goto fail;
        }
    }
    id->key_pem = pem;

    if (fc_identity_name_of(&pk, id->name, sizeof(id->name)) != 0) {
        goto fail;
    }

    char subject[80];
    snprintf(subject, sizeof(subject), "CN=%s", id->name);
    mbedtls_x509write_csr csr;
    mbedtls_x509write_csr_init(&csr);
    mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
    mbedtls_x509write_csr_set_key(&csr, &pk);
    id->csr_pem = malloc(1024);
    if (id->csr_pem == NULL ||
        (rc = mbedtls_x509write_csr_set_subject_name(&csr, subject)) != 0 ||
        (rc = mbedtls_x509write_csr_pem(&csr, (unsigned char *)id->csr_pem, 1024,
                                        mbedtls_ctr_drbg_random, fc_http_drbg())) != 0) {
        fc_port_log(FC_LOG_ERROR, "CSR generation failed");
        mbedtls_x509write_csr_free(&csr);
        goto fail;
    }
    mbedtls_x509write_csr_free(&csr);
    mbedtls_pk_free(&pk);
    return 0;

fail:
    mbedtls_pk_free(&pk);
    fc_identity_free(id);
    return -1;
}

void fc_identity_free(fc_identity_t *id)
{
    free(id->key_pem);
    free(id->csr_pem);
    id->key_pem = NULL;
    id->csr_pem = NULL;
}

void fc_identity_forget(void)
{
    fc_port_kv_del(KV_DEVICE_KEY);
}
