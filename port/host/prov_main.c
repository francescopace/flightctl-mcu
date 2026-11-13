/*
 * The field provisioning server (core/src/fc_prov.c) on this machine, with a
 * pretend radio, for tests/e2e/prov-host.sh: Espressif's esp_prov talks to it
 * as it would to a device's access point.
 *   fc-prov-host --pop SECRET [--port N] [--later]
 * The radio sees two networks. "fc-test-site" takes the password
 * "fc-test-pass" (joined two seconds after it is applied), and refuses any
 * other; "fc-test-other" is out of reach. --later: a radio that cannot join
 * while its access point is up (the Pico W's), so the server returns as soon
 * as the network is applied. Prints the network it got and exits 0.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fc_crypto.h"
#include "fc_prov.h"

static const char *s_ssid, *s_pass;
static time_t s_since;

static int join(void *ctx, const char *ssid, const char *pass)
{
    s_ssid = ssid;
    s_pass = pass;
    s_since = time(NULL);
    return 0;
}

static fc_prov_join_t join_state(void *ctx)
{
    if (strcmp(s_ssid, "fc-test-site") != 0) {
        return time(NULL) - s_since < 2 ? FC_PROV_JOINING : FC_PROV_NOT_FOUND;
    }
    if (strcmp(s_pass, "fc-test-pass") != 0) {
        return time(NULL) - s_since < 2 ? FC_PROV_JOINING : FC_PROV_BAD_PASSWORD;
    }
    return time(NULL) - s_since < 2 ? FC_PROV_JOINING : FC_PROV_JOINED;
}

int main(int argc, char **argv)
{
    const char *pop = NULL;
    int port = 8090, later = 0;
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "--pop") == 0 && i + 1 < argc) {
            pop = argv[++i];
        } else if (strcmp(argv[i], "--port") == 0 && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (strcmp(argv[i], "--later") == 0) {
            later = 1;
        } else {
            pop = NULL;
            break;
        }
    }
    if (pop == NULL || fc_crypto_init() != 0) {
        fprintf(stderr, "usage: %s --pop SECRET [--port N] [--later]\n", argv[0]);
        return 2;
    }
    static const fc_prov_ap_t aps[] = {
        {"fc-test-site", {0x02, 0, 0, 0, 0, 1}, -48, 6, FC_PROV_AUTH_WPA2_PSK},
        {"fc-test-other", {0x02, 0, 0, 0, 0, 2}, -81, 11, FC_PROV_AUTH_WPA2_WPA3_PSK},
    };
    fc_prov_config_t cfg = {
        .pop = pop,
        .port = (uint16_t)port,
        .aps = aps,
        .ap_count = 2,
        .join = later ? NULL : join,
        .join_state = join_state,
    };
    char ssid[33], pass[65];
    int joined = 0;
    if (fc_prov_serve(&cfg, ssid, pass, &joined) != 0) {
        return 1;
    }
    printf("network \"%s\" (%s)\n", ssid, joined ? "joined" : "to join");
    fflush(stdout);
    return 0;
}
