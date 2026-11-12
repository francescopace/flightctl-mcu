/*
 * OCI client: image references, index and manifest walk, digest checks, and
 * the cosign signature check, against an in-memory registry. Signatures are
 * made here with a key generated for the test.
 */
#include "../../core/src/fc_oci.c"

#include <stdlib.h>

#include "delta_vectors.h"
#include "port_stub.h"
#include "test.h"

/* ---- in-memory registry -------------------------------------------------- */

typedef struct {
    char path[400];
    char *body;
    size_t len;
} route_t;

static route_t s_routes[64];
static int s_nroutes;

static void reg_reset(void)
{
    for (int i = 0; i < s_nroutes; i++) {
        free(s_routes[i].body);
    }
    s_nroutes = 0;
}

static void serve(const char *path, const char *body, size_t len)
{
    route_t *r = &s_routes[s_nroutes++];
    snprintf(r->path, sizeof(r->path), "%s", path);
    r->body = malloc(len + 1);
    memcpy(r->body, body, len);
    r->body[len] = '\0';
    r->len = len;
}

static route_t *route(const char *path)
{
    for (int i = 0; i < s_nroutes; i++) {
        if (strcmp(s_routes[i].path, path) == 0) {
            return &s_routes[i];
        }
    }
    return NULL;
}

/* Credentials: when s_require is set, every request must carry it as its
 * Authorization header, or gets a 401 with s_challenge. A Bearer challenge
 * points at /token on the same host, which gives s_token for s_token_basic
 * (any Authorization when that is NULL). */
static const char *s_require, *s_challenge, *s_token, *s_token_basic;
static fc_http_target_t s_last;       /* the last request's target */
static char s_last_host[128], s_last_auth[256];
static int s_last_ca, s_with_auth_plain;

static void note(const fc_http_target_t *t)
{
    s_last = *t;
    snprintf(s_last_host, sizeof(s_last_host), "%s", t->host);
    snprintf(s_last_auth, sizeof(s_last_auth), "%s", t->authorization ? t->authorization : "");
    s_last_ca = t->ca_pem != NULL && strstr(t->ca_pem, "registry CA") != NULL;
    if (t->plain && t->authorization != NULL) {
        s_with_auth_plain++;
    }
}

static int denied(const fc_http_target_t *t)
{
    return s_require != NULL && (t->authorization == NULL || strcmp(t->authorization, s_require) != 0);
}

int fc_http_request(const fc_http_target_t *t, const char *method, const char *path, const char *content_type,
                    const char *body, size_t body_len, fc_http_response_t *resp)
{
    note(t);
    memset(resp, 0, sizeof(*resp));
    if (strncmp(path, "/token?", 7) == 0) {
        int ok = s_token != NULL && (s_token_basic == NULL || (t->authorization != NULL &&
                                                               strcmp(t->authorization, s_token_basic) == 0));
        char json[256];
        snprintf(json, sizeof(json), "{\"token\":\"%s\"}", ok ? s_token : "");
        resp->status = ok ? 200 : 401;
        resp->body = strdup(ok ? json : "");
        resp->body_len = strlen(resp->body);
        return 0;
    }
    if (denied(t)) {
        resp->status = 401;
        resp->body = strdup("");
        resp->www_authenticate = s_challenge ? strdup(s_challenge) : NULL;
        return 0;
    }
    route_t *r = route(path);
    resp->status = r ? 200 : 404;
    resp->body = malloc(r ? r->len + 1 : 1);
    resp->body_len = r ? r->len : 0;
    memcpy(resp->body, r ? r->body : "", resp->body_len + 1);
    return 0;
}

int fc_http_get_stream(const fc_http_target_t *t, const char *path, fc_http_data_cb on_data, void *ctx, int *status,
                       long *total)
{
    note(t);
    if (denied(t)) {
        *status = 401;
        *total = -1;
        return -1;
    }
    route_t *r = route(path);
    *status = r ? 200 : 404;
    *total = r ? (long)r->len : -1;
    return r && on_data(r->body, r->len, ctx) == 0 ? 0 : -1;
}

void fc_http_free(fc_http_response_t *resp)
{
    free(resp->body);
    resp->body = NULL;
    free(resp->www_authenticate);
    resp->www_authenticate = NULL;
}

fc_http_session_t *fc_http_session_new(void)
{
    return malloc(1);
}

void fc_http_session_close(fc_http_session_t *s) {}

void fc_http_session_free(fc_http_session_t *s)
{
    free(s);
}

/* Pushes a blob; returns its digest. */
static void push_blob(const char *repo, const char *data, size_t len, char digest[72])
{
    digest_of(data, len, digest);
    char path[400];
    snprintf(path, sizeof(path), "/v2/%s/blobs/%s", repo, digest);
    serve(path, data, len);
}

/* Pushes a manifest under its digest and, if given, a tag. */
static void push_manifest(const char *repo, const char *tag, const char *json, char digest[72])
{
    char path[400];
    digest_of(json, strlen(json), digest);
    snprintf(path, sizeof(path), "/v2/%s/manifests/%s", repo, digest);
    serve(path, json, strlen(json));
    if (tag) {
        snprintf(path, sizeof(path), "/v2/%s/manifests/%s", repo, tag);
        serve(path, json, strlen(json));
    }
}

/* An image with one layer: returns the manifest digest. */
static void push_image(const char *repo, const char *tag, const char *layer, const char *type, char mdigest[72])
{
    char ldigest[72], json[1024];
    push_blob(repo, layer, strlen(layer), ldigest);
    snprintf(json, sizeof(json),
             "{\"schemaVersion\":2,\"mediaType\":\"application/vnd.oci.image.manifest.v1+json\","
             "\"layers\":[{\"mediaType\":\"%s\",\"digest\":\"%s\",\"size\":%zu}]}",
             type, ldigest, strlen(layer));
    push_manifest(repo, tag, json, mdigest);
}

/* ---- signing --------------------------------------------------------------- */

static mbedtls_pk_context s_key, s_other;
static char s_pub[512];

/* Stores a cosign signature (tag sha256-<hex>.sig) for `digest`, signed with
 * `pk`, whose payload names `named` (normally the same digest). */
static void sign(const char *repo, const char *digest, const char *named, mbedtls_pk_context *pk)
{
    char payload[512];
    snprintf(payload, sizeof(payload),
             "{\"critical\":{\"identity\":{\"docker-reference\":\"r/%s\"},\"image\":{\"docker-manifest-digest\":"
             "\"%s\"},\"type\":\"cosign container image signature\"},\"optional\":null}",
             repo, named);
    char pdigest[72];
    push_blob(repo, payload, strlen(payload), pdigest);
    unsigned char hash[32], sig[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    size_t sig_len = 0, b64_len = 0;
    fc_sha256(payload, strlen(payload), hash);
    mbedtls_pk_sign(pk, MBEDTLS_MD_SHA256, hash, 32, sig, sizeof(sig), &sig_len);
    unsigned char b64[256];
    mbedtls_base64_encode(b64, sizeof(b64), &b64_len, sig, sig_len);
    char json[1024], tag[96], mdigest[72];
    snprintf(json, sizeof(json),
             "{\"schemaVersion\":2,\"layers\":[{\"mediaType\":\"application/vnd.dev.cosign.simplesigning.v1+json\","
             "\"digest\":\"%s\",\"size\":%zu,\"annotations\":{\"dev.cosignproject.cosign/signature\":\"%.*s\"}}]}",
             pdigest, strlen(payload), (int)b64_len, b64);
    snprintf(tag, sizeof(tag), "sha256-%s.sig", digest + 7);
    push_manifest(repo, tag, json, mdigest);
}

/* ---- tests ------------------------------------------------------------------ */

static int fetch(const char *image, char *err, size_t err_len, unsigned char **out, size_t *len)
{
    char digest[80];
    return fc_oci_fetch_layer(image, "test", NULL, 4096, out, len, digest, sizeof(digest), err, err_len);
}

static void references_are_parsed(void)
{
    oci_ref_t r;
    CHECK_INT(parse_ref("reg:5001/team/app:v1.2", &r), 0);
    CHECK_STR(r.host, "reg");
    CHECK_INT(r.port, 5001);
    CHECK_STR(r.repo, "team/app");
    CHECK_STR(r.ref, "v1.2");
    CHECK_INT(parse_ref("reg/app", &r), 0);
    CHECK_INT(r.port, 80);
    CHECK_STR(r.ref, "latest");
    CHECK_INT(parse_ref("reg:5001/app@sha256:abcd", &r), 0);
    CHECK_STR(r.ref, "sha256:abcd");
    CHECK_STR(r.repo, "app");
    CHECK_INT(parse_ref("no-slash", &r), -1);
}

static void a_layer_is_fetched_and_checked(void)
{
    reg_reset();
    fc_oci_set_signing_key(NULL);
    char md[72], err[160] = "";
    push_image("app", "v1", "WASM-BYTES", "application/vnd.wasm.content.layer.v1+wasm", md);
    unsigned char *out = NULL;
    size_t len = 0;
    CHECK_INT(fetch("reg:5001/app:v1", err, sizeof(err), &out, &len), 0);
    CHECK_INT(len, 10);
    CHECK(out && memcmp(out, "WASM-BYTES", 10) == 0);
    free(out);
}

static void an_index_entry_is_picked_by_architecture(void)
{
    reg_reset();
    fc_oci_set_signing_key(NULL);
    char m1[72], m2[72], idx[72], json[1024], err[160] = "";
    push_image("fw", NULL, "other-chip", "application/octet-stream", m1);
    push_image("fw", NULL, "this-chip", "application/octet-stream", m2);
    snprintf(json, sizeof(json),
             "{\"schemaVersion\":2,\"mediaType\":\"application/vnd.oci.image.index.v1+json\",\"manifests\":["
             "{\"digest\":\"%s\",\"platform\":{\"architecture\":\"other\"}},"
             "{\"digest\":\"%s\",\"platform\":{\"architecture\":\"test\"}}]}",
             m1, m2);
    push_manifest("fw", "v1", json, idx);
    unsigned char *out = NULL;
    size_t len = 0;
    CHECK_INT(fetch("reg:5001/fw:v1", err, sizeof(err), &out, &len), 0);
    CHECK(out && len == 9 && memcmp(out, "this-chip", 9) == 0);
    free(out);
    char digest[80];
    CHECK_INT(fc_oci_fetch_layer("reg:5001/fw:v1", "absent", NULL, 4096, &out, &len, digest, sizeof(digest), err,
                                 sizeof(err)),
              -1);
    CHECK_CONTAINS(err, "has nothing for absent");
}

static void tampered_content_is_refused(void)
{
    reg_reset();
    fc_oci_set_signing_key(NULL);
    char md[72], err[160] = "";
    push_image("app", "v1", "GOOD", "x", md);
    /* The registry now serves other bytes under the blob's digest. */
    for (int i = 0; i < s_nroutes; i++) {
        if (strstr(s_routes[i].path, "/blobs/")) {
            memcpy(s_routes[i].body, "EVIL", 4);
        }
    }
    unsigned char *out = NULL;
    size_t len = 0;
    CHECK_INT(fetch("reg:5001/app:v1", err, sizeof(err), &out, &len), -1);
    CHECK_CONTAINS(err, "digest mismatch");
}

static void absurd_or_too_large_layers_are_refused(void)
{
    reg_reset();
    fc_oci_set_signing_key(NULL);
    char md[72], err[160] = "";
    push_manifest("app", "huge",
                  "{\"layers\":[{\"mediaType\":\"x\",\"digest\":\"sha256:00\",\"size\":1e400}]}", md);
    unsigned char *out = NULL;
    size_t len = 0;
    CHECK_INT(fetch("reg:5001/app:huge", err, sizeof(err), &out, &len), -1);
    CHECK_CONTAINS(err, "no layer");
    push_image("app", "big", "0123456789", "x", md);
    char digest[80];
    CHECK_INT(fc_oci_fetch_layer("reg:5001/app:big", "test", NULL, 5, &out, &len, digest, sizeof(digest), err,
                                 sizeof(err)),
              -1);
    CHECK_CONTAINS(err, "max 5");
}

static void signatures_are_checked(void)
{
    reg_reset();
    fc_oci_set_signing_key(s_pub);
    char md[72], err[160] = "";
    unsigned char *out = NULL;
    size_t len = 0;

    push_image("app", "unsigned", "A", "x", md);
    CHECK_INT(fetch("reg:5001/app:unsigned", err, sizeof(err), &out, &len), -1);
    CHECK_CONTAINS(err, "is not signed");

    push_image("app", "signed", "B", "x", md);
    sign("app", md, md, &s_key);
    CHECK_INT(fetch("reg:5001/app:signed", err, sizeof(err), &out, &len), 0);
    free(out);

    push_image("app", "otherkey", "C", "x", md);
    sign("app", md, md, &s_other);
    CHECK_INT(fetch("reg:5001/app:otherkey", err, sizeof(err), &out, &len), -1);
    CHECK_CONTAINS(err, "no valid signature");

    /* A valid signature, but for another image. */
    char other[72];
    push_image("app", "victim", "D", "x", md);
    push_image("app", "decoy", "E", "x", other);
    sign("app", md, other, &s_key);
    CHECK_INT(fetch("reg:5001/app:victim", err, sizeof(err), &out, &len), -1);
    CHECK_CONTAINS(err, "no valid signature");
    fc_oci_set_signing_key(NULL);
}

static void firmware_needs_an_ota_slot(void)
{
    reg_reset();
    fc_oci_set_signing_key(NULL);
    char md[72], err[160] = "", digest[80];
    push_image("fw", "v1", "IMAGE", "application/vnd.espressif.app-image", md);
    /* The test port has no OTA: the download must not even start. */
    CHECK_INT(fc_oci_fetch_firmware("reg:5001/fw:v1", digest, sizeof(digest), err, sizeof(err)), -1);
    CHECK_CONTAINS(err, "cannot start OTA");
    CHECK_STR(digest, md);
}

/* ---- firmware deltas --------------------------------------------------------- */

#define DELTA_TYPE "application/vnd.flightctl-mcu.firmware.delta.v1+detools"

/* A firmware manifest: the whole image first (served only if `serve_full`),
 * then a delta from the image `from`. */
static void push_firmware(const char *tag, const unsigned char *full, size_t full_len, int serve_full,
                          const unsigned char *patch, size_t patch_len, const unsigned char *from, size_t from_len)
{
    char fd[72], pd[72], od[72], md[72], json[1536];
    digest_of(full, full_len, fd);
    if (serve_full) {
        push_blob("fw", (const char *)full, full_len, fd);
    }
    push_blob("fw", (const char *)patch, patch_len, pd);
    digest_of(from, from_len, od);
    snprintf(json, sizeof(json),
             "{\"schemaVersion\":2,\"mediaType\":\"application/vnd.oci.image.manifest.v1+json\",\"layers\":["
             "{\"mediaType\":\"application/vnd.espressif.app-image\",\"digest\":\"%s\",\"size\":%zu},"
             "{\"mediaType\":\"" DELTA_TYPE "\",\"digest\":\"%s\",\"size\":%zu,\"annotations\":{"
             "\"io.flightctl-mcu.delta.from\":\"%s\",\"io.flightctl-mcu.delta.from-size\":\"%zu\"}}]}",
             fd, full_len, pd, patch_len, od, from_len);
    push_manifest("fw", tag, json, md);
}

/* A device with an OTA slot, running `fw`. */
static void device_running(const unsigned char *fw, size_t len)
{
    reg_reset();
    stub_reset();
    fc_oci_set_signing_key(NULL);
    stub_ota_enabled = 1;
    stub_fw = fw;
    stub_fw_len = len;
    s_running_digest[0] = '\0';
    s_fw_delta[0] = '\0';
    s_fw_delta_loaded = 0;
}

static int update(char *err, size_t err_len)
{
    char digest[80];
    err[0] = '\0';
    return fc_oci_fetch_firmware("reg:5001/fw:v2", digest, sizeof(digest), err, err_len);
}

#define CHECK_SLOT(image)                                                   \
    do {                                                                    \
        CHECK_INT((int)stub_ota_len, (int)sizeof(image));                   \
        CHECK(memcmp(stub_ota, image, sizeof(image)) == 0);                 \
    } while (0)

static void firmware_comes_as_a_delta_from_the_running_image(void)
{
    char err[160];
    device_running(delta_old, sizeof(delta_old));
    /* The whole image is not even in the registry: only the delta can work. */
    push_firmware("v2", delta_new, sizeof(delta_new), 0, delta_patch, sizeof(delta_patch), delta_old,
                  sizeof(delta_old));
    CHECK_INT(update(err, sizeof(err)), 0);
    CHECK_STR(err, "");
    CHECK_SLOT(delta_new);
    CHECK_INT(stub_ota_commits, 1);
    CHECK_INT(stub_ota_aborts, 0);
    const char *why;
    CHECK_STR(fc_oci_delta_outcome(&why), "Applied");
    CHECK_STR(why, "");
    CHECK_STR(stub_kv("fw.delta"), "Applied");

    /* The firmware it rebooted into reads it back. */
    s_fw_delta[0] = '\0';
    s_fw_delta_loaded = 0;
    CHECK_STR(fc_oci_delta_outcome(&why), "Applied");
}

static void a_device_on_another_image_takes_the_whole_one(void)
{
    char err[160];
    /* Running something else, of the same size as the delta's base. */
    static unsigned char other[sizeof(delta_old)];
    memcpy(other, delta_old, sizeof(other));
    other[7] ^= 1;
    device_running(other, sizeof(other));
    push_firmware("v2", delta_new, sizeof(delta_new), 1, delta_patch, sizeof(delta_patch), delta_old,
                  sizeof(delta_old));
    CHECK_INT(update(err, sizeof(err)), 0);
    CHECK_SLOT(delta_new);
    CHECK_INT(stub_ota_commits, 1);
    CHECK_INT(stub_ota_aborts, 0); /* the delta was not tried */
    CHECK_STR(fc_oci_delta_outcome(NULL), "NotUsed");

    /* A port that cannot read its image, or an image shorter than the base. */
    device_running(NULL, 0);
    push_firmware("v2", delta_new, sizeof(delta_new), 1, delta_patch, sizeof(delta_patch), delta_old,
                  sizeof(delta_old));
    CHECK_INT(update(err, sizeof(err)), 0);
    CHECK_SLOT(delta_new);
    device_running(delta_old, 100);
    push_firmware("v2", delta_new, sizeof(delta_new), 1, delta_patch, sizeof(delta_patch), delta_old,
                  sizeof(delta_old));
    CHECK_INT(update(err, sizeof(err)), 0);
    CHECK_SLOT(delta_new);
    CHECK_INT(stub_ota_aborts, 0);
}

/* Whatever is wrong with a delta, the device ends up with the whole image,
 * checked as always, and the slot the delta touched is given up first. */
static void a_bad_delta_falls_back_to_the_whole_image(void)
{
    char err[160];
    /* It rebuilds an image, but not the one the manifest names. */
    static unsigned char wanted[sizeof(delta_new)];
    memcpy(wanted, delta_new, sizeof(wanted));
    wanted[sizeof(wanted) - 1] ^= 1;
    device_running(delta_old, sizeof(delta_old));
    push_firmware("v2", wanted, sizeof(wanted), 1, delta_patch, sizeof(delta_patch), delta_old, sizeof(delta_old));
    CHECK_INT(update(err, sizeof(err)), 0);
    CHECK_SLOT(wanted);
    CHECK_INT(stub_ota_aborts, 1);
    CHECK_INT(stub_ota_commits, 1);
    const char *why;
    CHECK_STR(fc_oci_delta_outcome(&why), "Fallback");
    CHECK_CONTAINS(why, "the rebuilt image is not sha256:");
    CHECK_CONTAINS(stub_kv("fw.delta"), "Fallback the rebuilt image");

    /* Corrupt, at every position in turn (its digest in the manifest is the
     * corrupt one's: only the decoder and the image digest stand in the way). */
    static unsigned char bad[sizeof(delta_patch)];
    for (size_t i = 0; i < sizeof(bad); i++) {
        memcpy(bad, delta_patch, sizeof(bad));
        bad[i] ^= 0x10;
        device_running(delta_old, sizeof(delta_old));
        push_firmware("v2", delta_new, sizeof(delta_new), 1, bad, sizeof(bad), delta_old, sizeof(delta_old));
        CHECK_INT(update(err, sizeof(err)), 0);
        CHECK_SLOT(delta_new);
        CHECK_INT(stub_ota_commits, 1);
    }

    /* Cut short. */
    device_running(delta_old, sizeof(delta_old));
    push_firmware("v2", delta_new, sizeof(delta_new), 1, delta_patch, sizeof(delta_patch) / 2, delta_old,
                  sizeof(delta_old));
    CHECK_INT(update(err, sizeof(err)), 0);
    CHECK_SLOT(delta_new);
    CHECK_INT(stub_ota_aborts, 1);

    /* Asking for a larger window than the device gives a decoder. */
    device_running(delta_old, sizeof(delta_old));
    push_firmware("v2", delta_new, sizeof(delta_new), 1, delta_patch_w13, sizeof(delta_patch_w13), delta_old,
                  sizeof(delta_old));
    CHECK_INT(update(err, sizeof(err)), 0);
    CHECK_SLOT(delta_new);
    CHECK_INT(stub_ota_aborts, 1);

    /* Not what its digest says: the download itself fails. */
    device_running(delta_old, sizeof(delta_old));
    push_firmware("v2", delta_new, sizeof(delta_new), 1, delta_patch, sizeof(delta_patch), delta_old,
                  sizeof(delta_old));
    char pd[72], path[400];
    digest_of(delta_patch, sizeof(delta_patch), pd);
    snprintf(path, sizeof(path), "/v2/fw/blobs/%s", pd);
    route(path)->body[sizeof(delta_patch) - 1] ^= 1;
    CHECK_INT(update(err, sizeof(err)), 0);
    CHECK_SLOT(delta_new);
    CHECK_INT(stub_ota_aborts, 1);
}

/* The image a delta rebuilds goes through the same checks at commit: what the
 * device refuses there, it would refuse as a whole image too. */
static void an_image_the_device_refuses_is_not_downloaded_again(void)
{
    char err[160];
    device_running(delta_old, sizeof(delta_old));
    push_firmware("v2", delta_new, sizeof(delta_new), 1, delta_patch, sizeof(delta_patch), delta_old,
                  sizeof(delta_old));
    stub_ota_end_result = FC_OTA_WRONG_VARIANT;
    CHECK_INT(update(err, sizeof(err)), FC_OCI_REFUSED);
    CHECK_CONTAINS(err, "another security variant");
    CHECK_INT(stub_ota_commits, 0);
    CHECK_INT(stub_ota_aborts, 0);
    CHECK_STR(fc_oci_delta_outcome(NULL), "");
}

/* A device updated by a firmware from before status.os.lastDelta finds how it
 * came in that firmware's words. */
static void the_outcome_written_by_an_older_firmware_is_read(void)
{
    const char *why;
    device_running(delta_old, sizeof(delta_old));
    fc_port_kv_set("fw.download", "delta 136 of 1085 KB", 20);
    CHECK_STR(fc_oci_delta_outcome(&why), "Applied");
    CHECK_STR(why, "");
    device_running(delta_old, sizeof(delta_old));
    fc_port_kv_set("fw.download", "full 1085 KB", 12);
    CHECK_STR(fc_oci_delta_outcome(&why), "NotUsed");

    /* The next update writes the new key and drops the old one. */
    push_firmware("v2", delta_new, sizeof(delta_new), 1, delta_patch, sizeof(delta_patch), delta_old,
                  sizeof(delta_old));
    char err[160];
    CHECK_INT(update(err, sizeof(err)), 0);
    CHECK_STR(stub_kv("fw.delta"), "Applied");
    CHECK(stub_kv("fw.download") == NULL);
}

/* ---- registry access ---------------------------------------------------------- */

#define CA "-----BEGIN CERTIFICATE-----\nregistry CA\n-----END CERTIFICATE-----\n"

static void reg_auth_reset(void)
{
    stub_reset();
    reg_reset();
    fc_oci_set_signing_key(NULL);
    s_require = s_challenge = s_token = s_token_basic = NULL;
    s_with_auth_plain = 0;
}

static void a_registry_is_reached_over_tls_when_the_device_has_its_ca(void)
{
    reg_auth_reset();
    char md[72], err[160] = "";
    unsigned char *out = NULL;
    size_t len = 0;
    push_image("app", "v1", "WASM", "application/vnd.wasm.content.layer.v1+wasm", md);
    CHECK_INT(fetch("reg:5001/app:v1", err, sizeof(err), &out, &len), 0);
    CHECK_INT(s_last.plain, 1); /* no CA: plain HTTP, as before */
    free(out);

    fc_port_write_file("/etc/containers/certs.d/reg:5001/ca.crt", CA, strlen(CA));
    CHECK_INT(fetch("reg:5001/app:v1", err, sizeof(err), &out, &len), 0);
    CHECK_INT(s_last.plain, 0);
    CHECK_INT(s_last_ca, 1);
    free(out);

    /* Without a port, TLS is on 443; the CA is keyed by the host alone. */
    fc_port_write_file("/etc/containers/certs.d/reg/ca.crt", CA, strlen(CA));
    CHECK_INT(fetch("reg/app:v1", err, sizeof(err), &out, &len), 0);
    CHECK_INT(s_last.port, 443);
    free(out);
    /* A CA for the host is for any port of it: the form a fleet can give
     * (Flight Control refuses ':' in a config file's path). */
    CHECK_INT(fetch("reg:7000/app:v1", err, sizeof(err), &out, &len), 0);
    CHECK_INT(s_last.plain, 0);
    CHECK_INT(s_last.port, 7000);
    free(out);
    CHECK_INT(fetch("other:5001/app:v1", err, sizeof(err), &out, &len), 0);
    CHECK_INT(s_last.plain, 1);
    free(out);
}

static void basic_credentials_are_sent_when_asked(void)
{
    reg_auth_reset();
    char md[72], err[160] = "";
    unsigned char *out = NULL;
    size_t len = 0;
    push_image("team/app", "v1", "WASM", "application/vnd.wasm.content.layer.v1+wasm", md);
    s_require = "Basic dTpw"; /* u:p */
    s_challenge = "Basic realm=\"registry\"";
    fc_port_write_file("/etc/containers/certs.d/reg:5443/ca.crt", CA, strlen(CA));
    CHECK(fetch("reg:5443/team/app:v1", err, sizeof(err), &out, &len) != 0);
    CHECK_CONTAINS(err, "no auth.json");

    const char *auth = "{\"auths\":{\"other:5443\":{\"auth\":\"bm8=\"},\"reg:5443\":{\"auth\":\"dTpw\"}}}";
    fc_port_write_file("/etc/containers/auth.json", auth, strlen(auth));
    err[0] = '\0';
    CHECK_INT(fetch("reg:5443/team/app:v1", err, sizeof(err), &out, &len), 0);
    CHECK_STR(s_last_auth, "Basic dTpw"); /* the blob too */
    free(out);

    /* A key for the repository wins over the registry's; username and
     * password are taken too. */
    auth = "{\"auths\":{\"https://reg:5443\":{\"auth\":\"bm8=\"},"
           "\"reg:5443/team\":{\"username\":\"u\",\"password\":\"p\"}}}";
    fc_port_write_file("/etc/ostree/auth.json", auth, strlen(auth));
    CHECK_INT(fetch("reg:5443/team/app:v1", err, sizeof(err), &out, &len), 0);
    free(out);
    /* ...but not for another repository that only starts the same. */
    push_image("teams/app", "v1", "WASM", "application/vnd.wasm.content.layer.v1+wasm", md);
    auth = "{\"auths\":{\"reg:5443/team\":{\"auth\":\"dTpw\"}}}";
    fc_port_write_file("/etc/ostree/auth.json", auth, strlen(auth));
    fc_port_write_file("/etc/containers/auth.json", "{}", 2);
    CHECK(fetch("reg:5443/teams/app:v1", err, sizeof(err), &out, &len) != 0);
    CHECK_INT(s_with_auth_plain, 0);
}

static void credentials_never_go_over_plain_http(void)
{
    reg_auth_reset();
    char md[72], err[200] = "";
    unsigned char *out = NULL;
    size_t len = 0;
    push_image("app", "v1", "WASM", "application/vnd.wasm.content.layer.v1+wasm", md);
    s_require = "Basic dTpzZWNyZXQ="; /* u:secret */
    s_challenge = "Basic realm=\"registry\"";
    const char *auth = "{\"auths\":{\"reg:5001\":{\"auth\":\"dTpzZWNyZXQ=\"}}}";
    fc_port_write_file("/etc/containers/auth.json", auth, strlen(auth));
    CHECK(fetch("reg:5001/app:v1", err, sizeof(err), &out, &len) != 0);
    CHECK_INT(s_with_auth_plain, 0);
    CHECK_CONTAINS(err, "/etc/containers/certs.d/reg/ca.crt");
    CHECK(strstr(err, "dTpzZWNyZXQ=") == NULL);

    /* With TLS, refused credentials are said so, without them. */
    fc_port_write_file("/etc/containers/certs.d/reg:5001/ca.crt", CA, strlen(CA));
    s_require = "Basic other";
    err[0] = '\0';
    CHECK(fetch("reg:5001/app:v1", err, sizeof(err), &out, &len) != 0);
    CHECK(strstr(err, "dTpzZWNyZXQ=") == NULL);
    CHECK_CONTAINS(err, "HTTP 401");
}

static void a_bearer_token_is_asked_for(void)
{
    reg_auth_reset();
    char md[72], err[200] = "";
    unsigned char *out = NULL;
    size_t len = 0;
    push_image("app", "v1", "WASM", "application/vnd.wasm.content.layer.v1+wasm", md);
    fc_port_write_file("/etc/containers/certs.d/reg:5443/ca.crt", CA, strlen(CA));
    s_require = "Bearer T0K3N";
    s_challenge = "Bearer realm=\"https://reg:5443/token\",service=\"reg\",scope=\"repository:app:pull\"";
    s_token = "T0K3N";
    /* A public repository: an anonymous token. */
    CHECK_INT(fetch("reg:5443/app:v1", err, sizeof(err), &out, &len), 0);
    CHECK_STR(s_last_auth, "Bearer T0K3N");
    free(out);
    /* A private one: the token for the credentials. */
    s_token_basic = "Basic dTpw";
    CHECK(fetch("reg:5443/app:v1", err, sizeof(err), &out, &len) != 0);
    CHECK_CONTAINS(err, "no credentials");
    const char *auth = "{\"auths\":{\"reg:5443\":{\"auth\":\"dTpw\"}}}";
    fc_port_write_file("/etc/containers/auth.json", auth, strlen(auth));
    CHECK_INT(fetch("reg:5443/app:v1", err, sizeof(err), &out, &len), 0);
    free(out);
    /* The token service elsewhere needs its own CA, and https. */
    s_challenge = "Bearer realm=\"https://auth.example:8443/token\",service=\"reg\"";
    CHECK(fetch("reg:5443/app:v1", err, sizeof(err), &out, &len) != 0);
    CHECK_CONTAINS(err, "certs.d/auth.example/ca.crt");
    fc_port_write_file("/etc/containers/certs.d/auth.example/ca.crt", CA, strlen(CA));
    CHECK_INT(fetch("reg:5443/app:v1", err, sizeof(err), &out, &len), 0);
    free(out);
    s_challenge = "Bearer realm=\"http://reg:5443/token\"";
    CHECK(fetch("reg:5443/app:v1", err, sizeof(err), &out, &len) != 0);
    CHECK_CONTAINS(err, "not on https");
    CHECK_INT(s_with_auth_plain, 0);
}

static void challenges_are_parsed(void)
{
    char v[64];
    const char *ch = "Bearer realm=\"https://r/t\",service=\"a \\\"b\\\"\", scope=repository:x:pull";
    CHECK_INT(challenge_param(ch, "realm", v, sizeof(v)), 1);
    CHECK_STR(v, "https://r/t");
    CHECK_INT(challenge_param(ch, "service", v, sizeof(v)), 1);
    CHECK_STR(v, "a \"b\"");
    CHECK_INT(challenge_param(ch, "SCOPE", v, sizeof(v)), 1);
    CHECK_STR(v, "repository:x:pull");
    CHECK_INT(challenge_param(ch, "error", v, sizeof(v)), 0);
    CHECK_INT(challenge_param("Basic", "realm", v, sizeof(v)), 0);
    char e[64];
    url_encode("repository:a/b:pull", e, sizeof(e));
    CHECK_STR(e, "repository%3Aa%2Fb%3Apull");
}

int main(void)
{
    stub_ec_key(&s_key);
    stub_ec_key(&s_other);
    mbedtls_pk_write_pubkey_pem(&s_key, (unsigned char *)s_pub, sizeof(s_pub));

    RUN(references_are_parsed);
    RUN(a_layer_is_fetched_and_checked);
    RUN(an_index_entry_is_picked_by_architecture);
    RUN(tampered_content_is_refused);
    RUN(absurd_or_too_large_layers_are_refused);
    RUN(signatures_are_checked);
    RUN(firmware_needs_an_ota_slot);
    RUN(firmware_comes_as_a_delta_from_the_running_image);
    RUN(a_device_on_another_image_takes_the_whole_one);
    RUN(a_bad_delta_falls_back_to_the_whole_image);
    RUN(an_image_the_device_refuses_is_not_downloaded_again);
    RUN(the_outcome_written_by_an_older_firmware_is_read);
    RUN(a_registry_is_reached_over_tls_when_the_device_has_its_ca);
    RUN(basic_credentials_are_sent_when_asked);
    RUN(credentials_never_go_over_plain_http);
    RUN(a_bearer_token_is_asked_for);
    RUN(challenges_are_parsed);
    reg_reset();
    mbedtls_pk_free(&s_key);
    mbedtls_pk_free(&s_other);
    return TEST_RESULT();
}
