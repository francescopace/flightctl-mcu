/* The C library with addresses outside the app's memory: each CASE (built by
 * gen-wasm-vectors.sh) must stop the app, not the device. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "fc_app.h"

FC_APP_ABI()

#define FAR ((void *)0x7ffffff0)

int32_t init(void)
{
    char buf[16] = "abc";
#if CASE == 1
    volatile size_t n = sizeof(buf);
    memcpy(FAR, buf, n);
#elif CASE == 2
    snprintf(buf, sizeof(buf), "%s", (char *)FAR);
#elif CASE == 3
    volatile size_t size = 100000;
    snprintf(buf, size, "%d", 1);
#elif CASE == 4
    const char *volatile twelve = "12";
    strtol(twelve, (char **)FAR, 10);
#elif CASE == 5
    abort();
#endif
    fc_set_state(buf, 3);
    return 0;
}

void tick(void) {}
