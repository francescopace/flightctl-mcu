/*
 * flightctl-mcu firmware: WiFi + SNTP, then the Flight Control agent.
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
#include "fc_prov.h"
#endif
#include "hw.h"
#include "cJSON.h"
#include "fc_agent.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
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

/* Whether the station reconnects by itself: not while field provisioning
 * tries a network, which reports how it went instead. */
static volatile bool s_reconnect = true;

#if CONFIG_FC_FIELD_PROVISIONING
static volatile fc_prov_join_t s_join = FC_PROV_JOINING;

/* Why a join failed, as Espressif's provisioning manager tells them apart. */
static fc_prov_join_t join_failure(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_AUTH_EXPIRE:
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_AUTH_FAIL:
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
    case WIFI_REASON_MIC_FAILURE:
        return FC_PROV_BAD_PASSWORD;
    default:
        return FC_PROV_NOT_FOUND;
    }
}
#endif

static void on_wifi(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && (id == WIFI_EVENT_STA_START || id == WIFI_EVENT_STA_DISCONNECTED)) {
        xEventGroupClearBits(s_wifi, WIFI_UP);
#if CONFIG_FC_FIELD_PROVISIONING
        if (id == WIFI_EVENT_STA_DISCONNECTED && !s_reconnect && s_join == FC_PROV_JOINING) {
            uint8_t reason = ((wifi_event_sta_disconnected_t *)data)->reason;
            if (reason != WIFI_REASON_ASSOC_LEAVE) { /* not our own disconnect before a try */
                s_join = join_failure(reason);
            }
        }
#endif
        if (s_reconnect) {
            esp_wifi_connect();
        }
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ev = data;
        ESP_LOGI(TAG, "got IP " IPSTR, IP2STR(&ev->ip_info.ip));
#if CONFIG_FC_FIELD_PROVISIONING
        s_join = FC_PROV_JOINED;
#endif
        xEventGroupSetBits(s_wifi, WIFI_UP);
    }
}

static void sta_config(wifi_config_t *wc, const char *ssid, const char *pass)
{
    memset(wc, 0, sizeof(*wc));
    strlcpy((char *)wc->sta.ssid, ssid, sizeof(wc->sta.ssid));
    strlcpy((char *)wc->sta.password, pass != NULL ? pass : "", sizeof(wc->sta.password));
}

#if CONFIG_FC_FIELD_PROVISIONING
/* Provisioning in the field (core/src/fc_prov.c, the same as on Zephyr): a
 * device that left the factory without WiFi (provision.py --pop, no
 * --wifi-ssid) scans for networks, then opens an access point, FC-xxxxxx,
 * where an installer gives it the site's network with esp_prov or
 * Espressif's provisioning app. Security 1: X25519 key exchange, the session
 * authenticated by the proof of possession, which is also the access point's
 * WPA2 password. The network is tried with the access point still up, so the
 * installer is told whether it worked. Once joined, it is kept with the
 * credentials (keep_network); provisioning the device again, or erasing the
 * flash, starts field provisioning again. */
static int field_join(void *ctx, const char *ssid, const char *pass)
{
    wifi_config_t wc;
    sta_config(&wc, ssid, pass);
    s_join = FC_PROV_JOINING;
    esp_wifi_disconnect();
    if (esp_wifi_set_config(WIFI_IF_STA, &wc) != ESP_OK || esp_wifi_connect() != ESP_OK) {
        s_join = FC_PROV_NOT_FOUND;
    }
    return 0;
}

static fc_prov_join_t field_join_state(void *ctx)
{
    return s_join;
}

static uint8_t auth_of(wifi_auth_mode_t m)
{
    switch (m) {
    case WIFI_AUTH_OPEN: return FC_PROV_AUTH_OPEN;
    case WIFI_AUTH_WEP: return FC_PROV_AUTH_WEP;
    case WIFI_AUTH_WPA_PSK: return FC_PROV_AUTH_WPA_PSK;
    case WIFI_AUTH_WPA2_PSK: return FC_PROV_AUTH_WPA2_PSK;
    case WIFI_AUTH_WPA_WPA2_PSK: return FC_PROV_AUTH_WPA_WPA2_PSK;
    case WIFI_AUTH_WPA2_ENTERPRISE: return FC_PROV_AUTH_WPA2_ENTERPRISE;
    case WIFI_AUTH_WPA3_PSK: return FC_PROV_AUTH_WPA3_PSK;
    case WIFI_AUTH_WPA2_WPA3_PSK: return FC_PROV_AUTH_WPA2_WPA3_PSK;
    default: return FC_PROV_AUTH_WPA2_PSK;
    }
}

/* The networks around, for the installer to pick from. */
static int scan(fc_prov_ap_t *aps)
{
    uint16_t n = FC_PROV_SCAN_MAX;
    wifi_ap_record_t *rec = calloc(n, sizeof(*rec));
    int count = 0;
    if (rec != NULL && esp_wifi_scan_start(NULL, true) == ESP_OK && esp_wifi_scan_get_ap_records(&n, rec) == ESP_OK) {
        for (int i = 0; i < n; i++) {
            fc_prov_ap_t *ap = &aps[count++];
            strlcpy(ap->ssid, (const char *)rec[i].ssid, sizeof(ap->ssid));
            memcpy(ap->bssid, rec[i].bssid, sizeof(ap->bssid));
            ap->rssi = rec[i].rssi;
            ap->channel = rec[i].primary;
            ap->auth = auth_of(rec[i].authmode);
        }
    } else {
        ESP_LOGW(TAG, "WiFi: the scan failed; the installer will have to type the network's name");
    }
    esp_wifi_clear_ap_list();
    free(rec);
    return count;
}

/* The network given in the field, kept with the credentials (encrypted in the
 * secure variant): the next boot joins it as a network given at the factory.
 * Not by the WiFi driver: the network it was given while tried, in RAM, is
 * the same, and setting it again with flash storage did not write it. */
static bool keep_network(const char *ssid, const char *pass)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open_from_partition(CREDS_PART, CREDS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_str(h, "wifi_ssid", ssid);
        if (err == ESP_OK) {
            err = nvs_set_str(h, "wifi_pass", pass);
        }
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "WiFi: keeping the network: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

static void field_provisioning(const char *pop)
{
    fc_prov_ap_t *aps = calloc(FC_PROV_SCAN_MAX, sizeof(*aps));
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    wifi_config_t ap = {.ap = {.max_connection = 2, .authmode = WIFI_AUTH_WPA2_PSK}};
    int n = snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "FC-%02X%02X%02X", mac[3], mac[4], mac[5]);
    ap.ap.ssid_len = (uint8_t)n;
    strlcpy((char *)ap.ap.password, pop, sizeof(ap.ap.password));

    /* Tried networks stay in RAM: the driver keeps only the one that works. */
    s_reconnect = false;
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    int ap_count = aps != NULL ? scan(aps) : 0;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
    ESP_LOGW(TAG, "no WiFi network: field provisioning on access point %s", (const char *)ap.ap.ssid);

    char ssid[33], pass[65];
    int joined = 0;
    fc_prov_config_t cfg = {
        .pop = pop,
        .port = 80,
        .aps = aps,
        .ap_count = ap_count,
        .join = field_join,
        .join_state = field_join_state,
    };
    while (fc_prov_serve(&cfg, ssid, pass, &joined) != 0 || !joined) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    free(aps);

    /* Joined: close the access point and keep the network. */
    s_reconnect = true;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    if (!keep_network(ssid, pass)) {
        ESP_LOGW(TAG, "WiFi: cannot keep the network: it will be asked for again after a restart");
    }
    memset(pass, 0, sizeof(pass));
    ESP_LOGI(TAG, "field provisioning: done");
}

/* A network from a field provisioning by an earlier firmware, which left it to
 * the WiFi driver to keep (Espressif's network_provisioning). */
static bool kept_network(void)
{
    wifi_config_t wc = {0};
    return esp_wifi_get_config(WIFI_IF_STA, &wc) == ESP_OK && wc.sta.ssid[0] != '\0';
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
        if (kept_network()) {
            ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
            ESP_ERROR_CHECK(esp_wifi_start()); /* with the network the driver kept */
        } else if (c->pop != NULL) {
            esp_netif_create_default_wifi_ap();
            field_provisioning(c->pop);
        } else {
            ESP_LOGE(TAG, "WiFi not configured: provision it (--wifi-ssid, or --pop to set it in the field)");
            vTaskDelay(portMAX_DELAY);
        }
        xEventGroupWaitBits(s_wifi, WIFI_UP, pdFALSE, pdTRUE, portMAX_DELAY);
        return;
    }
#endif
    /* Provisioned at the factory: this network, not one kept by the driver. */
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    wifi_config_t wc;
    sta_config(&wc, c->ssid, c->password);
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

/* A watchdog outside the agent's threads: no status update accepted for
 * this long restarts the device. The agent does it itself after 10 minutes
 * from its status thread, which a hung download or connection can hold: on
 * an ESP32-C5 running two apps with 64 KB memories the device went silent
 * for good (v3.14-rc10.1). Checked every minute on esp_timer's task. */
#define STATUS_WATCHDOG_S 1800

static void status_watchdog(void *arg)
{
    long age = fc_agent_status_age();
    if (age > STATUS_WATCHDOG_S) {
        ESP_LOGE(TAG, "no status update accepted for %ld s, the agent stuck: restarting", age);
        esp_restart();
    }
}

static void start_status_watchdog(void)
{
    const esp_timer_create_args_t args = {.callback = status_watchdog, .name = "status_wd"};
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) == ESP_OK) {
        esp_timer_start_periodic(t, 60ULL * 1000000);
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
    return 0;
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

    static creds_t creds;
    load_creds(&creds);
    static char labels[256];
    snprintf(labels, sizeof(labels), "chip=%s,%s", CONFIG_IDF_TARGET, creds.labels);

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
        .metrics_interval_s = CONFIG_FC_METRICS_INTERVAL,
        .ota_verify_timeout_s = CONFIG_FC_OTA_VERIFY_TIMEOUT,
        .on_config_file = on_config_file,
        .hw = hw_espidf(),
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

    /* The agent's spec loop runs on this task from here: at the priority of
     * its other thread and the apps', not the main task's (1). */
    vTaskPrioritySet(NULL, CONFIG_PTHREAD_TASK_PRIO_DEFAULT);
    start_status_watchdog();
    fc_agent_run(&cfg);
}
