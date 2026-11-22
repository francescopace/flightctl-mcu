/* malloc, calloc, realloc and free of wasm-apps/sdk/libc/stdlib.h, in a
 * 1 KB arena, built as an app is: the state has a bit set for each check
 * that failed (bad=0 if none did) and what the churn below did, which is the
 * same on every host. Where a check is about where a block is, its address
 * is kept as a number before it is freed or moved, as C allows; what malloc
 * gave goes through a volatile, or clang would take NULL for impossible. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fc_app.h"

FC_APP_ABI()

FC_HEAP(1024);

#define SLOTS 24

static int s_bad, s_bit = 1;
#define CHECK(c) (s_bad |= (c) ? 0 : s_bit, s_bit <<= 1)

static void *volatile s_got;
static void *got(void *p)
{
    s_got = p;
    return s_got;
}

static unsigned at(const void *p)
{
    return (unsigned)(size_t)p;
}

static int all(const unsigned char *p, int c, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        if (p[i] != (unsigned char)c) {
            return 0;
        }
    }
    return 1;
}

/* Blocks of random sizes, each filled with its own byte and checked when it
 * is freed: a block that overlapped another would have lost its bytes. */
static int s_allocs, s_nulls;
static void churn(void)
{
    unsigned char *slot[SLOTS] = {0};
    size_t len[SLOTS];
    unsigned r = 1;
    int ok = 1;
    for (int i = 0; i < 4000; i++) {
        r = r * 1103515245u + 12345u;
        int k = (int)((r >> 16) % SLOTS);
        if (slot[k] != NULL) {
            ok &= all(slot[k], k, len[k]);
            free(slot[k]);
            slot[k] = NULL;
            continue;
        }
        len[k] = 1 + (r >> 8) % 120;
        slot[k] = got(malloc(len[k]));
        if (slot[k] == NULL) {
            s_nulls++;
            continue;
        }
        s_allocs++;
        ok &= (at(slot[k]) & 7) == 0;
        memset(slot[k], k, len[k]);
    }
    for (int k = 0; k < SLOTS; k++) {
        ok &= slot[k] == NULL || all(slot[k], k, len[k]);
        free(slot[k]);
    }
    CHECK(ok);
}

int32_t init(void)
{
    /* Too large, and sizes whose product or rounding overflows. */
    volatile size_t big = 0x10000; /* 0x10000 * 0x10000 is 0 in 32 bits */
    CHECK(got(malloc((size_t)-1)) == NULL && got(malloc(1024)) == NULL && got(malloc(1017)) == NULL);
    CHECK(got(calloc(big, big)) == NULL && got(calloc((size_t)-1, 2)) == NULL);

    /* Until the arena is full: 8-aligned, apart, then NULL. */
    unsigned char *p[32];
    int n = 0;
    while (n < 32 && (p[n] = got(malloc(60))) != NULL) {
        memset(p[n], n, 60);
        n++;
    }
    CHECK(n == 1016 / 64);
    int ok = 1;
    for (int i = 0; i < n; i++) {
        ok &= (at(p[i]) & 7) == 0 && all(p[i], i, 60) && (i == 0 || at(p[i]) >= at(p[i - 1]) + 64);
    }
    CHECK(ok);
    /* Freed, they are joined again: the whole arena in one block. */
    unsigned first = at(p[0]);
    for (int i = 0; i < n; i++) {
        free(p[i]);
    }
    unsigned char *whole = got(malloc(1012));
    CHECK(whole != NULL && at(whole) == first);
    free(whole);
    CHECK(got(malloc(0)) != NULL); /* 8 bytes, kept to the end */
    free(NULL);

    /* calloc zeroes a block that was dirty when freed. */
    unsigned char *d = got(malloc(100));
    memset(d, 0xaa, 100);
    unsigned d_at = at(d);
    free(d);
    unsigned char *z = got(calloc(25, 4));
    CHECK(at(z) == d_at && all(z, 0, 100));

    /* realloc keeps the bytes: in place when the block after it is free,
     * moved when it is not. */
    unsigned char *a = got(realloc(NULL, 16));
    memset(a, 0x5a, 16);
    unsigned a_at = at(a);
    a = got(realloc(a, 200));
    CHECK(at(a) == a_at && all(a, 0x5a, 16));
    unsigned char *b = got(malloc(8));
    a = got(realloc(a, 400));
    CHECK(b != NULL && at(b) == a_at + 208 && a != NULL && at(a) > at(b) && all(a, 0x5a, 16));
    a_at = at(a);
    a = got(realloc(a, 10));
    CHECK(at(a) == a_at);
    free(a);
    free(b);
    free(z);

    /* A free block before a live one stays on the list when realloc grows
     * into the next free block and leaves a reusable tail. */
    unsigned char *left = got(malloc(12));
    unsigned char *middle = got(malloc(12));
    unsigned char *right = got(malloc(60));
    unsigned char *guard = got(malloc(12));
    unsigned left_at = at(left), middle_at = at(middle);
    memset(middle, 0x45, 12);
    memset(guard, 0x72, 12);
    free(left);
    free(right);
    middle = got(realloc(middle, 36));
    CHECK(at(middle) == middle_at && all(middle, 0x45, 12));
    left = got(malloc(12));
    right = got(malloc(36));
    CHECK(at(left) == left_at && at(right) == middle_at + 40 && all(guard, 0x72, 12));
    free(left);
    free(right);
    free(middle);
    free(guard);

    churn();
    /* All freed: the arena is one block again, but for malloc(0)'s. */
    unsigned char *rest = got(malloc(1012 - 8));
    CHECK(rest != NULL);
    free(rest);

    char out[64];
    int m = snprintf(out, sizeof(out), "bad=%x allocs=%d nulls=%d", s_bad, s_allocs, s_nulls);
    fc_set_state(out, m);
    return 0;
}

void tick(void) {}
