/* A C constructor that never returns: WAMR runs it (__wasm_call_ctors, which
 * the build exports) as the module is instantiated, before any call of the
 * app. It is stopped by the instruction budget, as a call of the app is. */
#include "fc_app.h"

FC_APP_ABI()

static volatile int s_spin = 1;

__attribute__((constructor)) static void forever(void)
{
    while (s_spin) {
    }
}

void tick(void) {}
