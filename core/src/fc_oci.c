/*
 * Pulls a firmware artifact from an OCI registry into the inactive OTA slot.
 *
 *   ref = host[:port]/repo[:tag|@sha256:...]   (plain HTTP registry, PoC)
 *
 * The reference may name an image index (one manifest per chip, selected by
 * platform.architecture == fc_port_fw_platform()) or a single manifest. The
 * firmware is the manifest's first layer; it is streamed to the OTA slot and
 * its sha256 checked against the layer digest before the slot is committed.
 */
#include "fc_oci.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "fc_http.h"
#include "fc_port.h"
#include "mbedtls/sha256.h"

#define ACCEPT_MANIFESTS "application/vnd.oci.image.index.v1+json, application/vnd.oci.image.manifest.v1+json"
#define INDEX_TYPE       "application/vnd.oci.image.index.v1+json"
#define TIMEOUT_S        30

typedef struct {
    char host[128];
    int port;
    char repo[128];
    char ref[96]; /* tag or digest */
} oci_ref_t;

static int parse_ref(const char *s, oci_ref_t *r)
{
    memset(r, 0, sizeof(*r));
    const char *slash = strchr(s, '/');
    if (slash == NULL || (size_t)(slash - s) >= sizeof(r->host)) {
        return -1;
    }
    memcpy(r->host, s, (size_t)(slash - s));
    char *colon = strchr(r->host, ':');
    r->port = 80;
    if (colon) {
        *colon = '\0';
        r->port = atoi(colon + 1);
    }
    const char *path = slash + 1;
    const char *at = strchr(path, '@');
    const char *last_slash = strrchr(path, '/');
    const char *tag = strrchr(last_slash ? last_slash : path, ':');
    const char *sep = at ? at : tag;
    size_t repo_len = sep ? (size_t)(sep - path) : strlen(path);
    if (repo_len == 0 || repo_len >= sizeof(r->repo)) {
        return -1;
    }
    memcpy(r->repo, path, repo_len);
    snprintf(r->ref, sizeof(r->ref), "%s", sep ? sep + 1 : "latest");
    return 0;
}

static void sha256_hex(const unsigned char h[32], char out[72])
{
    strcpy(out, "sha256:");
    for (int i = 0; i < 32; i++) {
        snprintf(out + 7 + i * 2, 3, "%02x", h[i]);
    }
}

static void digest_of(const void *data, size_t len, char out[72])
{
    unsigned char h[32];
    mbedtls_sha256(data, len, h, 0);
    sha256_hex(h, out);
}

static fc_http_target_t target(const oci_ref_t *r, const char *accept)
{
    fc_http_target_t t = {.host = r->host, .port = r->port, .plain = 1, .accept = accept, .timeout_s = TIMEOUT_S};
    return t;
}

/* GET a manifest; checks its digest when fetched by digest. */
static cJSON *get_manifest(const oci_ref_t *r, const char *ref, char digest[72], char *err, size_t err_len)
{
    char path[320];
    snprintf(path, sizeof(path), "/v2/%s/manifests/%s", r->repo, ref);
    fc_http_target_t t = target(r, ACCEPT_MANIFESTS);
    fc_http_response_t resp;
    if (fc_http_request(&t, "GET", path, NULL, NULL, 0, &resp) != 0 || resp.status != 200) {
        snprintf(err, err_len, "registry %s:%d: GET manifest %s: HTTP %d", r->host, r->port, ref, resp.status);
        fc_http_free(&resp);
        return NULL;
    }
    digest_of(resp.body, resp.body_len, digest);
    if (strncmp(ref, "sha256:", 7) == 0 && strcmp(ref, digest) != 0) {
        snprintf(err, err_len, "manifest %s: digest mismatch", ref);
        fc_http_free(&resp);
        return NULL;
    }
    cJSON *m = cJSON_Parse(resp.body);
    fc_http_free(&resp);
    if (m == NULL) {
        snprintf(err, err_len, "manifest %s: invalid JSON", ref);
    }
    return m;
}

typedef struct {
    mbedtls_sha256_context sha;
    void *ota;
    long done, total;
    int last_pct;
} sink_t;

static int sink(const void *data, size_t len, void *ctx)
{
    sink_t *s = ctx;
    mbedtls_sha256_update(&s->sha, data, len);
    if (fc_port_ota_write(s->ota, data, len) != 0) {
        return -1;
    }
    s->done += (long)len;
    int pct = (int)(s->done * 100 / s->total);
    if (pct / 10 != s->last_pct / 10) {
        fc_port_log(FC_LOG_INFO, "firmware download %d%% (%ld/%ld)", pct, s->done, s->total);
        s->last_pct = pct;
    }
    return 0;
}

int fc_oci_fetch_firmware(const char *image, char *digest, size_t digest_len, char *err, size_t err_len)
{
    oci_ref_t r;
    if (parse_ref(image, &r) != 0) {
        snprintf(err, err_len, "invalid image reference %s", image);
        return -1;
    }
    const char *platform = fc_port_fw_platform();
    char top_digest[72], mdigest[72];
    cJSON *top = get_manifest(&r, r.ref, top_digest, err, err_len);
    if (top == NULL) {
        return -1;
    }
    snprintf(digest, digest_len, "%s", top_digest);

    cJSON *manifest = top;
    const char *mt = cJSON_GetStringValue(cJSON_GetObjectItem(top, "mediaType"));
    if (mt != NULL && strcmp(mt, INDEX_TYPE) == 0) {
        const char *pick = NULL;
        const cJSON *e;
        cJSON_ArrayForEach(e, cJSON_GetObjectItem(top, "manifests")) {
            const char *arch = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(e, "platform"), "architecture"));
            if (arch != NULL && strcmp(arch, platform) == 0) {
                pick = cJSON_GetStringValue(cJSON_GetObjectItem(e, "digest"));
                break;
            }
        }
        if (pick == NULL) {
            snprintf(err, err_len, "%s has no firmware for %s", image, platform);
            cJSON_Delete(top);
            return -1;
        }
        manifest = get_manifest(&r, pick, mdigest, err, err_len);
        if (manifest == NULL) {
            cJSON_Delete(top);
            return -1;
        }
    }

    const cJSON *layer = cJSON_GetArrayItem(cJSON_GetObjectItem(manifest, "layers"), 0);
    const char *blob = cJSON_GetStringValue(cJSON_GetObjectItem(layer, "digest"));
    const cJSON *size = cJSON_GetObjectItem(layer, "size");
    char blob_digest[72] = "";
    long blob_size = cJSON_IsNumber(size) ? (long)size->valuedouble : -1;
    if (blob != NULL) {
        snprintf(blob_digest, sizeof(blob_digest), "%s", blob);
    }
    if (manifest != top) {
        cJSON_Delete(manifest);
    }
    cJSON_Delete(top);
    if (blob_digest[0] == '\0' || blob_size <= 0) {
        snprintf(err, err_len, "%s: manifest without a firmware layer", image);
        return -1;
    }

    fc_port_log(FC_LOG_INFO, "firmware %s (%s): %ld bytes, %s", image, platform, blob_size, blob_digest);
    sink_t s = {.total = blob_size, .last_pct = -10};
    if (fc_port_ota_begin((size_t)blob_size, &s.ota) != 0) {
        snprintf(err, err_len, "cannot start OTA (%ld bytes)", blob_size);
        return -1;
    }
    mbedtls_sha256_init(&s.sha);
    mbedtls_sha256_starts(&s.sha, 0);

    char path[320];
    snprintf(path, sizeof(path), "/v2/%s/blobs/%s", r.repo, blob_digest);
    fc_http_target_t t = target(&r, "application/octet-stream");
    int status;
    long total;
    int rc = fc_http_get_stream(&t, path, sink, &s, &status, &total);
    unsigned char h[32];
    mbedtls_sha256_finish(&s.sha, h);
    mbedtls_sha256_free(&s.sha);
    char got[72];
    sha256_hex(h, got);

    if (rc != 0 || s.done != blob_size) {
        snprintf(err, err_len, "firmware download failed (HTTP %d, %ld/%ld bytes)", status, s.done, blob_size);
    } else if (strcmp(got, blob_digest) != 0) {
        snprintf(err, err_len, "firmware digest mismatch: got %.19s...", got);
        rc = -1;
    } else if (fc_port_ota_end(s.ota, 1) != 0) {
        snprintf(err, err_len, "firmware image rejected by the bootloader checks");
        return -1;
    } else {
        return 0;
    }
    fc_port_ota_end(s.ota, 0);
    return -1;
}
