/* "arduino" in an app with tick(): a delay counts against the call's limit
 * in the device's functions. Each tick waits `ms` (150 at first, past one of
 * fc's waits) and says so; the fourth asks for 1500, past the limit. */
#include "fc_app.h"

FC_APP_ABI()

__attribute__((import_module("arduino"), import_name("delay"))) void delay(uint32_t ms);

static int32_t ticks;

void tick(void)
{
    ticks++;
    delay(ticks < 4 ? 150 : 1500);
    char s[] = "waited 0";
    s[7] = (char)('0' + ticks);
    fc_set_state(s, sizeof(s) - 1);
}
