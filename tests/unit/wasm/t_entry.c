/* Built for no device in particular, as CoreMark is: env.clock_ms (i32) and a
 * run() that returns a float. Run once through FC_ENTRY, past the budget of a
 * call. */
#include <stdint.h>

__attribute__((import_module("env"), import_name("clock_ms"))) int32_t clock_ms(void);

__attribute__((export_name("run"))) float run(void)
{
    volatile int32_t n = 0;
    while (clock_ms() < 300) {
        for (int i = 0; i < 1000; i++) {
            n++;
        }
    }
    return 1234.5678f;
}
