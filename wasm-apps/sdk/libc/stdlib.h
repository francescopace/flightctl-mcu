/* Numbers from text, and abort (fc_libc.h). abort() stops the app as a trap
 * does.
 *
 * malloc, calloc, realloc and free are the app's own, not the device's: a
 * small first-fit allocator, here, over an arena in the app's memory. An app
 * that uses them gives the arena's size once, at file scope, in bytes:
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
    static unsigned fc_heap_arena_[((bytes) + 7) / 8 * 2] __attribute__((aligned(8)));                              \
    __attribute__((noinline)) void *malloc(size_t n)                                                                 \
    {                                                                                                                \
        return fc_heap_malloc_((unsigned char *)fc_heap_arena_, sizeof(fc_heap_arena_), n);                          \
    }                                                                                                                \
    void free(void *p)                                                                                               \
    {                                                                                                                \
        fc_heap_free_(p);                                                                                            \
    }                                                                                                                \
    __attribute__((noinline)) void *calloc(size_t count, size_t size)                                                \
    {                                                                                                                \
        return fc_heap_calloc_(count, size);                                                                         \
    }                                                                                                                \
    __attribute__((noinline)) void *realloc(void *p, size_t n)                                                       \
    {                                                                                                                \
        return fc_heap_realloc_(sizeof(fc_heap_arena_), p, n);                                                       \
    }                                                                                                                \
    _Static_assert((bytes) >= 16, "FC_HEAP: at least 16 bytes")

/* Each block starts with a 4-byte word, its size with the header (a multiple
 * of 8), bit 0 set while it is in use; its bytes follow, 8-aligned. The
 * first header is at 4 in the arena, and a used block of size 0 marks the
 * end. Free blocks next to each other are joined when malloc passes them. */
#define FC_HEAP_USED_ 1u
#define FC_HEAP_HDR_(p) ((unsigned *)(p) - 1)

static inline unsigned *fc_heap_next_(unsigned *h)
{
    return (unsigned *)((unsigned char *)h + (*h & ~FC_HEAP_USED_));
}

/* h is free: it takes the free blocks after it. */
static inline void fc_heap_join_(unsigned *h)
{
    for (unsigned *n = fc_heap_next_(h); !(*n & FC_HEAP_USED_); n = fc_heap_next_(h)) {
        *h += *n;
    }
}

/* h is free and at least need: it keeps need, the rest stays free. */
static inline void fc_heap_split_(unsigned *h, unsigned need)
{
    if (*h - need >= 16) {
        *(unsigned *)((unsigned char *)h + need) = *h - need;
        *h = need;
    }
}

static inline unsigned fc_heap_need_(size_t n)
{
    return ((unsigned)n + 4 + 7) & ~7u;
}

static inline void *fc_heap_malloc_(unsigned char *base, unsigned bytes, size_t n)
{
    unsigned *first = (unsigned *)(base + 4);
    if (*first == 0) { /* the first call: one free block, then the end */
        *first = bytes - 8;
        *(unsigned *)(base + bytes - 4) = FC_HEAP_USED_;
    }
    if (n > bytes) {
        return NULL;
    }
    unsigned need = fc_heap_need_(n);
    for (unsigned *h = first; *h != FC_HEAP_USED_; h = fc_heap_next_(h)) {
        if (*h & FC_HEAP_USED_) {
            continue;
        }
        fc_heap_join_(h);
        if (*h >= need) {
            fc_heap_split_(h, need);
            *h |= FC_HEAP_USED_;
            return h + 1;
        }
    }
    return NULL;
}

static inline void fc_heap_free_(void *p)
{
    if (p != NULL) {
        *FC_HEAP_HDR_(p) &= ~FC_HEAP_USED_;
    }
}

/* calloc and realloc call malloc and free, so that its code is there once. */
static inline void *fc_heap_calloc_(size_t count, size_t size)
{
    if (size != 0 && count > (size_t)-1 / size) {
        return NULL;
    }
    void *p = malloc(count * size);
    return p != NULL ? __builtin_memset(p, 0, count * size) : NULL;
}

/* Grows in place into the free blocks after it when it can. */
static inline void *fc_heap_realloc_(unsigned bytes, void *p, size_t n)
{
    if (p == NULL) {
        return malloc(n);
    }
    unsigned *h = FC_HEAP_HDR_(p);
    size_t have = (*h & ~FC_HEAP_USED_) - 4;
    if (n <= have) {
        return p;
    }
    if (n <= bytes) {
        unsigned need = fc_heap_need_(n);
        *h &= ~FC_HEAP_USED_;
        fc_heap_join_(h);
        int fits = *h >= need;
        if (fits) {
            fc_heap_split_(h, need);
        }
        *h |= FC_HEAP_USED_;
        if (fits) {
            return p;
        }
    }
    void *q = malloc(n);
    if (q != NULL) {
        __builtin_memcpy(q, p, have);
        free(p);
    }
    return q;
}
