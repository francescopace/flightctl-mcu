/*
 * Firmware delta: a detools sequential patch, compressed with heatshrink,
 * applied as it arrives (fc_delta.h). The format, as detools 0.53 writes it:
 *
 *   a byte: the patch type (0, sequential) and its compression (4, heatshrink);
 *   the new image's size, as a size (below);
 *   a byte: heatshrink's window (high 4 bits + 4) and lookahead (low 4 bits + 3);
 *   heatshrink's stream, which holds, once expanded:
 *     the size of a data-format patch, which must be 0 (none);
 *     then, until the new image is complete:
 *       a size n, then n diff bytes, each added to the next byte of the old image;
 *       a size n, then n extra bytes, taken as they are;
 *       a signed size: how far to move in the old image.
 *
 * A size is bsdiff's: the first byte holds 6 bits of the value, a sign (0x40)
 * and "more follows" (0x80); each next byte 7 bits and "more follows".
 *
 * Heatshrink is LZSS on bits, most significant first: a 1 and 8 bits are a
 * byte; a 0, `window` bits (the distance back, minus 1) and `lookahead` bits
 * (the length, minus 1) repeat earlier output. The window starts as zeros.
 * Up to 7 zero bits pad the last byte; they can never make a whole token.
 *
 * Every size is checked against what is left of its image: the decoder never
 * reads outside the old image nor writes past the new one's size, and the
 * patch must end where the new image does.
 */
#include "fc_delta.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fc_port.h"

#define PATCH_SEQUENTIAL_HEATSHRINK 0x04

/* Old-image reads are mostly in order: read ahead. */
#define CACHE_SIZE 2048
/* New-image bytes go out in blocks of this size. */
#define OUT_SIZE 256

enum stage { ST_TYPE, ST_TO_SIZE, ST_WINDOW, ST_STREAM, ST_FAILED };
enum token { TK_TAG, TK_BYTE, TK_INDEX, TK_COUNT };
enum step { SP_DFPATCH, SP_DIFF_SIZE, SP_DIFF, SP_EXTRA_SIZE, SP_EXTRA, SP_SEEK, SP_DONE };

/* A size being read, one byte at a time. */
typedef struct {
    uint64_t value;
    unsigned shift; /* 0: the first byte is next */
    int negative;
} size_reader_t;

struct fc_delta {
    fc_delta_read_cb read;
    fc_delta_write_cb write;
    void *ctx;
    size_t from_size, to_size, patch_size;
    size_t fed;
    size_t from_pos; /* the next byte of the old image a diff adds to */
    size_t to_pos;   /* new-image bytes made so far, sent or in `out` */
    enum stage stage;
    size_reader_t size;
    /* heatshrink */
    unsigned window_bits, count_bits;
    unsigned char *window;
    size_t head;
    enum token token;
    unsigned need; /* bits left of the token's field */
    uint32_t bits;
    size_t distance;
    /* the patch inside the stream */
    enum step step;
    size_t left; /* bytes left of a diff or extra block */
    size_t cache_off, cache_len;
    size_t out_len;
    const char *why; /* the first error */
    unsigned char out[OUT_SIZE];
    unsigned char cache[CACHE_SIZE];
};

static int fail(fc_delta_t *d, const char *why)
{
    if (d->stage != ST_FAILED) {
        d->stage = ST_FAILED;
        d->why = why;
    }
    return -1;
}

/* Takes one byte of a size; 1 when the size is complete, 0 when more is
 * needed, -1 when it does not fit in 63 bits. */
static int size_byte(size_reader_t *s, unsigned char c)
{
    uint64_t part;
    if (s->shift == 0) {
        s->value = c & 0x3f;
        s->negative = (c & 0x40) != 0;
        s->shift = 6;
    } else {
        if (s->shift > 62) {
            return -1;
        }
        part = (uint64_t)(c & 0x7f) << s->shift;
        if ((part >> s->shift) != (uint64_t)(c & 0x7f) || (part >> 63) != 0) {
            return -1;
        }
        s->value |= part;
        s->shift += 7;
    }
    if (c & 0x80) {
        return 0;
    }
    s->shift = 0;
    return 1;
}

static int flush(fc_delta_t *d)
{
    if (d->out_len > 0 && d->write(d->out, d->out_len, d->ctx) != 0) {
        return fail(d, "cannot write the new image");
    }
    d->out_len = 0;
    return 0;
}

static int from_byte(fc_delta_t *d, unsigned char *c)
{
    size_t pos = d->from_pos;
    if (pos >= d->from_size) {
        return fail(d, "a diff reads past the old image");
    }
    if (pos < d->cache_off || pos - d->cache_off >= d->cache_len) {
        size_t n = d->from_size - pos < CACHE_SIZE ? d->from_size - pos : CACHE_SIZE;
        if (d->read(pos, d->cache, n, d->ctx) != 0) {
            d->cache_len = 0;
            return fail(d, "cannot read the old image");
        }
        d->cache_off = pos;
        d->cache_len = n;
    }
    *c = d->cache[pos - d->cache_off];
    d->from_pos = pos + 1;
    return 0;
}

static int emit(fc_delta_t *d, unsigned char c)
{
    d->out[d->out_len++] = c;
    d->to_pos++;
    return d->out_len == OUT_SIZE ? flush(d) : 0;
}

/* A block's size: what is left of the new image bounds it. */
static int block_size(fc_delta_t *d, enum step data, enum step after)
{
    if (d->size.negative || d->size.value > d->to_size - d->to_pos) {
        return fail(d, "a block runs past the new image");
    }
    d->left = (size_t)d->size.value;
    d->step = d->left > 0 ? data : after;
    return 0;
}

/* One byte of the expanded stream. */
static int patch_byte(fc_delta_t *d, unsigned char c)
{
    int r;
    switch (d->step) {
    case SP_DIFF:
    case SP_EXTRA: {
        unsigned char out = c;
        if (d->step == SP_DIFF) {
            unsigned char old;
            if (from_byte(d, &old) != 0) {
                return -1;
            }
            out = (unsigned char)(c + old);
        }
        if (emit(d, out) != 0) {
            return -1;
        }
        if (--d->left == 0) {
            d->step = d->step == SP_DIFF ? SP_EXTRA_SIZE : SP_SEEK;
        }
        return 0;
    }
    case SP_DONE:
        return fail(d, "data after the end of the new image");
    default:
        break;
    }
    r = size_byte(&d->size, c);
    if (r < 0) {
        return fail(d, "a size too large");
    }
    if (r == 0) {
        return 0;
    }
    switch (d->step) {
    case SP_DFPATCH:
        if (d->size.value != 0) {
            return fail(d, "data-format patches are not supported");
        }
        d->step = SP_DIFF_SIZE;
        return 0;
    case SP_DIFF_SIZE:
        return block_size(d, SP_DIFF, SP_EXTRA_SIZE);
    case SP_EXTRA_SIZE:
        return block_size(d, SP_EXTRA, SP_SEEK);
    case SP_SEEK: {
        uint64_t v = d->size.value;
        if (d->size.negative ? v > d->from_pos : v > d->from_size - d->from_pos) {
            return fail(d, "a move outside the old image");
        }
        d->from_pos = d->size.negative ? d->from_pos - (size_t)v : d->from_pos + (size_t)v;
        d->step = d->to_pos == d->to_size ? SP_DONE : SP_DIFF_SIZE;
        return 0;
    }
    default:
        return fail(d, "internal error");
    }
}

/* One expanded byte: into the window, then into the patch. */
static int expanded(fc_delta_t *d, unsigned char c)
{
    d->window[d->head++ & (((size_t)1 << d->window_bits) - 1)] = c;
    return patch_byte(d, c);
}

/* One byte of heatshrink's stream, bit by bit. */
static int stream_byte(fc_delta_t *d, unsigned char c)
{
    for (int i = 7; i >= 0; i--) {
        unsigned bit = (c >> i) & 1;
        if (d->token == TK_TAG) {
            d->token = bit ? TK_BYTE : TK_INDEX;
            d->need = bit ? 8 : d->window_bits;
            d->bits = 0;
            continue;
        }
        d->bits = (d->bits << 1) | bit;
        if (--d->need > 0) {
            continue;
        }
        if (d->token == TK_BYTE) {
            d->token = TK_TAG;
            if (expanded(d, (unsigned char)d->bits) != 0) {
                return -1;
            }
        } else if (d->token == TK_INDEX) {
            d->distance = (size_t)d->bits + 1;
            d->token = TK_COUNT;
            d->need = d->count_bits;
            d->bits = 0;
        } else {
            size_t mask = ((size_t)1 << d->window_bits) - 1;
            size_t count = (size_t)d->bits + 1;
            d->token = TK_TAG;
            while (count-- > 0) {
                if (expanded(d, d->window[(d->head - d->distance) & mask]) != 0) {
                    return -1;
                }
            }
        }
    }
    return 0;
}

static int header_byte(fc_delta_t *d, unsigned char c)
{
    switch (d->stage) {
    case ST_TYPE:
        if (c != PATCH_SEQUENTIAL_HEATSHRINK) {
            return fail(d, "not a sequential patch compressed with heatshrink");
        }
        d->stage = ST_TO_SIZE;
        return 0;
    case ST_TO_SIZE: {
        int r = size_byte(&d->size, c);
        if (r < 0) {
            return fail(d, "a size too large");
        }
        if (r == 1) {
            if (d->size.negative || d->size.value != d->to_size) {
                return fail(d, "the patch is for an image of another size");
            }
            d->stage = ST_WINDOW;
        }
        return 0;
    }
    case ST_WINDOW:
        d->window_bits = (c >> 4) + 4;
        d->count_bits = (c & 0xf) + 3;
        /* heatshrink's own limits, and the device's for its window. */
        if (d->window_bits > FC_DELTA_WINDOW_BITS || d->count_bits >= d->window_bits) {
            return fail(d, "a heatshrink window larger than the device allows");
        }
        d->window = calloc((size_t)1 << d->window_bits, 1);
        if (d->window == NULL) {
            return fail(d, "out of memory");
        }
        d->stage = ST_STREAM;
        return 0;
    default:
        return fail(d, "internal error");
    }
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
    d->stage = ST_TYPE;
    d->token = TK_TAG;
    d->step = SP_DFPATCH;
    return d;
}

int fc_delta_feed(fc_delta_t *d, const void *patch, size_t len)
{
    const unsigned char *p = patch;
    if (d->stage == ST_FAILED) {
        return -1;
    }
    if (len > d->patch_size - d->fed) {
        return fail(d, "more than the patch's size");
    }
    d->fed += len;
    for (size_t i = 0; i < len; i++) {
        int r = d->stage == ST_STREAM ? stream_byte(d, p[i]) : header_byte(d, p[i]);
        if (r != 0) {
            return -1;
        }
    }
    return 0;
}

int fc_delta_end(fc_delta_t *d)
{
    if (d->stage != ST_FAILED) {
        if (d->fed != d->patch_size) {
            fail(d, "the patch is incomplete");
        } else if (d->to_size == 0 ? d->stage != ST_WINDOW : d->stage != ST_STREAM || d->step != SP_DONE) {
            /* An empty image's patch is its header alone. */
            fail(d, "the patch ends before the new image");
        } else if (flush(d) == 0 && d->to_pos != d->to_size) {
            fail(d, "the patch ends before the new image");
        }
    }
    int ok = d->stage != ST_FAILED;
    if (!ok) {
        fc_port_log(FC_LOG_WARN, "firmware delta: %s", d->why);
    }
    free(d->window);
    free(d);
    return ok ? 0 : -1;
}
