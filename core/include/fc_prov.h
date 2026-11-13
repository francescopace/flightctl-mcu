/*
 * WiFi provisioning in the field, as the ESP-IDF firmware does it: the device
 * opens an access point and an installer gives it the site's network with
 * Espressif's provisioning app (ESP SoftAP Prov) or esp_prov. This is the
 * part that does not depend on the radio: the HTTP endpoints, the session
 * (protocomm Security 1: X25519, authenticated by a proof of possession, then
 * AES-256-CTR) and the network messages. The port opens the access point
 * before and closes it after, and joins the network it is given.
 */
#pragma once

#include <stdint.h>

#define FC_PROV_SCAN_MAX 16

/* WifiAuthMode in network_constants.proto */
enum {
    FC_PROV_AUTH_OPEN = 0,
    FC_PROV_AUTH_WEP = 1,
    FC_PROV_AUTH_WPA_PSK = 2,
    FC_PROV_AUTH_WPA2_PSK = 3,
    FC_PROV_AUTH_WPA_WPA2_PSK = 4,
    FC_PROV_AUTH_WPA2_ENTERPRISE = 5,
    FC_PROV_AUTH_WPA3_PSK = 6,
    FC_PROV_AUTH_WPA2_WPA3_PSK = 7,
};

typedef struct {
    char ssid[33];
    uint8_t bssid[6];
    int8_t rssi;
    uint8_t channel;
    uint8_t auth; /* FC_PROV_AUTH_* */
} fc_prov_ap_t;

typedef enum {
    FC_PROV_JOINING,
    FC_PROV_JOINED,
    FC_PROV_BAD_PASSWORD,
    FC_PROV_NOT_FOUND,
} fc_prov_join_t;

typedef struct {
    const char *pop; /* the proof of possession the installer must know */
    uint16_t port;   /* 80: where the provisioning app looks */
    /* The networks the device saw before it opened its access point: what
     * the installer picks from. */
    const fc_prov_ap_t *aps;
    int ap_count;
    /* Starts joining the network the installer applied, with the access point
     * still up, so that the installer is told whether it worked: 0, or -1
     * when the radio cannot do both at once. Then fc_prov_serve returns as
     * soon as the network is applied, and the port tries it after closing the
     * access point. NULL: the same as -1. */
    int (*join)(void *ctx, const char *ssid, const char *pass);
    fc_prov_join_t (*join_state)(void *ctx); /* of the join started by `join` */
    void *ctx;
} fc_prov_config_t;

/* Serves the installer until it has given a network: 0 with that network in
 * ssid and pass, and *joined set when the device has joined it already (the
 * port's `join`); -1 when the server cannot listen. */
int fc_prov_serve(const fc_prov_config_t *cfg, char ssid[33], char pass[65], int *joined);
