/* Counts its starts in storage, and checks the limits. */
#include "fc_app.h"

FC_APP_ABI()

static char big[FC_KV_QUOTA + 1];

int32_t init(void)
{
    int32_t n = 0;
    if (fc_kv_get(FC_STR("count"), &n, sizeof(n)) != (int32_t)sizeof(n)) {
        n = 0;
    }
    n++;
    if (fc_kv_set(FC_STR("count"), &n, sizeof(n)) != 0) {
        return 1;
    }
    char s[48];
    int32_t len = 0;
    s[len++] = 'c';
    s[len++] = ' ';
    len += fc_fmt_int(s + len, n);
    /* Too long a key, too big a value, a missing key, a value that fits. */
    s[len++] = ' ';
    len += fc_fmt_int(s + len, fc_kv_set(FC_STR("0123456789012345678901234567890123"), &n, 1));
    s[len++] = ' ';
    len += fc_fmt_int(s + len, fc_kv_set(FC_STR("big"), big, sizeof(big)));
    s[len++] = ' ';
    len += fc_fmt_int(s + len, fc_kv_del(FC_STR("nothere")));
    s[len++] = ' ';
    len += fc_fmt_int(s + len, fc_kv_set(FC_STR("big"), big, 4000));
    fc_set_state(s, len);
    return 0;
}

void tick(void) {}
