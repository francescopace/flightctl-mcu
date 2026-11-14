/* Imports a function the device does not have: refused when loaded. */
#include <stdint.h>

__attribute__((import_module("fc"), import_name("teleport"))) int32_t teleport(int32_t where);

__attribute__((export_name("tick"))) void tick(void)
{
    teleport(1);
}
