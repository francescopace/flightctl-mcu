/*
 * Pulls artifacts from an OCI registry: firmware (streamed into the inactive
 * OTA slot) and small layers such as WASM modules (into RAM).
 *
 *   ref = host[:port]/repo[:tag|@sha256:...]
 *
 * The registry is reached over TLS when the fleet gives the device its CA,
 * and with the credentials of an auth.json when it asks for them (see
 * "registry access" below); otherwise over plain HTTP.
 *
 * The reference may name an image index (one manifest per chip, selected by
 * platform.architecture == fc_port_fw_platform()) or a single manifest. The
 * firmware is the manifest's first layer; it is streamed to the OTA slot and
 * its sha256 checked against the layer digest before the slot is committed.
 *
 * A manifest may also carry deltas: layers of type FW_DELTA_TYPE, each a
 * patch (fc_delta.h) from the image named in its annotations to the first
 * layer. When one starts from the image that is running, the device downloads
 * it instead and rebuilds the new image, which must have the first layer's
 * digest; if anything goes wrong it downloads the first layer.
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
#include <strings.h>

#include "cJSON.h"
#include "fc_crypto.h"
#include "fc_delta.h"
#include "fc_http.h"
#include "fc_port.h"
#include "mbedtls/base64.h"
#include "mbedtls/pk.h"

#define ACCEPT_MANIFESTS "application/vnd.oci.image.index.v1+json, application/vnd.oci.image.manifest.v1+json"
#define INDEX_TYPE       "application/vnd.oci.image.index.v1+json"
#define TIMEOUT_S        30
#define SIG_ANNOTATION   "dev.cosignproject.cosign/signature"
#define MAX_SIG_PAYLOAD  4096
#define MAX_BLOB_SIZE    (64.0 * 1024 * 1024)
#define FW_DELTA_TYPE    "application/vnd.flightctl-mcu.firmware.delta.v1+detools"
#define DELTA_FROM       "io.flightctl-mcu.delta.from"      /* digest of the image the patch starts from */
#define DELTA_FROM_SIZE  "io.flightctl-mcu.delta.from-size" /* and its size in bytes */
#define MAX_DELTAS       8                                  /* deltas looked at in one manifest */
#define KV_FW_DELTA      "fw.delta"
#define KV_FW_DOWNLOAD   "fw.download" /* before: "delta 26 of 1085 KB", "full 1085 KB" */

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
    char hostport[134]; /* host[:port] as the reference has it: certs.d and auth.json are keyed by it */
    int port_given;
    /* Registry access, for one pull (reg_open, reg_close). */
    char *ca_pem;        /* the registry's CA: TLS; NULL: plain HTTP */
    char *basic;         /* base64 of user:password, from an auth.json; NULL: none */
    char *authorization; /* the header sent, once the registry has asked */
    char *challenge;     /* the registry's last WWW-Authenticate */
    fc_http_session_t *sess;
} oci_ref_t;

static int parse_ref(const char *s, oci_ref_t *r)
{
    memset(r, 0, sizeof(*r));
    const char *slash = strchr(s, '/');
    if (slash == NULL || (size_t)(slash - s) >= sizeof(r->host)) {
        return -1;
    }
    memcpy(r->host, s, (size_t)(slash - s));
    memcpy(r->hostport, s, (size_t)(slash - s));
    char *colon = strchr(r->host, ':');
    r->port = 80;
    if (colon) {
        *colon = '\0';
        r->port = atoi(colon + 1);
        r->port_given = 1;
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

/* The digest of what was hashed, or "" if it could not be computed: no
 * digest is equal to that. */
static int finish_hex(fc_sha256_t *s, char out[72])
{
    unsigned char h[32];
    if (fc_sha256_finish(s, h) != 0) {
        out[0] = '\0';
        return -1;
    }
    sha256_hex(h, out);
    return 0;
}

static int digest_of(const void *data, size_t len, char out[72])
{
    fc_sha256_t s;
    fc_sha256_start(&s);
    fc_sha256_update(&s, data, len);
    return finish_hex(&s, out);
}

/* ---- registry access ------------------------------------------------------
 * As containers have it on Linux, and with the files the Linux agent reads,
 * given to the device as config files in the fleet:
 *  - TLS when the device has the registry's CA, in
 *    /etc/containers/certs.d/<host:port>/ca.crt or, for any port of that
 *    host, certs.d/<host>/ca.crt (Flight Control refuses ':' in the path of
 *    a config file); plain HTTP otherwise.
 *  - Credentials from the first auth.json of AUTH_FILES with an entry for
 *    the registry (podman's format: {"auths":{"<host:port>":{"auth":
 *    "<base64 of user:password>"}}}; a key may also name a repository).
 *    They are sent only when the registry asks for them, as Basic or for a
 *    Bearer token, and never over plain HTTP.
 * Nothing about the credentials goes into errors or the log. */
#define CERTS_DIR "/etc/containers/certs.d/"
static const char *const AUTH_FILES[] = {"/etc/ostree/auth.json", "/etc/containers/auth.json",
                                         "/root/.config/containers/auth.json"};
#define MAX_TOKEN 8192

/* The CA for host[:port]: certs.d/<host:port>/ca.crt, else certs.d/<host>/ca.crt. */
static char *read_ca(const char *hostport)
{
    char path[200], *pem = NULL;
    size_t len = 0;
    snprintf(path, sizeof(path), CERTS_DIR "%s/ca.crt", hostport);
    if (fc_port_read_file(path, &pem, &len) == 0) {
        return pem;
    }
    const char *colon = strchr(hostport, ':');
    if (colon == NULL) {
        return NULL;
    }
    snprintf(path, sizeof(path), CERTS_DIR "%.*s/ca.crt", (int)(colon - hostport), hostport);
    return fc_port_read_file(path, &pem, &len) == 0 ? pem : NULL;
}

/* An auth.json key, without a scheme or a trailing slash, is for this
 * registry and repository: host:port, or host:port/<a prefix of the repo
 * ending at a slash>. Returns its length, 0 if it is not. */
static size_t auth_key_matches(const char *key, const oci_ref_t *r)
{
    if (strncmp(key, "https://", 8) == 0) {
        key += 8;
    } else if (strncmp(key, "http://", 7) == 0) {
        key += 7;
    }
    size_t klen = strlen(key);
    while (klen > 0 && key[klen - 1] == '/') {
        klen--;
    }
    size_t hl = strlen(r->hostport);
    if (klen < hl || strncmp(key, r->hostport, hl) != 0) {
        return 0;
    }
    if (klen == hl) {
        return klen;
    }
    if (key[hl] != '/') {
        return 0;
    }
    const char *ns = key + hl + 1;
    size_t nl = klen - hl - 1, rl = strlen(r->repo);
    return nl <= rl && strncmp(r->repo, ns, nl) == 0 && (nl == rl || r->repo[nl] == '/') ? klen : 0;
}

/* The credentials for `r` in auth.json `json`, as base64 user:password. */
static char *auth_from(const char *json, const oci_ref_t *r)
{
    cJSON *doc = cJSON_Parse(json);
    const cJSON *e, *best = NULL;
    size_t best_len = 0;
    cJSON_ArrayForEach(e, cJSON_GetObjectItem(doc, "auths")) {
        size_t n = e->string != NULL ? auth_key_matches(e->string, r) : 0;
        if (n > best_len) {
            best = e;
            best_len = n;
        }
    }
    char *out = NULL;
    const char *auth = cJSON_GetStringValue(cJSON_GetObjectItem(best, "auth"));
    const char *user = cJSON_GetStringValue(cJSON_GetObjectItem(best, "username"));
    const char *pass = cJSON_GetStringValue(cJSON_GetObjectItem(best, "password"));
    if (auth != NULL && auth[0]) {
        out = strdup(auth);
    } else if (user != NULL && pass != NULL) {
        size_t ul = strlen(user), pl = strlen(pass), olen = 0;
        unsigned char *plain = malloc(ul + pl + 2);
        size_t cap = ((ul + pl + 1 + 2) / 3) * 4 + 1;
        out = malloc(cap);
        if (plain != NULL && out != NULL) {
            memcpy(plain, user, ul);
            plain[ul] = ':';
            memcpy(plain + ul + 1, pass, pl);
            if (mbedtls_base64_encode((unsigned char *)out, cap, &olen, plain, ul + pl + 1) != 0) {
                free(out);
                out = NULL;
            }
            memset(plain, 0, ul + pl + 1);
        } else {
            free(out);
            out = NULL;
        }
        free(plain);
    }
    cJSON_Delete(doc);
    return out;
}

static void reg_close(oci_ref_t *r)
{
    free(r->ca_pem);
    if (r->basic != NULL) {
        memset(r->basic, 0, strlen(r->basic));
    }
    free(r->basic);
    free(r->authorization);
    free(r->challenge);
    fc_http_session_free(r->sess);
    r->ca_pem = r->basic = r->authorization = r->challenge = NULL;
    r->sess = NULL;
}

/* Parses `image` and gets what reaching its registry takes. */
static int reg_open(const char *image, oci_ref_t *r, char *err, size_t err_len)
{
    if (parse_ref(image, r) != 0) {
        snprintf(err, err_len, "invalid image reference %s", image);
        return -1;
    }
    r->ca_pem = read_ca(r->hostport);
    if (r->ca_pem != NULL && !r->port_given) {
        r->port = 443;
    }
    for (size_t i = 0; i < sizeof(AUTH_FILES) / sizeof(AUTH_FILES[0]) && r->basic == NULL; i++) {
        char *json = NULL;
        size_t len = 0;
        if (fc_port_read_file(AUTH_FILES[i], &json, &len) == 0) {
            r->basic = auth_from(json, r);
            memset(json, 0, len);
            free(json);
        }
    }
    r->sess = fc_http_session_new();
    return 0;
}

static fc_http_target_t target(const oci_ref_t *r, const char *accept)
{
    fc_http_target_t t = {.host = r->host,
                          .port = r->port,
                          .plain = r->ca_pem == NULL,
                          .ca_pem = r->ca_pem,
                          .authorization = r->authorization,
                          .accept = accept,
                          .timeout_s = TIMEOUT_S,
                          .session = r->sess};
    return t;
}

/* A parameter of a WWW-Authenticate challenge: name="value" or name=token. */
static int challenge_param(const char *ch, const char *name, char *out, size_t len)
{
    size_t nl = strlen(name);
    const char *p = strchr(ch, ' '); /* past the scheme */
    while (p != NULL && *p) {
        while (*p == ' ' || *p == ',' || *p == '\t') {
            p++;
        }
        const char *eq = strchr(p, '=');
        if (eq == NULL) {
            return 0;
        }
        int match = (size_t)(eq - p) == nl && strncasecmp(p, name, nl) == 0;
        p = eq + 1;
        size_t n = 0;
        if (*p == '"') {
            for (p++; *p && *p != '"'; p++) {
                if (*p == '\\' && p[1]) {
                    p++;
                }
                if (match && n + 1 < len) {
                    out[n++] = *p;
                }
            }
            if (*p == '"') {
                p++;
            }
        } else {
            for (; *p && *p != ','; p++) {
                if (match && n + 1 < len) {
                    out[n++] = *p;
                }
            }
        }
        if (match) {
            out[n] = '\0';
            return 1;
        }
    }
    return 0;
}

static void url_encode(const char *in, char *out, size_t len)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t n = 0;
    for (; *in && n + 4 < len; in++) {
        unsigned char ch = (unsigned char)*in;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' ||
            ch == '_' || ch == '.' || ch == '~') {
            out[n++] = (char)ch;
        } else {
            out[n++] = '%';
            out[n++] = hex[ch >> 4];
            out[n++] = hex[ch & 15];
        }
    }
    out[n] = '\0';
}

static char *header_value(const char *scheme, const char *cred)
{
    size_t n = strlen(scheme) + 1 + strlen(cred) + 1;
    char *h = malloc(n);
    if (h != NULL) {
        snprintf(h, n, "%s %s", scheme, cred);
    }
    return h;
}

/* A Bearer token from the challenge's realm, with the credentials if any
 * (anonymous otherwise, as for a public repository). */
static int bearer_token(oci_ref_t *r, char *err, size_t err_len)
{
    char realm[256], service[128] = "", scope[192] = "";
    if (!challenge_param(r->challenge, "realm", realm, sizeof(realm))) {
        snprintf(err, err_len, "registry %s: a token is asked for, and no realm given", r->hostport);
        return -1;
    }
    challenge_param(r->challenge, "service", service, sizeof(service));
    if (!challenge_param(r->challenge, "scope", scope, sizeof(scope))) {
        snprintf(scope, sizeof(scope), "repository:%s:pull", r->repo);
    }
    if (strncmp(realm, "https://", 8) != 0) {
        snprintf(err, err_len, "registry %s: its token service is not on https", r->hostport);
        return -1;
    }
    char thost[134], hp[134];
    const char *h = realm + 8, *slash = strchr(h, '/');
    size_t hl = slash ? (size_t)(slash - h) : strlen(h);
    if (hl == 0 || hl >= sizeof(hp)) {
        snprintf(err, err_len, "registry %s: bad token realm", r->hostport);
        return -1;
    }
    memcpy(hp, h, hl);
    hp[hl] = '\0';
    snprintf(thost, sizeof(thost), "%s", hp);
    int tport = 443;
    char *colon = strchr(thost, ':');
    if (colon != NULL) {
        *colon = '\0';
        tport = atoi(colon + 1);
    }
    int same = strcmp(hp, r->hostport) == 0 || (strcmp(thost, r->host) == 0 && tport == r->port);
    char *ca = same ? r->ca_pem : read_ca(hp);
    if (ca == NULL) {
        snprintf(err, err_len, "registry %s: no CA for its token service (" CERTS_DIR "%s/ca.crt)", r->hostport,
                 thost);
        return -1;
    }
    char path[700], es[200], esc[300];
    url_encode(service, es, sizeof(es));
    url_encode(scope, esc, sizeof(esc));
    const char *rpath = slash ? slash : "/";
    snprintf(path, sizeof(path), "%s%sservice=%s&scope=%s", rpath, strchr(rpath, '?') ? "&" : "?", es, esc);
    char *basic = r->basic != NULL ? header_value("Basic", r->basic) : NULL;
    fc_http_target_t t = {.host = thost,
                          .port = tport,
                          .ca_pem = ca,
                          .authorization = basic,
                          .accept = "application/json",
                          .timeout_s = TIMEOUT_S,
                          .session = same ? r->sess : NULL};
    fc_http_response_t resp;
    int rc = fc_http_request(&t, "GET", path, NULL, NULL, 0, &resp);
    if (basic != NULL) {
        memset(basic, 0, strlen(basic));
        free(basic);
    }
    if (!same) {
        free(ca);
    }
    if (rc != 0 || resp.status != 200) {
        snprintf(err, err_len, "registry %s: token service %s: HTTP %d%s", r->hostport, hp, resp.status,
                 resp.status == 401 ? (r->basic ? " (credentials refused)" : " (no credentials for it)") : "");
        fc_http_free(&resp);
        return -1;
    }
    cJSON *j = cJSON_Parse(resp.body);
    fc_http_free(&resp);
    const char *tok = cJSON_GetStringValue(cJSON_GetObjectItem(j, "token"));
    if (tok == NULL) {
        tok = cJSON_GetStringValue(cJSON_GetObjectItem(j, "access_token"));
    }
    if (tok == NULL || tok[0] == '\0' || strlen(tok) > MAX_TOKEN) {
        snprintf(err, err_len, "registry %s: token service %s gave no token", r->hostport, hp);
        cJSON_Delete(j);
        return -1;
    }
    r->authorization = header_value("Bearer", tok);
    cJSON_Delete(j);
    return r->authorization != NULL ? 0 : -1;
}

/* The registry answered 401 with r->challenge: get what it asks for. */
static int reg_authenticate(oci_ref_t *r, char *err, size_t err_len)
{
    free(r->authorization);
    r->authorization = NULL;
    if (r->challenge == NULL) {
        snprintf(err, err_len, "registry %s: HTTP 401 without a challenge", r->hostport);
        return -1;
    }
    if (r->ca_pem == NULL) {
        snprintf(err, err_len,
                 "registry %s asks for credentials: they go over TLS only, and the device has no CA for it "
                 "(" CERTS_DIR "%s/ca.crt)",
                 r->hostport, r->host);
        return -1;
    }
    if (strncasecmp(r->challenge, "Bearer", 6) == 0) {
        return bearer_token(r, err, err_len);
    }
    if (strncasecmp(r->challenge, "Basic", 5) != 0) {
        snprintf(err, err_len, "registry %s: authentication %.16s is not supported", r->hostport, r->challenge);
        return -1;
    }
    if (r->basic == NULL) {
        snprintf(err, err_len, "registry %s asks for credentials, and no auth.json has any for it", r->hostport);
        return -1;
    }
    r->authorization = header_value("Basic", r->basic);
    return r->authorization != NULL ? 0 : -1;
}

/* GET, authenticating once if the registry asks. 1: failed for a reason
 * written in `err`; otherwise as fc_http_request. */
static int reg_get(oci_ref_t *r, const char *path, const char *accept, fc_http_response_t *resp, char *err,
                   size_t err_len)
{
    for (int attempt = 0;; attempt++) {
        fc_http_target_t t = target(r, accept);
        int rc = fc_http_request(&t, "GET", path, NULL, NULL, 0, resp);
        if (rc != 0 || resp->status != 401 || attempt > 0) {
            return rc;
        }
        free(r->challenge);
        r->challenge = resp->www_authenticate;
        resp->www_authenticate = NULL;
        fc_http_free(resp);
        if (reg_authenticate(r, err, err_len) != 0) {
            resp->status = 401;
            return 1;
        }
    }
}

/* GET a manifest; checks its digest when fetched by digest. */
static cJSON *get_manifest(oci_ref_t *r, const char *ref, char digest[72], char *err, size_t err_len)
{
    char path[320];
    snprintf(path, sizeof(path), "/v2/%s/manifests/%s", r->repo, ref);
    fc_http_response_t resp;
    int rc = reg_get(r, path, ACCEPT_MANIFESTS, &resp, err, err_len);
    if (rc != 0 || resp.status != 200) {
        if (rc != 1) {
            snprintf(err, err_len, "registry %s: GET manifest %s: HTTP %d", r->hostport, ref, resp.status);
        }
        fc_http_free(&resp);
        return NULL;
    }
    if (digest_of(resp.body, resp.body_len, digest) != 0 ||
        (strncmp(ref, "sha256:", 7) == 0 && strcmp(ref, digest) != 0)) {
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
static char *get_small_blob(oci_ref_t *r, const char *digest, size_t *len)
{
    char path[320], err[160];
    snprintf(path, sizeof(path), "/v2/%s/blobs/%s", r->repo, digest);
    fc_http_response_t resp;
    char got[72];
    if (reg_get(r, path, "application/octet-stream", &resp, err, sizeof(err)) != 0 || resp.status != 200 ||
        resp.body_len > MAX_SIG_PAYLOAD) {
        fc_http_free(&resp);
        return NULL;
    }
    if (digest_of(resp.body, resp.body_len, got) != 0 || strcmp(got, digest) != 0) {
        fc_http_free(&resp);
        return NULL;
    }
    *len = resp.body_len;
    return resp.body;
}

/* One cosign signature layer: the payload must name `digest` and the
 * signature over it must verify with the key. */
static int check_signature_layer(oci_ref_t *r, mbedtls_pk_context *pk, const cJSON *layer, const char *digest)
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
    int rc = fc_sha256(payload, len, hash);
    if (rc == 0) {
        rc = mbedtls_pk_verify(pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sig_len);
    }
    cJSON *p = rc == 0 ? cJSON_Parse(payload) : NULL;
    const char *named = cJSON_GetStringValue(cJSON_GetObjectItem(
        cJSON_GetObjectItem(cJSON_GetObjectItem(p, "critical"), "image"), "docker-manifest-digest"));
    rc = rc == 0 && named != NULL && strcmp(named, digest) == 0 ? 0 : -1;
    cJSON_Delete(p);
    free(payload);
    return rc;
}

/* Is the manifest or index with this digest signed with the signing key? */
static int verify_signature(oci_ref_t *r, const char *image, const char *digest, char *err, size_t err_len)
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
                         char *err, size_t err_len, cJSON **layers_out)
{
    if (layers_out != NULL) {
        *layers_out = NULL;
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
    if (layers_out != NULL && blob_digest[0] != '\0' && *blob_size > 0) {
        *layers_out = cJSON_DetachItemFromObject(manifest, "layers"); /* the caller's now */
    }
    if (manifest != top) {
        cJSON_Delete(manifest);
    }
    cJSON_Delete(top);
    if (blob_digest[0] == '\0' || *blob_size <= 0) {
        snprintf(err, err_len, "%s: no %s%slayer", image, layer_type ? layer_type : "", layer_type ? " " : "");
        return -1;
    }
    return 0;
}

/* Streams a blob to `on_data`, checking its sha256. */
typedef struct {
    fc_sha256_t sha;
    fc_http_data_cb next;
    void *next_ctx;
    long done, total;
    int last_pct;
    int progress;
} sink_t;

static int sink(const void *data, size_t len, void *ctx)
{
    sink_t *s = ctx;
    fc_sha256_update(&s->sha, data, len);
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

static int fetch_blob(oci_ref_t *r, const char *blob_digest, long blob_size, fc_http_data_cb on_data,
                      void *ctx, int progress, char *err, size_t err_len)
{
    sink_t s = {.next = on_data, .next_ctx = ctx, .total = blob_size, .last_pct = -10, .progress = progress};
    fc_sha256_start(&s.sha);
    char path[320];
    snprintf(path, sizeof(path), "/v2/%s/blobs/%s", r->repo, blob_digest);
    fc_http_target_t t = target(r, "application/octet-stream");
    int status;
    long total;
    /* One connection to the registry at a time: a device short of RAM may
     * not hold two TLS sessions to it. */
    fc_http_session_close(r->sess);
    int rc = fc_http_get_stream(&t, path, sink, &s, &status, &total);
    if (rc != 0 && status == 401 && s.done == 0 && r->challenge != NULL) {
        /* A token that ran out since the manifests: get another, once. */
        if (reg_authenticate(r, err, err_len) != 0) {
            return -1;
        }
        fc_http_session_close(r->sess);
        t = target(r, "application/octet-stream");
        rc = fc_http_get_stream(&t, path, sink, &s, &status, &total);
    }
    char got[72];
    int hashed = finish_hex(&s.sha, got) == 0;
    if (rc != 0 || s.done != blob_size) {
        snprintf(err, err_len, "download failed (HTTP %d, %ld/%ld bytes)", status, s.done, blob_size);
        return -1;
    }
    if (!hashed || strcmp(got, blob_digest) != 0) {
        snprintf(err, err_len, "digest mismatch: got %.19s...", got);
        return -1;
    }
    return 0;
}

static int ota_sink(const void *data, size_t len, void *ctx)
{
    return fc_port_ota_write(ctx, data, len);
}

/* Commits the slot; FC_OCI_REFUSED with `err` set when the image is refused. */
static int ota_commit(void *ota, char *err, size_t err_len)
{
    int rc = fc_port_ota_end(ota, 1);
    if (rc == FC_OTA_TOO_OLD) {
        snprintf(err, err_len, "firmware older than this device accepts (anti-rollback security version)");
        return FC_OCI_REFUSED;
    }
    if (rc == FC_OTA_WRONG_VARIANT) {
        snprintf(err, err_len, "firmware built for another security variant (secure or plain) than this device");
        return FC_OCI_REFUSED;
    }
    if (rc != 0) {
        snprintf(err, err_len, "firmware image rejected by the bootloader checks");
        return FC_OCI_REFUSED;
    }
    return 0;
}

/* How the last firmware came, for status.os.lastDelta: "Applied",
 * "NotUsed", or "Fallback <why>"; kept across the reboot into it. */
static char s_fw_delta[136];
static int s_fw_delta_loaded;

const char *fc_oci_delta_outcome(const char **reason)
{
    if (!s_fw_delta_loaded) {
        char *v = NULL;
        size_t len = 0;
        if (fc_port_kv_get(KV_FW_DELTA, &v, &len) == 0) {
            snprintf(s_fw_delta, sizeof(s_fw_delta), "%.*s", (int)len, v);
        } else if (fc_port_kv_get(KV_FW_DOWNLOAD, &v, &len) == 0) {
            /* written by a firmware before this one, which took this one */
            snprintf(s_fw_delta, sizeof(s_fw_delta), "%s",
                     len >= 6 && memcmp(v, "delta ", 6) == 0 ? "Applied" : "NotUsed");
        }
        free(v);
        s_fw_delta_loaded = 1;
    }
    char *sp = strchr(s_fw_delta, ' ');
    if (reason != NULL) {
        *reason = sp != NULL ? sp + 1 : "";
    }
    static char outcome[16];
    snprintf(outcome, sizeof(outcome), "%.*s", sp != NULL ? (int)(sp - s_fw_delta) : (int)strlen(s_fw_delta),
             s_fw_delta);
    return outcome;
}

static void set_delta_outcome(const char *outcome, const char *why)
{
    snprintf(s_fw_delta, sizeof(s_fw_delta), "%s%s%s", outcome, why[0] ? " " : "", why);
    s_fw_delta_loaded = 1;
    fc_port_kv_set(KV_FW_DELTA, s_fw_delta, strlen(s_fw_delta));
    fc_port_kv_del(KV_FW_DOWNLOAD);
}

/* sha256 of the first `size` bytes of the running image, "sha256:<hex>";
 * -1 when the port cannot read it. Kept for the next call with that size:
 * the running image does not change. */
static size_t s_running_size;
static char s_running_digest[72];

static int running_image_digest(size_t size, char out[72])
{
    if (size == s_running_size && s_running_digest[0] != '\0') {
        memcpy(out, s_running_digest, 72);
        return 0;
    }
    unsigned char *buf = malloc(1024);
    if (buf == NULL) {
        return -1;
    }
    fc_sha256_t sha;
    fc_sha256_start(&sha);
    int rc = 0;
    for (size_t off = 0; off < size && rc == 0;) {
        size_t n = size - off < 1024 ? size - off : 1024;
        rc = fc_port_fw_read(off, buf, n);
        fc_sha256_update(&sha, buf, rc == 0 ? n : 0);
        off += n;
    }
    char digest[72];
    if (finish_hex(&sha, digest) != 0) {
        rc = -1;
    }
    free(buf);
    if (rc != 0) {
        return -1;
    }
    memcpy(s_running_digest, digest, 72);
    s_running_size = size;
    memcpy(out, s_running_digest, 72);
    return 0;
}

/* The delta in `layers` that starts from the running image, if any. */
static int find_delta(const cJSON *layers, long full_size, char delta_digest[72], long *delta_size, long *from_size)
{
    const cJSON *l;
    int seen = 0;
    cJSON_ArrayForEach(l, layers) {
        const char *lt = cJSON_GetStringValue(cJSON_GetObjectItem(l, "mediaType"));
        if (lt == NULL || strcmp(lt, FW_DELTA_TYPE) != 0) {
            continue;
        }
        if (++seen > MAX_DELTAS) {
            break;
        }
        const cJSON *ann = cJSON_GetObjectItem(l, "annotations");
        const char *from = cJSON_GetStringValue(cJSON_GetObjectItem(ann, DELTA_FROM));
        const char *fs = cJSON_GetStringValue(cJSON_GetObjectItem(ann, DELTA_FROM_SIZE));
        const char *blob = cJSON_GetStringValue(cJSON_GetObjectItem(l, "digest"));
        const cJSON *size = cJSON_GetObjectItem(l, "size");
        double sz = cJSON_IsNumber(size) ? size->valuedouble : -1;
        double fsz = fs != NULL ? strtod(fs, NULL) : -1;
        /* A delta as large as the image saves nothing. */
        if (from == NULL || blob == NULL || strlen(blob) >= 72 || !(sz >= 1 && sz < (double)full_size) ||
            !(fsz >= 1 && fsz <= MAX_BLOB_SIZE)) {
            continue;
        }
        char running[72];
        if (running_image_digest((size_t)fsz, running) != 0) {
            return 0; /* this port cannot read its image: no deltas at all */
        }
        if (strcmp(running, from) == 0) {
            snprintf(delta_digest, 72, "%s", blob);
            *delta_size = (long)sz;
            *from_size = (long)fsz;
            return 1;
        }
    }
    return 0;
}

typedef struct {
    void *ota;
    fc_sha256_t sha;
    fc_delta_t *delta;
} delta_ctx_t;

static int delta_read(size_t off, void *buf, size_t len, void *ctx)
{
    return fc_port_fw_read(off, buf, len);
}

static int delta_write(const void *data, size_t len, void *ctx)
{
    delta_ctx_t *c = ctx;
    fc_sha256_update(&c->sha, data, len);
    return fc_port_ota_write(c->ota, data, len);
}

static int delta_sink(const void *data, size_t len, void *ctx)
{
    return fc_delta_feed(((delta_ctx_t *)ctx)->delta, data, len);
}

/* Rebuilds the new image in the OTA slot from the running one and a delta.
 * 0: done; 1: not this way (`why` says it), the slot is untouched and the
 * whole image can still be downloaded; -1: the image is there and the device
 * refuses it (`err` set). */
static int fetch_firmware_delta(oci_ref_t *r, const char *delta_digest, long delta_size, long from_size,
                                const char *full_digest, long full_size, char *why, size_t why_len, char *err,
                                size_t err_len)
{
    delta_ctx_t c = {0};
    if (fc_port_ota_begin((size_t)full_size, &c.ota) != 0) {
        snprintf(why, why_len, "cannot start OTA");
        return 1;
    }
    c.delta = fc_delta_begin((size_t)delta_size, (size_t)from_size, (size_t)full_size, delta_read, delta_write, &c);
    if (c.delta == NULL) {
        fc_port_ota_end(c.ota, 0);
        snprintf(why, why_len, "out of memory");
        return 1;
    }
    fc_sha256_start(&c.sha);
    int ok = fetch_blob(r, delta_digest, delta_size, delta_sink, &c, 1, why, why_len) == 0;
    /* Even after a failed download: it frees the decoder. */
    if (fc_delta_end(c.delta) != 0 && ok) {
        snprintf(why, why_len, "the delta does not apply");
        ok = 0;
    }
    char got[72];
    int hashed = finish_hex(&c.sha, got) == 0;
    if (ok && (!hashed || strcmp(got, full_digest) != 0)) {
        snprintf(why, why_len, "the rebuilt image is not %.19s...", full_digest);
        ok = 0;
    }
    if (!ok) {
        fc_port_ota_end(c.ota, 0);
        return 1;
    }
    return ota_commit(c.ota, err, err_len);
}

static int fetch_firmware(oci_ref_t *rp, const char *image, char *digest, size_t digest_len, char *err,
                          size_t err_len)
{
    char blob_digest[72];
    long blob_size;
    cJSON *layers = NULL;
    const char *platform = fc_port_fw_platform();
    if (resolve_layer(image, platform, NULL, rp, blob_digest, &blob_size, digest, digest_len, err, err_len,
                      &layers) != 0) {
        return -1;
    }
    char delta_digest[72];
    long delta_size = 0, from_size = 0;
    int has_delta = find_delta(layers, blob_size, delta_digest, &delta_size, &from_size);
    cJSON_Delete(layers);
    char why[120] = "";
    if (has_delta) {
        fc_port_log(FC_LOG_INFO, "firmware %s (%s): %ld bytes, as a delta of %ld from the running image", image,
                    platform, blob_size, delta_size);
        int rc = fetch_firmware_delta(rp, delta_digest, delta_size, from_size, blob_digest, blob_size, why,
                                      sizeof(why), err, err_len);
        if (rc == 0) {
            fc_port_log(FC_LOG_INFO, "firmware came as a delta: %ld of %ld bytes", delta_size, blob_size);
            set_delta_outcome("Applied", "");
        }
        if (rc <= 0) {
            return rc;
        }
        fc_port_log(FC_LOG_WARN, "firmware delta not used (%s): downloading the whole image", why);
    }
    fc_port_log(FC_LOG_INFO, "firmware %s (%s): %ld bytes, %s", image, platform, blob_size, blob_digest);
    void *ota;
    if (fc_port_ota_begin((size_t)blob_size, &ota) != 0) {
        snprintf(err, err_len, "cannot start OTA (%ld bytes)", blob_size);
        return -1;
    }
    char ferr[120];
    if (fetch_blob(rp, blob_digest, blob_size, ota_sink, ota, 1, ferr, sizeof(ferr)) != 0) {
        snprintf(err, err_len, "firmware %s", ferr);
        fc_port_ota_end(ota, 0);
        return -1;
    }
    int rc = ota_commit(ota, err, err_len);
    if (rc != 0) {
        return rc;
    }
    set_delta_outcome(has_delta ? "Fallback" : "NotUsed", has_delta ? why : "");
    return 0;
}

int fc_oci_fetch_firmware(const char *image, char *digest, size_t digest_len, char *err, size_t err_len)
{
    oci_ref_t r;
    if (reg_open(image, &r, err, err_len) != 0) {
        return -1;
    }
    int rc = fetch_firmware(&r, image, digest, digest_len, err, err_len);
    reg_close(&r);
    return rc;
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

static int fetch_layer(oci_ref_t *rp, const char *image, const char *arch, const char *layer_type,
                       size_t max_size, unsigned char **out, size_t *out_len, char *digest, size_t digest_len,
                       char *err, size_t err_len)
{
    char blob_digest[72];
    long blob_size;
    if (resolve_layer(image, arch, layer_type, rp, blob_digest, &blob_size, digest, digest_len, err, err_len,
                      NULL) != 0) {
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
    if (fetch_blob(rp, blob_digest, blob_size, mem_sink, &m, 0, ferr, sizeof(ferr)) != 0) {
        snprintf(err, err_len, "%s: %s", image, ferr);
        free(m.buf);
        return -1;
    }
    *out = m.buf;
    *out_len = m.len;
    return 0;
}

int fc_oci_fetch_layer(const char *image, const char *arch, const char *layer_type, size_t max_size,
                       unsigned char **out, size_t *out_len, char *digest, size_t digest_len,
                       char *err, size_t err_len)
{
    oci_ref_t r;
    if (reg_open(image, &r, err, err_len) != 0) {
        return -1;
    }
    int rc = fetch_layer(&r, image, arch, layer_type, max_size, out, out_len, digest, digest_len, err, err_len);
    reg_close(&r);
    return rc;
}
