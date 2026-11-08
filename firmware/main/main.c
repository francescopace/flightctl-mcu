/*
 * flightctl-mcu firmware: the alarm app, WiFi + SNTP, then the Flight Control agent.
 */
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_app_desc.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "alarm.h"
#include "fc_agent.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

int fc_port_espidf_init(void);

extern const char ca_pem[] asm("_binary_ca_crt_start");
extern const char enroll_crt_pem[] asm("_binary_enroll_crt_start");
extern const char enroll_key_pem[] asm("_binary_enroll_key_start");
#ifdef FC_SIGNING_KEY
extern const char signing_pub_pem[] asm("_binary_signing_pub_start");
#else
static const char *const signing_pub_pem = NULL;
#endif

static const char *TAG = "main";
static EventGroupHandle_t s_wifi;
#define WIFI_UP BIT0

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        xEventGroupClearBits(s_wifi, WIFI_UP);
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&ev->ip_info.ip));
        xEventGroupSetBits(s_wifi, WIFI_UP);
    }
}

static void wifi_start(void)
{
    s_wifi = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL);
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, CONFIG_FC_WIFI_SSID, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, CONFIG_FC_WIFI_PASSWORD, sizeof(wc.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
    ESP_ERROR_CHECK(esp_wifi_start());
    xEventGroupWaitBits(s_wifi, WIFI_UP, pdFALSE, pdTRUE, portMAX_DELAY);
}

/* Certificate validity checks (MBEDTLS_HAVE_TIME_DATE) need the real time. */
static void time_sync(void)
{
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_netif_sntp_init(&cfg);
    while (esp_netif_sntp_sync_wait(pdMS_TO_TICKS(10000)) != ESP_OK) {
        ESP_LOGW(TAG, "waiting for SNTP time sync...");
    }
    time_t now = time(NULL);
    ESP_LOGI(TAG, "time synced: %s", ctime(&now));
}

/* "k=v,k=v" -> { "k", "v", ..., NULL } */
static const char **parse_labels(const char *spec)
{
    static const char *out[17];
    static char buf[128];
    strlcpy(buf, spec, sizeof(buf));
    int n = 0;
    for (char *save, *tok = strtok_r(buf, ",", &save); tok && n < 16; tok = strtok_r(NULL, ",", &save)) {
        char *eq = strchr(tok, '=');
        if (eq) {
            *eq = '\0';
            out[n++] = tok;
            out[n++] = eq + 1;
        }
    }
    out[n] = NULL;
    return out;
}

/* Safety net for a new firmware stuck before the agent runs (e.g. no WiFi):
 * the agent confirms the image once it reaches Flight Control; if that has
 * not happened by now, go back to the previous image. */
#define OTA_GUARD_S 240

static void ota_guard(void *arg)
{
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) == ESP_OK &&
        st == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGE(TAG, "new firmware not confirmed after %d s: rolling back", OTA_GUARD_S);
        esp_ota_mark_app_invalid_rollback_and_reboot();
    }
}

static void start_ota_guard(void)
{
    esp_ota_img_states_t st;
    if (esp_ota_get_state_partition(esp_ota_get_running_partition(), &st) != ESP_OK ||
        st != ESP_OTA_IMG_PENDING_VERIFY) {
        return;
    }
    const esp_timer_create_args_t args = {.callback = ota_guard, .name = "ota_guard"};
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_once(t, (uint64_t)OTA_GUARD_S * 1000000);
    }
    ESP_LOGW(TAG, "firmware %s pending verification", esp_app_get_description()->version);
}

void app_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        /* Left over from a previous firmware: start clean. */
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    ESP_ERROR_CHECK(fc_port_espidf_init() == 0 ? ESP_OK : ESP_FAIL);
    start_ota_guard();
    ESP_LOGI(TAG, "flightctl-mcu firmware %s", esp_app_get_description()->version);
    alarm_start(); /* runs offline too, with the last stored config */

    int n_natives = 0;
    const void *natives = alarm_wasm_natives(&n_natives);
    fc_agent_config_t cfg = {
        .server_host = CONFIG_FC_SERVER_HOST,
        .server_port = CONFIG_FC_SERVER_PORT,
        .ca_pem = ca_pem,
        .enrollment_cert_pem = enroll_crt_pem,
        .enrollment_key_pem = enroll_key_pem,
        .signing_pub_pem = signing_pub_pem,
        .labels = parse_labels("chip=" CONFIG_IDF_TARGET "," CONFIG_FC_LABELS),
        .agent_version = "flightctl-mcu/0.1.0",
        .status_interval_s = CONFIG_FC_STATUS_INTERVAL,
        .on_config_file = alarm_on_config_file,
        .custom_info = alarm_custom_info,
        .wasm_natives = natives,
        .wasm_natives_count = n_natives,
    };
    fc_agent_apps_start(&cfg); /* WASM apps saved by a previous run, before the network */

    if (strlen(CONFIG_FC_WIFI_SSID) == 0) {
        ESP_LOGE(TAG, "WiFi not configured: create firmware/sdkconfig.wifi (see README)");
        return;
    }
    if (strlen(CONFIG_FC_SERVER_HOST) == 0) {
        ESP_LOGE(TAG, "Flight Control endpoint not configured: create firmware/sdkconfig.local (see README)");
        return;
    }
    wifi_start();
    time_sync();


    fc_agent_run(&cfg);
}
