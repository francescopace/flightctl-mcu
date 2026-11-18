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
 * enough. NULL when there is no block worth holding. Given back with
 * fc_heap_unhold (NULL is fine). (fc_http.c) */
void *fc_heap_hold(size_t leave);
/* Give back the bytes above a nonzero limit. Use the returned pointer. */
void *fc_heap_trim_hold(void *hold, size_t limit);
void fc_heap_unhold(void *hold);

/*
 * The heap's low-water mark over a stretch of work, what is held above left
 * out: a hold is not used, and during a TLS handshake it took the largest
 * block but 24 KB, so the heap's own low (fc_port_heap_watch) fell by that
 * much. The port's watch runs while a stretch is open; at each hold taken or
 * given back its low is read and the bytes held meanwhile added to it.
 * fc_heap_low_begin returns a stretch (>= 0), or -1 when the port cannot
 * watch or FC_HEAP_LOWS stretches are open; fc_heap_low_peek reads it,
 * fc_heap_low_end reads and closes it. Both return the low with the holds
 * left out, and in *raw (may be NULL) the heap's own; -1 when unknown.
 */
#define FC_HEAP_LOWS 4
int fc_heap_low_begin(void);
long fc_heap_low_peek(int stretch, long *raw);
long fc_heap_low_end(int stretch, long *raw);
