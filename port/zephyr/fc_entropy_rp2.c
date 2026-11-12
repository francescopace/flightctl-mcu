/*
 * Zephyr's entropy device on the RP2040, which has no random generator and,
 * in Zephyr, no entropy driver (the one for Raspberry Pi chips is for the
 * RP2350): the chip's ring oscillator. Its frequency wanders with
 * temperature, voltage and noise, and the RANDOMBIT register samples it. One
 * sample is far from one bit of entropy, so each bit handed out is sixteen
 * samples folded together, taken a few microseconds apart. It is slow (a
 * byte takes 0.4 ms), and it says so (ENTROPY_NEEDS_PRNG): Zephyr and PSA
 * seed their generators from it and do not draw from it directly.
 *
 * Not a certified source: much better than the test generator this replaces,
 * whose output can be predicted, and not measured beyond the check below.
 */
#define DT_DRV_COMPAT flightctl_rp2040_rosc_rng

#include <zephyr/device.h>
#include <zephyr/drivers/entropy.h>
#include <zephyr/kernel.h>

#include <hardware/structs/rosc.h>

#include "fc_port.h"

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

static int rosc_get_entropy(const struct device *dev, uint8_t *buf, uint16_t len)
{
    static int checked;
    ARG_UNUSED(dev);
    if (!checked) {
        if (!rosc_alive()) {
            fc_port_log(FC_LOG_ERROR, "entropy: the ring oscillator gives no random bits");
            return -EIO;
        }
        checked = 1;
    }
    for (uint16_t i = 0; i < len; i++) {
        buf[i] = rosc_byte();
    }
    return 0;
}

static DEVICE_API(entropy, rosc_entropy_api) = {
    .get_entropy = rosc_get_entropy,
};

DEVICE_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, PRE_KERNEL_1, CONFIG_ENTROPY_INIT_PRIORITY, &rosc_entropy_api);
