/* "<app> <board> <firmware> <device or the error>". */
#include "fc_app.h"

FC_APP_ABI()

static int32_t put(char *s, int32_t n, const char *key, int32_t klen)
{
    int32_t r = fc_info(key, klen, s + n, 16);
    if (r < 0) {
        return n + fc_fmt_int(s + n, r);
    }
    return n + r;
}

int32_t init(void)
{
    char s[80];
    int32_t n = put(s, 0, FC_STR("app"));
    s[n++] = ' ';
    n = put(s, n, FC_STR("board"));
    s[n++] = ' ';
    n = put(s, n, FC_STR("firmware"));
    s[n++] = ' ';
    n = put(s, n, FC_STR("device"));
    s[n++] = ' ';
    n = put(s, n, FC_STR("color"));
    fc_set_state(s, n);
    return 0;
}

void tick(void) {}
