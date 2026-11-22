/* Numbers from text, and abort (fc_libc.h). abort() stops the app as a trap
 * does.
 *
 * malloc, calloc, realloc and free are the app's own, not the device's: a
 * first-fit over an address-ordered free list in the app's memory. An app
 * gives the arena's size once, at file scope, in bytes:
 *
 *     FC_HEAP(4096);
 *
 * The arena is part of the app's memory as its other arrays are, so it counts
 * in the size the device checks before it loads the app; an app that does
 * not call malloc has neither the arena nor the code. One that calls malloc
 * without FC_HEAP does not link (undefined symbol: fc_malloc). Each
 * block takes 4 bytes more than asked, rounded up to 8; malloc gives NULL
 * when no free block is large enough. Not the memory.grow of wasi-libc's
 * malloc: that would keep the app's whole 64 KB page (docs/APPS.md). */
#pragma once
#include "fc_libc.h"
FC_LIBC(atoi) int atoi(const char *s);
FC_LIBC(strtol) long strtol(const char *s, char **end, int base);
FC_LIBC(strtoul) unsigned long strtoul(const char *s, char **end, int base);
FC_LIBC(abort) _Noreturn void abort(void);

/* Under other names: a function the app's own file defines as malloc, clang
 * would take for the C library's, which touches none of the app's memory,
 * and two calls would give the same block. */
#define malloc fc_malloc
#define calloc fc_calloc
#define realloc fc_realloc
#define free fc_free
void *malloc(size_t n);
void *calloc(size_t count, size_t size);
void *realloc(void *p, size_t n);
void free(void *p);

/* The arena, 8-aligned, of the size given rounded up to 8, and the four
 * functions over it, in one file of the app. noinline: malloc's code once. */
#define FC_HEAP(bytes)                                                                                               \
    static unsigned fc_heap_arena_[((bytes) + 7) / 8 * 2] __attribute__((aligned(8)));                               \
    __attribute__((noinline)) void *malloc(size_t n)                                                                 \
    {                                                                                                                \
        return fc_heap_malloc_(fc_heap_arena_, sizeof(fc_heap_arena_), n);                                           \
    }                                                                                                                \
    void free(void *p)                                                                                               \
    {                                                                                                                \
        fc_heap_free_(fc_heap_arena_, p);                                                                            \
    }                                                                                                                \
    __attribute__((noinline)) void *calloc(size_t count, size_t size)                                                \
    {                                                                                                                \
        return fc_heap_calloc_(count, size);                                                                         \
    }                                                                                                                \
    __attribute__((noinline)) void *realloc(void *p, size_t n)                                                       \
    {                                                                                                                \
        return fc_heap_realloc_(fc_heap_arena_, sizeof(fc_heap_arena_), p, n);                                       \
    }                                                                                                                \
    _Static_assert((bytes) >= 16, "FC_HEAP: at least 16 bytes")

/* Indices and sizes count 32-bit words. arena[0] holds the free-list head.
 * Each block has a one-word size header. Only free blocks store a next link,
 * in their first payload word. The first payload is 8-byte aligned. */
static inline unsigned fc_heap_words_(size_t n)
{
    return ((unsigned)n + 4 + 7) / 8 * 2;
}

/* Take the front of a listed block. Split only if at least
 * 16 bytes remain. Return the number of words actually consumed. */
static inline unsigned fc_heap_take_(unsigned *arena, unsigned *link, unsigned need)
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

static inline void *fc_heap_malloc_(unsigned *arena, unsigned bytes, size_t n)
{
    if (n > bytes - 12) {
        return NULL;
    }
    if (!arena[1]) {
        arena[0] = 1;
        arena[1] = bytes / sizeof(arena[0]) - 2;
    }
    unsigned need = fc_heap_words_(n);
    for (unsigned *link = arena; *link; link = &arena[*link + 1]) {
        unsigned block = *link;
        if (arena[block] >= need) {
            fc_heap_take_(arena, link, need);
            return &arena[block + 1];
        }
    }
    return NULL;
}

/* List order follows physical addresses, so the insertion point also
 * identifies the only free neighbors that can be coalesced. */
static inline unsigned *fc_heap_insertion_(unsigned *arena, unsigned block)
{
    unsigned *link = arena;
    while (*link && *link < block) {
        link = &arena[*link + 1];
    }
    return link;
}

static inline void fc_heap_free_(unsigned *arena, void *p)
{
    if (!p) {
        return;
    }
    unsigned block = (unsigned *)p - arena - 1;
    unsigned *link = fc_heap_insertion_(arena, block);
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

static inline void *fc_heap_calloc_(size_t count, size_t size)
{
    if (size != 0 && count > (size_t)-1 / size) {
        return NULL;
    }
    void *p = malloc(count * size);
    return p != NULL ? __builtin_memset(p, 0, count * size) : NULL;
}

static inline void *fc_heap_realloc_(unsigned *arena, unsigned bytes, void *p, size_t n)
{
    if (!p) {
        return malloc(n);
    }
    if (n > bytes - 12) {
        return NULL;
    }
    unsigned block = (unsigned *)p - arena - 1;
    unsigned span = arena[block];
    unsigned need = fc_heap_words_(n);
    if (need <= span) {
        return p;
    }

    unsigned right = block + span;
    unsigned *link = fc_heap_insertion_(arena, right);
    if (*link == right && arena[right] >= need - span) {
        arena[block] += fc_heap_take_(arena, link, need - span);
        return p;
    }
    void *new_p = malloc(n);
    if (new_p) {
        __builtin_memcpy(new_p, p, (span - 1) * sizeof(arena[0]));
        free(p);
    }
    return new_p;
}
