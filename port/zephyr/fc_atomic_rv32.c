/* RV32IMC has aligned word loads and stores. GCC emits library calls for
 * read-modify-write operations. Use Zephyr's interrupt-safe atomics. */
#include <stdbool.h>
#include <stdint.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/util.h>

BUILD_ASSERT(sizeof(atomic_val_t) == sizeof(uint32_t));

uint32_t __atomic_exchange_4(volatile void *ptr, uint32_t value, int order)
{
    return (uint32_t)atomic_set((atomic_t *)ptr, (atomic_val_t)value);
}

uint32_t __atomic_fetch_add_4(volatile void *ptr, uint32_t value, int order)
{
    return (uint32_t)atomic_add((atomic_t *)ptr, (atomic_val_t)value);
}

bool __atomic_compare_exchange_4(volatile void *ptr, void *expected, uint32_t desired,
                                 bool weak, int success_order, int failure_order)
{
    atomic_t *word = (atomic_t *)ptr;
    uint32_t prior = *(uint32_t *)expected;
    for (;;) {
        uint32_t observed = (uint32_t)atomic_get(word);
        if (observed != prior) {
            *(uint32_t *)expected = observed;
            return false;
        }
        if (atomic_cas(word, (atomic_val_t)prior, (atomic_val_t)desired)) {
            return true;
        }
    }
}
