#pragma once

#include <stdlib.h>

#include "fc_port.h"

/*
 * Keeping the heap's largest block whole while something long-lived but in
 * small pieces is allocated: a WASM module as it loads (fc_apps.c), a TLS
 * connection as it opens (fc_http.c). The block is held for that time, so the
 * pieces go into the smaller holes, then given back: an app's memory, which
 * comes in one block, finds it whole. Without it, on an ESP32-C5 the agent's
 * connection, opened again after a download while no app held that block,
 * left 52 KB in one piece and a 64 KB memory no longer fitted. Whoever holds
 * it tries again without the hold when what it allocates did not fit.
 */
#define FC_HOLD_MIN (32 * 1024) /* a block worth keeping whole */

/* Holds the largest block but `leave` bytes of it (taken from its end): room
 * there for what is allocated meanwhile, when the smaller holes may not be
 * enough. NULL when there is no block worth holding. */
static inline void *fc_heap_hold(size_t leave)
{
    long big = fc_port_heap_largest();
    return big >= (long)(FC_HOLD_MIN + leave) ? malloc((size_t)big - leave - 64) : NULL;
}
