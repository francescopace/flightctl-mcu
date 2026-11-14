/* Logs with a negative length: it must trap, not read memory. */
#include "fc_app.h"

FC_APP_ABI()

void tick(void)
{
    FC_STATE("logging");
    fc_log(FC_LOG_INFO, "x", -1);
    FC_STATE("not trapped");
}
