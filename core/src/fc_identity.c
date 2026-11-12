/*
 * Device identity: the device key, device name and PKCS#10 CSR.
 *
 * The key is ECDSA P-256 in storage, or in hardware (ECDSA P-256 or RSA)
 * where the port has a place for it (fc_port_hwkey_*): there the key is
 * generated here, the CSR written with it, and the port seals it; only the
 * CSR stays in storage.
 *
 * The device name must match what the Flight Control service derives from
 * the public key (pkg/crypto/key.go, HashPublicKey + identity.generateDeviceName):
 *   ECDSA: lower(base32hex_nopad(sha256(trimLeftZeros(X) || trimLeftZeros(Y))))
 *   RSA:   lower(base32hex_nopad(sha256(trimLeftZeros(N))))
 */
#include "fc_identity.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fc_crypto.h"
#include "fc_port.h"
#include "mbedtls/asn1.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_csr.h"

#define KV_DEVICE_KEY "device.key"
#define KV_DEVICE_CSR "device.csr" /* only with the key in hardware */

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

static int sha256_name(const unsigned char *a, size_t al, const unsigned char *b, size_t bl, char *name)
{
    unsigned char hash[32];
    fc_sha256_t sha;
    fc_sha256_start(&sha);
    fc_sha256_update(&sha, a, al);
    fc_sha256_update(&sha, b, bl);
    if (fc_sha256_finish(&sha, hash) != 0) {
        return -1;
    }
    base32hex_lower(hash, sizeof(hash), name);
    return 0;
}

int fc_identity_name_of(mbedtls_pk_context *pk, char *name, size_t name_len)
{
    /* The public key as PSA exports it: for RSA the DER of
     * RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER }
     * (RSA-4096: 526 bytes), for a curve the point 0x04 || X || Y. */
    unsigned char raw[600];
    size_t len = 0;
    if (name_len < 53 || mbedtls_pk_write_pubkey_psa(pk, raw, sizeof(raw), &len) != 0) {
        return -1;
    }
    psa_key_type_t type = mbedtls_pk_get_key_type(pk);
    if (PSA_KEY_TYPE_IS_RSA(type)) {
        unsigned char *n = raw;
        size_t nl = 0;
        if (mbedtls_asn1_get_tag(&n, raw + len, &nl, MBEDTLS_ASN1_CONSTRUCTED | MBEDTLS_ASN1_SEQUENCE) != 0 ||
            mbedtls_asn1_get_tag(&n, raw + len, &nl, MBEDTLS_ASN1_INTEGER) != 0) {
            return -1;
        }
        while (nl > 0 && *n == 0) { n++; nl--; } /* minimal big-endian: no leading zeros */
        return sha256_name(n, nl, NULL, 0, name);
    }
    if (!PSA_KEY_TYPE_IS_ECC(type) || len != 65 || raw[0] != 0x04) {
        return -1;
    }
    const unsigned char *x = raw + 1, *y = raw + 33;
    size_t xl = 32, yl = 32;
    while (xl > 0 && *x == 0) { x++; xl--; }
    while (yl > 0 && *y == 0) { y++; yl--; }
    return sha256_name(x, xl, y, yl, name);
}

/* A new key pair, ECDSA P-256 or RSA of `bits`, in `pk`. It is generated in
 * PSA and copied out, so `pk` holds it by itself and it can be written as PEM
 * or handed to the port. */
static int generate_key(int bits, mbedtls_pk_context *pk)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    if (bits == FC_HWKEY_ECDSA_P256) {
        psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
        psa_set_key_bits(&attr, 256);
    } else {
        psa_set_key_type(&attr, PSA_KEY_TYPE_RSA_KEY_PAIR); /* public exponent 65537 */
        psa_set_key_bits(&attr, (size_t)bits);
    }
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_EXPORT);
    psa_key_id_t id = PSA_KEY_ID_NULL;
    if (fc_crypto_init() != 0) {
        return -1;
    }
    int rc = (int)psa_generate_key(&attr, &id);
    if (rc == 0) {
        rc = mbedtls_pk_copy_from_psa(id, pk);
        psa_destroy_key(id);
    }
    return rc;
}

static int write_csr(mbedtls_pk_context *pk, const char *name, char **out)
{
    char subject[80];
    snprintf(subject, sizeof(subject), "CN=%s", name);
    mbedtls_x509write_csr csr;
    mbedtls_x509write_csr_init(&csr);
    mbedtls_x509write_csr_set_md_alg(&csr, MBEDTLS_MD_SHA256);
    mbedtls_x509write_csr_set_key(&csr, pk);
    int rc = -1;
    *out = malloc(2048); /* an RSA-4096 CSR is about 1.7 KB */
    if (*out != NULL && mbedtls_x509write_csr_set_subject_name(&csr, subject) == 0 &&
        mbedtls_x509write_csr_pem(&csr, (unsigned char *)*out, 2048) == 0) {
        rc = 0;
    } else {
        fc_port_log(FC_LOG_ERROR, "CSR generation failed");
        free(*out);
        *out = NULL;
    }
    mbedtls_x509write_csr_free(&csr);
    return rc;
}

/* A device key in hardware: its CSR is stored, the key is the port's. */
static int load_hw(fc_identity_t *id)
{
    char *csr_pem = NULL;
    size_t len = 0;
    if (fc_port_kv_get(KV_DEVICE_CSR, &csr_pem, &len) != 0) {
        return -1;
    }
    mbedtls_pk_context hw;
    mbedtls_pk_init(&hw);
    mbedtls_x509_csr csr;
    mbedtls_x509_csr_init(&csr);
    int parsed = fc_crypto_init() == 0 && mbedtls_x509_csr_parse(&csr, (unsigned char *)csr_pem, len + 1) == 0 &&
                 fc_identity_name_of(&csr.pk, id->name, sizeof(id->name)) == 0;
    int ok = parsed && fc_port_hwkey_pk(&hw, &csr.pk) == 0;
    mbedtls_pk_free(&hw);
    if (!ok) {
        fc_port_log(FC_LOG_ERROR, "device key in hardware: %s", parsed ? "not available" : "stored CSR is invalid");
        mbedtls_x509_csr_free(&csr);
        free(csr_pem);
        return -2;
    }
    mbedtls_x509_csr_free(&csr);
    id->csr_pem = csr_pem;
    id->hw_key = 1;
    return 0;
}

/* First boot with a port that holds keys: generated here, sealed there. */
static int create_hw(fc_identity_t *id, int bits)
{
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int rc;
    if (bits == FC_HWKEY_ECDSA_P256) {
        fc_port_log(FC_LOG_WARN,
                    "no device key stored: generating one (ECDSA P-256) for the hardware, this is a new device");
    } else {
        fc_port_log(FC_LOG_WARN,
                    "no device key stored: generating one (RSA-%d) for the hardware, this is a new device", bits);
    }
    if ((rc = generate_key(bits, &pk)) != 0) {
        fc_port_log(FC_LOG_ERROR, "key generation failed (-0x%04x)", -rc);
        mbedtls_pk_free(&pk);
        return -1;
    }
    char *csr_pem = NULL;
    if (fc_identity_name_of(&pk, id->name, sizeof(id->name)) != 0 || write_csr(&pk, id->name, &csr_pem) != 0 ||
        fc_port_kv_set(KV_DEVICE_CSR, csr_pem, strlen(csr_pem)) != 0) {
        free(csr_pem);
        mbedtls_pk_free(&pk);
        return -1;
    }
    rc = fc_port_hwkey_seal(&pk);
    mbedtls_pk_free(&pk); /* zeroizes the private key */
    if (rc != 0) {
        fc_port_kv_del(KV_DEVICE_CSR);
        free(csr_pem);
        return -1;
    }
    fc_port_log(FC_LOG_INFO, "device key sealed in hardware: software can no longer read it");
    id->csr_pem = csr_pem;
    id->hw_key = 1;
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
    int bits = fc_port_hwkey_bits();
    if (fc_port_kv_get(KV_DEVICE_KEY, &pem, &pem_len) != 0 && bits > 0) {
        /* No key in storage: the hardware has it, or gets a new one. A key
         * that was in storage before stays there (the device name with it). */
        if ((rc = load_hw(id)) == 0 || (rc == -1 && create_hw(id, bits) == 0)) {
            return 0;
        }
        memset(id, 0, sizeof(*id));
        fc_port_log(FC_LOG_ERROR, "device key in hardware failed: using a key in storage instead");
    }
    if (pem != NULL) {
        rc = fc_crypto_init() != 0 ? -1 : mbedtls_pk_parse_key(&pk, (unsigned char *)pem, pem_len + 1, NULL, 0);
        if (rc != 0) {
            fc_port_log(FC_LOG_ERROR, "stored device key is invalid (-0x%04x)", -rc);
            free(pem);
            goto fail;
        }
    } else {
        /* First boot, or the storage was erased: either way a new device. */
        fc_port_log(FC_LOG_WARN, "no device key stored: generating one (ECDSA P-256), this is a new device");
        if ((rc = generate_key(FC_HWKEY_ECDSA_P256, &pk)) != 0) {
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

    if (write_csr(&pk, id->name, &id->csr_pem) != 0) {
        goto fail;
    }
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
    fc_port_kv_del(KV_DEVICE_CSR);
    fc_port_hwkey_forget();
}
