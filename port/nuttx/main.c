#include <nuttx/config.h>
#include "fc_ota_arm.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <netutils/netlib.h>
#include <netutils/ntpclient.h>
#ifdef CONFIG_FLIGHTCTL_SERIAL_NETWORK
#include <arpa/inet.h>
#include <nuttx/net/slip.h>
#elif !defined(CONFIG_STM32_ETHMAC)
#include <wireless/wapi.h>
#endif
#ifdef CONFIG_ARCH_CHIP_ESP32S3
#include "esp32s3_spiflash_mtd.h"
#elif !defined(CONFIG_ARCH_ARM)
#include "espressif/esp_spiflash_mtd.h"
#endif
#include "cJSON.h"
#include "fc_agent.h"
#include "fc_hw_nuttx.h"

int fc_nuttx_storage_init(void);
#if defined(CONFIG_ESP32S3_APP_FORMAT_MCUBOOT) || defined(FC_ARM_PLATFORM)
void fc_nuttx_start_trial(void);
#endif

static int factory_read(size_t offset, void *buf, size_t len)
{
#ifdef CONFIG_FLIGHTCTL_NRF7002DK
    memcpy(buf, (const void *)(0x000fc000u + offset), len);
    return 0;
#elif defined(CONFIG_ARCH_BOARD_NUCLEO_F767ZI)
    memcpy(buf, (const void *)(0x08020000u + offset), len);
    return 0;
#elif defined(CONFIG_ARCH_BOARD_RASPBERRYPI_PICO_2_W)
    /* FCC1 partition in the XIP flash, immediately before LittleFS. */
    memcpy(buf, (const void *)(0x10300000u + offset), len);
    return 0;
#else
#ifdef CONFIG_ARCH_CHIP_ESP32S3
    struct mtd_dev_s *flash = esp32s3_spiflash_mtd();
    const off_t base = CONFIG_ESP32S3_STORAGE_MTD_OFFSET - 0x4000;
#else
    struct mtd_dev_s *flash = esp_spiflash_mtd();
#ifdef CONFIG_ARCH_CHIP_ESP32
    const off_t base = CONFIG_ESP32_STORAGE_MTD_OFFSET - 0x4000;
#else
    const off_t base = CONFIG_ESPRESSIF_STORAGE_MTD_OFFSET - 0x4000;
#endif
#endif
    return flash && MTD_READ(flash, base + offset, len, buf) == (ssize_t)len ? 0 : -1;
#endif
}

static cJSON *factory(void)
{
    const size_t size = 16 * 1024;
    uint8_t hdr[8];
    if (factory_read(0, hdr, sizeof(hdr)) || memcmp(hdr, "FCC1", 4)) {
        return NULL;
    }
    uint32_t n;
    memcpy(&n, hdr + 4, sizeof(n)); /* FCC1 stores the length little endian. */
    if (!n || n > size - sizeof(hdr)) {
        return NULL;
    }
    char *json = malloc(n + 1);
    cJSON *result = NULL;
    if (json && factory_read(sizeof(hdr), json, n) == 0) {
        json[n] = 0;
        result = cJSON_Parse(json);
    }
    free(json);
    return result;
}

static const char *field(cJSON *obj, const char *key)
{
    return cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(obj, key));
}

static int network(const char *ssid, const char *pass)
{
#ifdef CONFIG_FLIGHTCTL_SERIAL_NETWORK
    struct in_addr address = { .s_addr = htonl(0xc0a87f02) }; /* 192.168.127.2 */
    struct in_addr gateway = { .s_addr = htonl(0xc0a87f01) };
    struct in_addr mask = { .s_addr = htonl(0xffffff00) };
    if (slip_initialize(0, CONFIG_FLIGHTCTL_SLIP_DEVICE) < 0 ||
        netlib_set_ipv4addr("sl0", &address) < 0 ||
        netlib_set_ipv4netmask("sl0", &mask) < 0 ||
        netlib_set_dripv4addr("sl0", &gateway) < 0 ||
        netlib_set_ipv4dnsaddr(&gateway) < 0 || netlib_ifup("sl0") < 0) {
        return -1;
    }
    puts("fc: serial IP ready, synchronizing time");
#elif defined(CONFIG_STM32_ETHMAC)
    while (netlib_ifup("eth0") < 0 || netlib_obtain_ipv4addr("eth0") < 0) {
        puts("fc: waiting for Ethernet link and DHCP");
        sleep(5);
    }
    puts("fc: Ethernet and DHCP ready, synchronizing time");
#else
    if (!ssid || !*ssid || strlen(ssid) > 32 || !pass || strlen(pass) > 63) {
        return -1;
    }
    struct wpa_wconfig_s wifi = {
        .ifname = "wlan0", .sta_mode = WAPI_MODE_MANAGED,
        .auth_wpa = *pass ? IW_AUTH_WPA_VERSION_WPA2 : IW_AUTH_WPA_VERSION_DISABLED,
        .cipher_mode = *pass ? IW_AUTH_CIPHER_CCMP : IW_AUTH_CIPHER_NONE,
        .alg = *pass ? WPA_ALG_CCMP : WPA_ALG_NONE,
        .ssid = ssid, .ssidlen = strlen(ssid), .passphrase = pass, .phraselen = strlen(pass),
    };
    while (netlib_ifup("wlan0") < 0 || wpa_driver_wext_associate(&wifi) < 0 ||
           netlib_obtain_ipv4addr("wlan0") < 0) {
        puts("fc: retrying Wi-Fi and DHCP");
        sleep(5);
    }
    puts("fc: Wi-Fi and DHCP ready, synchronizing time");
#endif
#ifdef CONFIG_ARCH_CHIP_ESP32C3
    /* A previous firmware can leave an arbitrary epoch in the RTC registers.
     * NTP can correct an offset of less than 2^31 seconds from its seed. */
    const time_t seed = 1704067200; /* 2024-01-01 UTC */
    time_t now = time(NULL);
    if (now < 0 || now - seed >= INT32_MAX) {
        struct timespec initial = { .tv_sec = seed - 1 };
        if (clock_settime(CLOCK_REALTIME, &initial) < 0) {
            return -1;
        }
    }
#endif
    if (ntpc_start_with_list(CONFIG_NETUTILS_NTPCLIENT_SERVER) < 0) {
        puts("fc: could not start the NTP client");
        return -1;
    }
#ifndef CONFIG_NETUTILS_NTPCLIENT_STAY_ON
    struct ntpc_status_s sample;
    while (ntpc_status(&sample) < 0 || sample.nsamples == 0) {
        sleep(1);
    }
#endif
    while (time(NULL) < 1704067200) {
        sleep(1);
    }
    puts("fc: clock ready for TLS");
    return 0;
}

int main(int argc, char **argv)
{
#ifdef CONFIG_ARCH_CHIP_ESP32S3
    fc_nuttx_boot_reason();
#endif
#if defined(CONFIG_ESP32S3_APP_FORMAT_MCUBOOT) || defined(FC_ARM_PLATFORM)
    fc_nuttx_start_trial();
#endif
    cJSON *creds = factory();
    cJSON *port = cJSON_GetObjectItem(creds, "server_port");
    if (!field(creds, "server_host") || !field(creds, "ca") || !field(creds, "enroll_crt") ||
        !field(creds, "enroll_key") || !field(creds, "signing_pub") ||
        !cJSON_IsNumber(port) || port->valueint < 1 || port->valueint > 65535) {
        puts("fc: missing endpoint, credentials or signing key in factory partition");
        cJSON_Delete(creds);
        return 1;
    }
#ifdef CONFIG_ARCH_BOARD_NUCLEO_F767ZI
    const char *layout = field(creds, "storage_format");
    if (!layout || strcmp(layout, "nuttx-nvs")) {
        puts("fc: provision NuttX storage before starting the agent");
        cJSON_Delete(creds);
        return 1;
    }
#endif
    if (fc_nuttx_storage_init() != 0) {
        puts("fc: persistent storage unavailable");
        cJSON_Delete(creds);
        return 1;
    }
    const char *labels[33] = {0};
    size_t n = 0;
    cJSON *item;
    cJSON_ArrayForEach(item, cJSON_GetObjectItem(creds, "labels")) {
        if (cJSON_IsString(item) && item->string && n + 2 < 33) {
            labels[n++] = item->string;
            labels[n++] = item->valuestring;
        }
    }
    fc_agent_config_t cfg = {
        .server_host = field(creds, "server_host"), .server_port = port->valueint,
        .ca_pem = field(creds, "ca"), .enrollment_cert_pem = field(creds, "enroll_crt"),
        .enrollment_key_pem = field(creds, "enroll_key"), .signing_pub_pem = field(creds, "signing_pub"),
        .agent_version = FC_AGENT_VERSION, .labels = labels, .hw = &fc_nuttx_hardware,
    };
    puts("fc: " FC_AGENT_VERSION " on NuttX");
    fc_agent_apps_start(&cfg);
    int rc = network(field(creds, "wifi_ssid"), field(creds, "wifi_pass"));
    if (rc == 0) {
        rc = fc_agent_run(&cfg);
    }
    /* The app and status threads retain pointers into the factory config. */
    return rc == 0 ? 0 : 1;
}
