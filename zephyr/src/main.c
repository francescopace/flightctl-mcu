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
#include "wasm_export.h"

/* The fc_creds partition, written by deploy/provision.py --format zephyr:
 * "FCC1", the length of what follows (32 bits, little-endian), then a JSON
 * object with the fields of the ESP-IDF firmware's fc_creds: server_host,
 * server_port, ca, enroll_crt, enroll_key, signing_pub, labels. */
#define CREDS_MAGIC "FCC1"
#define CREDS_MAX   (12 * 1024)

static cJSON *load_creds(void)
{
    const struct flash_area *fa;
    if (flash_area_open(FIXED_PARTITION_ID(fc_creds_partition), &fa) != 0) {
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

/* Simulated hardware for WASM apps, as on the host: a temperature that swings
 * 30..46 C over two minutes, a 40 C threshold, an LED printed on change. */
static int32_t sim_temp_mc(wasm_exec_env_t env)
{
    long t = (long)(k_uptime_get() / 1000) % 120;
    return 30000 + (int32_t)(t < 60 ? t : 120 - t) * 266;
}

static int32_t sim_threshold_mc(wasm_exec_env_t env)
{
    return 40000;
}

static void sim_led(wasm_exec_env_t env, int32_t r, int32_t g, int32_t b)
{
    static int32_t last = -1;
    int32_t v = (r << 16) | (g << 8) | b;
    if (v != last) {
        printk("LED r=%d g=%d b=%d\n", (int)r, (int)g, (int)b);
        last = v;
    }
}

static NativeSymbol s_natives[] = {
    {"temp_mc", (void *)sim_temp_mc, "()i", NULL},
    {"threshold_mc", (void *)sim_threshold_mc, "()i", NULL},
    {"led", (void *)sim_led, "(iii)", NULL},
};

int main(void)
{
    fc_port_log(FC_LOG_INFO, "flightctl-mcu on Zephyr, board %s", CONFIG_BOARD_TARGET);
    if (fc_port_zephyr_init() != 0) {
        return 1;
    }
    cJSON *creds = load_creds();
    const char *host = str(creds, "server_host");
    const cJSON *port = cJSON_GetObjectItem(creds, "server_port");
    if (host == NULL || str(creds, "ca") == NULL || str(creds, "enroll_crt") == NULL ||
        str(creds, "enroll_key") == NULL) {
        fc_port_log(FC_LOG_ERROR, "no credentials: write the fc_creds partition (deploy/provision.py --format zephyr)");
        return 1;
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

    wait_for_time();
    fc_agent_config_t cfg = {
        .server_host = host,
        .server_port = cJSON_IsNumber(port) ? port->valueint : 8443,
        .ca_pem = str(creds, "ca"),
        .enrollment_cert_pem = str(creds, "enroll_crt"),
        .enrollment_key_pem = str(creds, "enroll_key"),
        .signing_pub_pem = str(creds, "signing_pub"),
        .labels = labels,
        .agent_version = "flightctl-mcu/0.1.0",
        .status_interval_s = 60,
        .wasm_natives = s_natives,
        .wasm_natives_count = sizeof(s_natives) / sizeof(s_natives[0]),
    };
    return fc_agent_run(&cfg) == 0 ? 0 : 1;
}
