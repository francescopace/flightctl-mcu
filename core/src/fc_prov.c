/*
 * WiFi provisioning in the field: Espressif's protocol, so that the same
 * installer tools serve the ESP-IDF firmware and this one (fc_prov.h).
 *
 * The installer speaks HTTP/1.1 to the device's access point, one POST per
 * message, on five endpoints:
 *  - proto-ver: what the device offers (JSON, in clear);
 *  - prov-session: the session, protocomm's Security 1. The installer and
 *    the device exchange X25519 public keys; the shared secret, XORed with
 *    the SHA-256 of the proof of possession, is an AES-256 key, and the
 *    device's random is the counter of one AES-CTR stream that encrypts
 *    everything after, both ways. Each side then proves it has the same key
 *    by encrypting the other's public key: a wrong proof of possession fails
 *    there, before anything is sent;
 *  - prov-config: the network, its status (network_config.proto);
 *  - prov-scan: the networks the device sees (network_scan.proto);
 *  - prov-ctrl: start again after a failure (network_ctrl.proto).
 * The messages are protobuf; the few fields used are read and written here.
 * A session is tied to a cookie, or to its connection for an installer that
 * keeps none, as protocomm's HTTP transport does.
 */
#include "fc_prov.h"

#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "fc_crypto.h"
#include "fc_port.h"

#define HEAD_MAX 1024 /* request line and headers */
#define BODY_MAX 1024 /* the largest message: a network, 32 + 64 bytes and framing */
#define OUT_MAX  1600 /* the largest answer: FC_PROV_SCAN_MAX networks */
#define IO_TIMEOUT_MS 5000
/* Once the device has joined, how long it waits for the installer to ask,
 * so that the installer is told. */
#define TELL_JOINED_S 20
/* Once the installer has been told, how long the server stays up before the
 * port closes the access point: the answer must reach it first, and an
 * installer may ask again (ESP-IDF's provisioning manager waits too). */
#define AFTER_TOLD_S 5

/* ---- protobuf ---------------------------------------------------------- */

typedef struct {
    uint8_t *buf;
    size_t len, cap;
    int err;
} pb_w;

static void pb_put(pb_w *w, const void *p, size_t n)
{
    if (w->err || n > w->cap - w->len) {
        w->err = 1;
        return;
    }
    if (n > 0) {
        memcpy(w->buf + w->len, p, n);
    }
    w->len += n;
}

static void pb_raw(pb_w *w, uint64_t v)
{
    uint8_t b[10];
    size_t n = 0;
    do {
        b[n] = (uint8_t)(v & 0x7f);
        v >>= 7;
        if (v != 0) {
            b[n] |= 0x80;
        }
        n++;
    } while (v != 0);
    pb_put(w, b, n);
}

/* A varint field. Written even when 0: a field of a oneof must be there to
 * say which one it is. Negative int32 values are 64-bit two's complement on
 * the wire. */
static void pb_num(pb_w *w, int field, int64_t v)
{
    pb_raw(w, (uint64_t)field << 3);
    pb_raw(w, (uint64_t)v);
}

static void pb_bytes(pb_w *w, int field, const void *p, size_t n)
{
    pb_raw(w, (uint64_t)field << 3 | 2);
    pb_raw(w, n);
    pb_put(w, p, n);
}

/* A message inside another: written to `inner` first. */
static void pb_msg(pb_w *w, int field, const pb_w *inner)
{
    if (inner->err) {
        w->err = 1;
        return;
    }
    pb_bytes(w, field, inner->buf, inner->len);
}

static int pb_varint(const uint8_t **p, const uint8_t *end, uint64_t *v)
{
    uint64_t r = 0;
    for (int shift = 0; shift < 64 && *p < end; shift += 7) {
        uint8_t b = *(*p)++;
        r |= (uint64_t)(b & 0x7f) << shift;
        if ((b & 0x80) == 0) {
            *v = r;
            return 0;
        }
    }
    return -1;
}

/* Looks for `field` in a message: 1 found (a varint in *num, or bytes in
 * *data and *len), 0 not there, -1 not a valid message. The last one wins,
 * as protobuf says. */
static int pb_find(const uint8_t *msg, size_t msg_len, int field, uint64_t *num, const uint8_t **data,
                   size_t *len)
{
    const uint8_t *p = msg, *end = msg + msg_len;
    int found = 0;
    while (p < end) {
        uint64_t key, v;
        if (pb_varint(&p, end, &key) != 0) {
            return -1;
        }
        int wire = (int)(key & 7);
        int match = (key >> 3) == (uint64_t)field;
        if (wire == 0) {
            if (pb_varint(&p, end, &v) != 0) {
                return -1;
            }
            if (match && num != NULL) {
                *num = v;
                found = 1;
            }
        } else if (wire == 2) {
            if (pb_varint(&p, end, &v) != 0 || v > (uint64_t)(end - p)) {
                return -1;
            }
            if (match && data != NULL) {
                *data = p;
                *len = (size_t)v;
                found = 1;
            }
            p += v;
        } else if (wire == 5 || wire == 1) {
            size_t skip = wire == 5 ? 4 : 8;
            if ((size_t)(end - p) < skip) {
                return -1;
            }
            p += skip;
        } else {
            return -1;
        }
    }
    return found;
}

static uint64_t pb_get_num(const uint8_t *msg, size_t len, int field)
{
    uint64_t v = 0;
    return pb_find(msg, len, field, &v, NULL, NULL) == 1 ? v : 0;
}

/* A submessage or bytes field; an absent one is empty (not an error). */
static int pb_get_bytes(const uint8_t *msg, size_t len, int field, const uint8_t **data, size_t *dlen)
{
    *data = (const uint8_t *)"";
    *dlen = 0;
    return pb_find(msg, len, field, NULL, data, dlen) < 0 ? -1 : 0;
}

/* ---- the server's state ------------------------------------------------- */

enum { SESS_NONE, SESS_CMD1, SESS_READY };
enum { NET_NONE, NET_SET, NET_JOINING, NET_FAILED, NET_JOINED, NET_LATER };

typedef struct {
    const fc_prov_config_t *cfg;
    /* the session */
    uint32_t cookie;
    int sock;
    int state;
    uint8_t dev_pub[32], cli_pub[32], iv[16];
    mbedtls_svc_key_id_t aes;
    psa_cipher_operation_t ctr;
    /* the network */
    char ssid[33], pass[65];
    int net;
    fc_prov_join_t failed; /* why, for NET_FAILED */
    int told_joined;
    time_t joined_at, told_at;
    /* buffers, not on the stack: the main thread of a small device has a few KB */
    char head[HEAD_MAX + 1];
    uint8_t body[BODY_MAX], plain[BODY_MAX], answer[OUT_MAX], out[OUT_MAX], inner[OUT_MAX];
} prov_t;

static void session_end(prov_t *p)
{
    psa_cipher_abort(&p->ctr);
    psa_destroy_key(p->aes);
    p->aes = MBEDTLS_SVC_KEY_ID_INIT;
    p->state = SESS_NONE;
    p->cookie = 0;
    p->sock = -1;
}

static int ct_differ(const uint8_t *a, const uint8_t *b, size_t n)
{
    uint8_t d = 0;
    for (size_t i = 0; i < n; i++) {
        d |= a[i] ^ b[i];
    }
    return d != 0;
}

/* The CTR stream, the same one for what comes in and what goes out. */
static int ctr(prov_t *p, const uint8_t *in, size_t len, uint8_t *out)
{
    size_t n = 0;
    return psa_cipher_update(&p->ctr, in, len, out, len, &n) == PSA_SUCCESS && n == len ? 0 : -1;
}

/* ---- prov-session -------------------------------------------------------- */

/* SessionData { sec_ver = 1, sec1 = Sec1Payload { msg, <field> = inner } } */
static size_t session_answer(prov_t *p, int msg, int field, const pb_w *inner)
{
    uint8_t sec1_buf[128];
    pb_w sec1 = {sec1_buf, 0, sizeof(sec1_buf), 0};
    pb_num(&sec1, 1, msg);
    pb_msg(&sec1, field, inner);
    pb_w w = {p->answer, 0, sizeof(p->answer), 0};
    pb_num(&w, 2, 1);
    pb_msg(&w, 11, &sec1);
    return w.err ? 0 : w.len;
}

/* Session_Command0: the installer's public key. Answers with the device's
 * and the random that starts the counter. */
static size_t session_cmd0(prov_t *p, const uint8_t *sc0, size_t sc0_len)
{
    const uint8_t *key;
    size_t key_len;
    if (pb_get_bytes(sc0, sc0_len, 1, &key, &key_len) != 0 || key_len != 32) {
        return 0;
    }
    memcpy(p->cli_pub, key, 32);

    psa_key_attributes_t attr = PSA_KEY_ATTRIBUTES_INIT;
    psa_set_key_type(&attr, PSA_KEY_TYPE_ECC_KEY_PAIR(PSA_ECC_FAMILY_MONTGOMERY));
    psa_set_key_bits(&attr, 255);
    psa_set_key_usage_flags(&attr, PSA_KEY_USAGE_DERIVE);
    psa_set_key_algorithm(&attr, PSA_ALG_ECDH);
    mbedtls_svc_key_id_t ecdh = MBEDTLS_SVC_KEY_ID_INIT;
    uint8_t shared[32], digest[32];
    size_t n = 0, m = 0;
    int ok = fc_crypto_init() == 0 && psa_generate_key(&attr, &ecdh) == PSA_SUCCESS &&
             psa_export_public_key(ecdh, p->dev_pub, sizeof(p->dev_pub), &n) == PSA_SUCCESS && n == 32 &&
             psa_raw_key_agreement(PSA_ALG_ECDH, ecdh, p->cli_pub, 32, shared, sizeof(shared), &m) ==
                 PSA_SUCCESS &&
             m == 32;
    psa_destroy_key(ecdh);
    if (ok) {
        fc_sha256_t h;
        fc_sha256_start(&h);
        fc_sha256_update(&h, p->cfg->pop, strlen(p->cfg->pop));
        ok = fc_sha256_finish(&h, digest) == 0;
    }
    for (int i = 0; i < 32; i++) {
        shared[i] ^= digest[i];
    }
    if (ok) {
        psa_key_attributes_t a = PSA_KEY_ATTRIBUTES_INIT;
        psa_set_key_type(&a, PSA_KEY_TYPE_AES);
        psa_set_key_bits(&a, 256);
        psa_set_key_usage_flags(&a, PSA_KEY_USAGE_ENCRYPT | PSA_KEY_USAGE_DECRYPT);
        psa_set_key_algorithm(&a, PSA_ALG_CTR);
        p->ctr = psa_cipher_operation_init();
        ok = fc_random(p->iv, sizeof(p->iv)) == 0 && psa_import_key(&a, shared, 32, &p->aes) == PSA_SUCCESS &&
             psa_cipher_encrypt_setup(&p->ctr, p->aes, PSA_ALG_CTR) == PSA_SUCCESS &&
             psa_cipher_set_iv(&p->ctr, p->iv, sizeof(p->iv)) == PSA_SUCCESS;
    }
    memset(shared, 0, sizeof(shared));
    if (!ok) {
        fc_port_log(FC_LOG_ERROR, "provisioning: cannot set up the session");
        return 0;
    }
    /* SessionResp0 { status = Success, device_pubkey, device_random } */
    uint8_t buf[96];
    pb_w r = {buf, 0, sizeof(buf), 0};
    pb_num(&r, 1, 0);
    pb_bytes(&r, 2, p->dev_pub, 32);
    pb_bytes(&r, 3, p->iv, sizeof(p->iv));
    p->state = SESS_CMD1;
    return session_answer(p, 1, 21, &r);
}

/* Session_Command1: the device's public key, encrypted by the installer.
 * Right only if the installer knows the proof of possession. */
static size_t session_cmd1(prov_t *p, const uint8_t *sc1, size_t sc1_len)
{
    const uint8_t *verify;
    size_t len;
    uint8_t check[32], proof[32];
    if (pb_get_bytes(sc1, sc1_len, 2, &verify, &len) != 0 || len != 32 || ctr(p, verify, 32, check) != 0) {
        return 0;
    }
    if (ct_differ(check, p->dev_pub, 32)) {
        fc_port_log(FC_LOG_WARN, "provisioning: wrong proof of possession, session refused");
        return 0;
    }
    if (ctr(p, p->cli_pub, 32, proof) != 0) {
        return 0;
    }
    /* SessionResp1 { status = Success, device_verify_data } */
    uint8_t buf[48];
    pb_w r = {buf, 0, sizeof(buf), 0};
    pb_num(&r, 1, 0);
    pb_bytes(&r, 3, proof, 32);
    p->state = SESS_READY;
    fc_port_log(FC_LOG_INFO, "provisioning: session established");
    return session_answer(p, 3, 23, &r);
}

static size_t on_session(prov_t *p, const uint8_t *in, size_t len)
{
    const uint8_t *sec1, *payload;
    size_t sec1_len, payload_len;
    if (pb_get_num(in, len, 2) != 1 || pb_get_bytes(in, len, 11, &sec1, &sec1_len) != 0) {
        return 0;
    }
    uint64_t msg = pb_get_num(sec1, sec1_len, 1);
    if (msg == 0 && p->state == SESS_NONE && pb_get_bytes(sec1, sec1_len, 20, &payload, &payload_len) == 0) {
        return session_cmd0(p, payload, payload_len);
    }
    if (msg == 2 && p->state == SESS_CMD1 && pb_get_bytes(sec1, sec1_len, 22, &payload, &payload_len) == 0) {
        return session_cmd1(p, payload, payload_len);
    }
    return 0;
}

/* ---- prov-config, prov-scan, prov-ctrl (decrypted) ------------------------ */

static void join_check(prov_t *p)
{
    if (p->net != NET_JOINING) {
        return;
    }
    fc_prov_join_t st = p->cfg->join_state(p->cfg->ctx);
    if (st == FC_PROV_JOINED) {
        p->net = NET_JOINED;
        p->joined_at = time(NULL);
        fc_port_log(FC_LOG_INFO, "provisioning: joined \"%s\"", p->ssid);
    } else if (st != FC_PROV_JOINING) {
        p->net = NET_FAILED;
        p->failed = st;
        fc_port_log(FC_LOG_WARN, "provisioning: cannot join \"%s\" (%s), waiting for another network", p->ssid,
                    st == FC_PROV_BAD_PASSWORD ? "wrong password" : "not found");
    }
}

static size_t on_config(prov_t *p, const uint8_t *in, size_t len)
{
    uint64_t msg = pb_get_num(in, len, 1);
    uint8_t buf[96];
    pb_w r = {buf, 0, sizeof(buf), 0};
    pb_w w = {p->answer, 0, sizeof(p->answer), 0};
    if (msg == 0) { /* CmdGetWifiStatus */
        join_check(p);
        /* WifiStationState: Connected 0, Connecting 1, Disconnected 2, ConnectionFailed 3 */
        pb_num(&r, 1, 0);
        switch (p->net) {
        case NET_JOINED: {
            pb_num(&r, 2, 0);
            uint8_t cbuf[48];
            pb_w c = {cbuf, 0, sizeof(cbuf), 0};
            pb_bytes(&c, 3, p->ssid, strlen(p->ssid));
            pb_msg(&r, 11, &c);
            if (!p->told_joined) {
                p->told_joined = 1;
                p->told_at = time(NULL);
            }
            break;
        }
        case NET_JOINING:
        case NET_LATER:
            pb_num(&r, 2, 1);
            break;
        case NET_FAILED:
            pb_num(&r, 2, 3);
            pb_num(&r, 10, p->failed == FC_PROV_BAD_PASSWORD ? 0 : 1); /* AuthError, WifiNetworkNotFound */
            break;
        default:
            pb_num(&r, 2, 2);
        }
        pb_num(&w, 1, 1);
        pb_msg(&w, 11, &r);
    } else if (msg == 2) { /* CmdSetWifiConfig */
        const uint8_t *cmd, *ssid, *pass;
        size_t cmd_len, ssid_len, pass_len;
        int ok = pb_get_bytes(in, len, 12, &cmd, &cmd_len) == 0 &&
                 pb_get_bytes(cmd, cmd_len, 1, &ssid, &ssid_len) == 0 &&
                 pb_get_bytes(cmd, cmd_len, 2, &pass, &pass_len) == 0 && ssid_len >= 1 && ssid_len <= 32 &&
                 pass_len <= 64 && memchr(ssid, 0, ssid_len) == NULL && memchr(pass, 0, pass_len) == NULL &&
                 p->net != NET_JOINING && p->net != NET_JOINED;
        if (ok) {
            memcpy(p->ssid, ssid, ssid_len);
            p->ssid[ssid_len] = '\0';
            memcpy(p->pass, pass, pass_len);
            p->pass[pass_len] = '\0';
            p->net = NET_SET;
            fc_port_log(FC_LOG_INFO, "provisioning: network \"%s\" received", p->ssid);
        }
        pb_num(&r, 1, ok ? 0 : 4); /* Success, InvalidArgument */
        pb_num(&w, 1, 3);
        pb_msg(&w, 13, &r);
    } else if (msg == 4) { /* CmdApplyWifiConfig */
        int ok = p->net == NET_SET; /* after a failure, a network must be set again */
        if (ok) {
            if (p->cfg->join != NULL && p->cfg->join(p->cfg->ctx, p->ssid, p->pass) == 0) {
                p->net = NET_JOINING;
            } else {
                p->net = NET_LATER;
            }
        }
        pb_num(&r, 1, ok ? 0 : 4);
        pb_num(&w, 1, 5);
        pb_msg(&w, 15, &r);
    } else {
        return 0;
    }
    return w.err ? 0 : w.len;
}

static size_t on_scan(prov_t *p, const uint8_t *in, size_t len)
{
    uint64_t msg = pb_get_num(in, len, 1);
    const fc_prov_config_t *cfg = p->cfg;
    int count = cfg->ap_count < 0 ? 0 : cfg->ap_count > FC_PROV_SCAN_MAX ? FC_PROV_SCAN_MAX : cfg->ap_count;
    pb_w w = {p->answer, 0, sizeof(p->answer), 0};
    pb_w r = {p->inner, 0, sizeof(p->inner) - 16, 0};
    if (msg == 0) { /* CmdScanWifiStart: the networks were seen before the access point opened */
        pb_num(&w, 1, 1);
        pb_num(&w, 2, 0);
        pb_msg(&w, 11, &r);
    } else if (msg == 2) { /* CmdScanWifiStatus */
        pb_num(&r, 1, 1);
        pb_num(&r, 2, count);
        pb_num(&w, 1, 3);
        pb_num(&w, 2, 0);
        pb_msg(&w, 13, &r);
    } else if (msg == 4) { /* CmdScanWifiResult */
        const uint8_t *cmd;
        size_t cmd_len;
        if (pb_get_bytes(in, len, 14, &cmd, &cmd_len) != 0) {
            return 0;
        }
        uint64_t start = pb_get_num(cmd, cmd_len, 1), n = pb_get_num(cmd, cmd_len, 2);
        for (uint64_t i = start; i < (uint64_t)count && i - start < n; i++) {
            const fc_prov_ap_t *ap = &cfg->aps[i];
            uint8_t ebuf[80];
            pb_w e = {ebuf, 0, sizeof(ebuf), 0};
            pb_bytes(&e, 1, ap->ssid, strnlen(ap->ssid, 32));
            pb_num(&e, 2, ap->channel);
            pb_num(&e, 3, ap->rssi);
            pb_bytes(&e, 4, ap->bssid, 6);
            pb_num(&e, 5, ap->auth);
            pb_msg(&r, 1, &e);
        }
        pb_num(&w, 1, 5);
        pb_num(&w, 2, 0);
        pb_msg(&w, 15, &r);
    } else {
        return 0;
    }
    return w.err ? 0 : w.len;
}

static size_t on_ctrl(prov_t *p, const uint8_t *in, size_t len)
{
    uint64_t msg = pb_get_num(in, len, 1);
    if (msg != 1 && msg != 3) { /* CmdCtrlWifiReset, CmdCtrlWifiReprov */
        return 0;
    }
    if (p->net != NET_JOINING && p->net != NET_JOINED && p->net != NET_LATER) {
        p->net = NET_NONE;
    }
    pb_w w = {p->answer, 0, sizeof(p->answer), 0};
    pb_w r = {NULL, 0, 0, 0};
    pb_num(&w, 1, (int64_t)msg + 1);
    pb_num(&w, 2, 0);
    pb_msg(&w, msg == 1 ? 12 : 14, &r);
    return w.err ? 0 : w.len;
}

/* ---- HTTP ------------------------------------------------------------------ */

static int wait_in(int fd)
{
    struct pollfd pf = {.fd = fd, .events = POLLIN};
    return poll(&pf, 1, IO_TIMEOUT_MS) == 1 ? 0 : -1;
}

static int send_all(int fd, const void *buf, size_t len)
{
    const char *b = buf;
    while (len > 0) {
        ssize_t n = send(fd, b, len, 0);
        if (n <= 0) {
            return -1;
        }
        b += n;
        len -= (size_t)n;
    }
    return 0;
}

static int respond(int fd, int status, const uint8_t *body, size_t len, uint32_t cookie)
{
    char head[192];
    int n = snprintf(head, sizeof(head),
                     "HTTP/1.1 %d %s\r\nContent-Type: application/octet-stream\r\nContent-Length: %u\r\n",
                     status, status == 200 ? "OK" : status == 404 ? "Not Found" : "Internal Server Error",
                     (unsigned)len);
    if (cookie != 0) {
        n += snprintf(head + n, sizeof(head) - (size_t)n, "Set-Cookie: session=%lu\r\n", (unsigned long)cookie);
    }
    if (status != 200) {
        n += snprintf(head + n, sizeof(head) - (size_t)n, "Connection: close\r\n");
    }
    n += snprintf(head + n, sizeof(head) - (size_t)n, "\r\n");
    return send_all(fd, head, (size_t)n) == 0 && send_all(fd, body, len) == 0 ? 0 : -1;
}

/* The value of a header, in place, or NULL. */
static char *header(char *head, const char *name)
{
    size_t len = strlen(name);
    for (char *line = strstr(head, "\r\n"); line != NULL; line = strstr(line, "\r\n")) {
        line += 2;
        if (strncasecmp(line, name, len) == 0 && line[len] == ':') {
            char *v = line + len + 1;
            while (*v == ' ') {
                v++;
            }
            return v;
        }
    }
    return NULL;
}

/* One request on a connection: 0 to keep it, -1 to close it. */
static int serve_request(prov_t *p, int fd)
{
    size_t got = 0;
    char *end = NULL;
    while (end == NULL) {
        if (got == HEAD_MAX || wait_in(fd) != 0) {
            return -1;
        }
        ssize_t n = recv(fd, p->head + got, HEAD_MAX - got, 0);
        if (n <= 0) {
            return -1;
        }
        got += (size_t)n;
        p->head[got] = '\0';
        end = strstr(p->head, "\r\n\r\n");
    }
    size_t head_len = (size_t)(end - p->head) + 4;
    *end = '\0';
    char *len_s = header(p->head, "Content-Length");
    char *cookie_s = header(p->head, "Cookie");
    long body_len = len_s != NULL ? strtol(len_s, NULL, 10) : 0;
    if (strncmp(p->head, "POST /", 6) != 0 || body_len <= 0 || body_len > BODY_MAX ||
        got - head_len > (size_t)body_len) {
        respond(fd, 500, NULL, 0, 0);
        return -1;
    }
    char *ep = p->head + 6;
    ep[strcspn(ep, " ?")] = '\0';
    memcpy(p->body, p->head + head_len, got - head_len);
    for (size_t have = got - head_len; have < (size_t)body_len;) {
        if (wait_in(fd) != 0) {
            return -1;
        }
        ssize_t n = recv(fd, p->body + have, (size_t)body_len - have, 0);
        if (n <= 0) {
            return -1;
        }
        have += (size_t)n;
    }

    /* The same session: its cookie, or its connection when no cookie came. */
    int same;
    if (cookie_s != NULL) {
        char *v = strstr(cookie_s, "session=");
        same = p->cookie != 0 && v != NULL && strtoul(v + 8, NULL, 10) == p->cookie;
    } else {
        same = p->cookie != 0 && fd == p->sock;
    }
    uint32_t set_cookie = 0;
    if (!same) {
        session_end(p);
        do {
            fc_random(&p->cookie, sizeof(p->cookie));
        } while (p->cookie == 0);
        p->sock = fd;
        set_cookie = p->cookie;
    }

    size_t out_len = 0;
    const uint8_t *out = p->answer;
    int found = 1;
    if (strcmp(ep, "proto-ver") == 0) {
        out_len = (size_t)snprintf((char *)p->answer, sizeof(p->answer),
                                   "{\"prov\":{\"ver\":\"netprov-v1.2\",\"sec_ver\":1,"
                                   "\"cap\":[\"wifi_prov\",\"wifi_scan\"]}}");
    } else if (strcmp(ep, "prov-session") == 0) {
        out_len = on_session(p, p->body, (size_t)body_len);
        if (out_len == 0 && p->state == SESS_CMD1) {
            /* A wrong proof: it costs the installer a second, and a new session. */
            sleep(1);
        }
    } else if (strcmp(ep, "prov-config") == 0 || strcmp(ep, "prov-scan") == 0 || strcmp(ep, "prov-ctrl") == 0) {
        if (p->state == SESS_READY && ctr(p, p->body, (size_t)body_len, p->plain) == 0) {
            size_t n = ep[5] == 'c' && ep[6] == 'o' ? on_config(p, p->plain, (size_t)body_len)
                       : ep[5] == 's'               ? on_scan(p, p->plain, (size_t)body_len)
                                                    : on_ctrl(p, p->plain, (size_t)body_len);
            if (n > 0 && ctr(p, p->answer, n, p->out) == 0) {
                out = p->out;
                out_len = n;
            }
        }
    } else {
        found = 0;
    }
    if (out_len == 0) {
        session_end(p);
        respond(fd, found ? 500 : 404, NULL, 0, 0);
        return -1;
    }
    return respond(fd, 200, out, out_len, set_cookie);
}

/* ---- the server ------------------------------------------------------------ */

/* Closes a connection. A session tied to it (no cookie) ends with it: the
 * next connection may get the same descriptor, and is another installer. */
static void drop(prov_t *p, int fd)
{
    if (p->sock == fd) {
        p->sock = -1;
    }
    close(fd);
}

int fc_prov_serve(const fc_prov_config_t *cfg, char ssid[33], char pass[65], int *joined)
{
    prov_t *p = calloc(1, sizeof(*p));
    int lfd = socket(AF_INET, SOCK_STREAM, 0);
    struct sockaddr_in addr = {.sin_family = AF_INET, .sin_port = htons(cfg->port)};
    int one = 1;
    if (p == NULL || lfd < 0 || setsockopt(lfd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)) != 0 ||
        bind(lfd, (struct sockaddr *)&addr, sizeof(addr)) != 0 || listen(lfd, 2) != 0) {
        fc_port_log(FC_LOG_ERROR, "provisioning: cannot listen on port %u (errno %d)", cfg->port, errno);
        if (lfd >= 0) {
            close(lfd);
        }
        free(p);
        return -1;
    }
    p->cfg = cfg;
    p->sock = -1;
    p->aes = MBEDTLS_SVC_KEY_ID_INIT;
    p->ctr = psa_cipher_operation_init();
    fc_port_log(FC_LOG_INFO, "provisioning: waiting for an installer on port %u", cfg->port);
    int cfd = -1;
    for (;;) {
        struct pollfd pf[2] = {{.fd = lfd, .events = POLLIN}, {.fd = cfd, .events = POLLIN}};
        int ready = poll(pf, cfd >= 0 ? 2 : 1, 500);
        if (ready > 0 && cfd >= 0 && pf[1].revents != 0 && serve_request(p, cfd) != 0) {
            drop(p, cfd);
            cfd = -1;
        }
        if (p->net == NET_LATER) {
            break; /* answered: the port joins after closing its access point */
        }
        join_check(p);
        if (p->net == NET_JOINED && ((p->told_joined && time(NULL) - p->told_at >= AFTER_TOLD_S) ||
                                     time(NULL) - p->joined_at >= TELL_JOINED_S)) {
            break;
        }
        /* One installer at a time: a new connection (an app that reconnects) replaces the old one. */
        if (ready > 0 && (pf[0].revents & POLLIN) != 0) {
            int fd = accept(lfd, NULL, NULL);
            if (fd >= 0) {
                if (cfd >= 0) {
                    drop(p, cfd);
                }
                cfd = fd;
            }
        }
    }
    if (cfd >= 0) {
        close(cfd);
    }
    close(lfd);
    session_end(p);
    strcpy(ssid, p->ssid);
    strcpy(pass, p->pass);
    *joined = p->net == NET_JOINED;
    memset(p, 0, sizeof(*p));
    free(p);
    return 0;
}
