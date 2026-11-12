/*
 * Fuzzes what the agent does with server answers: the rendered device spec
 * (config files, applications, OS image) and certificate signing requests.
 * Downloads always fail, so nothing is installed and nothing reboots.
 */
#include <unistd.h>
#define sleep(s) ((void)(s), 0u) /* retries must not stall the fuzzer */

#include "../../core/src/fc_agent.c"

#include <stdlib.h>

int fc_oci_fetch_firmware(const char *image, char *digest, size_t digest_len, char *err, size_t err_len)
{
    snprintf(err, err_len, "no registry in the fuzzer");
    return -1;
}

int fc_oci_fetch_layer(const char *image, const char *arch, const char *layer_type, size_t max_size,
                       unsigned char **out, size_t *out_len, char *digest, size_t digest_len, char *err,
                       size_t err_len)
{
    snprintf(err, err_len, "no registry in the fuzzer");
    return -1;
}

void fc_oci_set_signing_key(const char *pub_pem) {}

static const char *s_delta_outcome = "", *s_delta_reason = "";

const char *fc_oci_delta_outcome(const char **reason)
{
    *reason = s_delta_reason;
    return s_delta_outcome;
}

void fc_port_host_init(const char *data_dir, int debug);

static int on_config_file(const char *path, const char *data, size_t len, void *ctx)
{
    return len > 0 && data[0] == '!' ? -1 : 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static agent_t a;
    static fc_agent_config_t cfg = {.on_config_file = on_config_file};
    static int inited;
    if (!inited) {
        char dir[] = "/tmp/fc-fuzz-XXXXXX";
        fc_port_host_init(mkdtemp(dir), 0);
        fc_http_global_init();
        fc_apps_init(NULL, 0);
        a.cfg = &cfg;
        pthread_mutex_init(&a.lock, NULL);
        pthread_cond_init(&a.status_wake, NULL);
        snprintf(a.id.name, sizeof(a.id.name), "fuzzdevice");
        inited = 1;
    }
    if (size == 0) {
        return 0;
    }
    char *json = malloc(size);
    memcpy(json, data + 1, size - 1);
    json[size - 1] = '\0';
    if (data[0] & 1) {
        snprintf(a.csr_name, sizeof(a.csr_name), "fuzzdevice-0000");
        handle_csr(&a, json);
    } else {
        /* A spec may ask for decommissioning: that only marks it (the wipe
         * waits for a status accepted, and there is no server here). */
        a.decom = DECOM_NONE;
        apply_rendered(&a, json);
        cJSON_Delete(build_status(&a));
    }
    free(json);
    return 0;
}
