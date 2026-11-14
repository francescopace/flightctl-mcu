/* Sets its state to "v1" once: test_apps.c makes "v2" by changing that
 * string in the module. */
#include "fc_app.h"

FC_APP_ABI()

int32_t init(void)
{
    FC_STATE("v1");
    return 0;
}

void tick(void) {}
