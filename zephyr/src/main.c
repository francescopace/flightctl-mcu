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

#ifdef CONFIG_WIFI
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>

/* Where several access points carry the network, the Pico W's chip may join a
 * far one (Infineon's driver takes no access point's address, and telling the
 * chip to prefer the strongest changed nothing): there, name lookups and
 * handshakes time out. A weak join is given up for another one, a few times.
 * Espressif's driver scans every channel and joins the strongest
 * (CONFIG_ESP32_WIFI_STA_SCAN_ALL, in the board's file). */
#define WIFI_GOOD_DBM (-60)
#define WIFI_JOINS 5

static K_SEM_DEFINE(s_ip_up, 0, 1);
static struct net_mgmt_event_callback s_ip_cb;

static void on_ip(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
    if (event == NET_EVENT_IPV4_ADDR_ADD) {
        k_sem_give(&s_ip_up);
    }
}

/* Joins the network in the credentials (wifi_ssid, wifi_pass) and waits for
 * an address. */
static void wifi_up(const cJSON *creds)
{
    const char *ssid = str(creds, "wifi_ssid");
    const char *pass = str(creds, "wifi_pass");
    if (ssid == NULL) {
        fc_port_log(FC_LOG_ERROR, "no WiFi network in the credentials (provision.py --wifi-ssid)");
        k_sleep(K_FOREVER);
    }
    struct net_if *iface = net_if_get_default();
    net_mgmt_init_event_callback(&s_ip_cb, on_ip, NET_EVENT_IPV4_ADDR_ADD);
    net_mgmt_add_event_callback(&s_ip_cb);
    struct wifi_connect_req_params p = {
        .ssid = (const uint8_t *)ssid,
        .ssid_length = (uint8_t)strlen(ssid),
        .psk = (const uint8_t *)pass,
        .psk_length = pass != NULL ? (uint8_t)strlen(pass) : 0,
        .security = pass != NULL ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE,
        .channel = WIFI_CHANNEL_ANY,
        .band = WIFI_FREQ_BAND_2_4_GHZ,
        .mfp = WIFI_MFP_OPTIONAL,
    };
    int joins = 0;
    for (;;) {
        fc_port_log(FC_LOG_INFO, "WiFi: joining \"%s\"", ssid);
        k_sem_reset(&s_ip_up);
        if (net_mgmt(NET_REQUEST_WIFI_CONNECT, iface, &p, sizeof(p)) == 0) {
            struct wifi_iface_status st = {0};
            int known = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &st, sizeof(st)) == 0;
            /* Infineon's driver has joined when the request returns; others
             * (Espressif's) join after it, and choose the access point
             * themselves. */
            if (IS_ENABLED(CONFIG_WIFI_AIROC) && known && st.rssi < WIFI_GOOD_DBM && ++joins < WIFI_JOINS) {
                fc_port_log(FC_LOG_INFO, "WiFi: access point ..:%02x:%02x is weak (%d dBm), looking for a nearer one",
                            st.bssid[4], st.bssid[5], st.rssi);
                net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
                k_sleep(K_SECONDS(1));
                continue;
            }
            if (k_sem_take(&s_ip_up, K_SECONDS(30)) == 0) {
                known = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, iface, &st, sizeof(st)) == 0;
                if (known) {
                    fc_port_log(FC_LOG_INFO, "WiFi: connected (channel %d, %d dBm, access point ..:%02x:%02x)",
                                st.channel, st.rssi, st.bssid[4], st.bssid[5]);
                } else {
                    fc_port_log(FC_LOG_INFO, "WiFi: connected");
                }
                return;
            }
            net_mgmt(NET_REQUEST_WIFI_DISCONNECT, iface, NULL, 0);
        }
        fc_port_log(FC_LOG_WARN, "WiFi: no connection yet, trying again");
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
        stop("no credentials: write the fc_creds partition (deploy/provision.py --format zephyr)");
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
    wifi_up(creds);
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
        .ota_verify_timeout_s = CONFIG_FC_OTA_VERIFY_TIMEOUT,
        .wasm_natives = s_natives,
        .wasm_natives_count = sizeof(s_natives) / sizeof(s_natives[0]),
    };
    fc_agent_run(&cfg);
    stop("the agent stopped");
    return 1;
}
