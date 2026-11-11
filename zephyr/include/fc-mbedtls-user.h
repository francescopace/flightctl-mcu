/* Added to Zephyr's mbedTLS configuration (CONFIG_MBEDTLS_USER_CONFIG_FILE). */

/* The agent logs TLS failures by name (mbedtls_strerror); Zephyr builds the
 * error strings only with mbedTLS's debug output. */
#define MBEDTLS_ERROR_C

/* On the RP2040 the entropy comes from the port (fc_entropy_rp2.c), not from
 * Zephyr's entropy API, which has no driver there. */
#if defined(CONFIG_FC_ENTROPY_RP2_ROSC)
#undef MBEDTLS_NO_DEFAULT_ENTROPY_SOURCES
#define MBEDTLS_ENTROPY_HARDWARE_ALT
#endif

/* A TLS connection keeps a buffer for each direction. Records from the
 * server can be 16 KB; what the agent sends goes out in records of 4 KB, as
 * in the ESP-IDF build: 12 KB less per connection. */
#undef MBEDTLS_SSL_OUT_CONTENT_LEN
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096
