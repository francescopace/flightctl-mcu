/* Observe interpreter checkpoints independently of wall-clock time. */
#include <stdatomic.h>

static atomic_uint checkpoints;

void test_wasm_checkpoint(void)
{
    atomic_fetch_add_explicit(&checkpoints, 1, memory_order_relaxed);
}

unsigned test_wasm_checkpoint_count(void)
{
    return atomic_load_explicit(&checkpoints, memory_order_relaxed);
}
