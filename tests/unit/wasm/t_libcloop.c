/* A tick that loops over the C library: memset is native, outside the
 * instruction budget, so the budget alone would let it run for minutes. The
 * call is stopped after a second, as a wait in fc's functions is. */
#include <string.h>

#include "fc_app.h"

FC_APP_ABI()

static char s_big[60000]; /* large enough that the host takes over a second */

void tick(void)
{
    for (;;) {
        memset(s_big, 1, sizeof(s_big));
        __asm__ volatile("" : : "r"(s_big) : "memory"); /* s_big is read: clang keeps the memset */
    }
}
