/* Built for the earlier interface (module "env", tick(now_ms)): refused. */
#include <stdint.h>

__attribute__((import_module("env"), import_name("set_state"))) void set_state(const char *s, int32_t len);

__attribute__((export_name("tick"))) void tick(int32_t now_ms)
{
    set_state("old", 3 + (now_ms & 0));
}
