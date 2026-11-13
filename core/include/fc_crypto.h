/*
 * The cryptography the core and the ports share, on PSA Crypto (mbedTLS 4
 * has no other way to it): random bytes and SHA-256.
 *
 * PSA must be initialized before anything else in it is called. These start
 * it themselves, so no caller has to know who comes first; starting it again
 * costs nothing.
 */
#pragma once

#include <stddef.h>
#include <string.h>

#include "psa/crypto.h"

/* The agent calls PSA from several threads at once (the status thread, the
 * spec thread, app downloads), and PSA's key table is shared between them
 * only safely with the threading layer (tests/unit/test_threads.c). */
#if !defined(MBEDTLS_THREADING_C)
#error "mbedTLS must be built with MBEDTLS_THREADING_C (and MBEDTLS_THREADING_PTHREAD or _ALT)"
#endif

static inline int fc_crypto_init(void)
{
    return psa_crypto_init() == PSA_SUCCESS ? 0 : -1;
}

static inline int fc_random(void *buf, size_t len)
{
    return fc_crypto_init() == 0 && psa_generate_random(buf, len) == PSA_SUCCESS ? 0 : -1;
}

typedef struct {
    psa_hash_operation_t op;
    int failed;
} fc_sha256_t;

static inline void fc_sha256_start(fc_sha256_t *s)
{
    s->op = psa_hash_operation_init();
    s->failed = fc_crypto_init() != 0 || psa_hash_setup(&s->op, PSA_ALG_SHA_256) != PSA_SUCCESS;
}

static inline void fc_sha256_update(fc_sha256_t *s, const void *data, size_t len)
{
    if (!s->failed && len > 0 && psa_hash_update(&s->op, data, len) != PSA_SUCCESS) {
        s->failed = 1;
    }
}

/* 0, or -1 with `out` zeroed: a digest that was not computed must not be
 * compared with anything. Ends the hash either way. */
static inline int fc_sha256_finish(fc_sha256_t *s, unsigned char out[32])
{
    size_t n = 0;
    if (!s->failed && psa_hash_finish(&s->op, out, 32, &n) == PSA_SUCCESS && n == 32) {
        return 0;
    }
    psa_hash_abort(&s->op);
    memset(out, 0, 32);
    return -1;
}

/* Ends a hash whose digest is not wanted; harmless on one that has ended. */
static inline void fc_sha256_abort(fc_sha256_t *s)
{
    psa_hash_abort(&s->op);
}

static inline int fc_sha256(const void *data, size_t len, unsigned char out[32])
{
    fc_sha256_t s;
    fc_sha256_start(&s);
    fc_sha256_update(&s, data, len);
    return fc_sha256_finish(&s, out);
}
