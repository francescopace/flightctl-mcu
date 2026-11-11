/*
 * Fuzzes the firmware delta decoder (fc_delta.c over detools and heatshrink)
 * with arbitrary patches against a fixed old image. A device decodes a patch
 * before its digest is checked, so the decoder sees whatever the network
 * gives it: it must never read outside the old image, write more than the
 * new image holds, or allocate more than its window.
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "fc_delta.h"
#include "fc_port.h"

#define OLD_SIZE 4096
#define NEW_SIZE 4096

static unsigned char s_old[OLD_SIZE];
static size_t s_written;

static int old_read(size_t off, void *buf, size_t len, void *ctx)
{
    if (off > OLD_SIZE || len > OLD_SIZE - off) {
        abort(); /* fc_delta lets a read through only inside the image */
    }
    memcpy(buf, s_old + off, len);
    return 0;
}

static int new_write(const void *data, size_t len, void *ctx)
{
    s_written += len;
    if (s_written > NEW_SIZE) {
        abort(); /* never more than the new image */
    }
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    for (size_t i = 0; i < OLD_SIZE; i++) {
        s_old[i] = (unsigned char)(i * 31 + (i >> 8));
    }
    s_written = 0;
    fc_delta_t *d = fc_delta_begin(size, OLD_SIZE, NEW_SIZE, old_read, new_write, NULL);
    if (d == NULL) {
        return 0;
    }
    /* In pieces, as from the network: the first byte says how large. */
    size_t step = size > 0 ? (size_t)data[0] + 1 : 1;
    for (size_t off = 0; off < size;) {
        size_t n = size - off < step ? size - off : step;
        if (fc_delta_feed(d, data + off, n) != 0) {
            break;
        }
        off += n;
    }
    fc_delta_end(d);
    return 0;
}

void fc_port_log(fc_log_level_t level, const char *fmt, ...) {}
