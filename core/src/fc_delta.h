/*
 * Firmware delta: rebuilds a new image from the one that is running and a
 * patch, as the patch arrives.
 *
 * The patch is a detools "sequential" patch compressed with heatshrink
 * (https://github.com/eerimoq/detools, the format of Espressif's
 * esp_delta_ota): bsdiff's diff and extra blocks in the order they are
 * needed. The old image is read at random; the new one comes out in order,
 * ready for an OTA slot.
 *
 * The patch is not trusted: nothing here tells a good image from a bad one,
 * only that reads stay inside the old image and no more than `to_size`
 * bytes come out. The caller checks the new image's digest.
 */
#ifndef FC_DELTA_H
#define FC_DELTA_H

#include <stddef.h>

/* The largest heatshrink window a patch may ask for: 2^12 bytes of RAM. */
#define FC_DELTA_WINDOW_BITS 12

typedef struct fc_delta fc_delta_t;

/* Reads `len` bytes of the old image at `off`; 0 or -1. */
typedef int (*fc_delta_read_cb)(size_t off, void *buf, size_t len, void *ctx);
/* Takes the next bytes of the new image; 0 or -1. */
typedef int (*fc_delta_write_cb)(const void *data, size_t len, void *ctx);

/* NULL when out of memory. */
fc_delta_t *fc_delta_begin(size_t patch_size, size_t from_size, size_t to_size, fc_delta_read_cb read,
                           fc_delta_write_cb write, void *ctx);
/* The next bytes of the patch; 0, or -1 (then only fc_delta_end is left). */
int fc_delta_feed(fc_delta_t *d, const void *patch, size_t len);
/* Frees `d`. 0 when the whole patch was applied and gave exactly `to_size`
 * bytes; -1 otherwise. */
int fc_delta_end(fc_delta_t *d);

#endif
