#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { ARENA = 40960, CAPACITY = ARENA - 12, SLOTS = 80, GROUPS = 7 };
FC_HEAP(ARENA);
__attribute__((import_module("env"), import_name("clock_ms"))) unsigned clock_ms(void);

/* Both versions put the first payload at offset 8. Verify every address before
 * using this layout to construct separated holes without an extra pointer array. */
static unsigned char *arena(void)
{
    return (unsigned char *)fc_heap_arena_;
}

static unsigned char *payload(unsigned index)
{
    return arena() + 8 + index * 8;
}

static int full_recovery(void)
{
    unsigned char *all = malloc(CAPACITY);
    if (all != payload(0) || malloc(1) != NULL || malloc(CAPACITY + 1) != NULL) {
        return 0;
    }
    memset(all, 0x59, CAPACITY);
    free(all);
    return 1;
}

static uint32_t random_state = 1789;
static uint32_t next_random(void)
{
    random_state = random_state * 1664525u + 1013904223u;
    return random_state;
}

static int intact(const unsigned char *p, unsigned size, unsigned char tag)
{
    for (unsigned i = 0; i < size; ++i) {
        if (p[i] != tag) {
            return 0;
        }
    }
    return 1;
}

static int churn(void)
{
    unsigned char *slot[SLOTS] = {0};
    unsigned size[SLOTS];
    unsigned failures = 0;
    for (unsigned epoch = 0; epoch < 12; ++epoch) {
        for (unsigned i = 0; i < 2048; ++i) {
            unsigned k = (next_random() >> 12) % SLOTS;
            unsigned n = 1 + (next_random() >> 12) % 2048;
            int occupied = slot[k] != NULL;
            if (occupied && !intact(slot[k], size[k], k + 1)) {
                return 0;
            }
            if (n % 3 == 0) {
                free(slot[k]);
                slot[k] = NULL;
                continue;
            }
            unsigned char *p = realloc(slot[k], n);
            if (!p) {
                ++failures;
                continue;
            }
            if (occupied && !intact(p, n < size[k] ? n : size[k], k + 1)) {
                return 0;
            }
            memset(p, k + 1, n);
            slot[k] = p;
            size[k] = n;
        }
        for (unsigned k = 0; k < SLOTS; ++k) {
            if (slot[k] && !intact(slot[k], size[k], k + 1)) {
                return 0;
            }
            free(slot[k]);
            slot[k] = NULL;
        }
        if (!full_recovery()) {
            return 0;
        }
    }
    printf("FRAG churn ops=24576 recovered=12 failures=%u\n", failures);
    return 1;
}

/* Replay one real free from exactly the same allocator state. Either allocator
 * can update the head, predecessor or target. A two-sided merge only reads the
 * right neighbor. Undo these words identically for the real and empty calls. */
struct replay {
    unsigned *head, *prior, *target;
    unsigned head_value, prior_size, prior_next, target_size, target_next;
};
static void undo(const struct replay *r)
{
    *r->head = r->head_value;
    r->prior[0] = r->prior_size;
    r->prior[1] = r->prior_next;
    r->target[0] = r->target_size;
    r->target[1] = r->target_next;
}

static void empty(void *p)
{
    __asm__ volatile("" : : "r"(p) : "memory");
}

static void (*volatile operation)(void *);
static unsigned batch(const struct replay *r, unsigned count, int real)
{
    operation = real ? free : empty;
    unsigned start = clock_ms();
    for (unsigned i = 0; i < count; ++i) {
        operation(r->target + 1);
        undo(r);
    }
    return clock_ms() - start;
}

static uint32_t checksum(void)
{
    uint32_t h = 2166136261u;
    for (unsigned i = 0; i < ARENA; ++i) {
        h = (h ^ arena()[i]) * 16777619u;
    }
    return h;
}

static int measure(unsigned holes, unsigned target, unsigned prior, int both)
{
    struct replay r = {.head = (unsigned *)arena(),
                       .prior = (unsigned *)payload(prior) - 1,
                       .target = (unsigned *)payload(target) - 1};
    r.head_value = *r.head;
    r.prior_size = r.prior[0];
    r.prior_next = r.prior[1];
    r.target_size = r.target[0];
    r.target_next = r.target[1];
    uint32_t original = checksum();
    unsigned count = 64;
    while (batch(&r, count, 1) < 100 && count < 262144) {
        count *= 2;
    }
    unsigned raw[GROUPS], control[GROUPS], max_ms = 0;
    for (unsigned g = 0; g < GROUPS; ++g) {
        if (!(g & 1)) {
            control[g] = batch(&r, count, 0);
        }
        raw[g] = batch(&r, count, 1);
        if (g & 1) {
            control[g] = batch(&r, count, 0);
        }
    }
    operation = free;
    for (unsigned i = 0; i < 256; ++i) {
        unsigned start = clock_ms();
        operation(r.target + 1);
        unsigned dt = clock_ms() - start;
        if (dt > max_ms) {
            max_ms = dt;
        }
        undo(&r);
    }
    if (checksum() != original) {
        return 0;
    }
    printf("FRAG case %u %d n=%u max=%u\n", holes, both, count, max_ms);
    printf("FRAG real %u %u %u %u %u %u %u\n", raw[0], raw[1], raw[2], raw[3], raw[4], raw[5], raw[6]);
    printf("FRAG ctrl %u %u %u %u %u %u %u\n", control[0], control[1], control[2], control[3], control[4],
           control[5], control[6]);
    return 1;
}

__attribute__((export_name("run"))) int run(void)
{
    if (!full_recovery() || !churn()) {
        return -1;
    }
    /* Minimum-sized blocks. The last allocation absorbs the final 8-byte
     * remainder, giving 5,118 live blocks and up to 2,559 separated holes. */
    const unsigned blocks = 5118, last = blocks - 1;
    for (unsigned i = 0; i < blocks; ++i) {
        if (malloc(4) != payload(i)) {
            return -2;
        }
    }
    if (malloc(1)) {
        return -3;
    }
    unsigned holes = 0;
    static const volatile unsigned levels[] = {0, 128, 1024, 2559};
    for (unsigned k = 0; k < sizeof(levels) / sizeof(levels[0]); ++k) {
        /* Descending order within each group limits setup cost. */
        for (unsigned j = levels[k]; j > holes; --j) {
            free(payload(2 * (j - 1)));
        }
        holes = levels[k];
        if (holes == 2559 && malloc(5) != NULL) {
            return -7;
        }
        if (!measure(holes, last, holes ? 2 * (holes - 1) : 0, 0)) {
            return -4;
        }
    }
    /* The immediately preceding odd block now has free neighbors on both sides. */
    free(payload(last));
    if (!measure(holes - 1, last - 2, last - 3, 1)) {
        return -5;
    }
    for (unsigned i = last - 2; i > 0; i -= 2) {
        free(payload(i));
        if (i == 1) {
            break;
        }
    }
    if (!full_recovery()) {
        return -6;
    }
    printf("FRAG done capacity=%u blocks=%u\n", CAPACITY, blocks);
    return CAPACITY;
}
