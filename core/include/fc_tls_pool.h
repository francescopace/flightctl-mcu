#pragma once

#include <stddef.h>

/*
 * The TLS receive buffers, reserved once, early: a TLS connection needs one
 * block of about 16.7 KB to receive the largest record the server may send,
 * and on a heap shared with WiFi and the apps such a block can be gone by the
 * time a connection opens. The device holds two TLS connections at most (the
 * agent's, with the spec's long-poll and the status over one HTTP/2
 * connection, and the registry's), so there are two.
 *
 * A port routes mbedTLS's allocations here: ESP-IDF with
 * CONFIG_MBEDTLS_CUSTOM_MEM_ALLOC (esp_mbedtls_mem_calloc), Zephyr with
 * MBEDTLS_PLATFORM_CALLOC_MACRO. Allocations of a receive buffer's size take
 * a slot; anything else, or a third buffer, comes from the heap.
 */

#define FC_TLS_POOL_SLOTS 2
/* A slot's size, and the smallest allocation served from one: mbedTLS's
 * receive buffer is MBEDTLS_SSL_IN_CONTENT_LEN (16384) plus the record's
 * header, IV, tag and padding. */
#define FC_TLS_POOL_SLOT_SIZE (17 * 1024)
#define FC_TLS_POOL_MIN 16384

/* Reserves the slots, once; called at the agent's start, before the apps. */
int fc_tls_pool_init(void);
/* A zeroed slot for n * size bytes, or NULL (not a receive buffer, or none free). */
void *fc_tls_pool_take(size_t n, size_t size);
/* Gives a slot back: 1 if p was one, 0 if it is not the pool's. */
int fc_tls_pool_give(void *p);
/* calloc and free with the pool first, the C heap otherwise. */
void *fc_tls_calloc(size_t n, size_t size);
void fc_tls_free(void *p);
/* Slots in use now, and the most ever at once (for the status). */
void fc_tls_pool_usage(int *now, int *peak);
