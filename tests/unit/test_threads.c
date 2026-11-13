/*
 * PSA Crypto from several threads at once, as the agent uses it: the status
 * thread and the spec thread each open TLS connections (an ECDHE key and an
 * ECDSA signature per handshake), and app downloads hash at the same time.
 * PSA keeps its keys in one global table, which is safe across threads only
 * when mbedTLS is built with MBEDTLS_THREADING_C. fc_crypto.h refuses to
 * build without it; with it forced off, two or three of these four threads
 * failed on every run (mbedTLS 4.1.1).
 */
#include <pthread.h>

#include "fc_crypto.h"
#include "psa/crypto.h"
#include "test.h"

#define THREADS 4
#define ROUNDS  150

static int worker_ok[THREADS];

static int round_trip(void)
{
    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_SECP_R1));
    psa_set_key_bits(&attr, 256);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_SIGN_HASH | PSA_KEY_USAGE_VERIFY_HASH);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDSA(PSA_ALG_SHA_256));
    mbedtls_svc_key_id_t key = MBEDTLS_SVC_KEY_ID_INIT;
    if (psa_generate_key(&attr, &key) != PSA_SUCCESS) {
        return -1;
    }
    unsigned char hash[32], sig[PSA_SIGNATURE_MAX_SIZE];
    size_t sig_len = 0;
    int ok = fc_random(hash, sizeof(hash)) == 0 &&
             psa_sign_hash(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, sizeof(hash), sig, sizeof(sig), &sig_len) ==
                 PSA_SUCCESS &&
             psa_verify_hash(key, PSA_ALG_ECDSA(PSA_ALG_SHA_256), hash, sizeof(hash), sig, sig_len) ==
                 PSA_SUCCESS;
    return psa_destroy_key(key) == PSA_SUCCESS && ok ? 0 : -1;
}

static void *worker(void *arg)
{
    int *ok = arg;
    *ok = 1;
    for (int i = 0; i < ROUNDS && *ok; i++) {
        *ok = round_trip() == 0;
    }
    return NULL;
}

static void test_concurrent_keys(void)
{
    CHECK_INT(fc_crypto_init(), 0);
    pthread_t t[THREADS];
    for (int i = 0; i < THREADS; i++) {
        CHECK_INT(pthread_create(&t[i], NULL, worker, &worker_ok[i]), 0);
    }
    for (int i = 0; i < THREADS; i++) {
        pthread_join(t[i], NULL);
        CHECK(worker_ok[i]);
    }
}

int main(void)
{
    RUN(test_concurrent_keys);
    return TEST_RESULT();
}
