/* Added to Zephyr's mbedTLS configuration (CONFIG_MBEDTLS_USER_CONFIG_FILE). */

/* The agent logs TLS failures by name (mbedtls_strerror); Zephyr builds the
 * error strings only with mbedTLS's debug output. */
#define MBEDTLS_ERROR_C

/* A TLS connection keeps a buffer for each direction. Records from the
 * server can be 16 KB; what the agent sends goes out in records of 4 KB, as
 * in the ESP-IDF build: 12 KB less per connection. */
#undef MBEDTLS_SSL_OUT_CONTENT_LEN
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096
