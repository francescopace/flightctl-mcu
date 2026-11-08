/*
 * Pulls artifacts from an OCI registry: firmware (streamed into the inactive
 * OTA slot) and small layers such as WASM modules (into RAM).
 *
 *   ref = host[:port]/repo[:tag|@sha256:...]   (plain HTTP registry)
 *
 * The reference may name an image index (one manifest per chip, selected by
 * platform.architecture == fc_port_fw_platform()) or a single manifest. The
 * firmware is the manifest's first layer; it is streamed to the OTA slot and
 * its sha256 checked against the layer digest before the slot is committed.
 *
 * With a signing key set, the digest the reference resolves to must carry a
 * cosign signature by that key (tag sha256-<hex>.sig, simplesigning payload
 * naming the digest). Manifests below it are fetched by digest and checked,
 * and so is every blob, so the signature covers the whole image.
 */
#include "fc_oci.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "fc_http.h"
#include "fc_port.h"
#include "mbedtls/base64.h"
#include "mbedtls/pk.h"
#include "mbedtls/sha256.h"

#define ACCEPT_MANIFESTS "application/vnd.oci.image.index.v1+json, application/vnd.oci.image.manifest.v1+json"
#define INDEX_TYPE       "application/vnd.oci.image.index.v1+json"
#define TIMEOUT_S        30
#define SIG_ANNOTATION   "dev.cosignproject.cosign/signature"
#define MAX_SIG_PAYLOAD  4096
#define MAX_BLOB_SIZE    (64.0 * 1024 * 1024)

static const char *s_signing_key;

void fc_oci_set_signing_key(const char *pub_pem)
{
    s_signing_key = pub_pem && pub_pem[0] ? pub_pem : NULL;
}

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

/* Small blob into RAM, sha256 checked. Caller frees. */
static char *get_small_blob(const oci_ref_t *r, const char *digest, size_t *len)
{
    char path[320];
    snprintf(path, sizeof(path), "/v2/%s/blobs/%s", r->repo, digest);
    fc_http_target_t t = target(r, "application/octet-stream");
    fc_http_response_t resp;
    char got[72];
    if (fc_http_request(&t, "GET", path, NULL, NULL, 0, &resp) != 0 || resp.status != 200 ||
        resp.body_len > MAX_SIG_PAYLOAD) {
        fc_http_free(&resp);
        return NULL;
    }
    digest_of(resp.body, resp.body_len, got);
    if (strcmp(got, digest) != 0) {
        fc_http_free(&resp);
        return NULL;
    }
    *len = resp.body_len;
    return resp.body;
}

/* One cosign signature layer: the payload must name `digest` and the
 * signature over it must verify with the key. */
static int check_signature_layer(const oci_ref_t *r, mbedtls_pk_context *pk, const cJSON *layer, const char *digest)
{
    const char *pdigest = cJSON_GetStringValue(cJSON_GetObjectItem(layer, "digest"));
    const char *sig_b64 =
        cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(layer, "annotations"), SIG_ANNOTATION));
    if (pdigest == NULL || sig_b64 == NULL) {
        return -1;
    }
    unsigned char sig[160];
    size_t sig_len = 0;
    if (mbedtls_base64_decode(sig, sizeof(sig), &sig_len, (const unsigned char *)sig_b64, strlen(sig_b64)) != 0) {
        return -1;
    }
    size_t len = 0;
    char *payload = get_small_blob(r, pdigest, &len);
    if (payload == NULL) {
        return -1;
    }
    unsigned char hash[32];
    mbedtls_sha256((const unsigned char *)payload, len, hash, 0);
    int rc = mbedtls_pk_verify(pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sig_len);
    cJSON *p = rc == 0 ? cJSON_Parse(payload) : NULL;
    const char *named = cJSON_GetStringValue(cJSON_GetObjectItem(
        cJSON_GetObjectItem(cJSON_GetObjectItem(p, "critical"), "image"), "docker-manifest-digest"));
    rc = rc == 0 && named != NULL && strcmp(named, digest) == 0 ? 0 : -1;
    cJSON_Delete(p);
    free(payload);
    return rc;
}

/* Is the manifest or index with this digest signed with the signing key? */
static int verify_signature(const oci_ref_t *r, const char *image, const char *digest, char *err, size_t err_len)
{
    if (s_signing_key == NULL) {
        return 0;
    }
    char tag[80], sdigest[72];
    snprintf(tag, sizeof(tag), "sha256-%s.sig", digest + 7);
    cJSON *sigs = get_manifest(r, tag, sdigest, err, err_len);
    if (sigs == NULL) {
        snprintf(err, err_len, "%s is not signed", image);
        return -1;
    }
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    int ok = 0;
    if (mbedtls_pk_parse_public_key(&pk, (const unsigned char *)s_signing_key, strlen(s_signing_key) + 1) == 0) {
        const cJSON *l;
        int tried = 0;
        cJSON_ArrayForEach(l, cJSON_GetObjectItem(sigs, "layers")) {
            if (++tried > 8) {
                break; /* a few signatures at most, whatever the registry lists */
            }
            if (check_signature_layer(r, &pk, l, digest) == 0) {
                ok = 1;
                break;
            }
        }
    } else {
        fc_port_log(FC_LOG_ERROR, "the signing key in the firmware cannot be parsed");
    }
    mbedtls_pk_free(&pk);
    cJSON_Delete(sigs);
    if (!ok) {
        snprintf(err, err_len, "%s: no valid signature from the fleet key", image);
        return -1;
    }
    return 0;
}

/* Resolves `image` to one layer: through an index (picking the manifest whose
 * platform.architecture is `arch`) or a plain manifest, then the first layer
 * whose mediaType is `layer_type` (NULL: the first layer). */
static int resolve_layer(const char *image, const char *arch, const char *layer_type, oci_ref_t *r,
                         char blob_digest[72], long *blob_size, char *digest, size_t digest_len,
                         char *err, size_t err_len)
{
    if (parse_ref(image, r) != 0) {
        snprintf(err, err_len, "invalid image reference %s", image);
        return -1;
    }
    char top_digest[72], mdigest[72];
    cJSON *top = get_manifest(r, r->ref, top_digest, err, err_len);
    if (top == NULL) {
        return -1;
    }
    snprintf(digest, digest_len, "%s", top_digest);
    if (verify_signature(r, image, top_digest, err, err_len) != 0) {
        cJSON_Delete(top);
        return -1;
    }

    cJSON *manifest = top;
    const char *mt = cJSON_GetStringValue(cJSON_GetObjectItem(top, "mediaType"));
    if (mt != NULL && strcmp(mt, INDEX_TYPE) == 0) {
        const char *pick = NULL;
        const cJSON *e;
        cJSON_ArrayForEach(e, cJSON_GetObjectItem(top, "manifests")) {
            const char *a = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(e, "platform"), "architecture"));
            if (a != NULL && strcmp(a, arch) == 0) {
                pick = cJSON_GetStringValue(cJSON_GetObjectItem(e, "digest"));
                break;
            }
        }
        if (pick == NULL) {
            snprintf(err, err_len, "%s has nothing for %s", image, arch);
            cJSON_Delete(top);
            return -1;
        }
        manifest = get_manifest(r, pick, mdigest, err, err_len);
        if (manifest == NULL) {
            cJSON_Delete(top);
            return -1;
        }
    }

    const cJSON *layer = NULL, *l;
    cJSON_ArrayForEach(l, cJSON_GetObjectItem(manifest, "layers")) {
        const char *lt = cJSON_GetStringValue(cJSON_GetObjectItem(l, "mediaType"));
        if (layer_type == NULL || (lt != NULL && strcmp(lt, layer_type) == 0)) {
            layer = l;
            break;
        }
    }
    const char *blob = cJSON_GetStringValue(cJSON_GetObjectItem(layer, "digest"));
    const cJSON *size = cJSON_GetObjectItem(layer, "size");
    blob_digest[0] = '\0';
    /* Any size the device could hold; also keeps the cast to long defined
     * whatever number the manifest has (NaN, inf, 1e300). */
    double sz = cJSON_IsNumber(size) ? size->valuedouble : -1;
    *blob_size = sz >= 1 && sz <= MAX_BLOB_SIZE ? (long)sz : -1;
    if (blob != NULL) {
        snprintf(blob_digest, 72, "%s", blob);
    }
    if (manifest != top) {
        cJSON_Delete(manifest);
    }
    cJSON_Delete(top);
    if (blob_digest[0] == '\0' || *blob_size <= 0) {
        snprintf(err, err_len, "%s: no %s layer", image, layer_type ? layer_type : "");
        return -1;
    }
    return 0;
}

/* Streams a blob to `on_data`, checking its sha256. */
typedef struct {
    mbedtls_sha256_context sha;
    fc_http_data_cb next;
    void *next_ctx;
    long done, total;
    int last_pct;
    int progress;
} sink_t;

static int sink(const void *data, size_t len, void *ctx)
{
    sink_t *s = ctx;
    mbedtls_sha256_update(&s->sha, data, len);
    if (s->next(data, len, s->next_ctx) != 0) {
        return -1;
    }
    s->done += (long)len;
    int pct = (int)(s->done * 100 / s->total);
    if (s->progress && pct / 10 != s->last_pct / 10) {
        fc_port_log(FC_LOG_INFO, "download %d%% (%ld/%ld)", pct, s->done, s->total);
        s->last_pct = pct;
    }
    return 0;
}

static int fetch_blob(const oci_ref_t *r, const char *blob_digest, long blob_size, fc_http_data_cb on_data,
                      void *ctx, int progress, char *err, size_t err_len)
{
    sink_t s = {.next = on_data, .next_ctx = ctx, .total = blob_size, .last_pct = -10, .progress = progress};
    mbedtls_sha256_init(&s.sha);
    mbedtls_sha256_starts(&s.sha, 0);
    char path[320];
    snprintf(path, sizeof(path), "/v2/%s/blobs/%s", r->repo, blob_digest);
    fc_http_target_t t = target(r, "application/octet-stream");
    int status;
    long total;
    int rc = fc_http_get_stream(&t, path, sink, &s, &status, &total);
    unsigned char h[32];
    mbedtls_sha256_finish(&s.sha, h);
    mbedtls_sha256_free(&s.sha);
    char got[72];
    sha256_hex(h, got);
    if (rc != 0 || s.done != blob_size) {
        snprintf(err, err_len, "download failed (HTTP %d, %ld/%ld bytes)", status, s.done, blob_size);
        return -1;
    }
    if (strcmp(got, blob_digest) != 0) {
        snprintf(err, err_len, "digest mismatch: got %.19s...", got);
        return -1;
    }
    return 0;
}

static int ota_sink(const void *data, size_t len, void *ctx)
{
    return fc_port_ota_write(ctx, data, len);
}

int fc_oci_fetch_firmware(const char *image, char *digest, size_t digest_len, char *err, size_t err_len)
{
    oci_ref_t r;
    char blob_digest[72];
    long blob_size;
    const char *platform = fc_port_fw_platform();
    if (resolve_layer(image, platform, NULL, &r, blob_digest, &blob_size, digest, digest_len, err, err_len) != 0) {
        return -1;
    }
    fc_port_log(FC_LOG_INFO, "firmware %s (%s): %ld bytes, %s", image, platform, blob_size, blob_digest);
    void *ota;
    if (fc_port_ota_begin((size_t)blob_size, &ota) != 0) {
        snprintf(err, err_len, "cannot start OTA (%ld bytes)", blob_size);
        return -1;
    }
    char ferr[120];
    if (fetch_blob(&r, blob_digest, blob_size, ota_sink, ota, 1, ferr, sizeof(ferr)) != 0) {
        snprintf(err, err_len, "firmware %s", ferr);
        fc_port_ota_end(ota, 0);
        return -1;
    }
    if (fc_port_ota_end(ota, 1) != 0) {
        snprintf(err, err_len, "firmware image rejected by the bootloader checks");
        return -1;
    }
    return 0;
}

typedef struct {
    unsigned char *buf;
    size_t len, cap;
} mem_sink_t;

static int mem_sink(const void *data, size_t len, void *ctx)
{
    mem_sink_t *m = ctx;
    if (m->len + len > m->cap) {
        return -1;
    }
    memcpy(m->buf + m->len, data, len);
    m->len += len;
    return 0;
}

int fc_oci_fetch_layer(const char *image, const char *arch, const char *layer_type, size_t max_size,
                       unsigned char **out, size_t *out_len, char *digest, size_t digest_len,
                       char *err, size_t err_len)
{
    oci_ref_t r;
    char blob_digest[72];
    long blob_size;
    if (resolve_layer(image, arch, layer_type, &r, blob_digest, &blob_size, digest, digest_len, err, err_len) != 0) {
        return -1;
    }
    if ((size_t)blob_size > max_size) {
        snprintf(err, err_len, "%s: layer is %ld bytes (max %u)", image, blob_size, (unsigned)max_size);
        return -1;
    }
    mem_sink_t m = {.buf = malloc((size_t)blob_size), .cap = (size_t)blob_size};
    if (m.buf == NULL) {
        snprintf(err, err_len, "%s: out of memory (%ld bytes)", image, blob_size);
        return -1;
    }
    char ferr[120];
    if (fetch_blob(&r, blob_digest, blob_size, mem_sink, &m, 0, ferr, sizeof(ferr)) != 0) {
        snprintf(err, err_len, "%s: %s", image, ferr);
        free(m.buf);
        return -1;
    }
    *out = m.buf;
    *out_len = m.len;
    return 0;
}
