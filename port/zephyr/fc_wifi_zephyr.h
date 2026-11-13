/* The WiFi network on Zephyr boards that have one (src/wifi.c). */
#pragma once

#include "cJSON.h"

/* Joins the network in the credentials (wifi_ssid, wifi_pass), else the one
 * kept from field provisioning, else asks an installer for one (prov_pop).
 * Returns once the device has an address. */
void fc_wifi_up(const cJSON *creds);
