/* Tail calls (built with -mtail-call): 100000 calls back and forth between two
 * functions, as return_call, without a frame each. */
#include <stdint.h>

__attribute__((noinline)) static int64_t pong(int32_t n, int64_t acc);

__attribute__((noinline)) static int64_t ping(int32_t n, int64_t acc)
{
    if (n == 0) {
        return acc;
    }
    __attribute__((musttail)) return pong(n - 1, acc + 1);
}

__attribute__((noinline)) static int64_t pong(int32_t n, int64_t acc)
{
    if (n == 0) {
        return acc;
    }
    __attribute__((musttail)) return ping(n - 1, acc + 1);
}

__attribute__((export_name("_start"))) int64_t start(void)
{
    volatile int32_t n = 100000;
    return ping(n, 0);
}
