/*
 * Entropy for mbedTLS on the RP2040, which has no random generator and, in
 * Zephyr 4.3, no entropy driver: the chip's ring oscillator. Its frequency
 * wanders with temperature, voltage and noise, and the RANDOMBIT register
 * samples it. One sample is far from one bit of entropy, so each bit handed
 * out is sixteen samples folded together, taken a few microseconds apart;
 * mbedTLS then hashes what it gathers before seeding its generator.
 *
 * Not a certified source: much better than the test generator this replaces,
 * whose output can be predicted, and not measured beyond the check below.
 */
#include <zephyr/kernel.h>

#include <hardware/structs/rosc.h>

#include "fc_port.h"
#include "mbedtls/entropy.h"

#define SAMPLES_PER_BIT 16

static uint8_t rosc_byte(void)
{
    uint8_t out = 0;
    for (int bit = 0; bit < 8; bit++) {
        uint32_t b = 0;
        for (int i = 0; i < SAMPLES_PER_BIT; i++) {
            b ^= rosc_hw->randombit & 1u;
            k_busy_wait(3);
        }
        out = (uint8_t)((out << 1) | b);
    }
    return out;
}

/* An oscillator that is stopped, or locked to the system clock, gives the
 * same bit every time: refuse to call that entropy. */
static int rosc_alive(void)
{
    unsigned ones = 0;
    for (int i = 0; i < 1024; i++) {
        ones += rosc_hw->randombit & 1u;
        k_busy_wait(3);
    }
    fc_port_log(FC_LOG_INFO, "entropy: ring oscillator, %u ones in 1024 samples", ones);
    return ones > 128 && ones < 896;
}

/* mbedTLS's hardware entropy source (MBEDTLS_ENTROPY_HARDWARE_ALT). */
int mbedtls_hardware_poll(void *data, unsigned char *output, size_t len, size_t *olen)
{
    static int checked;
    ARG_UNUSED(data);
    if (!checked) {
        if (!rosc_alive()) {
            fc_port_log(FC_LOG_ERROR, "entropy: the ring oscillator gives no random bits");
            return MBEDTLS_ERR_ENTROPY_SOURCE_FAILED;
        }
        checked = 1;
    }
    for (size_t i = 0; i < len; i++) {
        output[i] = rosc_byte();
    }
    *olen = len;
    return 0;
}
