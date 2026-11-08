/*
 * Fuzzes image reference parsing and the manifest/index walk, including the
 * signature check: every registry answer is the fuzz input.
 */
#include "../../core/src/fc_oci.c"

static const uint8_t *s_data;
static size_t s_size;

int fc_http_request(const fc_http_target_t *t, const char *method, const char *path, const char *content_type,
                    const char *body, size_t body_len, fc_http_response_t *resp)
{
    resp->status = 200;
    resp->body = malloc(s_size + 1);
    memcpy(resp->body, s_data, s_size);
    resp->body[s_size] = '\0';
    resp->body_len = s_size;
    return 0;
}

int fc_http_get_stream(const fc_http_target_t *t, const char *path, fc_http_data_cb on_data, void *ctx, int *status,
                       long *total)
{
    *status = 200;
    *total = (long)s_size;
    return on_data(s_data, s_size, ctx);
}

void fc_http_free(fc_http_response_t *resp)
{
    free(resp->body);
    resp->body = NULL;
}

static const char KEY[] = "-----BEGIN PUBLIC KEY-----\n"
                          "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEj8005omGzvaDZ9hj0LHDns3zB7AJ\n"
                          "ZPHzp4MPoc4wGGI9EZ7b5NHtflY2w3OlRjesW68CC8CsOI23FCMtG0OhxQ==\n"
                          "-----END PUBLIC KEY-----\n";

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    s_data = data;
    s_size = size;
    char *ref = malloc(size + 1);
    memcpy(ref, data, size);
    ref[size] = '\0';
    oci_ref_t r;
    parse_ref(ref, &r);
    free(ref);

    char digest[80], err[160];
    unsigned char *out = NULL;
    size_t out_len = 0;
    fc_oci_set_signing_key(size > 0 && data[0] & 1 ? KEY : NULL);
    if (fc_oci_fetch_layer("registry:5000/flightctl-mcu/app:v1", "esp32c3", NULL, 4096, &out, &out_len, digest,
                           sizeof(digest), err, sizeof(err)) == 0) {
        free(out);
    }
    return 0;
}
