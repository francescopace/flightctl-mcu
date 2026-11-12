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
#include "esp_mac.h"
#include "esp_wifi.h"
#if CONFIG_FC_FIELD_PROVISIONING
#include "network_provisioning/manager.h"
#if CONFIG_FC_FIELD_PROVISIONING_BLE
#include "network_provisioning/scheme_ble.h"
/* BLE: advertised as FC-xxxxxx; the Bluetooth memory is given back when
 * provisioning is over, or right away on a device that has its network. */
#define PROV_SCHEME network_prov_scheme_ble
#define PROV_SCHEME_HANDLER NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM
#define PROV_HOW "over BLE as"
#else
#include "network_provisioning/scheme_softap.h"
#define PROV_SCHEME network_prov_scheme_softap
#define PROV_SCHEME_HANDLER NETWORK_PROV_EVENT_HANDLER_NONE
#define PROV_HOW "on access point"
#endif
#endif
#include "alarm.h"
#include "cJSON.h"
#include "fc_agent.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

int fc_port_espidf_init(void);

static const char *TAG = "main";

/* What the device needs to reach its Flight Control, from the fc_creds
 * partition (tools/provision.sh). WiFi, endpoint and labels fall back to the
 * build settings, for development. */
typedef struct {
    char *ca, *enroll_crt, *enroll_key, *signing_pub;
    char *host, *labels, *ssid, *password, *pop;
    int port;
} creds_t;

static char *creds_str(nvs_handle_t h, const char *key)
{
    size_t len = 0;
    if (nvs_get_str(h, key, NULL, &len) != ESP_OK || len == 0) {
        return NULL;
    }
    char *s = malloc(len);
    if (s != NULL && nvs_get_str(h, key, s, &len) != ESP_OK) {
        free(s);
        s = NULL;
    }
    return s;
}

#if CONFIG_NVS_ENCRYPTION
/* fc_creds is written in clear by provisioning (tools/provision.py), and is
 * only a drop box: what is in it moves into the encrypted store, then it is
 * erased. A second provisioning replaces what the store holds. */
#define CREDS_PART "nvs_store"
#define CREDS_NS "fc_creds"

static esp_err_t copy_entry(nvs_handle_t from, nvs_handle_t to, const nvs_entry_info_t *e)
{
    size_t len = 0;
    esp_err_t err;
    switch (e->type) {
    case NVS_TYPE_I32: {
        int32_t v;
        err = nvs_get_i32(from, e->key, &v);
        return err == ESP_OK ? nvs_set_i32(to, e->key, v) : err;
    }
    case NVS_TYPE_STR:
    case NVS_TYPE_BLOB: {
        int str = e->type == NVS_TYPE_STR;
        err = str ? nvs_get_str(from, e->key, NULL, &len) : nvs_get_blob(from, e->key, NULL, &len);
        if (err != ESP_OK) {
            return err;
        }
        char *v = malloc(len);
        if (v == NULL) {
            return ESP_ERR_NO_MEM;
        }
        err = str ? nvs_get_str(from, e->key, v, &len) : nvs_get_blob(from, e->key, v, &len);
        if (err == ESP_OK) {
            err = str ? nvs_set_str(to, e->key, v) : nvs_set_blob(to, e->key, v, len);
        }
        free(v);
        return err;
    }
    default:
        ESP_LOGW(TAG, "fc_creds: %s has an unexpected type, skipped", e->key);
        return ESP_OK;
    }
}

static void import_creds(void)
{
    nvs_handle_t from, to;
    if (nvs_flash_init_partition("fc_creds") != ESP_OK) {
        return;
    }
    if (nvs_open_from_partition("fc_creds", "fc", NVS_READONLY, &from) != ESP_OK) {
        nvs_flash_deinit_partition("fc_creds"); /* nothing new */
        return;
    }
    esp_err_t err = nvs_open_from_partition(CREDS_PART, CREDS_NS, NVS_READWRITE, &to);
    int n = 0;
    if (err == ESP_OK) {
        err = nvs_erase_all(to);
        nvs_iterator_t it = NULL;
        esp_err_t found = nvs_entry_find("fc_creds", "fc", NVS_TYPE_ANY, &it);
        while (err == ESP_OK && found == ESP_OK) {
            nvs_entry_info_t e;
            nvs_entry_info(it, &e);
            err = copy_entry(from, to, &e);
            n++;
            found = nvs_entry_next(&it);
        }
        nvs_release_iterator(it);
        if (err == ESP_OK) {
            err = nvs_commit(to);
        }
        nvs_close(to);
    }
    nvs_close(from);
    nvs_flash_deinit_partition("fc_creds");
    if (err != ESP_OK) {
        /* fc_creds stays: the next boot tries again. */
        ESP_LOGE(TAG, "moving fc_creds to the encrypted store: %s", esp_err_to_name(err));
        return;
    }
    nvs_flash_erase_partition("fc_creds");
    ESP_LOGI(TAG, "provisioning moved to the encrypted store (%d entries), fc_creds erased", n);
}
#else
#define CREDS_PART "fc_creds"
#define CREDS_NS "fc"
#endif

static void load_creds(creds_t *c)
{
    memset(c, 0, sizeof(*c));
    c->port = CONFIG_FC_SERVER_PORT;
    nvs_handle_t h;
#if CONFIG_NVS_ENCRYPTION
    import_creds();
    if (nvs_open_from_partition(CREDS_PART, CREDS_NS, NVS_READONLY, &h) == ESP_OK) {
#else
    if (nvs_flash_init_partition(CREDS_PART) == ESP_OK &&
        nvs_open_from_partition(CREDS_PART, CREDS_NS, NVS_READONLY, &h) == ESP_OK) {
#endif
        c->ca = creds_str(h, "ca");
        c->enroll_crt = creds_str(h, "enroll_crt");
        c->enroll_key = creds_str(h, "enroll_key");
        c->signing_pub = creds_str(h, "signing_pub");
        c->host = creds_str(h, "server_host");
        c->labels = creds_str(h, "labels");
        c->ssid = creds_str(h, "wifi_ssid");
        c->password = creds_str(h, "wifi_pass");
        c->pop = creds_str(h, "prov_pop");
        int32_t port;
        if (nvs_get_i32(h, "server_port", &port) == ESP_OK) {
            c->port = (int)port;
        }
        nvs_close(h);
    }
    if (c->host == NULL) {
        c->host = strdup(CONFIG_FC_SERVER_HOST);
    }
    if (c->labels == NULL) {
        c->labels = strdup(CONFIG_FC_LABELS);
    }
    if (c->ssid == NULL) {
        c->ssid = strdup(CONFIG_FC_WIFI_SSID);
        c->password = strdup(CONFIG_FC_WIFI_PASSWORD);
    } else if (c->password == NULL) {
        c->password = strdup("");
    }
#if CONFIG_FC_TEST_SERVER_PORT
    c->port = CONFIG_FC_TEST_SERVER_PORT;
#endif
}
static EventGroupHandle_t s_wifi;
#define WIFI_UP BIT0
#define PROV_OVER BIT1 /* field provisioning ended, its memory given back */

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

#if CONFIG_FC_FIELD_PROVISIONING
/* Provisioning in the field: a device that left the factory without WiFi
 * (provision.py --pop, no --wifi-ssid) opens an access point, FC-xxxxxx (or,
 * in the ble variant, advertises it over BLE), where an installer gives it
 * the site's network with esp_prov or Espressif's provisioning app. Security 1: X25519 key exchange, the
 * session authenticated by the proof of possession, which is also the
 * access point's WPA2 password. The network is kept by the WiFi driver (in
 * the default NVS partition, encrypted in the secure variant); erasing it
 * (or the flash) starts provisioning again. */
static void on_prov(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == NETWORK_PROV_WIFI_CRED_RECV) {
        ESP_LOGI(TAG, "field provisioning: network \"%s\" received", (const char *)((wifi_sta_config_t *)data)->ssid);
    } else if (id == NETWORK_PROV_WIFI_CRED_FAIL) {
        ESP_LOGW(TAG, "field provisioning: cannot join that network (%s), waiting for another",
                 *(network_prov_wifi_sta_fail_reason_t *)data == NETWORK_PROV_WIFI_STA_AUTH_ERROR ? "wrong password" : "not found");
        network_prov_mgr_reset_wifi_sm_state_on_failure();
    } else if (id == NETWORK_PROV_END) {
        ESP_LOGI(TAG, "field provisioning: done");
        network_prov_mgr_deinit();
        xEventGroupSetBits(s_wifi, PROV_OVER);
    }
}

/* 1: the WiFi driver has a network from an earlier field provisioning;
 * 2: provisioning started (the manager runs WiFi); 0: neither. */
static int field_provisioning(const creds_t *c)
{
    network_prov_mgr_config_t cfg = {
        .scheme = PROV_SCHEME,
        .scheme_event_handler = PROV_SCHEME_HANDLER,
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(cfg));
    bool done = false;
    network_prov_mgr_is_wifi_provisioned(&done);
    if (done) {
        network_prov_mgr_deinit();
        return 1; /* the driver starts with the stored network */
    }
    if (c->pop == NULL) {
        network_prov_mgr_deinit();
        return 0;
    }
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    char name[16];
    snprintf(name, sizeof(name), "FC-%02X%02X%02X", mac[3], mac[4], mac[5]);
    esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID, on_prov, NULL);
    ESP_LOGW(TAG, "no WiFi network: field provisioning " PROV_HOW " %s", name);
#if CONFIG_FC_FIELD_PROVISIONING_BLE
    ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1, c->pop, name, NULL));
#else
    ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1, c->pop, name, c->pop));
#endif
    return 2;
}
#endif

static void wifi_start(const creds_t *c)
{
    s_wifi = xEventGroupCreate();
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_wifi, NULL);
#if CONFIG_FC_FIELD_PROVISIONING
    if (c->ssid[0] == '\0') {
#if !CONFIG_FC_FIELD_PROVISIONING_BLE
        esp_netif_create_default_wifi_ap();
#endif
        int fp = field_provisioning(c);
        if (fp == 0) {
            ESP_LOGE(TAG, "WiFi not configured: provision it (--wifi-ssid, or --pop to set it in the field)");
            vTaskDelay(portMAX_DELAY);
        }
        if (fp == 1) {
            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
            ESP_ERROR_CHECK(esp_wifi_start());
        }
        /* After provisioning, not during it: its service (the BLE stack
         * above all) holds memory the agent needs for an update. */
        xEventGroupWaitBits(s_wifi, fp == 2 ? WIFI_UP | PROV_OVER : WIFI_UP, pdFALSE, pdTRUE, portMAX_DELAY);
        return;
    }
#endif
    /* Provisioned at the factory: this network, not one kept by the driver. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wc = {0};
    strlcpy((char *)wc.sta.ssid, c->ssid, sizeof(wc.sta.ssid));
    strlcpy((char *)wc.sta.password, c->password, sizeof(wc.sta.password));
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
    static char buf[256];
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
#define OTA_GUARD_S (CONFIG_FC_OTA_VERIFY_TIMEOUT + 90)

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

int fc_port_espidf_revoke_secure_boot_key(int index);

/* Device security settings from the fleet's config, as a file:
 *   /etc/flightctl-mcu/secure-boot.json  {"revokeKeys": [2]}
 * revokes Secure Boot key slots for good (a spare key that was exposed, see
 * docs/SECURITY.md). The device refuses the file, and the update is reported
 * as failed, when a key cannot be revoked: the one that signs the running
 * firmware or the bootloader, or any key on a device without Secure Boot. */
#define SECURE_BOOT_CONFIG "/etc/flightctl-mcu/secure-boot.json"

static int secure_boot_config(const char *data)
{
    cJSON *doc = cJSON_Parse(data);
    const cJSON *keys = cJSON_GetObjectItemCaseSensitive(doc, "revokeKeys");
    int rc = cJSON_IsArray(keys) ? 0 : -1;
    const cJSON *k;
    cJSON_ArrayForEach(k, keys) {
        if (!cJSON_IsNumber(k) || fc_port_espidf_revoke_secure_boot_key(k->valueint) != 0) {
            rc = -1;
        }
    }
    if (rc != 0) {
        ESP_LOGW(TAG, "%s refused", SECURE_BOOT_CONFIG);
    }
    cJSON_Delete(doc);
    return rc;
}

static int on_config_file(const char *path, const char *data, size_t len, void *ctx)
{
    if (strcmp(path, SECURE_BOOT_CONFIG) == 0) {
        return secure_boot_config(data);
    }
    return alarm_on_config_file(path, data, len, ctx);
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

    static creds_t creds;
    load_creds(&creds);
    static char labels[256];
    snprintf(labels, sizeof(labels), "chip=%s,%s", CONFIG_IDF_TARGET, creds.labels);

    int n_natives = 0;
    const void *natives = alarm_wasm_natives(&n_natives);
    fc_agent_config_t cfg = {
        .server_host = creds.host,
        .server_port = creds.port,
        .ca_pem = creds.ca,
        .enrollment_cert_pem = creds.enroll_crt,
        .enrollment_key_pem = creds.enroll_key,
        .signing_pub_pem = creds.signing_pub,
        .labels = parse_labels(labels),
        .agent_version = "flightctl-mcu/0.1.0",
        .status_interval_s = CONFIG_FC_STATUS_INTERVAL,
        .ota_verify_timeout_s = CONFIG_FC_OTA_VERIFY_TIMEOUT,
        .on_config_file = on_config_file,
        .custom_info = alarm_custom_info,
        .wasm_natives = natives,
        .wasm_natives_count = n_natives,
    };
    fc_agent_apps_start(&cfg); /* WASM apps saved by a previous run, before the network */

    if (creds.ca == NULL || creds.enroll_crt == NULL || creds.enroll_key == NULL || creds.host[0] == '\0') {
        ESP_LOGE(TAG, "not provisioned: write the fc_creds partition with tools/provision.sh (see README)");
        return;
    }
#if !CONFIG_FC_FIELD_PROVISIONING
    if (creds.ssid[0] == '\0') {
        ESP_LOGE(TAG, "WiFi not configured: provision it, or build with port/espidf/sdkconfig.wifi");
        return;
    }
#endif
    ESP_LOGI(TAG, "Flight Control %s:%d, image signatures %s", creds.host, creds.port,
             creds.signing_pub ? "checked" : "NOT checked (no signing key provisioned)");
    wifi_start(&creds);
    time_sync();


    fc_agent_run(&cfg);
}
