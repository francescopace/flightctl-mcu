/* Added to Zephyr's mbedTLS configuration (CONFIG_MBEDTLS_USER_CONFIG_FILE). */

/* The agent logs TLS failures by name (mbedtls_strerror); Zephyr builds the
 * error strings only with mbedTLS's debug output. */
#define MBEDTLS_ERROR_C
