/*
 * OCI client: image references, index and manifest walk, digest checks, and
 * the cosign signature check, against an in-memory registry. Signatures are
 * made here with a key generated for the test.
 */
#include "../../core/src/fc_oci.c"

#include <stdlib.h>

#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
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

int fc_http_request(const fc_http_target_t *t, const char *method, const char *path, const char *content_type,
                    const char *body, size_t body_len, fc_http_response_t *resp)
{
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
    route_t *r = route(path);
    *status = r ? 200 : 404;
    *total = r ? (long)r->len : -1;
    return r && on_data(r->body, r->len, ctx) == 0 ? 0 : -1;
}

void fc_http_free(fc_http_response_t *resp)
{
    free(resp->body);
    resp->body = NULL;
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
static mbedtls_ctr_drbg_context s_drbg;

static void gen_key(mbedtls_pk_context *pk)
{
    mbedtls_pk_init(pk);
    mbedtls_pk_setup(pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY));
    mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(*pk), mbedtls_ctr_drbg_random, &s_drbg);
}

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
    mbedtls_sha256((const unsigned char *)payload, strlen(payload), hash, 0);
    mbedtls_pk_sign(pk, MBEDTLS_MD_SHA256, hash, 32, sig, sizeof(sig), &sig_len, mbedtls_ctr_drbg_random, &s_drbg);
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
    s_fw_download[0] = '\0';
    s_fw_download_loaded = 0;
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
    CHECK_CONTAINS(fc_oci_firmware_download(), "delta 0 of 3 KB");
    CHECK_CONTAINS(stub_kv("fw.download"), "delta");
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
    CHECK_STR(fc_oci_firmware_download(), "full 3 KB");

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
    CHECK_STR(fc_oci_firmware_download(), "full 3 KB");

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
    CHECK_STR(fc_oci_firmware_download(), "");
}

int main(void)
{
    mbedtls_entropy_context entropy;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&s_drbg);
    mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &entropy, NULL, 0);
    psa_crypto_init();
    gen_key(&s_key);
    gen_key(&s_other);
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
    reg_reset();
    mbedtls_pk_free(&s_key);
    mbedtls_pk_free(&s_other);
    mbedtls_ctr_drbg_free(&s_drbg);
    mbedtls_entropy_free(&entropy);
    return TEST_RESULT();
}
