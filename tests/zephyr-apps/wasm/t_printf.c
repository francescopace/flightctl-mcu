/* The C library's formatter at its deepest, for the stack of the app's
 * thread (tests/zephyr-apps): printf with %f, widths and precisions, a value
 * with 300 digits before the point, and snprintf's state. */
#include <stdio.h>

#include "fc_app.h"

FC_APP_ABI()

int32_t init(void)
{
    volatile double pi = 3.14159265358979, tiny = -0.000123, big = 1e300;
    volatile long l = -2147483647L;
    printf("f %f %.3f %20.10f %-8.1f|\n", pi, tiny, pi, pi);
    printf("big %f\n", big);
    printf("p %.0f|%.1f|%.2f|%.3f|%.4f|%.5f|%.6f|%.7f\n", pi, pi, pi, pi, pi, pi, pi, pi);
    printf("q %.2f|%.2f|%.2f|%.2f|%.2f\n", 1.0, 2.5, 0.125, 100.0, 3.14);
    printf("d %d %ld %u %x %08x %s %c\n", -1, l, 4000000000u, 0xbeefu, 255u, "str", 'z');
    char out[64];
    int n = snprintf(out, sizeof(out), "%.2f %.1f %d", pi, tiny, 42);
    fc_set_state(out, n);
    return 0;
}

void tick(void) {}
