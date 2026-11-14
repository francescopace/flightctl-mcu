/* tick() with an argument, but importing from "fc": refused when loaded. */
#include <stdint.h>

__attribute__((import_module("fc"), import_name("set_state"))) void set_state(const char *s, int32_t len);

__attribute__((export_name("tick"))) void tick2(int32_t a, int32_t b)
{
    set_state("x", a + b);
}
