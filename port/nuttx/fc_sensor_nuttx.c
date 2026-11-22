#include <nuttx/config.h>
#include <errno.h>
#ifdef CONFIG_ARCH_BOARD_RASPBERRYPI_PICO_2_W
#include <nuttx/arch.h>
#include "arm_internal.h"
#include "hardware/rp23xx_adc.h"
#elif defined(CONFIG_ARCH_BOARD_NUCLEO_F767ZI)
#include <nuttx/analog/adc.h>
#include <nuttx/analog/ioctl.h>
#include <poll.h>
#include <sys/ioctl.h>
#endif
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "fc_hw_nuttx.h"

static pthread_mutex_t temp_lock = PTHREAD_MUTEX_INITIALIZER;


int fc_nuttx_sensor(const char *name, int32_t *value)
{
    if (strcmp(name, "die_temp")) {
        return FC_HW_ENOENT;
    }
    int rc = FC_HW_EIO;
    pthread_mutex_lock(&temp_lock);
#ifdef CONFIG_ARCH_BOARD_RASPBERRYPI_PICO_2_W
    /* ADC is dedicated to the die sensor. NuttX starts its 48 MHz clock. */
    uint32_t control = RP23XX_ADC_CS_EN | RP23XX_ADC_CS_TS_EN |
                       (4u << RP23XX_ADC_CS_AINSEL_SHIFT);
    putreg32(control, RP23XX_ADC_CS);
    up_udelay(20);
    if (getreg32(RP23XX_ADC_CS) & RP23XX_ADC_CS_READY) {
        putreg32(control | RP23XX_ADC_CS_START_ONCE, RP23XX_ADC_CS);
        for (unsigned i = 0; i < 100; i++) {
            up_udelay(1);
            uint32_t status = getreg32(RP23XX_ADC_CS);
            if (status & RP23XX_ADC_CS_READY) {
                uint32_t raw = getreg32(RP23XX_ADC_RESULT) & RP23XX_ADC_RESULT_MASK;
                if (!(status & RP23XX_ADC_CS_ERR) && raw && raw < RP23XX_ADC_RESULT_MASK) {
                    int32_t uv = (int64_t)raw * 3300000 / 4096;
                    *value = 27000 - (uv - 706000) * (int64_t)1000 / 1721;
                    rc = 0;
                }
                break;
            }
        }
    }
    putreg32(0, RP23XX_ADC_CS);
#elif defined(CONFIG_FLIGHTCTL_NRF7002DK)
    int fd = open("/dev/die_temp", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        if (read(fd, value, sizeof(*value)) == sizeof(*value)) rc = 0;
        close(fd);
    }
#elif defined(CONFIG_ARCH_BOARD_NUCLEO_F767ZI)
    int fd = open("/dev/adc0", O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd >= 0) {
        struct pollfd ready = { .fd = fd, .events = POLLIN };
        struct adc_msg_s sample;
        /* F767 factory ADC counts at 30 and 110 C with a 3.3 V reference. */
        int32_t cal30 = *(const volatile uint16_t *)0x1ff0f44c;
        int32_t cal110 = *(const volatile uint16_t *)0x1ff0f44e;
        usleep(20);
        if (ioctl(fd, ANIOC_TRIGGER, 0) == 0 && poll(&ready, 1, 100) == 1 &&
            read(fd, &sample, sizeof(sample)) == sizeof(sample) && sample.am_channel == 18 &&
            sample.am_data > 0 && sample.am_data < 4096 && cal30 > 0 && cal30 < 4096 &&
            cal110 > 0 && cal110 < 4096 && cal110 != cal30) {
            *value = 30000 + (sample.am_data - cal30) * 80000 / (cal110 - cal30);
            rc = 0;
        }
        close(fd);
    }
#else
    int temp_fd = open(CONFIG_ESPRESSIF_TEMP_PATH, O_RDONLY | O_CLOEXEC);
    if (temp_fd >= 0) {
        float degrees = 0;
        /* The NuttX temperature driver fills the float and returns OK (0). */
        if (read(temp_fd, &degrees, sizeof(degrees)) >= 0) {
            *value = (int32_t)(degrees * 1000.0f);
            rc = 0;
        }
        close(temp_fd);
    }
#endif
    pthread_mutex_unlock(&temp_lock);
    return rc;
}

static pthread_mutex_t subscription_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake = PTHREAD_COND_INITIALIZER;
static pthread_t sampler;
static int mode, zone, closing;
static int32_t low, high;
static void (*notify)(void *);
static void *context;

static int temperature_zone(int32_t mc)
{
    return mc <= low ? -1 : mc >= high ? 1 : 0;
}

static void *sample(void *arg)
{
    pthread_mutex_lock(&subscription_lock);
    while (mode) {
        struct timespec deadline;
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_sec++;
        int rc = 0;
        while (mode && rc == 0) {
            rc = pthread_cond_timedwait(&wake, &subscription_lock, &deadline);
        }
        int32_t mc;
        if (mode && rc == ETIMEDOUT && fc_nuttx_sensor("die_temp", &mc) == 0) {
            int next = temperature_zone(mc);
            if (mode == FC_HW_SENSOR_READY || next != zone) {
                notify(context);
            }
            zone = next;
        }
    }
    pthread_mutex_unlock(&subscription_lock);
    return NULL;
}

int fc_nuttx_subscribe(int id, int requested, int32_t lo, int32_t hi, void (*cb)(void *), void *ctx)
{
    if (id != -1) return FC_HW_ENOTSUP;
    if (!cb || (requested != FC_HW_SENSOR_READY && (requested != FC_HW_SENSOR_THRESHOLD || lo >= hi))) {
        return FC_HW_EINVAL;
    }
    pthread_mutex_lock(&subscription_lock);
    int rc = FC_HW_EBUSY;
    if (!mode && !closing) {
        int32_t mc;
        rc = fc_nuttx_sensor("die_temp", &mc);
        if (!rc) {
            low = lo;
            high = hi;
            zone = temperature_zone(mc);
            mode = requested;
            notify = cb;
            context = ctx;
            if (pthread_create(&sampler, NULL, sample, NULL) != 0) {
                mode = 0;
                notify = NULL;
                context = NULL;
                rc = FC_HW_ENOSPC;
            }
        }
    }
    pthread_mutex_unlock(&subscription_lock);
    return rc;
}

void fc_nuttx_unsubscribe(int id)
{
    pthread_mutex_lock(&subscription_lock);
    if (id == 0 && mode) {
        closing = 1;
        mode = 0;
        notify = NULL;
        context = NULL;
        pthread_cond_signal(&wake);
        pthread_mutex_unlock(&subscription_lock);
        pthread_join(sampler, NULL);
        pthread_mutex_lock(&subscription_lock);
        closing = 0;
    }
    pthread_mutex_unlock(&subscription_lock);
}
