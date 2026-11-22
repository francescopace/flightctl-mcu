#include <nuttx/config.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <pthread.h>
#include <stdint.h>
#include <string.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>
#include <nuttx/clock.h>
#include "esp32s3_i2c.h"
#include "esp32s3_spi.h"
#include "espressif/esp_gpio.h"
#include "fc_hw_nuttx.h"

/* The upper half's timeout applies to each message. Pass the caller's one
 * deadline to the lower half for both halves of a repeated-start read. */
int fc_i2c_timed(struct i2c_master_s *, struct i2c_msg_s *, int, clock_t);

static pthread_mutex_t i2c_lock = PTHREAD_MUTEX_INITIALIZER;
static struct i2c_master_s *i2c;
int fc_nuttx_i2c_open(const fc_hw_bus_t *bus)
{
    if (bus->sda != CONFIG_ESP32S3_I2C0_SDAPIN || bus->scl != CONFIG_ESP32S3_I2C0_SCLPIN)
        return FC_HW_ENOTSUP;
    if (bus->hz && (bus->hz < 1000 || bus->hz > 1000000)) return FC_HW_EINVAL;
    pthread_mutex_lock(&i2c_lock);
    struct i2c_master_s *dev = esp32s3_i2cbus_initialize(0);
    if (dev) i2c = dev;
    pthread_mutex_unlock(&i2c_lock);
    return dev ? (bus->hz > 0 ? bus->hz : 100000) : FC_HW_EIO;
}

int fc_nuttx_i2c_transfer(int hz, int addr, const uint8_t *w, size_t wl, uint8_t *r, size_t rl, int ms)
{
    struct i2c_msg_s msgs[2];
    int n = 0;
    if (wl || !rl) {
        msgs[n++] = (struct i2c_msg_s){.frequency = hz, .addr = addr,
            .flags = rl ? I2C_M_NOSTOP : 0, .buffer = (uint8_t *)w, .length = wl};
    }
    if (rl) {
        msgs[n++] = (struct i2c_msg_s){.frequency = hz, .addr = addr,
            .flags = I2C_M_READ, .buffer = r, .length = rl};
    }
    int rc = fc_i2c_timed(i2c, msgs, n, MSEC2TICK(ms));
    return rc == 0 ? 0 : rc == -ETIMEDOUT ? FC_HW_ETIMEDOUT : FC_HW_EIO;
}

void fc_nuttx_i2c_close(int h)
{
    pthread_mutex_lock(&i2c_lock);
    esp32s3_i2cbus_uninitialize(i2c);
    pthread_mutex_unlock(&i2c_lock);
}

static pthread_mutex_t spi_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t cs_available = PTHREAD_COND_INITIALIZER;
static struct spi_dev_s *spi;
static struct { int hz, mode; } devices[49];
static int owner = -1;

int fc_nuttx_spi_open(const fc_hw_bus_t *b)
{
    if (b->sck != CONFIG_ESP32S3_SPI2_CLKPIN || b->mosi != CONFIG_ESP32S3_SPI2_MOSIPIN ||
        b->miso != CONFIG_ESP32S3_SPI2_MISOPIN || b->cs < 0 || b->cs >= 49) return FC_HW_ENOTSUP;
    pthread_mutex_lock(&spi_lock);
    int rc = FC_HW_EBUSY;
    if (!devices[b->cs].hz) {
        spi = esp32s3_spibus_initialize(2);
        if (spi) {
            esp_gpiowrite(b->cs, true);
            esp_gpio_matrix_out(b->cs, MATRIX_DETACH_OUT_SIG, false, false);
            esp_configgpio(b->cs, OUTPUT);
            devices[b->cs].hz = b->hz > 0 ? b->hz : 1000000;
            devices[b->cs].mode = b->mode;
            rc = b->cs;
        } else rc = FC_HW_EIO;
    }
    pthread_mutex_unlock(&spi_lock);
    return rc;
}

static struct timespec deadline(int ms)
{
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    t.tv_nsec += (ms % 1000) * 1000000L;
    t.tv_sec += ms / 1000 + t.tv_nsec / 1000000000L;
    t.tv_nsec %= 1000000000L;
    return t;
}

static void deselect(int h)
{
    if (owner == h) {
        esp_gpiowrite(h, true);
        owner = -1;
        pthread_cond_broadcast(&cs_available);
    }
}

int fc_nuttx_spi_transfer(int h, const uint8_t *tx, uint8_t *rx, size_t len, int keep, int ms)
{
    struct timespec end = deadline(ms);
    if (pthread_mutex_timedlock(&spi_lock, &end) != 0) return FC_HW_EBUSY;
    while (owner != -1 && owner != h) {
        if (pthread_cond_timedwait(&cs_available, &spi_lock, &end) != 0) {
            pthread_mutex_unlock(&spi_lock);
            return FC_HW_EBUSY;
        }
    }
    SPI_SETMODE(spi, devices[h].mode);
    SPI_SETFREQUENCY(spi, devices[h].hz);
    SPI_SETBITS(spi, 8);
    owner = h;
    esp_gpiowrite(h, false);
    /* Xtensa's polled SPI driver fetches a full word for its last partial
     * word. Stage each piece so it never reads past the caller's buffer. */
    uint32_t words[16];
    while (len) {
        size_t n = len < sizeof(words) ? len : sizeof(words);
        memset(words, 0xff, sizeof(words));
        if (tx) memcpy(words, tx, n);
        uint32_t result[16];
        SPI_EXCHANGE(spi, words, rx ? result : NULL, n);
        if (tx) tx += n;
        if (rx) { memcpy(rx, result, n); rx += n; }
        len -= n;
    }
    if (!keep) deselect(h);
    pthread_mutex_unlock(&spi_lock);
    return 0;
}

void fc_nuttx_spi_release(int h)
{
    pthread_mutex_lock(&spi_lock);
    deselect(h);
    pthread_mutex_unlock(&spi_lock);
}

void fc_nuttx_spi_close(int h)
{
    pthread_mutex_lock(&spi_lock);
    deselect(h);
    esp_configgpio(h, INPUT);
    devices[h].hz = 0;
    esp32s3_spibus_uninitialize(spi);
    pthread_mutex_unlock(&spi_lock);
}

struct serial {
    int fd, announced, tx, rx, de;
    bool claimed;
    bool active;
    pthread_t thread;
    void (*notify)(void *, int);
    void *ctx;
};
static struct serial ports[2];
static pthread_mutex_t uart_lock = PTHREAD_MUTEX_INITIALIZER;

static void *watch_rx(void *arg)
{
    struct serial *p = arg;
    pthread_mutex_lock(&uart_lock);
    while (p->active) {
        int count;
        if (ioctl(p->fd, FIONREAD, (unsigned long)&count) == 0) {
            if (count > p->announced && p->notify) p->notify(p->ctx, count);
            p->announced = count;
        }
        pthread_mutex_unlock(&uart_lock);
        usleep(10000);
        pthread_mutex_lock(&uart_lock);
    }
    pthread_mutex_unlock(&uart_lock);
    return NULL;
}

void fc_nuttx_uart_route(int port, int tx, int rx, int de);

static void serial_release(struct serial *p)
{
    esp_gpio_matrix_out(p->tx, MATRIX_DETACH_OUT_SIG, false, false);
    esp_configgpio(p->tx, INPUT);
    esp_configgpio(p->rx, INPUT);
    if (p->de >= 0) esp_configgpio(p->de, INPUT);
    p->claimed = false;
}

int fc_nuttx_uart_open(const fc_hw_bus_t *b, int baud, int format, void (*cb)(void *, int), void *ctx)
{
    if (b->rx < 0 || b->tx < 0 || b->de == 0) return FC_HW_ENOTSUP;
    if (baud < 1 || baud > 5000000) return FC_HW_EINVAL;
    int h = b->de >= 0 ? 1 : 0;
    pthread_mutex_lock(&uart_lock);
    if (ports[h].claimed && b->de < 0) h = 1;
    struct serial *p = &ports[h];
    int rc = FC_HW_EBUSY;
    if (!p->claimed) {
        p->claimed = true;
        p->tx = b->tx; p->rx = b->rx; p->de = b->de;
        fc_nuttx_uart_route(h, b->tx, b->rx, b->de);
        rc = FC_HW_EIO;
        p->fd = open(h ? "/dev/ttyS2" : "/dev/ttyS1", O_RDWR | O_NONBLOCK | O_NOCTTY);
        struct termios settings;
        if (p->fd >= 0 && tcgetattr(p->fd, &settings) == 0) {
            cfmakeraw(&settings);
            settings.c_cflag &= ~(CSIZE | PARENB | PARODD | CSTOPB);
            settings.c_cflag |= CS8 | (format == FC_HW_UART_8E1 ? PARENB : format == FC_HW_UART_8N2 ? CSTOPB : 0);
            if (cfsetspeed(&settings, baud) == 0 && tcsetattr(p->fd, TCSANOW, &settings) == 0) {
                p->notify = cb;
                p->ctx = ctx;
                p->announced = 0;
                p->active = true;
                if (pthread_create(&p->thread, NULL, watch_rx, p) == 0) rc = h;
                else { p->active = false; p->notify = NULL; p->ctx = NULL; }
            }
        }
        if (rc < 0) {
            if (p->fd >= 0) close(p->fd);
            serial_release(p);
        }
    }
    pthread_mutex_unlock(&uart_lock);
    return rc;
}

int fc_nuttx_uart_read(int h, uint8_t *buf, size_t len, int ms)
{
    struct serial *p = &ports[h];
    struct pollfd f = {.fd = p->fd, .events = POLLIN};
    int ready = poll(&f, 1, ms);
    if (ready <= 0) return ready == 0 ? 0 : FC_HW_EIO;
    pthread_mutex_lock(&uart_lock);
    ssize_t n = read(f.fd, buf, len);
    if (n > 0) p->announced = n >= p->announced ? 0 : p->announced - n;
    int rc = n >= 0 ? n : errno == EAGAIN ? 0 : FC_HW_EIO;
    pthread_mutex_unlock(&uart_lock);
    return rc;
}

int fc_nuttx_uart_write(int h, const uint8_t *buf, size_t len, int ms)
{
    struct pollfd f = {.fd = ports[h].fd, .events = POLLOUT};
    int ready = poll(&f, 1, ms);
    if (ready <= 0) return ready == 0 ? 0 : FC_HW_EIO;
    ssize_t n = write(f.fd, buf, len);
    return n >= 0 ? n : errno == EAGAIN ? 0 : FC_HW_EIO;
}

void fc_nuttx_uart_close(int h)
{
    struct serial *p = &ports[h];
    pthread_mutex_lock(&uart_lock);
    p->active = false;
    pthread_mutex_unlock(&uart_lock);
    pthread_join(p->thread, NULL);
    pthread_mutex_lock(&uart_lock);
    close(p->fd);
    p->notify = NULL;
    p->ctx = NULL;
    serial_release(p);
    pthread_mutex_unlock(&uart_lock);
}
