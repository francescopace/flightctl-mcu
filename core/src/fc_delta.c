/* Firmware delta: a detools sequential patch applied as it arrives. */
#include "fc_delta.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "detools.h"
#include "fc_port.h"

/* detools and heatshrink allocate through these (their malloc and free are
 * renamed when they are compiled). The only allocation they make is the
 * heatshrink decoder: its state, an input buffer of 256 bytes and a window of
 * the size the patch asks for. A patch asking for more than
 * FC_DELTA_WINDOW_BITS gets NULL, and is refused. */
#define MAX_ALLOC ((1u << FC_DELTA_WINDOW_BITS) + 1024)

void *fc_delta_alloc(size_t size)
{
    return size <= MAX_ALLOC ? malloc(size) : NULL;
}

void fc_delta_free(void *p)
{
    free(p);
}

/* Old-image reads are short (128 bytes) and mostly in order: read ahead. */
#define CACHE_SIZE 2048

struct fc_delta {
    struct detools_apply_patch_t apply;
    fc_delta_read_cb read;
    fc_delta_write_cb write;
    void *ctx;
    size_t from_size, to_size, patch_size;
    size_t fed, written;
    long long from_pos; /* within the old image: checked at each seek and read */
    size_t cache_off, cache_len;
    int failed;
    unsigned char cache[CACHE_SIZE];
};

static int from_read(void *arg, uint8_t *buf, size_t size)
{
    fc_delta_t *d = arg;
    if (d->from_pos < 0 || (unsigned long long)d->from_pos > d->from_size ||
        size > d->from_size - (size_t)d->from_pos) {
        return -1; /* outside the old image */
    }
    size_t pos = (size_t)d->from_pos;
    while (size > 0) {
        if (pos < d->cache_off || pos >= d->cache_off + d->cache_len) {
            size_t n = d->from_size - pos < CACHE_SIZE ? d->from_size - pos : CACHE_SIZE;
            if (d->read(pos, d->cache, n, d->ctx) != 0) {
                d->cache_len = 0;
                return -1;
            }
            d->cache_off = pos;
            d->cache_len = n;
        }
        size_t n = d->cache_off + d->cache_len - pos;
        if (n > size) {
            n = size;
        }
        memcpy(buf, d->cache + (pos - d->cache_off), n);
        buf += n;
        pos += n;
        size -= n;
    }
    d->from_pos = (long long)pos;
    return 0;
}

/* A patch made from two images never leaves the old one. Refusing here also
 * keeps detools' own count of the position, an int, from overflowing. */
static int from_seek(void *arg, int offset)
{
    fc_delta_t *d = arg;
    long long pos = d->from_pos + offset;
    if (pos < 0 || (unsigned long long)pos > d->from_size) {
        return -1;
    }
    d->from_pos = pos;
    return 0;
}

static int to_write(void *arg, const uint8_t *buf, size_t size)
{
    fc_delta_t *d = arg;
    if (size > d->to_size - d->written) {
        return -1; /* more than the new image holds */
    }
    if (d->write(buf, size, d->ctx) != 0) {
        return -1;
    }
    d->written += size;
    return 0;
}

fc_delta_t *fc_delta_begin(size_t patch_size, size_t from_size, size_t to_size, fc_delta_read_cb read,
                           fc_delta_write_cb write, void *ctx)
{
    fc_delta_t *d = calloc(1, sizeof(*d));
    if (d == NULL) {
        return NULL;
    }
    d->read = read;
    d->write = write;
    d->ctx = ctx;
    d->from_size = from_size;
    d->to_size = to_size;
    d->patch_size = patch_size;
    detools_apply_patch_init(&d->apply, from_read, from_seek, patch_size, to_write, d);
    return d;
}

int fc_delta_feed(fc_delta_t *d, const void *patch, size_t len)
{
    if (d->failed || len > d->patch_size - d->fed) {
        d->failed = 1;
        return -1;
    }
    d->fed += len;
    int rc = detools_apply_patch_process(&d->apply, patch, len);
    if (rc < 0) {
        fc_port_log(FC_LOG_WARN, "firmware delta: %s", detools_error_as_string(rc));
        d->failed = 1;
        return -1;
    }
    return 0;
}

int fc_delta_end(fc_delta_t *d)
{
    int rc = -1;
    if (d->failed) {
        /* No more decoding after an error: only free the decompressor. */
        if (d->apply.patch_reader.destroy != NULL) {
            d->apply.patch_reader.destroy(&d->apply.patch_reader);
        }
    } else {
        rc = detools_apply_patch_finalize(&d->apply); /* the new image's size */
    }
    int ok = !d->failed && d->fed == d->patch_size && rc >= 0 && (size_t)rc == d->to_size &&
             d->written == d->to_size;
    if (!d->failed && !ok) {
        fc_port_log(FC_LOG_WARN, "firmware delta: %s",
                    rc < 0 ? detools_error_as_string(rc) : "the patch is for an image of another size");
    }
    free(d);
    return ok ? 0 : -1;
}
