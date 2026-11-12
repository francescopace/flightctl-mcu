/*
 * Host "fake device": runs the same agent core as the firmware.
 *
 *   fc-host-agent --server agent-api.192.0.2.10.nip.io:8443 \
 *       --ca .secrets/ca.crt --enroll-cert .secrets/enroll.crt \
 *       --enroll-key .secrets/enroll.key [--signing-key .secrets/signing.pub] \
 *       [--data-dir DIR] [--label k=v]... [-v]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "fc_agent.h"
#include "wasm_export.h"

void fc_port_host_init(const char *data_dir, int debug);

/* Simulated hardware for WASM apps: a temperature that swings 30..46 C over
 * two minutes, a fixed 40 C threshold and an LED printed on change. */
static int32_t host_temp_mc(wasm_exec_env_t env)
{
    long t = time(NULL) % 120;
    return 30000 + (int32_t)(t < 60 ? t : 120 - t) * 266;
}

static int32_t host_threshold_mc(wasm_exec_env_t env)
{
    return 40000;
}

static void host_led(wasm_exec_env_t env, int32_t r, int32_t g, int32_t b)
{
    static int32_t last = -1;
    int32_t v = (r << 16) | (g << 8) | b;
    if (v != last) {
        fprintf(stderr, "LED r=%d g=%d b=%d\n", r, g, b);
        last = v;
    }
}

static NativeSymbol s_natives[] = {
    {"temp_mc", (void *)host_temp_mc, "()i", NULL},
    {"threshold_mc", (void *)host_threshold_mc, "()i", NULL},
    {"led", (void *)host_led, "(iii)", NULL},
};

static char *read_file(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "cannot open %s\n", path);
        exit(2);
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc((size_t)n + 1);
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) {
        exit(2);
    }
    buf[n] = '\0';
    fclose(f);
    return buf;
}

int main(int argc, char **argv)
{
    const char *server = NULL, *ca = NULL, *cert = NULL, *key = NULL, *signing = NULL;
    const char *data_dir = "build/host-data";
    const char *labels[33] = {0};
    int nlabels = 0, debug = 0;

    for (int i = 1; i < argc; i++) {
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (strcmp(argv[i], "--server") == 0 && v) {
            server = v; i++;
        } else if (strcmp(argv[i], "--ca") == 0 && v) {
            ca = v; i++;
        } else if (strcmp(argv[i], "--enroll-cert") == 0 && v) {
            cert = v; i++;
        } else if (strcmp(argv[i], "--enroll-key") == 0 && v) {
            key = v; i++;
        } else if (strcmp(argv[i], "--signing-key") == 0 && v) {
            signing = v; i++;
        } else if (strcmp(argv[i], "--data-dir") == 0 && v) {
            data_dir = v; i++;
        } else if (strcmp(argv[i], "--label") == 0 && v && nlabels < 30) {
            char *kv = strdup(v), *eq = strchr(kv, '=');
            if (eq == NULL) {
                fprintf(stderr, "--label expects key=value\n");
                return 2;
            }
            *eq = '\0';
            labels[nlabels++] = kv;
            labels[nlabels++] = eq + 1;
            i++;
        } else if (strcmp(argv[i], "-v") == 0) {
            debug = 1;
        } else {
            fprintf(stderr, "unknown argument: %s\n", argv[i]);
            return 2;
        }
    }
    if (!server || !ca || !cert || !key) {
        fprintf(stderr, "usage: %s --server host:port --ca F --enroll-cert F --enroll-key F "
                        "[--signing-key F] [--data-dir D] [--label k=v]... [-v]\n", argv[0]);
        return 2;
    }

    char *host = strdup(server);
    char *colon = strrchr(host, ':');
    int port = 443;
    if (colon) {
        *colon = '\0';
        port = atoi(colon + 1);
    }

    fc_port_host_init(data_dir, debug);
    fc_agent_config_t cfg = {
        .server_host = host,
        .server_port = port,
        .ca_pem = read_file(ca),
        .enrollment_cert_pem = read_file(cert),
        .enrollment_key_pem = read_file(key),
        .signing_pub_pem = signing ? read_file(signing) : NULL,
        .labels = labels,
        .agent_version = "flightctl-mcu/0.1.0",
        .status_interval_s = 60,
        .wasm_natives = s_natives,
        .wasm_natives_count = sizeof(s_natives) / sizeof(s_natives[0]),
    };
    return fc_agent_run(&cfg) == 0 ? 0 : 1;
}
