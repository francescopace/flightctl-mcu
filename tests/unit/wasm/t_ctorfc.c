/* A C constructor that calls the device: it runs as the module is
 * instantiated, with the app's context, so its state is the app's. */
#include "fc_app.h"

FC_APP_ABI()

__attribute__((constructor)) static void hello(void)
{
    FC_STATE("from ctor");
}

void tick(void) {}
