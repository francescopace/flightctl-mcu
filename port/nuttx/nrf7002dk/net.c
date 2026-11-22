/* The nRF5340 RNG and TEMP peripherals belong to the network core. */
#include <nuttx/config.h>
#include <nuttx/arch.h>
#include <nuttx/fs/fs.h>
#include <nuttx/mutex.h>
#include <errno.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include "arm_internal.h"
#include "hardware/nrf53_memorymap.h"
#include "hardware/nrf53_temp.h"

#define RNG_START (NRF53_RNG_BASE + 0x000)
#define RNG_STOP (NRF53_RNG_BASE + 0x004)
#define RNG_READY (NRF53_RNG_BASE + 0x100)
#define RNG_CONFIG (NRF53_RNG_BASE + 0x504)
#define RNG_VALUE (NRF53_RNG_BASE + 0x508)
static mutex_t rng_lock = NXMUTEX_INITIALIZER;
static mutex_t temp_lock = NXMUTEX_INITIALIZER;

static bool ready(uintptr_t event)
{
    for (unsigned i = 0; i < 1000; i++) {
        if (getreg32(event)) return true;
        up_udelay(10);
    }
    return false;
}

static ssize_t random_read(struct file *f, char *buf, size_t len)
{
    int rc = nxmutex_lock(&rng_lock);
    if (rc < 0) return rc;
    putreg32(1, RNG_CONFIG); /* Bias correction enabled. */
    putreg32(0, RNG_READY);
    putreg32(1, RNG_START);
    size_t count = 0;
    while (count < len && ready(RNG_READY)) {
        buf[count++] = (uint8_t)getreg32(RNG_VALUE);
        putreg32(0, RNG_READY);
    }
    putreg32(1, RNG_STOP);
    nxmutex_unlock(&rng_lock);
    return count || !len ? (ssize_t)count : -EIO;
}

static ssize_t temperature_read(struct file *f, char *buf, size_t len)
{
    if (len != sizeof(int32_t)) return -EINVAL;
    int rc = nxmutex_lock(&temp_lock);
    if (rc < 0) return rc;
    putreg32(0, NRF53_TEMP_EVENTS_DATARDY);
    putreg32(1, NRF53_TEMP_TASKS_START);
    if (ready(NRF53_TEMP_EVENTS_DATARDY)) {
        int32_t mc = (int32_t)getreg32(NRF53_TEMP_TEMP) * 250;
        memcpy(buf, &mc, sizeof(mc));
        rc = sizeof(mc);
    } else {
        rc = -ETIMEDOUT;
    }
    putreg32(1, NRF53_TEMP_TASKS_STOP);
    nxmutex_unlock(&temp_lock);
    return rc;
}

static const struct file_operations rng_ops = { .read = random_read };
static const struct file_operations temp_ops = { .read = temperature_read };

void devurandom_register(void)
{
    register_driver("/dev/urandom", &rng_ops, 0444, NULL);
}

int fc_netcore_main(int argc, char **argv)
{
    return register_driver("/dev/die_temp", &temp_ops, 0444, NULL);
}
