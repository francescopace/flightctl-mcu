/* Device identity: key storage, device name (as Flight Control computes it), CSR. */
#include "fc_identity.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/pk.h"
#include "mbedtls/x509_csr.h"
#include "port_stub.h"
#include "psa/crypto.h"
#include "test.h"

int fc_http_global_init(void);
mbedtls_ctr_drbg_context *fc_http_drbg(void);

/* A test key, not a secret: its private scalar, from which the test builds
 * the PEM the device would store. The name was computed independently
 * (openssl + Python): lower(base32hex_nopad(sha256(X || Y))) of the public key. */
static const char VECTOR_D[] = "afc90406ca43850448e89adf5a056d68cad3ce53b6db6622ee0fe3007cb57c6f";
static const char VECTOR_NAME[] = "ucjcc3q6dnkmr15i4r3u5cscoh09aovmabo0qtvbs1e802k70h7g";

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
    int rc = unhex(VECTOR_D, d, sizeof(d));
    if (rc == 0) {
        rc = mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    }
    if (rc == 0) {
        rc = mbedtls_ecp_read_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), d, sizeof(d));
    }
    if (rc == 0) {
        rc = mbedtls_ecp_keypair_calc_public(mbedtls_pk_ec(pk), mbedtls_ctr_drbg_random, fc_http_drbg());
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
    stub_reset();
    return TEST_RESULT();
}
