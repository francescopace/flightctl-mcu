/* Experimental DeltaBlue allocator. Reuse the benchmark's arena declaration,
 * workload and entry point. Indices and block sizes below count 32-bit words.
 * Word 0 holds the list head, the last word stays reserved as in FC_HEAP.
 * Every block has a one-word size header. Free blocks hold their next link
 * in the first payload word, so allocated blocks have the usual capacity. */
#include <stdlib.h>
#undef FC_HEAP
#define FC_HEAP(bytes) static unsigned arena[(bytes) / sizeof(unsigned)] __attribute__((aligned(8)))
#include "deltablue.c"

_Static_assert(sizeof(unsigned) == 4, "requires 32-bit allocator words");

/* Take the front of a listed block. As in the SDK, split only if at least
 * 16 bytes remain. Return the number of words actually consumed. */
static unsigned take_block(unsigned *link, unsigned need)
{
    unsigned block = *link;
    unsigned span = arena[block];
    unsigned next = arena[block + 1];
    if (span - need >= 4) {
        unsigned tail = block + need;
        arena[tail] = span - need;
        arena[tail + 1] = next;
        next = tail;
        span = need;
    }
    *link = next;
    arena[block] = span;
    return span;
}

__attribute__((noinline)) void *malloc(size_t n)
{
    if (n > sizeof(arena) - 12) return NULL;
    if (!arena[1]) {
        arena[0] = 1;
        arena[1] = sizeof(arena) / sizeof(arena[0]) - 2;
    }
    unsigned need = fc_heap_need_(n) / sizeof(arena[0]);
    for (unsigned *link = arena; *link; link = &arena[*link + 1]) {
        unsigned block = *link;
        if (arena[block] >= need) {
            take_block(link, need);
            return &arena[block + 1];
        }
    }
    return NULL;
}

/* List order follows physical addresses, so the insertion point also
 * identifies the only free neighbors that can be coalesced. */
static unsigned *insertion_link(unsigned block)
{
    unsigned *link = arena;
    while (*link && *link < block) link = &arena[*link + 1];
    return link;
}

void free(void *p)
{
    if (!p) return;
    unsigned block = (unsigned *)p - arena - 1;
    unsigned *link = insertion_link(block);
    unsigned next = *link;
    if (next && block + arena[block] == next) {
        arena[block] += arena[next];
        next = arena[next + 1];
    }
    if (link != arena) {
        unsigned previous = (unsigned)(link - arena) - 1;
        if (previous + arena[previous] == block) {
            arena[previous] += arena[block];
            *link = next;
            return;
        }
    }
    arena[block + 1] = next;
    *link = block;
}

__attribute__((noinline)) void *calloc(size_t count, size_t size)
{
    return fc_heap_calloc_(count, size);
}

__attribute__((noinline)) void *realloc(void *p, size_t n)
{
    if (!p) return malloc(n);
    if (n > sizeof(arena) - 12) return NULL;
    unsigned block = (unsigned *)p - arena - 1;
    unsigned span = arena[block];
    unsigned need = fc_heap_need_(n) / sizeof(arena[0]);
    if (need <= span) return p;

    unsigned right = block + span;
    unsigned *link = insertion_link(right);
    if (*link == right && arena[right] >= need - span) {
        arena[block] += take_block(link, need - span);
        return p;
    }
    void *new_p = malloc(n);
    if (new_p) {
        __builtin_memcpy(new_p, p, (span - 1) * sizeof(arena[0]));
        free(p);
    }
    return new_p;
}
