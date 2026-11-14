/* A periodic timer fires three times, then is closed: a fourth event would
 * show as "late". */
#include "fc_app.h"

FC_APP_ABI()

static int32_t timer, n;

int32_t init(void)
{
    timer = fc_timer_start(20, 1);
    return timer < 0;
}

void on_event(int32_t type, int32_t h, int32_t value)
{
    if (type != FC_EV_TIMER) {
        return;
    }
    if (h != timer) {
        FC_STATE("wrong handle");
        return;
    }
    if (++n == 3) {
        fc_close(timer);
        FC_STATE("timer 3");
    } else if (n > 3) {
        FC_STATE("late");
    }
}
