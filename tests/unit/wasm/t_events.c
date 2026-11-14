/* Handles each config event slowly, so that a burst overflows its queue:
 * "lost <n>" when it hears of it. */
#include "fc_app.h"

FC_APP_ABI()

void on_event(int32_t type, int32_t h, int32_t value)
{
    if (type == FC_EV_OVERFLOW) {
        char s[16] = "lost ";
        int32_t n = 5;
        n += fc_fmt_int(s + n, value);
        fc_set_state(s, n);
    } else if (type == FC_EV_CONFIG) {
        fc_sleep_ms(50);
    }
}
