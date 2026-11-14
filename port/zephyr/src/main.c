/*
 * flightctl-mcu on Zephyr: credentials from the fc_creds partition, the
 * time from SNTP, then the Flight Control agent.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <zephyr/kernel.h>
#include <zephyr/net/sntp.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/byteorder.h>

#include "cJSON.h"
#include "fc_agent.h"
#include "fc_port.h"
#include "fc_port_zephyr.h"
#include "fc_wifi_zephyr.h"

/* The fc_creds partition, written by tools/provision.py --format zephyr:
 * "FCC1", the length of what follows (32 bits, little-endian), then a JSON
 * object with the fields of the ESP-IDF firmware's fc_creds: server_host,
 * server_port, ca, enroll_crt, enroll_key, signing_pub, labels. */
#define CREDS_MAGIC "FCC1"
#define CREDS_MAX   (12 * 1024)

static cJSON *load_creds(void)
{
    const struct flash_area *fa;
    if (flash_area_open(PARTITION_ID(fc_creds_partition), &fa) != 0) {
        return NULL;
    }
    uint8_t hdr[8];
    cJSON *creds = NULL;
    if (flash_area_read(fa, 0, hdr, sizeof(hdr)) == 0 && memcmp(hdr, CREDS_MAGIC, 4) == 0) {
        uint32_t len = sys_get_le32(hdr + 4);
        char *json = len <= CREDS_MAX && len + sizeof(hdr) <= fa->fa_size ? malloc(len + 1) : NULL;
        if (json != NULL && flash_area_read(fa, sizeof(hdr), json, len) == 0) {
            json[len] = '\0';
            creds = cJSON_Parse(json);
        }
        free(json);
    }
    flash_area_close(fa);
    return creds;
}

static const char *str(const cJSON *creds, const char *name)
{
    const char *v = cJSON_GetStringValue(cJSON_GetObjectItem(creds, name));
    return v != NULL && v[0] != '\0' ? v : NULL;
}

/* Certificates are checked against the clock, and a device knows no time. */
#ifdef CONFIG_ARCH_POSIX
#include "native_rtc.h"

/* native_sim: the simulated real-time clock starts at the host's time. */
static void wait_for_time(void)
{
    uint64_t us = native_rtc_gettime_us(RTC_CLOCK_REALTIME);
    struct timespec ts = {.tv_sec = (time_t)(us / 1000000), .tv_nsec = (long)(us % 1000000) * 1000};
    clock_settime(CLOCK_REALTIME, &ts);
}
#else
static void wait_for_time(void)
{
    for (;;) {
        struct sntp_time t;
        if (sntp_simple(CONFIG_FC_SNTP_SERVER, 5000, &t) == 0) {
            struct timespec ts = {.tv_sec = (time_t)t.seconds};
            clock_settime(CLOCK_REALTIME, &ts);
            return;
        }
        fc_port_log(FC_LOG_WARN, "waiting for the time from %s...", CONFIG_FC_SNTP_SERVER);
        k_sleep(K_SECONDS(5));
    }
}
#endif


/* Simulated hardware for WASM apps, as on the host: a chip temperature that
 * swings 30..46 C over two minutes, and an LED (led0) printed when it
 * changes. No GPIO yet. */
static int sim_sensor(const char *name, int32_t *value)
{
    if (strcmp(name, "die_temp") != 0) {
        return FC_HW_ENOENT;
    }
    long t = (long)(k_uptime_get() / 1000) % 120;
    *value = 30000 + (int32_t)(t < 60 ? t : 120 - t) * 266;
    return 0;
}

static int sim_led_rgb(const fc_hw_led_t *led, uint8_t r, uint8_t g, uint8_t b)
{
    static int32_t last = -1;
    int32_t v = (r << 16) | (g << 8) | b;
    if (v != last) {
        printk("LED r=%d g=%d b=%d\n", (int)r, (int)g, (int)b);
        last = v;
    }
    return 0;
}

static int sim_led_open(const fc_hw_led_t *led)
{
    return 0;
}

static void sim_led_close(const fc_hw_led_t *led)
{
    sim_led_rgb(led, 0, 0, 0);
}

static char *sim_board(void)
{
    return strdup("{\"led\": {\"led0\": {\"dt\": \"simulated\"}}}");
}

static const fc_hw_t s_hw = {
    .board = CONFIG_BOARD_TARGET,
    .firmware = CONFIG_FC_FW_VERSION,
    .board_defaults = sim_board,
    .led_open = sim_led_open,
    .led_rgb = sim_led_rgb,
    .led_close = sim_led_close,
    .sensor_read = sim_sensor,
};

/* Nothing can be done, and maybe no one was watching when it was first
 * said: say it again every few seconds. */
static void stop(const char *why)
{
    for (;;) {
        fc_port_log(FC_LOG_ERROR, "%s", why);
        k_sleep(K_SECONDS(10));
    }
}

int main(void)
{
    fc_port_log(FC_LOG_INFO, "flightctl-mcu %s on Zephyr, board %s", CONFIG_FC_FW_VERSION, CONFIG_BOARD_TARGET);
    if (fc_port_zephyr_init() != 0) {
        stop("cannot mount the storage (the fc_storage partition)");
    }
    cJSON *creds = load_creds();
    const char *host = str(creds, "server_host");
    const cJSON *port = cJSON_GetObjectItem(creds, "server_port");
    if (host == NULL || str(creds, "ca") == NULL || str(creds, "enroll_crt") == NULL ||
        str(creds, "enroll_key") == NULL) {
        stop("no credentials: write the fc_creds partition (tools/provision.py --format zephyr)");
    }
    /* {"k": "v", ...} to the {k, v, ..., NULL} the agent takes */
    static const char *labels[33];
    int n = 0;
    const cJSON *l;
    cJSON_ArrayForEach(l, cJSON_GetObjectItem(creds, "labels")) {
        if (n < 30 && cJSON_IsString(l)) {
            labels[n++] = l->string;
            labels[n++] = l->valuestring;
        }
    }

#ifdef CONFIG_WIFI
    fc_wifi_up(creds);
#endif
    wait_for_time();
    size_t heap, heap_free, heap_min, kheap, kheap_free;
    fc_heap_stats(&heap, &heap_free, &heap_min);
    fc_kernel_heap_stats(&kheap, &kheap_free);
    fc_port_log(FC_LOG_INFO, "memory: heap %u of %u bytes free, for thread stacks %u of %u", (unsigned)heap_free,
                (unsigned)heap, (unsigned)kheap_free, (unsigned)kheap);
    fc_agent_config_t cfg = {
        .server_host = host,
        .server_port = CONFIG_FC_TEST_SERVER_PORT != 0 ? CONFIG_FC_TEST_SERVER_PORT
                       : cJSON_IsNumber(port)           ? port->valueint
                                                        : 8443,
        .ca_pem = str(creds, "ca"),
        .enrollment_cert_pem = str(creds, "enroll_crt"),
        .enrollment_key_pem = str(creds, "enroll_key"),
        .signing_pub_pem = str(creds, "signing_pub"),
        .labels = labels,
        .agent_version = "flightctl-mcu/0.1.0",
        .status_interval_s = 60,
        .metrics_interval_s = CONFIG_FC_METRICS_INTERVAL,
        .ota_verify_timeout_s = CONFIG_FC_OTA_VERIFY_TIMEOUT,
        .hw = &s_hw,
    };
    fc_agent_run(&cfg);
    stop("the agent stopped");
    return 1;
}
