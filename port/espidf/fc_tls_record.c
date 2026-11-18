/* ESP-IDF 6's dynamic RX helper reads the record header, then makes one
 * mbedtls_calloc for that record. A spec poll and MQTT can run on separate
 * tasks, so only the thread inside the helper may borrow the reserved slot. */
#include "fc_tls_pool.h"
#include "mbedtls/ssl.h"

static _Thread_local unsigned rx_depth;

int fc_idf_allocating_tls_rx(void)
{
    return rx_depth != 0;
}

int __real_esp_mbedtls_add_rx_buffer(mbedtls_ssl_context *ssl);

int __wrap_esp_mbedtls_add_rx_buffer(mbedtls_ssl_context *ssl)
{
    rx_depth++;
    int rc = __real_esp_mbedtls_add_rx_buffer(ssl);
    rx_depth--;
    return rc;
}
