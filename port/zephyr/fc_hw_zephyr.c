/*
 * The hardware WASM apps reach on Zephyr (fc_hw.h), named in the devicetree:
 * the children of the node with compatible "flightctl,app-hw"
 * (dts/bindings/flightctl,app-hw.yaml) are the board map's names, and each
 * child's properties say what it is (a GPIO, an ADC channel, a PWM, an I2C
 * bus, an SPI device, a UART or a temperature probe). The core gets them
 * as {"dt": name} entries
 * and passes a node's index where it takes a pin.
 *
 * A board without that node has none of them: apps still have led0 and
 * die_temp, simulated, as before. Each kind is built when its driver class
 * is (CONFIG_GPIO, CONFIG_ADC, ...), so a board enables only what it names.
 *
 * Levels are electrical: the core makes them logical with the board map's
 * active_low, which comes from the GPIO's devicetree flags. Interrupts go
 * from the ISR to a work item, which calls the core. Zephyr's I2C and SPI
 * calls take no timeout: a transfer lasts as long as the driver makes it.
 */
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/ring_buffer.h>

#include "cJSON.h"
#include "fc_hw.h"

#ifdef CONFIG_SENSOR
#include <zephyr/drivers/sensor.h>
#endif
#ifdef CONFIG_GPIO
#include <zephyr/drivers/gpio.h>
#endif
#ifdef CONFIG_ADC
#include <zephyr/drivers/adc.h>
#endif
#ifdef CONFIG_PWM
#include <zephyr/drivers/pwm.h>
#endif
#ifdef CONFIG_I2C
#include <zephyr/drivers/i2c.h>
#endif
#ifdef CONFIG_SPI
#include <zephyr/drivers/spi.h>
#endif
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
#include <zephyr/drivers/uart.h>
#endif

/* ---- the devicetree's names ---------------------------------------------------- */

typedef struct {
    const char *name;
    int kind;
    int safe; /* -1: none */
#ifdef CONFIG_GPIO
    struct gpio_dt_spec gpio, cs, de;
#endif
#ifdef CONFIG_ADC
    struct adc_dt_spec adc;
#endif
#ifdef CONFIG_PWM
    struct pwm_dt_spec pwm;
#endif
#ifdef CONFIG_SENSOR
    bool die_temperature;
#endif
    bool has_de;
    const struct device *bus; /* I2C, SPI, UART or a temperature sensor */
    int mode, hz, baud;
} node_t;


#if DT_HAS_COMPAT_STATUS_OKAY(flightctl_app_hw)
#define FC_HW_DT DT_COMPAT_GET_ANY_STATUS_OKAY(flightctl_app_hw)

#define NODE_KIND(n)                                                                                            \
    COND_CODE_1(DT_NODE_HAS_PROP(n, gpios), (FC_HW_KIND_GPIO),                                                 \
    (COND_CODE_1(DT_NODE_HAS_PROP(n, io_channels), (FC_HW_KIND_ADC),                                           \
    (COND_CODE_1(DT_NODE_HAS_PROP(n, pwms), (FC_HW_KIND_PWM),                                                  \
    (COND_CODE_1(DT_NODE_HAS_PROP(n, i2c_bus), (FC_HW_KIND_I2C),                                               \
    (COND_CODE_1(DT_NODE_HAS_PROP(n, spi_bus), (FC_HW_KIND_SPI),                                               \
    (COND_CODE_1(DT_NODE_HAS_PROP(n, uart), (FC_HW_KIND_UART),                                             \
    (COND_CODE_1(DT_NODE_HAS_PROP(n, temperature_sensor), (FC_HW_KIND_SENSOR), (0))))))))))))))

#ifdef CONFIG_SENSOR
#define NODE_SENSOR(n) DEVICE_DT_GET_OR_NULL(DT_PHANDLE(n, temperature_sensor))
#else
#define NODE_SENSOR(n) NULL
#endif

#define NODE_BUS(n)                                                                                             \
    COND_CODE_1(DT_NODE_HAS_PROP(n, i2c_bus), (DEVICE_DT_GET(DT_PHANDLE(n, i2c_bus))),                         \
    (COND_CODE_1(DT_NODE_HAS_PROP(n, spi_bus), (DEVICE_DT_GET(DT_PHANDLE(n, spi_bus))),                        \
    (COND_CODE_1(DT_NODE_HAS_PROP(n, uart), (DEVICE_DT_GET(DT_PHANDLE(n, uart))), (NODE_SENSOR(n)))))))

#define NODE(n)                                                                                                 \
    {                                                                                                           \
        .name = DT_NODE_FULL_NAME(n), .kind = NODE_KIND(n), .safe = DT_PROP_OR(n, safe, -1),                    \
        IF_ENABLED(CONFIG_GPIO, (.gpio = GPIO_DT_SPEC_GET_OR(n, gpios, {0}),                                    \
                                 .cs = GPIO_DT_SPEC_GET_OR(n, cs_gpios, {0}),                                   \
                                 .de = GPIO_DT_SPEC_GET_OR(n, de_gpios, {0}),))                                 \
        IF_ENABLED(CONFIG_ADC, (.adc = COND_CODE_1(DT_NODE_HAS_PROP(n, io_channels), (ADC_DT_SPEC_GET(n)), ({0})),)) \
        IF_ENABLED(CONFIG_PWM, (.pwm = COND_CODE_1(DT_NODE_HAS_PROP(n, pwms), (PWM_DT_SPEC_GET(n)), ({0})),))  \
        IF_ENABLED(CONFIG_SENSOR, (.die_temperature = DT_PROP(n, die_temperature),))                            \
        .has_de = DT_NODE_HAS_PROP(n, de_gpios), .bus = NODE_BUS(n),                                           \
        .mode = DT_PROP_OR(n, spi_mode, 0), .hz = DT_PROP_OR(n, clock_frequency, 0),                           \
        .baud = DT_PROP_OR(n, current_speed, 0),                                                                \
    }

static const node_t s_nodes[] = {DT_FOREACH_CHILD_STATUS_OKAY_SEP(FC_HW_DT, NODE, (,))};
#define NODES ARRAY_SIZE(s_nodes)
#else
static const node_t s_nodes[1];
#define NODES 0
#endif

/* The simulated LED, which every board has as led0. */
#define SIM_LED "simulated"

static __maybe_unused const node_t *fz_node(int id, int kind)
{
    return id >= 0 && id < (int)NODES && s_nodes[id].kind == kind ? &s_nodes[id] : NULL;
}

/* A UART a name may not take: the console's. */
static int fz_is_console(const struct device *dev)
{
#if DT_HAS_CHOSEN(zephyr_console)
    return dev == DEVICE_DT_GET(DT_CHOSEN(zephyr_console));
#else
    return 0;
#endif
}

/* The kinds this firmware was built with. */
static int fz_built(int kind)
{
    switch (kind) {
    case FC_HW_KIND_GPIO:
        return IS_ENABLED(CONFIG_GPIO);
    case FC_HW_KIND_SENSOR:
        return IS_ENABLED(CONFIG_SENSOR);
    case FC_HW_KIND_ADC:
        return IS_ENABLED(CONFIG_ADC);
    case FC_HW_KIND_PWM:
        return IS_ENABLED(CONFIG_PWM);
    case FC_HW_KIND_I2C:
        return IS_ENABLED(CONFIG_I2C);
    case FC_HW_KIND_SPI:
        return IS_ENABLED(CONFIG_SPI) && IS_ENABLED(CONFIG_GPIO);
    case FC_HW_KIND_UART:
        return IS_ENABLED(CONFIG_UART_INTERRUPT_DRIVEN);
    default:
        return 0;
    }
}

static int fz_dt_find(int kind, const char *name)
{
    if (kind == FC_HW_KIND_LED) {
        return strcmp(name, SIM_LED) == 0 ? 0 : -1;
    }
    for (int i = 0; i < (int)NODES; i++) {
        if (s_nodes[i].kind == kind && strcmp(s_nodes[i].name, name) == 0 && fz_built(kind) &&
            !(kind == FC_HW_KIND_UART && fz_is_console(s_nodes[i].bus))) {
            return i;
        }
    }
    return -1;
}

static const char *fz_section(int kind)
{
    static const char *const s[] = {"", "gpio", "led", "i2c", "spi", "uart", "adc", "pwm", "sensor"};
    return kind > 0 && kind < (int)ARRAY_SIZE(s) ? s[kind] : NULL;
}

/* The firmware's board map: led0, simulated, and each devicetree name. */
static char *fz_board_defaults(void)
{
    cJSON *doc = cJSON_CreateObject();
    cJSON *led = cJSON_AddObjectToObject(cJSON_AddObjectToObject(doc, "led"), "led0");
    cJSON_AddStringToObject(led, "dt", SIM_LED);
    for (int i = 0; i < (int)NODES; i++) {
        const node_t *n = &s_nodes[i];
        if (fz_section(n->kind) == NULL || fz_dt_find(n->kind, n->name) != i) {
            continue;
        }
        cJSON *sec = cJSON_GetObjectItem(doc, fz_section(n->kind));
        if (sec == NULL) {
            sec = cJSON_AddObjectToObject(doc, fz_section(n->kind));
        }
        cJSON *e = cJSON_AddObjectToObject(sec, n->name);
        cJSON_AddStringToObject(e, "dt", n->name);
#ifdef CONFIG_GPIO
        if (n->kind == FC_HW_KIND_GPIO && (n->gpio.dt_flags & GPIO_ACTIVE_LOW)) {
            cJSON_AddBoolToObject(e, "active_low", 1);
        }
#endif
        if (n->safe >= 0) {
            cJSON_AddNumberToObject(e, "safe", n->safe);
        }
    }
    char *s = cJSON_PrintUnformatted(doc);
    cJSON_Delete(doc);
    return s;
}

#ifdef CONFIG_SENSOR
/* Zephyr's fetch and channel_get share a sample buffer in the driver. */
K_MUTEX_DEFINE(fz_sensor_lock);

static int fz_sensor_read(int id, int32_t *value)
{
    const node_t *n = fz_node(id, FC_HW_KIND_SENSOR);
    if (!n || !device_is_ready(n->bus)) {
        return FC_HW_ENOTSUP;
    }
    enum sensor_channel chan = n->die_temperature ? SENSOR_CHAN_DIE_TEMP : SENSOR_CHAN_AMBIENT_TEMP;
    struct sensor_value sample;
    k_mutex_lock(&fz_sensor_lock, K_FOREVER);
    int rc = sensor_sample_fetch(n->bus);
    if (rc == 0) {
        rc = sensor_channel_get(n->bus, chan, &sample);
    }
    k_mutex_unlock(&fz_sensor_lock);
    if (rc != 0) {
        return rc == -ENOTSUP ? FC_HW_ENOTSUP : FC_HW_EIO;
    }
    *value = (int32_t)sensor_value_to_milli(&sample);
    return 0;
}

/* Drivers may keep the trigger pointer beyond a call. These descriptors
 * are immutable, including while a delayed driver callback is finishing. */
static const struct sensor_trigger fz_triggers[2][2] = {
    {{SENSOR_TRIG_DATA_READY, SENSOR_CHAN_AMBIENT_TEMP}, {SENSOR_TRIG_THRESHOLD, SENSOR_CHAN_AMBIENT_TEMP}},
    {{SENSOR_TRIG_DATA_READY, SENSOR_CHAN_DIE_TEMP}, {SENSOR_TRIG_THRESHOLD, SENSOR_CHAN_DIE_TEMP}},
};
static struct {
    const struct sensor_trigger *trigger;
    void (*notify)(void *);
    void *ctx;
} fz_watches[NODES ? NODES : 1];
K_MUTEX_DEFINE(fz_dispatch_lock);

static void fz_sensor_triggered(const struct device *dev, const struct sensor_trigger *trigger)
{
    k_mutex_lock(&fz_dispatch_lock, K_FOREVER);
    for (int i = 0; i < (int)NODES; i++) {
        if (s_nodes[i].bus == dev && fz_watches[i].trigger == trigger && fz_watches[i].notify) {
            fz_watches[i].notify(fz_watches[i].ctx);
            break;
        }
    }
    k_mutex_unlock(&fz_dispatch_lock);
}

static void fz_watch_set(int id, const struct sensor_trigger *trigger, void (*notify)(void *), void *ctx)
{
    k_mutex_lock(&fz_dispatch_lock, K_FOREVER);
    fz_watches[id].trigger = trigger;
    fz_watches[id].notify = notify;
    fz_watches[id].ctx = ctx;
    k_mutex_unlock(&fz_dispatch_lock);
}

static int fz_sensor_subscribe(int id, int mode, int32_t low, int32_t high, void (*notify)(void *), void *ctx)
{
    const node_t *n = fz_node(id, FC_HW_KIND_SENSOR);
    if (!n || !device_is_ready(n->bus) || !DEVICE_API_GET(sensor, n->bus)->trigger_set) {
        return FC_HW_ENOTSUP;
    }
    if (mode != FC_HW_SENSOR_READY && (mode != FC_HW_SENSOR_THRESHOLD || low >= high)) {
        return FC_HW_EINVAL;
    }
    k_mutex_lock(&fz_sensor_lock, K_FOREVER);
    /* Two names wired to the same chip share its trigger registers. */
    for (int i = 0; i < (int)NODES; i++) {
        if (s_nodes[i].bus == n->bus && fz_watches[i].trigger) {
            k_mutex_unlock(&fz_sensor_lock);
            return FC_HW_EBUSY;
        }
    }
    const struct sensor_trigger *trig = &fz_triggers[n->die_temperature][mode == FC_HW_SENSOR_THRESHOLD];
    int rc = 0;
    if (mode == FC_HW_SENSOR_THRESHOLD) {
        struct sensor_value v;
        sensor_value_from_milli(&v, low);
        rc = sensor_attr_set(n->bus, trig->chan, SENSOR_ATTR_LOWER_THRESH, &v);
        if (rc == 0) {
            sensor_value_from_milli(&v, high);
            rc = sensor_attr_set(n->bus, trig->chan, SENSOR_ATTR_UPPER_THRESH, &v);
        }
    }
    if (rc == 0) {
        fz_watch_set(id, trig, notify, ctx);
        rc = sensor_trigger_set(n->bus, trig, fz_sensor_triggered);
        if (rc != 0) {
            fz_watch_set(id, NULL, NULL, NULL);
        }
    }
    k_mutex_unlock(&fz_sensor_lock);
    if (rc == -ENOTSUP || rc == -ENOSYS) {
        return FC_HW_ENOTSUP;
    }
    return rc == 0 ? id : rc == -EINVAL ? FC_HW_EINVAL : FC_HW_EIO;
}

static void fz_sensor_unsubscribe(int id)
{
    k_mutex_lock(&fz_sensor_lock, K_FOREVER);
    const struct sensor_trigger *trigger = fz_watches[id].trigger;
    fz_watch_set(id, NULL, NULL, NULL); /* drains a running callback before releasing ctx */
    sensor_trigger_set(s_nodes[id].bus, trigger, NULL);
    k_mutex_unlock(&fz_sensor_lock);
}

#endif

/* ---- GPIO --------------------------------------------------------------------------- */

#ifdef CONFIG_GPIO
static struct gpio_callback s_gpio_cb[NODES ? NODES : 1];
static struct k_work s_gpio_work[NODES ? NODES : 1];
static void (*volatile s_edge_cb[NODES ? NODES : 1])(void *);
static void *s_edge_ctx[NODES ? NODES : 1];
static uint8_t s_cb_added[NODES ? NODES : 1];

static int fz_gpio_setup(int id, int mode)
{
    const node_t *n = fz_node(id, FC_HW_KIND_GPIO);
    if (n == NULL || !gpio_is_ready_dt(&n->gpio)) {
        return FC_HW_ENOTSUP;
    }
    gpio_flags_t f = mode == FC_HW_GPIO_OUT      ? GPIO_OUTPUT
                     : mode == FC_HW_GPIO_OUT_OD ? GPIO_OUTPUT | GPIO_OPEN_DRAIN
                                                 : GPIO_INPUT;
    f |= mode == FC_HW_GPIO_IN_PULLUP ? GPIO_PULL_UP : mode == FC_HW_GPIO_IN_PULLDOWN ? GPIO_PULL_DOWN : 0;
    return gpio_pin_configure(n->gpio.port, n->gpio.pin, f) == 0 ? 0 : FC_HW_EIO;
}

static int fz_gpio_read(int id)
{
    const node_t *n = fz_node(id, FC_HW_KIND_GPIO);
    int v = n ? gpio_pin_get_raw(n->gpio.port, n->gpio.pin) : -1;
    return v < 0 ? FC_HW_EIO : v;
}

static int fz_gpio_write(int id, int level)
{
    const node_t *n = fz_node(id, FC_HW_KIND_GPIO);
    return n && gpio_pin_set_raw(n->gpio.port, n->gpio.pin, level != 0) == 0 ? 0 : FC_HW_EIO;
}

static void fz_gpio_release(int id)
{
    const node_t *n = fz_node(id, FC_HW_KIND_GPIO);
    if (n != NULL) {
        gpio_pin_configure(n->gpio.port, n->gpio.pin, GPIO_INPUT);
    }
}

static void fz_edge_isr(const struct device *port, struct gpio_callback *cb, uint32_t pins)
{
    k_work_submit(&s_gpio_work[cb - s_gpio_cb]);
}

static void fz_edge_work(struct k_work *w)
{
    int id = (int)(w - s_gpio_work);
    void (*cb)(void *) = s_edge_cb[id];
    if (cb != NULL) {
        cb(s_edge_ctx[id]);
    }
}

static int fz_gpio_irq(int id, int edge, void (*cb)(void *), void *ctx)
{
    const node_t *n = fz_node(id, FC_HW_KIND_GPIO);
    if (n == NULL) {
        return FC_HW_EINVAL;
    }
    gpio_pin_interrupt_configure(n->gpio.port, n->gpio.pin, GPIO_INT_DISABLE);
    if (s_cb_added[id]) {
        gpio_remove_callback(n->gpio.port, &s_gpio_cb[id]);
        s_cb_added[id] = 0;
        s_edge_cb[id] = NULL;
        k_work_cancel_sync(&s_gpio_work[id], &(struct k_work_sync){0}); /* no callback once this returns */
    }
    if (edge == 0) {
        return 0;
    }
    s_edge_ctx[id] = ctx;
    s_edge_cb[id] = cb;
    k_work_init(&s_gpio_work[id], fz_edge_work);
    gpio_init_callback(&s_gpio_cb[id], fz_edge_isr, BIT(n->gpio.pin));
    gpio_flags_t f = edge == 1 ? GPIO_INT_EDGE_RISING : edge == 2 ? GPIO_INT_EDGE_FALLING : GPIO_INT_EDGE_BOTH;
    if (gpio_add_callback(n->gpio.port, &s_gpio_cb[id]) != 0) {
        s_edge_cb[id] = NULL;
        return FC_HW_ENOTSUP;
    }
    s_cb_added[id] = 1;
    if (gpio_pin_interrupt_configure(n->gpio.port, n->gpio.pin, f) != 0) {
        gpio_remove_callback(n->gpio.port, &s_gpio_cb[id]);
        s_cb_added[id] = 0;
        s_edge_cb[id] = NULL;
        return FC_HW_ENOTSUP;
    }
    return 0;
}
#endif

/* ---- ADC ------------------------------------------------------------------------------ */

#ifdef CONFIG_ADC
static int fz_adc_open(int id)
{
    const node_t *n = fz_node(id, FC_HW_KIND_ADC);
    if (n == NULL || !adc_is_ready_dt(&n->adc)) {
        return FC_HW_ENOTSUP;
    }
    return adc_channel_setup_dt(&n->adc) == 0 ? id : FC_HW_EIO;
}

static int fz_adc_read_mv(int id)
{
    const node_t *n = fz_node(id, FC_HW_KIND_ADC);
    if (n == NULL) {
        return FC_HW_EINVAL;
    }
    int16_t raw = 0;
    struct adc_sequence seq = {.buffer = &raw, .buffer_size = sizeof(raw)};
    adc_sequence_init_dt(&n->adc, &seq);
    if (adc_read_dt(&n->adc, &seq) != 0) {
        return FC_HW_EIO;
    }
    int32_t mv = raw;
    if (adc_raw_to_millivolts_dt(&n->adc, &mv) != 0) {
        return FC_HW_EIO;
    }
    return mv < 0 ? 0 : (int)mv;
}

static void fz_adc_close(int id) {}
#endif

/* ---- PWM ------------------------------------------------------------------------------ */

#ifdef CONFIG_PWM
static uint32_t s_period_ns[NODES ? NODES : 1];

static int fz_pwm_open(int id, int hz)
{
    const node_t *n = fz_node(id, FC_HW_KIND_PWM);
    if (n == NULL || !pwm_is_ready_dt(&n->pwm)) {
        return FC_HW_ENOTSUP;
    }
    if (hz <= 0 || hz > 40000000) {
        return FC_HW_EINVAL;
    }
    s_period_ns[id] = NSEC_PER_SEC / (uint32_t)hz;
    return pwm_set_dt(&n->pwm, s_period_ns[id], 0) == 0 ? id : FC_HW_EINVAL;
}

static int fz_pwm_duty(int id, uint32_t duty)
{
    const node_t *n = fz_node(id, FC_HW_KIND_PWM);
    if (n == NULL) {
        return FC_HW_EINVAL;
    }
    uint32_t pulse = (uint32_t)((uint64_t)s_period_ns[id] * duty / 65535u);
    return pwm_set_dt(&n->pwm, s_period_ns[id], pulse) == 0 ? 0 : FC_HW_EIO;
}

static void fz_pwm_close(int id)
{
    const node_t *n = fz_node(id, FC_HW_KIND_PWM);
    if (n != NULL) {
        pwm_set_dt(&n->pwm, s_period_ns[id], 0); /* low */
    }
}
#endif

/* ---- I2C -------------------------------------------------------------------------------- */

#ifdef CONFIG_I2C
static int fz_i2c_open(const fc_hw_bus_t *b)
{
    const node_t *n = fz_node(b->dt, FC_HW_KIND_I2C);
    if (n == NULL || !device_is_ready(n->bus)) {
        return FC_HW_ENOTSUP;
    }
    int hz = b->hz > 0 ? b->hz : n->hz;
    if (hz > 0) {
        uint32_t speed = hz <= 100000 ? I2C_SPEED_STANDARD : hz <= 400000 ? I2C_SPEED_FAST : I2C_SPEED_FAST_PLUS;
        int rc = i2c_configure(n->bus, I2C_MODE_CONTROLLER | I2C_SPEED_SET(speed));
        if (rc != 0 && rc != -ENOSYS) {
            return FC_HW_EINVAL;
        }
    }
    return b->dt;
}

static int fz_i2c_transfer(int id, int addr, const uint8_t *w, size_t wlen, uint8_t *r, size_t rlen, int timeout_ms)
{
    const node_t *n = fz_node(id, FC_HW_KIND_I2C);
    if (n == NULL) {
        return FC_HW_EINVAL;
    }
    int rc = wlen > 0 && rlen > 0 ? i2c_write_read(n->bus, (uint16_t)addr, w, wlen, r, rlen)
             : wlen > 0           ? i2c_write(n->bus, w, (uint32_t)wlen, (uint16_t)addr)
                                  : i2c_read(n->bus, r, (uint32_t)rlen, (uint16_t)addr);
    return rc == 0 ? 0 : rc == -ETIMEDOUT || rc == -EAGAIN ? FC_HW_ETIMEDOUT : FC_HW_EIO;
}

static void fz_i2c_close(int id) {}
#endif

/* ---- SPI -------------------------------------------------------------------------------- */

#if defined(CONFIG_SPI) && defined(CONFIG_GPIO)
#define SPI_PIECE 64

/* One lock per controller, taken for a transfer and held while a device
 * keeps its chip select: Zephyr's own lock waits without end. */
static struct {
    const struct device *dev;
    struct k_sem lock;
} s_spi_bus[NODES ? NODES : 1];
static uint8_t s_spi_kept[NODES ? NODES : 1];
static K_MUTEX_DEFINE(s_spi_table);

static struct k_sem *fz_spi_lock(const struct device *dev)
{
    k_mutex_lock(&s_spi_table, K_FOREVER);
    struct k_sem *l = NULL;
    for (size_t i = 0; i < ARRAY_SIZE(s_spi_bus) && l == NULL; i++) {
        if (s_spi_bus[i].dev == dev) {
            l = &s_spi_bus[i].lock;
        } else if (s_spi_bus[i].dev == NULL) {
            s_spi_bus[i].dev = dev;
            k_sem_init(&s_spi_bus[i].lock, 1, 1);
            l = &s_spi_bus[i].lock;
        }
    }
    k_mutex_unlock(&s_spi_table);
    return l;
}

static int fz_spi_open(const fc_hw_bus_t *b)
{
    const node_t *n = fz_node(b->dt, FC_HW_KIND_SPI);
    if (n == NULL || !device_is_ready(n->bus) || n->cs.port == NULL || !gpio_is_ready_dt(&n->cs)) {
        return FC_HW_ENOTSUP;
    }
    if (gpio_pin_configure_dt(&n->cs, GPIO_OUTPUT_INACTIVE) != 0 || fz_spi_lock(n->bus) == NULL) {
        return FC_HW_EIO;
    }
    s_spi_kept[b->dt] = 0;
    return b->dt;
}

static void fz_spi_end(int id)
{
    const node_t *n = &s_nodes[id];
    gpio_pin_set_dt(&n->cs, 0);
    s_spi_kept[id] = 0;
    k_sem_give(fz_spi_lock(n->bus));
}

static int fz_spi_transfer(int id, const uint8_t *tx, uint8_t *rx, size_t len, int keep_cs, int timeout_ms)
{
    static const uint8_t ones[SPI_PIECE] = {[0 ... SPI_PIECE - 1] = 0xFF};
    const node_t *n = fz_node(id, FC_HW_KIND_SPI);
    if (n == NULL) {
        return FC_HW_EINVAL;
    }
    if (!s_spi_kept[id] && k_sem_take(fz_spi_lock(n->bus), K_MSEC(timeout_ms)) != 0) {
        return FC_HW_EBUSY;
    }
    struct spi_config cfg = {
        .frequency = n->hz > 0 ? (uint32_t)n->hz : 1000000,
        .operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB | SPI_OP_MODE_CONTROLLER |
                     ((n->mode & 2) ? SPI_MODE_CPOL : 0) | ((n->mode & 1) ? SPI_MODE_CPHA : 0),
    };
    gpio_pin_set_dt(&n->cs, 1);
    int rc = 0;
    /* With nothing to send, 0xFF, a piece at a time. */
    size_t step = tx != NULL ? len : SPI_PIECE;
    for (size_t at = 0; at < len && rc == 0; at += step) {
        size_t k = len - at < step ? len - at : step;
        struct spi_buf tb = {.buf = (void *)(tx != NULL ? tx + at : ones), .len = k};
        struct spi_buf rb = {.buf = rx != NULL ? rx + at : NULL, .len = k};
        struct spi_buf_set ts = {.buffers = &tb, .count = 1}, rs = {.buffers = &rb, .count = 1};
        rc = spi_transceive(n->bus, &cfg, &ts, rx != NULL ? &rs : NULL);
    }
    if (rc == 0 && keep_cs) {
        s_spi_kept[id] = 1;
        return 0;
    }
    fz_spi_end(id);
    return rc == 0 ? 0 : FC_HW_EIO;
}

static void fz_spi_release(int id)
{
    if (fz_node(id, FC_HW_KIND_SPI) != NULL && s_spi_kept[id]) {
        fz_spi_end(id);
    }
}

static void fz_spi_close(int id)
{
    const node_t *n = fz_node(id, FC_HW_KIND_SPI);
    if (n != NULL) {
        fz_spi_release(id);
        gpio_pin_configure_dt(&n->cs, GPIO_INPUT);
    }
}
#endif

/* ---- UART ------------------------------------------------------------------------------ */

#ifdef CONFIG_UART_INTERRUPT_DRIVEN
#define UART_RX_BUF 512

/* No per-slot work items for boards without a transceiver. */
#define HAS_DE(n) || DT_NODE_HAS_PROP(n, de_gpios)
#if defined(CONFIG_GPIO) && DT_HAS_COMPAT_STATUS_OKAY(flightctl_app_hw)
#define FZ_HAS_RS485 (0 DT_FOREACH_CHILD_STATUS_OKAY(FC_HW_DT, HAS_DE))
#else
#define FZ_HAS_RS485 0
#endif

#if FZ_HAS_RS485
#if defined(FC_TEST_RS485)
/* The test controls FIFO space and the separate shift register. */
extern int fc_test_uart_tx_state(const struct device *dev, bool *room, bool *idle);
#define fz_uart_tx_state fc_test_uart_tx_state
#else
#if defined(CONFIG_SOC_SERIES_ESP32S3)
#include <hal/uart_ll.h>
#endif
static int fz_uart_tx_state(const struct device *dev, bool *room, bool *idle)
{
#if defined(CONFIG_SOC_SERIES_ESP32S3)
    /* The same S3 HAL idle check as the UART driver uses for UART_TX_DONE:
     * FIFO empty and the serializer idle. uart_irq_tx_complete is ISR-only. */
#define TX_STATE(n)                                                                                      \
    if (dev == DEVICE_DT_GET(n)) {                                                                       \
        uart_dev_t *hw = (uart_dev_t *)DT_REG_ADDR(n);                                                    \
        *room = uart_ll_get_txfifo_len(hw) > 0;                                                          \
        *idle = uart_ll_is_tx_idle(hw);                                                                  \
        return 0;                                                                                        \
    }
    DT_FOREACH_STATUS_OKAY(espressif_esp32_uart, TX_STATE)
#undef TX_STATE
#endif
    return FC_HW_ENOTSUP;
}
#endif
#endif

typedef struct {
    int id;
    bool claimed;
#if FZ_HAS_RS485
    struct k_work_delayable drain;
    bool drain_initialized, driving, closed;
#endif
    struct ring_buf rb;
    uint8_t buf[UART_RX_BUF];
    struct k_sem data;
    struct k_work work;
    void (*volatile cb)(void *, int);
    void *ctx;
} uart_slot_t;

static uart_slot_t s_uart[2];
static K_MUTEX_DEFINE(s_uart_lock0);
static K_MUTEX_DEFINE(s_uart_lock1);
static struct k_mutex *const s_uart_locks[] = {&s_uart_lock0, &s_uart_lock1};
BUILD_ASSERT(ARRAY_SIZE(s_uart) == ARRAY_SIZE(s_uart_locks));

#if FZ_HAS_RS485
/* Under this slot's mutex. A pending tail still owns the UART, even after close.
 * Never leave the system workqueue waiting on the transceiver. */
static void fz_uart_release_idle(uart_slot_t *u)
{
    const node_t *n = &s_nodes[u->id];
    bool room, idle;
    if (u->driving && fz_uart_tx_state(n->bus, &room, &idle) == 0 && idle &&
        gpio_pin_set_dt(&n->de, 0) == 0) {
        u->driving = false;
    }
    if (u->driving) {
        k_work_reschedule(&u->drain, K_TICKS(1));
    } else if (u->closed) {
        u->claimed = false;
    }
}

static void fz_uart_drain(struct k_work *work)
{
    uart_slot_t *u = CONTAINER_OF(k_work_delayable_from_work(work), uart_slot_t, drain);
    struct k_mutex *lock = s_uart_locks[u - s_uart];
    if (k_mutex_lock(lock, K_NO_WAIT) != 0) {
        k_work_reschedule(&u->drain, K_TICKS(1));
        return;
    }
    if (u->driving) {
        fz_uart_release_idle(u);
    }
    k_mutex_unlock(lock);
}
#endif

static void fz_uart_isr(const struct device *dev, void *arg)
{
    uart_slot_t *u = arg;
    uart_irq_update(dev);
    while (uart_irq_rx_ready(dev) > 0) {
        uint8_t tmp[32];
        int n = uart_fifo_read(dev, tmp, sizeof(tmp));
        if (n <= 0) {
            break;
        }
        ring_buf_put(&u->rb, tmp, (uint32_t)n); /* what does not fit is lost */
        k_sem_give(&u->data);
        k_work_submit(&u->work);
    }
}

static void fz_uart_work(struct k_work *w)
{
    uart_slot_t *u = CONTAINER_OF(w, uart_slot_t, work);
    void (*cb)(void *, int) = u->cb;
    uint32_t n = ring_buf_size_get(&u->rb);
    if (cb != NULL && n > 0) {
        cb(u->ctx, (int)n);
    }
}

static int fz_uart_open_locked(const fc_hw_bus_t *b, int baud, int format, void (*cb)(void *, int), void *ctx)
{
    const node_t *n = fz_node(b->dt, FC_HW_KIND_UART);
    if (n == NULL || !device_is_ready(n->bus) || fz_is_console(n->bus)) {
        return FC_HW_ENOTSUP;
    }
    int i = ARRAY_SIZE(s_uart);
    for (int j = 0; j < (int)ARRAY_SIZE(s_uart); j++) {
        if (s_uart[j].claimed) {
            const node_t *owner = &s_nodes[s_uart[j].id];
            if (owner->bus == n->bus) {
                return FC_HW_EBUSY;
            }
#if FZ_HAS_RS485
            if (owner->has_de && n->has_de && owner->de.port == n->de.port && owner->de.pin == n->de.pin) {
                return FC_HW_EBUSY;
            }
#endif
        } else if (i == (int)ARRAY_SIZE(s_uart)) {
            i = j;
        }
    }
    if (i == (int)ARRAY_SIZE(s_uart)) {
        return FC_HW_EBUSY;
    }
    if (n->has_de) {
#if FZ_HAS_RS485
        bool room, idle;
        if (!gpio_is_ready_dt(&n->de) || fz_uart_tx_state(n->bus, &room, &idle) != 0) {
            return FC_HW_ENOTSUP;
        }
        if (!idle) {
            return FC_HW_EBUSY;
        }
#else
        return FC_HW_ENOTSUP;
#endif
    }
    struct uart_config c = {
        .baudrate = (uint32_t)baud,
        .parity = format == FC_HW_UART_8E1 ? UART_CFG_PARITY_EVEN : UART_CFG_PARITY_NONE,
        .stop_bits = format == FC_HW_UART_8N2 ? UART_CFG_STOP_BITS_2 : UART_CFG_STOP_BITS_1,
        .data_bits = UART_CFG_DATA_BITS_8,
        .flow_ctrl = UART_CFG_FLOW_CTRL_NONE,
    };
    int rc = uart_configure(n->bus, &c);
    if (rc != 0 && rc != -ENOSYS) {
        return FC_HW_EINVAL;
    }
    uart_slot_t *u = &s_uart[i];
#if FZ_HAS_RS485
    if (n->has_de) {
        if (rc != 0) {
            return FC_HW_ENOTSUP; /* require the requested framing and flow control */
        }
        if (gpio_pin_configure_dt(&n->de, GPIO_OUTPUT_INACTIVE) != 0) {
            return FC_HW_EIO;
        }
        if (!u->drain_initialized) {
            k_work_init_delayable(&u->drain, fz_uart_drain);
            u->drain_initialized = true;
        }
    }
    u->closed = false;
#endif
    u->id = b->dt;
    u->claimed = true;
    ring_buf_init(&u->rb, sizeof(u->buf), u->buf);
    k_sem_init(&u->data, 0, 1);
    k_work_init(&u->work, fz_uart_work);
    u->ctx = ctx;
    u->cb = cb;
    uart_irq_callback_user_data_set(n->bus, fz_uart_isr, u);
    uart_irq_rx_enable(n->bus);
    return i;
}

static int fz_uart_open(const fc_hw_bus_t *b, int baud, int format, void (*cb)(void *, int), void *ctx)
{
    /* Check the two claims together, including a closing UART's last stop bit. */
    if (cb == NULL) {
        return FC_HW_EINVAL;
    }
    k_mutex_lock(&s_uart_lock0, K_FOREVER);
    k_mutex_lock(&s_uart_lock1, K_FOREVER);
    int rc = fz_uart_open_locked(b, baud, format, cb, ctx);
    k_mutex_unlock(&s_uart_lock1);
    k_mutex_unlock(&s_uart_lock0);
    return rc;
}

static int fz_uart_write(int i, const uint8_t *d, size_t len, int timeout_ms)
{
    int64_t end = k_uptime_get() + timeout_ms;
    if (i < 0 || i >= (int)ARRAY_SIZE(s_uart)) {
        return FC_HW_EINVAL;
    }
    struct k_mutex *lock = s_uart_locks[i];
    if (k_mutex_lock(lock, K_MSEC(timeout_ms)) != 0) {
        return FC_HW_EBUSY;
    }
    if (s_uart[i].cb == NULL) {
        k_mutex_unlock(lock);
        return FC_HW_EINVAL;
    }
    uart_slot_t *u = &s_uart[i];
    const node_t *n = &s_nodes[u->id];
    int done = 0;
    while ((size_t)done < len && k_uptime_get() < end) {
#if FZ_HAS_RS485
        if (n->has_de) {
            bool room, idle;
            if (fz_uart_tx_state(n->bus, &room, &idle) != 0) {
                done = done ? done : FC_HW_EIO;
                break;
            }
            if (!room) {
                k_sleep(K_TICKS(1));
                continue;
            }
            if (!u->driving) {
                if (gpio_pin_set_dt(&n->de, 1) != 0) {
                    done = FC_HW_EIO;
                    break;
                }
                u->driving = true;
            }
        }
#endif
        uart_poll_out(n->bus, d[done++]);
    }
#if FZ_HAS_RS485
    if (u->driving) {
        bool room, idle;
        while (fz_uart_tx_state(n->bus, &room, &idle) == 0 && !idle && k_uptime_get() < end) {
            k_sleep(K_TICKS(1));
        }
        fz_uart_release_idle(u);
    }
#endif
    k_mutex_unlock(lock);
    return done;
}

static int fz_uart_read(int i, uint8_t *b, size_t cap, int timeout_ms)
{
    if (i < 0 || i >= (int)ARRAY_SIZE(s_uart) || s_uart[i].cb == NULL) {
        return FC_HW_EINVAL;
    }
    uart_slot_t *u = &s_uart[i];
    if (ring_buf_is_empty(&u->rb)) {
        /* A give left from data already read would end the wait at once. */
        k_sem_reset(&u->data);
        if (ring_buf_is_empty(&u->rb)) {
            k_sem_take(&u->data, K_MSEC(timeout_ms));
        }
    }
    const struct device *dev = s_nodes[u->id].bus;
    uart_irq_rx_disable(dev);
    uint32_t n = ring_buf_get(&u->rb, b, (uint32_t)cap);
    uart_irq_rx_enable(dev);
    return (int)n;
}

static void fz_uart_close(int i)
{
    if (i < 0 || i >= (int)ARRAY_SIZE(s_uart)) {
        return;
    }
    struct k_mutex *lock = s_uart_locks[i];
    k_mutex_lock(lock, K_FOREVER);
    if (s_uart[i].cb == NULL) {
        k_mutex_unlock(lock);
        return;
    }
    uart_slot_t *u = &s_uart[i];
    uart_irq_rx_disable(s_nodes[u->id].bus);
    u->cb = NULL;
    k_mutex_unlock(lock);
    k_work_cancel_sync(&u->work, &(struct k_work_sync){0}); /* no callback once this returns */
    k_mutex_lock(lock, K_FOREVER);
#if FZ_HAS_RS485
    if (u->driving) {
        u->closed = true;
        fz_uart_release_idle(u);
    } else
#endif
    {
        u->claimed = false;
    }
    k_mutex_unlock(lock);
}
#endif

void fc_hw_zephyr_fill(fc_hw_t *hw)
{
    hw->dt_find = fz_dt_find;
    hw->board_defaults = fz_board_defaults;
#ifdef CONFIG_SENSOR
    hw->sensor_read_dt = fz_sensor_read;
    hw->sensor_subscribe = fz_sensor_subscribe;
    hw->sensor_unsubscribe = fz_sensor_unsubscribe;
#endif
#ifdef CONFIG_GPIO
    hw->gpio_setup = fz_gpio_setup;
    hw->gpio_read = fz_gpio_read;
    hw->gpio_write = fz_gpio_write;
    hw->gpio_irq = fz_gpio_irq;
    hw->gpio_release = fz_gpio_release;
#endif
#ifdef CONFIG_ADC
    hw->adc_open = fz_adc_open;
    hw->adc_read_mv = fz_adc_read_mv;
    hw->adc_close = fz_adc_close;
#endif
#ifdef CONFIG_PWM
    hw->pwm_open = fz_pwm_open;
    hw->pwm_duty = fz_pwm_duty;
    hw->pwm_close = fz_pwm_close;
#endif
#ifdef CONFIG_I2C
    hw->i2c_open = fz_i2c_open;
    hw->i2c_transfer = fz_i2c_transfer;
    hw->i2c_close = fz_i2c_close;
#endif
#if defined(CONFIG_SPI) && defined(CONFIG_GPIO)
    hw->spi_open = fz_spi_open;
    hw->spi_transfer = fz_spi_transfer;
    hw->spi_release = fz_spi_release;
    hw->spi_close = fz_spi_close;
#endif
#ifdef CONFIG_UART_INTERRUPT_DRIVEN
    hw->uart_open = fz_uart_open;
    hw->uart_write = fz_uart_write;
    hw->uart_read = fz_uart_read;
    hw->uart_close = fz_uart_close;
#endif
}
