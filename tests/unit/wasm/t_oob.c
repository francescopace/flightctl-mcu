/* Asks for the temperature into the last byte of its memory: it must trap. */
#include "fc_app.h"

FC_APP_ABI()

void tick(void)
{
    FC_STATE("reading");
    fc_sensor_read(FC_STR("die_temp"), (int32_t *)(uintptr_t)65535);
    FC_STATE("not trapped");
}
