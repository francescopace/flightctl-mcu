/* A tick that runs the app's own instructions, with a C library call now and
 * then: its time is almost all the budget's, so it is stopped by the budget,
 * however long that lasts, not by the limit on the device's functions. */
#include <string.h>

#include "fc_app.h"

FC_APP_ABI()

static char s_text[16] = "budget";

void tick(void)
{
    unsigned n = 0;
    for (;;) {
        for (int i = 0; i < 1000; i++) {
            n = n * 1103515245u + 12345u;
        }
        s_text[0] = (char)('a' + n % 26);
        n += (unsigned)strlen(s_text);
        __asm__ volatile("" : : "r"(s_text), "r"(n) : "memory");
    }
}
