/* A tick that only sleeps, forever: stopped after a second. */
#include "fc_app.h"

FC_APP_ABI()

void tick(void)
{
    FC_STATE("sleeping");
    for (;;) {
        fc_sleep_ms(100);
    }
}
