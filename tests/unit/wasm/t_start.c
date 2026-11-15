/* A _start() that never returns, with env.clock_ms as an i64: the device runs
 * it once and must still be able to remove it. */
#include <stdint.h>

__attribute__((import_module("env"), import_name("clock_ms"))) int64_t clock_ms(void);

__attribute__((export_name("_start"))) void start(void)
{
    volatile int64_t t = 0;
    for (;;) {
        t += clock_ms();
    }
}
