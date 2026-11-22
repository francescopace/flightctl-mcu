#include <esp_wifi.h>

#include "fc_wifi_zephyr.h"

/* The Zephyr station request selects 2.4 GHz. The Espressif dual-band
 * driver uses its own band setting when it scans for the configured SSID. */
int fc_wifi_c5_select_band(void)
{
    return esp_wifi_set_band_mode(WIFI_BAND_MODE_2G_ONLY);
}
