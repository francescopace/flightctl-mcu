/* An app whose tick never returns: the device must stop it anyway. */
#include "fc_app.h"

FC_APP_ABI()

void tick(void)
{
    volatile int32_t n = 0;
    FC_STATE("looping");
    for (;;) {
        n++;
    }
}
