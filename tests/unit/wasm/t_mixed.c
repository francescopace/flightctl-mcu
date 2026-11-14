/* Imports from both interfaces: refused when loaded. */
#include <stdint.h>

__attribute__((import_module("fc"), import_name("set_state"))) void set_state(const char *s, int32_t len);
__attribute__((import_module("env"), import_name("log"))) void old_log(const char *s, int32_t len);

__attribute__((export_name("tick"))) void tick(void)
{
    set_state("a", 1);
    old_log("b", 1);
}
