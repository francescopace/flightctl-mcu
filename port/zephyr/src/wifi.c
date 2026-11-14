/*
 * The WiFi network, on boards that have one: given at the factory (fc_creds,
 * wifi_ssid), or by an installer in the field (core/src/fc_prov.c), as the
 * ESP-IDF firmware does it (docs/PROVISIONING.md).
 *
 * In the field: with no network at the factory and none kept from an earlier
 * provisioning, a device that has a proof of possession (prov_pop) scans for
 * networks, then opens an access point, FC-xxxxxx (the end of its MAC),
 * protected by that proof, and serves Espressif's provisioning protocol on
 * 192.168.4.1:80. Where the radio can be an access point and a station at
 * once (Espressif's, CONFIG_ESP32_WIFI_AP_STA_MODE), it tries the network
 * with the access point still up and tells the installer whether it joined;
 * one that cannot closes the access point to try it, and opens it again if it
 * fails. A network is kept (fc_port_kv "wifi") once the device has joined it,
 * for the credentials it was given with: writing fc_creds again forgets it.
 * Only on boards built with a DHCP server (CONFIG_NET_DHCPV4_SERVER): not on
 * the Pico W, whose driver cannot open an access point (docs/ZEPHYR.md).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/dhcpv4_server.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_mgmt.h>
#include <zephyr/net/wifi_mgmt.h>

#include "cJSON.h"
#include "fc_crypto.h"
#include "fc_port.h"
#include "fc_prov.h"
#include "fc_wifi_zephyr.h"

/* Where several access points carry the network, the Pico W's chip may join a
 * far one (Infineon's driver takes no access point's address, and telling the
 * chip to prefer the strongest changed nothing): there, name lookups and
 * handshakes time out. A weak join is given up for another one, a few times.
 * Espressif's driver scans every channel and joins the strongest
 * (CONFIG_ESP32_WIFI_STA_SCAN_ALL, in the board's file). */
#define WIFI_GOOD_DBM (-60)
#define WIFI_JOINS    5
/* A network from the field, tried with the access point closed: so many
 * times before the access point opens again. */
#define FIELD_TRIES 3
#define KV_WIFI     "wifi"

static K_SEM_DEFINE(s_ip_up, 0, 1);
static K_SEM_DEFINE(s_scan_done, 0, 1);
static struct net_mgmt_event_callback s_ip_cb, s_wifi_cb;
static struct net_if *s_sta, *s_ap;

/* The network being tried while the access point is up */
static volatile int s_connect_status = -1; /* -1: no result yet */
static volatile int s_sta_ip;

/* The networks seen, while provisioning only: on the heap, not in static RAM. */
static fc_prov_ap_t *s_aps;
static int s_ap_count;

static void on_ip(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
    if (event == NET_EVENT_IPV4_ADDR_ADD && iface == s_sta) {
        s_sta_ip = 1;
        k_sem_give(&s_ip_up);
    }
}

static uint8_t auth_of(enum wifi_security_type t)
{
    switch (t) {
    case WIFI_SECURITY_TYPE_NONE:
        return FC_PROV_AUTH_OPEN;
    case WIFI_SECURITY_TYPE_WEP:
        return FC_PROV_AUTH_WEP;
    case WIFI_SECURITY_TYPE_WPA_PSK:
        return FC_PROV_AUTH_WPA_PSK;
    case WIFI_SECURITY_TYPE_SAE:
        return FC_PROV_AUTH_WPA3_PSK;
    case WIFI_SECURITY_TYPE_WPA_AUTO_PERSONAL:
        return FC_PROV_AUTH_WPA2_WPA3_PSK;
    case WIFI_SECURITY_TYPE_EAP:
        return FC_PROV_AUTH_WPA2_ENTERPRISE;
    default:
        return FC_PROV_AUTH_WPA2_PSK;
    }
}

/* A scan result: kept if it is a network not seen yet (the strongest of its
 * access points), the strongest FC_PROV_SCAN_MAX of them. */
static void scan_result(const struct wifi_scan_result *r)
{
    if (s_aps == NULL || r->ssid_length == 0 || r->ssid_length > 32) {
        return; /* hidden */
    }
    int at = -1;
    for (int i = 0; i < s_ap_count; i++) {
        if (strlen(s_aps[i].ssid) == r->ssid_length && memcmp(s_aps[i].ssid, r->ssid, r->ssid_length) == 0) {
            if (r->rssi <= s_aps[i].rssi) {
                return;
            }
            at = i;
            break;
        }
    }
    if (at < 0) {
        if (s_ap_count < FC_PROV_SCAN_MAX) {
            at = s_ap_count++;
        } else {
            at = 0; /* the weakest, if this one is stronger */
            for (int i = 1; i < s_ap_count; i++) {
                if (s_aps[i].rssi < s_aps[at].rssi) {
                    at = i;
                }
            }
            if (r->rssi <= s_aps[at].rssi) {
                return;
            }
        }
    }
    fc_prov_ap_t *ap = &s_aps[at];
    memset(ap, 0, sizeof(*ap));
    memcpy(ap->ssid, r->ssid, r->ssid_length);
    memcpy(ap->bssid, r->mac, r->mac_length < 6 ? r->mac_length : 6);
    ap->rssi = r->rssi;
    ap->channel = r->channel;
    ap->auth = auth_of(r->security);
}

static void on_wifi(struct net_mgmt_event_callback *cb, uint64_t event, struct net_if *iface)
{
    if (event == NET_EVENT_WIFI_SCAN_RESULT) {
        scan_result(cb->info);
    } else if (event == NET_EVENT_WIFI_SCAN_DONE) {
        k_sem_give(&s_scan_done);
    } else if (event == NET_EVENT_WIFI_CONNECT_RESULT && iface == s_sta) {
        s_connect_status = ((const struct wifi_status *)cb->info)->status;
    } else if (event == NET_EVENT_WIFI_AP_STA_CONNECTED || event == NET_EVENT_WIFI_AP_STA_DISCONNECTED) {
        const struct wifi_ap_sta_info *sta = cb->info;
        fc_port_log(FC_LOG_INFO, "provisioning: a device %s the access point (..:%02x:%02x)",
                    event == NET_EVENT_WIFI_AP_STA_CONNECTED ? "joined" : "left", sta->mac[4], sta->mac[5]);
    }
}

static struct wifi_connect_req_params params(const char *ssid, const char *pass)
{
    return (struct wifi_connect_req_params){
        .ssid = (const uint8_t *)ssid,
        .ssid_length = (uint8_t)strlen(ssid),
        .psk = (const uint8_t *)pass,
        .psk_length = pass != NULL ? (uint8_t)strlen(pass) : 0,
        .security = pass != NULL && pass[0] != '\0' ? WIFI_SECURITY_TYPE_PSK : WIFI_SECURITY_TYPE_NONE,
        .channel = WIFI_CHANNEL_ANY,
        .band = WIFI_FREQ_BAND_2_4_GHZ,
        .mfp = WIFI_MFP_OPTIONAL,
    };
}

/* Joins a network and waits for an address: 0, or -1 after `tries` tries
 * (0: until it works). */
static int join(const char *ssid, const char *pass, int tries)
{
    struct wifi_connect_req_params p = params(ssid, pass);
    int joins = 0;
    for (int t = 1;; t++) {
        fc_port_log(FC_LOG_INFO, "WiFi: joining \"%s\"", ssid);
        k_sem_reset(&s_ip_up);
        if (net_mgmt(NET_REQUEST_WIFI_CONNECT, s_sta, &p, sizeof(p)) == 0) {
            struct wifi_iface_status st = {0};
            int known = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, s_sta, &st, sizeof(st)) == 0;
            /* Infineon's driver has joined when the request returns; others
             * (Espressif's) join after it, and choose the access point
             * themselves. */
            if (IS_ENABLED(CONFIG_WIFI_AIROC) && known && st.rssi < WIFI_GOOD_DBM && ++joins < WIFI_JOINS) {
                fc_port_log(FC_LOG_INFO, "WiFi: access point ..:%02x:%02x is weak (%d dBm), looking for a nearer one",
                            st.bssid[4], st.bssid[5], st.rssi);
                net_mgmt(NET_REQUEST_WIFI_DISCONNECT, s_sta, NULL, 0);
                k_sleep(K_SECONDS(1));
                t--;
                continue;
            }
            if (k_sem_take(&s_ip_up, K_SECONDS(30)) == 0) {
                known = net_mgmt(NET_REQUEST_WIFI_IFACE_STATUS, s_sta, &st, sizeof(st)) == 0;
                if (known) {
                    fc_port_log(FC_LOG_INFO, "WiFi: connected (channel %d, %d dBm, access point ..:%02x:%02x)",
                                st.channel, st.rssi, st.bssid[4], st.bssid[5]);
                } else {
                    fc_port_log(FC_LOG_INFO, "WiFi: connected");
                }
                return 0;
            }
            net_mgmt(NET_REQUEST_WIFI_DISCONNECT, s_sta, NULL, 0);
        }
        if (tries > 0 && t >= tries) {
            return -1;
        }
        fc_port_log(FC_LOG_WARN, "WiFi: no connection yet, trying again");
        k_sleep(K_SECONDS(5));
    }
}

/* ---- in the field ---------------------------------------------------------- */

#ifdef CONFIG_NET_DHCPV4_SERVER

/* With the access point up (fc_prov_config_t.join): the station interface
 * joins on its own, and on_wifi / on_ip say how it went. */
static int field_join(void *ctx, const char *ssid, const char *pass)
{
    if (s_ap == s_sta) {
        return -1; /* one interface: the access point must close first */
    }
    static struct wifi_connect_req_params p;
    p = params(ssid, pass);
    s_connect_status = -1;
    s_sta_ip = 0;
    net_mgmt(NET_REQUEST_WIFI_DISCONNECT, s_sta, NULL, 0);
    return net_mgmt(NET_REQUEST_WIFI_CONNECT, s_sta, &p, sizeof(p)) == 0 ? 0 : -1;
}

static fc_prov_join_t field_join_state(void *ctx)
{
    int st = s_connect_status;
    if (s_sta_ip) {
        return FC_PROV_JOINED;
    }
    if (st < 0 || st == WIFI_STATUS_CONN_SUCCESS) {
        return FC_PROV_JOINING;
    }
    net_mgmt(NET_REQUEST_WIFI_DISCONNECT, s_sta, NULL, 0);
    return st == WIFI_STATUS_CONN_WRONG_PASSWORD ? FC_PROV_BAD_PASSWORD : FC_PROV_NOT_FOUND;
}

static void scan(void)
{
    s_ap_count = 0;
    k_sem_reset(&s_scan_done);
    if (net_mgmt(NET_REQUEST_WIFI_SCAN, s_sta, NULL, 0) != 0 || k_sem_take(&s_scan_done, K_SECONDS(15)) != 0) {
        fc_port_log(FC_LOG_WARN, "WiFi: the scan failed; the installer will have to type the network's name");
    }
}

/* The access point, its address and its DHCP server: 0, or -1. */
static int ap_open(const char *pop, char name[16])
{
    struct net_linkaddr *mac = net_if_get_link_addr(s_ap);
    snprintf(name, 16, "FC-%02X%02X%02X", mac->addr[3], mac->addr[4], mac->addr[5]);
    struct wifi_connect_req_params p = params(name, pop);
    p.channel = 6;
    p.bandwidth = WIFI_FREQ_BANDWIDTH_20MHZ;
    struct net_in_addr addr = {.s4_addr = {192, 168, 4, 1}}, mask = {.s4_addr = {255, 255, 255, 0}},
                       pool = {.s4_addr = {192, 168, 4, 2}};
    /* The address, the gateway installers are given, and the DHCP server
     * first, then the access point, as Zephyr's AP+STA sample does. */
    net_if_ipv4_addr_add(s_ap, &addr, NET_ADDR_MANUAL, 0);
    net_if_ipv4_set_netmask_by_addr(s_ap, &addr, &mask);
    net_if_ipv4_set_gw(s_ap, &addr);
    int rc = net_dhcpv4_server_start(s_ap, &pool);
    if (rc != 0) {
        fc_port_log(FC_LOG_ERROR, "WiFi: cannot start the DHCP server (%d)", rc);
    }
    if (net_mgmt(NET_REQUEST_WIFI_AP_ENABLE, s_ap, &p, sizeof(p)) != 0) {
        fc_port_log(FC_LOG_ERROR, "WiFi: cannot open the provisioning access point");
        net_dhcpv4_server_stop(s_ap);
        net_if_ipv4_addr_rm(s_ap, &addr);
        return -1;
    }
    return 0;
}

static void ap_close(void)
{
    struct net_in_addr addr = {.s4_addr = {192, 168, 4, 1}}, none = {0};
    net_dhcpv4_server_stop(s_ap);
    net_if_ipv4_set_gw(s_ap, &none);
    net_if_ipv4_addr_rm(s_ap, &addr);
    net_mgmt(NET_REQUEST_WIFI_AP_DISABLE, s_ap, NULL, 0);
}

/* The credentials partition, by a hash of it: a network given in the field is
 * kept for these credentials only. Writing the partition again (a new proof of
 * possession, another installation) forgets it, and the device asks again. */
static char s_creds_id[17];

static void creds_id(const cJSON *creds)
{
    char *text = cJSON_PrintUnformatted(creds);
    unsigned char d[32];
    fc_sha256_t h;
    fc_sha256_start(&h);
    fc_sha256_update(&h, text, text != NULL ? strlen(text) : 0);
    fc_sha256_finish(&h, d);
    free(text);
    for (int i = 0; i < 8; i++) {
        snprintf(s_creds_id + 2 * i, 3, "%02x", d[i]);
    }
}

static void keep(const char *ssid, const char *pass)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "ssid", ssid);
    cJSON_AddStringToObject(o, "pass", pass);
    cJSON_AddStringToObject(o, "creds", s_creds_id);
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (text == NULL || fc_port_kv_set(KV_WIFI, text, strlen(text)) != 0) {
        fc_port_log(FC_LOG_WARN, "WiFi: cannot keep the network: it will be asked for again after a restart");
    }
    free(text);
}

static void field_provisioning(const char *pop)
{
    char ssid[33], pass[65];
    for (;;) {
        s_aps = calloc(FC_PROV_SCAN_MAX, sizeof(*s_aps));
        scan();
        char name[16];
        while (ap_open(pop, name) != 0) {
            k_sleep(K_SECONDS(10));
        }
        fc_port_log(FC_LOG_WARN,
                    "no WiFi network: field provisioning on access point %s (password: the proof of possession)", name);
        fc_prov_config_t cfg = {
            .pop = pop,
            .port = 80,
            .aps = s_aps,
            .ap_count = s_ap_count,
            .join = field_join,
            .join_state = field_join_state,
        };
        int joined = 0;
        int rc = fc_prov_serve(&cfg, ssid, pass, &joined);
        ap_close();
        free(s_aps);
        s_aps = NULL;
        s_ap_count = 0;
        if (rc != 0) {
            k_sleep(K_SECONDS(10));
            continue;
        }
        if (joined || join(ssid, pass, FIELD_TRIES) == 0) {
            keep(ssid, pass);
            fc_port_log(FC_LOG_INFO, "field provisioning: done");
            memset(pass, 0, sizeof(pass));
            return;
        }
        fc_port_log(FC_LOG_WARN, "WiFi: cannot join \"%s\": opening the access point again", ssid);
    }
}
#endif /* CONFIG_NET_DHCPV4_SERVER */

void fc_wifi_up(const cJSON *creds)
{
    s_sta = net_if_get_wifi_sta();
    s_ap = IS_ENABLED(CONFIG_ESP32_WIFI_AP_STA_MODE) ? net_if_get_wifi_sap() : s_sta;
    if (s_ap == NULL) {
        s_ap = s_sta;
    }
    net_mgmt_init_event_callback(&s_ip_cb, on_ip, NET_EVENT_IPV4_ADDR_ADD);
    net_mgmt_add_event_callback(&s_ip_cb);
    net_mgmt_init_event_callback(&s_wifi_cb, on_wifi,
                                 NET_EVENT_WIFI_SCAN_RESULT | NET_EVENT_WIFI_SCAN_DONE | NET_EVENT_WIFI_CONNECT_RESULT |
                                     NET_EVENT_WIFI_AP_STA_CONNECTED | NET_EVENT_WIFI_AP_STA_DISCONNECTED);
    net_mgmt_add_event_callback(&s_wifi_cb);

    const char *ssid = cJSON_GetStringValue(cJSON_GetObjectItem(creds, "wifi_ssid"));
    const char *pop = cJSON_GetStringValue(cJSON_GetObjectItem(creds, "prov_pop"));
    if (ssid != NULL && ssid[0] != '\0') {
        join(ssid, cJSON_GetStringValue(cJSON_GetObjectItem(creds, "wifi_pass")), 0);
        return;
    }
    char *kept = NULL;
    size_t len = 0;
    creds_id(creds);
    if (fc_port_kv_get(KV_WIFI, &kept, &len) == 0) {
        cJSON *w = cJSON_Parse(kept);
        free(kept);
        const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(w, "ssid"));
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(w, "creds"));
        if (s != NULL && (id == NULL || strcmp(id, s_creds_id) != 0)) {
            fc_port_log(FC_LOG_INFO, "WiFi: the credentials were written again: the network \"%s\" is forgotten", s);
            fc_port_kv_del(KV_WIFI);
            s = NULL;
        }
        if (s != NULL) {
            join(s, cJSON_GetStringValue(cJSON_GetObjectItem(w, "pass")), 0);
            cJSON_Delete(w);
            return;
        }
        cJSON_Delete(w);
    }
#ifdef CONFIG_NET_DHCPV4_SERVER
    if (pop != NULL && pop[0] != '\0') {
        field_provisioning(pop);
        return;
    }
#endif
    for (;;) {
        fc_port_log(FC_LOG_ERROR, IS_ENABLED(CONFIG_NET_DHCPV4_SERVER)
                                      ? "no WiFi network: provision it (provision.py --wifi-ssid, or --pop to set "
                                        "it in the field)"
                                      : "no WiFi network: provision it (provision.py --wifi-ssid); this board has "
                                        "no provisioning in the field");
        k_sleep(K_SECONDS(10));
    }
}
