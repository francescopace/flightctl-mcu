/* Device identity: key storage, device name (as Flight Control computes it), CSR. */
#include "fc_identity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/pk.h"
#include "mbedtls/x509_csr.h"
#include "port_stub.h"
#include "psa/crypto.h"
#include "test.h"

int fc_http_global_init(void);

/* A test key, not a secret: its private scalar, from which the test builds
 * the PEM the device would store. The name was computed independently
 * (openssl + Python): lower(base32hex_nopad(sha256(X || Y))) of the public key. */
static const char VECTOR_D[] = "afc90406ca43850448e89adf5a056d68cad3ce53b6db6622ee0fe3007cb57c6f";
static const char VECTOR_NAME[] = "ucjcc3q6dnkmr15i4r3u5cscoh09aovmabo0qtvbs1e802k70h7g";

/* An RSA public modulus and its name, computed independently (openssl +
 * Python): lower(base32hex_nopad(sha256(N))), as Flight Control does. */
static const char RSA_N[] =
    "c9a5b0a4625375388debb721969f62a61a0e0b74ba04466e3185a1eab4407310d19514a5db684bee5efd93e034b10399cdc5207d054d9491e4"
    "1dbd27c4c13b163396d3ba64ef76c768999f47b755d71c1fd9d9e5409dabfc09f4e364afb8d67854bb8f79ff846f174c3d35d24901423a7122"
    "a9c82397140c1a62258de4713c26a325d0b995d51aa72e56e7a712f55e3b4a26bd4b7b948a76d64c7ca0b62fe312b69ecae78d1e525c49ea8e"
    "c18f8e4191ee95f2bfe13f543f5142ce0e63d6882fe56bda60e254a17db05212264e6a3767fe75f02e5317ec4f21291ca6a9928e066cc30b2d"
    "cc389365460d0b20152a3c0baa0acef5265f0a33614b2bbfd54c21d7";
static const char RSA_NAME[] = "invgjavhdg5lc3q4murkkqvu321c2nqkkv8q3k30gngkfkh9q1lg";

static char s_vector_key[512];

static int unhex(const char *hex, unsigned char *out, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        unsigned v;
        if (sscanf(hex + 2 * i, "%2x", &v) != 1) {
            return -1;
        }
        out[i] = (unsigned char)v;
    }
    return 0;
}

/* The vector key as the PEM fc_identity_load reads. */
static int make_vector_key(void)
{
    unsigned char d[32];
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_EXPORT);
    psa_key_id_t id = PSA_KEY_ID_NULL;
    int rc = unhex(VECTOR_D, d, sizeof(d));
    if (rc == 0) {
        rc = (int)psa_import_key(&attr, d, sizeof(d), &id);
    }
    if (rc == 0) {
        rc = mbedtls_pk_copy_from_psa(id, &pk);
        psa_destroy_key(id);
    }
    if (rc == 0) {
        rc = mbedtls_pk_write_key_pem(&pk, (unsigned char *)s_vector_key, sizeof(s_vector_key));
    }
    mbedtls_pk_free(&pk);
    return rc;
}


static void a_stored_key_gives_the_known_name(void)
{
    stub_reset();
    fc_port_kv_set("device.key", s_vector_key, strlen(s_vector_key));
    fc_identity_t id = {0};
    CHECK_INT(fc_identity_load(&id), 0);
    CHECK_STR(id.name, VECTOR_NAME);
    fc_identity_free(&id);
}

static void the_csr_names_the_device(void)
{
    stub_reset();
    fc_port_kv_set("device.key", s_vector_key, strlen(s_vector_key));
    fc_identity_t id = {0};
    CHECK_INT(fc_identity_load(&id), 0);
    mbedtls_x509_csr csr;
    mbedtls_x509_csr_init(&csr);
    CHECK_INT(mbedtls_x509_csr_parse(&csr, (const unsigned char *)id.csr_pem, strlen(id.csr_pem) + 1), 0);
    char subject[128];
    mbedtls_x509_dn_gets(subject, sizeof(subject), &csr.subject);
    char expected[128];
    snprintf(expected, sizeof(expected), "CN=%s", VECTOR_NAME);
    CHECK_STR(subject, expected);
    mbedtls_x509_csr_free(&csr);
    fc_identity_free(&id);
}

static void a_new_key_is_stored_and_reused(void)
{
    stub_reset();
    fc_identity_t a = {0}, b = {0};
    CHECK_INT(fc_identity_load(&a), 0);
    CHECK_INT(strlen(a.name), 52);
    CHECK(stub_kv("device.key") != NULL);
    CHECK_INT(fc_identity_load(&b), 0);
    CHECK_STR(b.name, a.name);
    fc_identity_free(&a);
    fc_identity_free(&b);
}

static void forgetting_the_key_makes_a_new_device(void)
{
    stub_reset();
    fc_identity_t a = {0}, b = {0};
    CHECK_INT(fc_identity_load(&a), 0);
    fc_identity_forget();
    CHECK(stub_kv("device.key") == NULL);
    CHECK_INT(fc_identity_load(&b), 0);
    CHECK(strcmp(a.name, b.name) != 0);
    fc_identity_free(&a);
    fc_identity_free(&b);
}

static void an_rsa_key_gives_the_known_name(void)
{
    /* RSAPublicKey ::= SEQUENCE { modulus INTEGER, publicExponent INTEGER }:
     * the modulus has its top bit set, so its INTEGER starts with a zero. */
    unsigned char der[4 + 4 + 1 + 256 + 5] = {0x30, 0x82, 0x01, 0x0a, 0x02, 0x82, 0x01, 0x01, 0x00};
    static const unsigned char e[] = {0x02, 0x03, 0x01, 0x00, 0x01};
    CHECK_INT(unhex(RSA_N, der + 9, 256), 0);
    memcpy(der + 9 + 256, e, sizeof(e));
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_RSA_PUBLIC_KEY);
    psa_key_id_t id = PSA_KEY_ID_NULL;
    CHECK_INT(psa_import_key(&attr, der, sizeof(der), &id), 0);
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    CHECK_INT(mbedtls_pk_copy_from_psa(id, &pk), 0);
    psa_destroy_key(id);
    char name[64];
    CHECK_INT(fc_identity_name_of(&pk, name, sizeof(name)), 0);
    CHECK_STR(name, RSA_NAME);
    mbedtls_pk_free(&pk);
}

/* Name of the key in a CSR, and whether `pk` signs for that key. With an RSA
 * key, PKCS#1 v1.5: what the stub's key allows (TLS 1.3 needs RSA-PSS, which
 * the firmware checks at sealing). */
static int csr_key_signs(const char *csr_pem, mbedtls_pk_context *pk, char *name)
{
    mbedtls_x509_csr csr;
    mbedtls_x509_csr_init(&csr);
    unsigned char hash[32] = {1, 2, 3}, sig[512];
    size_t sig_len = 0;
    int rc = mbedtls_x509_csr_parse(&csr, (const unsigned char *)csr_pem, strlen(csr_pem) + 1);
    if (rc == 0) {
        rc = fc_identity_name_of(&csr.pk, name, 64);
    }
    if (rc == 0) {
        rc = mbedtls_pk_sign(pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sizeof(sig), &sig_len);
    }
    if (rc == 0) {
        rc = mbedtls_pk_verify(&csr.pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sig_len);
    }
    mbedtls_x509_csr_free(&csr);
    return rc;
}

static void a_new_key_goes_to_the_hardware(int bits)
{
    stub_reset();
    stub_hwkey_bits = bits;
    fc_identity_t id = {0};
    CHECK_INT(fc_identity_load(&id), 0);
    CHECK_INT(id.hw_key, 1);
    CHECK(id.key_pem == NULL);
    CHECK(stub_kv("device.key") == NULL);
    CHECK_STR(stub_kv("device.csr"), id.csr_pem);
    CHECK_INT(stub_hwkey_seals, 1);
    /* The CSR is for the sealed key, and names the device. */
    mbedtls_pk_context hw;
    mbedtls_pk_init(&hw);
    CHECK_INT(fc_port_hwkey_pk(&hw, NULL), 0);
    char name[64] = "";
    CHECK_INT(csr_key_signs(id.csr_pem, &hw, name), 0);
    CHECK_STR(name, id.name);
    mbedtls_pk_free(&hw);
    /* Next boot: same key, nothing generated. */
    fc_identity_t again = {0};
    CHECK_INT(fc_identity_load(&again), 0);
    CHECK_INT(again.hw_key, 1);
    CHECK_STR(again.name, id.name);
    CHECK_INT(stub_hwkey_seals, 1);
    /* Forgotten (re-enrollment): a new key in the hardware, a new name. */
    fc_identity_forget();
    fc_identity_t next = {0};
    CHECK_INT(fc_identity_load(&next), 0);
    CHECK_INT(next.hw_key, 1);
    CHECK(strcmp(next.name, id.name) != 0);
    CHECK_INT(stub_hwkey_seals, 2);
    fc_identity_free(&id);
    fc_identity_free(&again);
    fc_identity_free(&next);
}

static void a_new_rsa_key_goes_to_the_hardware(void)
{
    a_new_key_goes_to_the_hardware(2048);
}

/* The ECDSA peripheral's key: the same kind of key and name as in storage. */
static void a_new_ecdsa_key_goes_to_the_hardware(void)
{
    a_new_key_goes_to_the_hardware(FC_HWKEY_ECDSA_P256);
    fc_identity_t id = {0};
    CHECK_INT(fc_identity_load(&id), 0);
    mbedtls_x509_csr csr;
    mbedtls_x509_csr_init(&csr);
    CHECK_INT(mbedtls_x509_csr_parse(&csr, (const unsigned char *)id.csr_pem, strlen(id.csr_pem) + 1), 0);
    CHECK(PSA_KEY_TYPE_IS_ECC(mbedtls_pk_get_key_type(&csr.pk)));
    CHECK_INT((int)mbedtls_pk_get_bitlen(&csr.pk), 256);
    mbedtls_x509_csr_free(&csr);
    fc_identity_free(&id);
}

static void a_key_in_storage_stays_there(void)
{
    stub_reset();
    stub_hwkey_bits = 2048;
    fc_port_kv_set("device.key", s_vector_key, strlen(s_vector_key));
    fc_identity_t id = {0};
    CHECK_INT(fc_identity_load(&id), 0);
    CHECK_INT(id.hw_key, 0);
    CHECK_STR(id.name, VECTOR_NAME);
    CHECK_INT(stub_hwkey_seals, 0);
    fc_identity_free(&id);
}

static void a_failed_seal_falls_back_to_storage(void)
{
    stub_reset();
    stub_hwkey_bits = 2048;
    stub_hwkey_seal_fails = 1;
    fc_identity_t id = {0};
    CHECK_INT(fc_identity_load(&id), 0);
    CHECK_INT(id.hw_key, 0);
    CHECK(stub_kv("device.key") != NULL);
    CHECK(stub_kv("device.csr") == NULL);
    /* and keeps that key afterwards, even once the hardware works */
    stub_hwkey_seal_fails = 0;
    fc_identity_t again = {0};
    CHECK_INT(fc_identity_load(&again), 0);
    CHECK_INT(again.hw_key, 0);
    CHECK_STR(again.name, id.name);
    fc_identity_free(&id);
    fc_identity_free(&again);
}

static void a_corrupt_key_is_an_error(void)
{
    stub_reset();
    fc_port_kv_set("device.key", "not a key", 9);
    fc_identity_t id = {0};
    CHECK(fc_identity_load(&id) != 0);
}

int main(void)
{
    psa_crypto_init();
    fc_http_global_init(); /* the random generator the identity uses */
    if (make_vector_key() != 0) {
        printf("FAIL: cannot build the vector key\n");
        return 1;
    }
    RUN(a_stored_key_gives_the_known_name);
    RUN(the_csr_names_the_device);
    RUN(a_new_key_is_stored_and_reused);
    RUN(forgetting_the_key_makes_a_new_device);
    RUN(a_corrupt_key_is_an_error);
    RUN(an_rsa_key_gives_the_known_name);
    RUN(a_new_rsa_key_goes_to_the_hardware);
    RUN(a_new_ecdsa_key_goes_to_the_hardware);
    RUN(a_key_in_storage_stays_there);
    RUN(a_failed_seal_falls_back_to_storage);
    stub_reset();
    return TEST_RESULT();
}
