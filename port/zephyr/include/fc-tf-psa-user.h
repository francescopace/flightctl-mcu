/* Added to Zephyr's TF-PSA-Crypto configuration (CONFIG_TF_PSA_CRYPTO_USER_CONFIG_FILE). */

/* The agent calls PSA from several threads at once (status, spec, app
 * downloads): PSA's key table needs the threading layer, on Zephyr's POSIX
 * mutexes (CONFIG_POSIX_API). Zephyr has no Kconfig option for it. */
#define MBEDTLS_THREADING_C
#define MBEDTLS_THREADING_PTHREAD
