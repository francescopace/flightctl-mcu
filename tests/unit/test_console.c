/*
 * The remote console (fc_console.c) against a fake stream: the headers it
 * opens with, one command per session, the prompt with its line editing,
 * the exit status as Kubernetes words it, sessions remembered across a
 * restart, a second session refused, reboot, and bad input from the server.
 */
#include "../../core/src/fc_console.c"

#include "port_stub.h"
#include "test.h"

/* ---- a fake stream ------------------------------------------------------------- */

struct fc_http_stream {
    int id;
};

static pthread_mutex_t f_lock = PTHREAD_MUTEX_INITIALIZER;
static struct fc_http_stream f_streams[4];
static int f_opened, f_closed, f_ended, f_server_gone;
static char f_headers[512];
static uint8_t f_out[16384]; /* what the device sent */
static size_t f_out_len;
static uint8_t f_in[4096]; /* what the server has for it */
static size_t f_in_len, f_in_at;

fc_http_stream_t *fc_http_stream_open(const fc_http_target_t *t, const char *path, const char *content_type,
                                      const char *const *headers)
{
    pthread_mutex_lock(&f_lock);
    size_t n = (size_t)snprintf(f_headers, sizeof(f_headers), "%s %s", path, content_type);
    for (const char *const *h = headers; h[0] != NULL; h += 2) {
        n += (size_t)snprintf(f_headers + n, sizeof(f_headers) - n, " %s=%s", h[0], h[1]);
    }
    fc_http_stream_t *st = &f_streams[f_opened % 4];
    st->id = f_opened++;
    pthread_mutex_unlock(&f_lock);
    return st;
}

int fc_http_stream_write(fc_http_stream_t *st, const void *data, size_t len, int end, int timeout_ms)
{
    pthread_mutex_lock(&f_lock);
    if (len > 0 && f_out_len + len <= sizeof(f_out)) {
        memcpy(f_out + f_out_len, data, len);
        f_out_len += len;
    }
    f_ended += end;
    pthread_mutex_unlock(&f_lock);
    return 0;
}

int fc_http_stream_read(fc_http_stream_t *st, void *buf, size_t cap, int timeout_ms)
{
    pthread_mutex_lock(&f_lock);
    size_t left = f_in_len - f_in_at;
    int gone = f_server_gone;
    if (left > 0) {
        size_t k = cap < left ? cap : left;
        memcpy(buf, f_in + f_in_at, k);
        f_in_at += k;
        pthread_mutex_unlock(&f_lock);
        return (int)k;
    }
    pthread_mutex_unlock(&f_lock);
    if (gone) {
        return -1;
    }
    usleep((useconds_t)(timeout_ms < 20 ? timeout_ms : 20) * 1000);
    return 0;
}

void fc_http_stream_status(fc_http_stream_t *st, int *http_status, int *grpc_status) {}

void fc_http_stream_close(fc_http_stream_t *st)
{
    pthread_mutex_lock(&f_lock);
    f_closed++;
    pthread_mutex_unlock(&f_lock);
}

void fc_apps_status(cJSON *apps, cJSON *summary, cJSON *custom_info)
{
    cJSON *a = cJSON_CreateObject();
    cJSON_AddStringToObject(a, "name", "alarm");
    cJSON_AddStringToObject(a, "status", "Running");
    cJSON_AddItemToArray(apps, a);
    cJSON_AddStringToObject(custom_info, "app.alarm", "ok");
}

/* The server sends stdin: StreamResponse{payload: 0, bytes} in a gRPC frame. */
static void type_in(const char *text)
{
    size_t tl = strlen(text), plen = 1 + tl;
    uint8_t m[300];
    size_t n = 5;
    m[n++] = 0x0a;
    m[n++] = (uint8_t)plen;
    m[n++] = CH_STDIN;
    memcpy(m + n, text, tl);
    n += tl;
    m[0] = 0;
    m[1] = m[2] = m[3] = 0;
    m[4] = (uint8_t)(n - 5);
    pthread_mutex_lock(&f_lock);
    memcpy(f_in + f_in_len, m, n);
    f_in_len += n;
    pthread_mutex_unlock(&f_lock);
}

static void raw_in(const void *b, size_t n)
{
    pthread_mutex_lock(&f_lock);
    memcpy(f_in + f_in_len, b, n);
    f_in_len += n;
    pthread_mutex_unlock(&f_lock);
}

/* What the device sent on a channel, joined. */
static const char *sent(uint8_t channel)
{
    static char text[16384];
    size_t t = 0;
    pthread_mutex_lock(&f_lock);
    for (size_t at = 0; at + 5 <= f_out_len;) {
        uint32_t len = (uint32_t)f_out[at + 1] << 24 | (uint32_t)f_out[at + 2] << 16 | (uint32_t)f_out[at + 3] << 8 |
                       f_out[at + 4];
        const uint8_t *m = f_out + at + 5;
        size_t p = 1, plen = m[p++];
        if (plen & 0x80) {
            plen = (plen & 0x7f) | (size_t)m[p++] << 7;
        }
        if (m[0] == 0x0a && m[p] == channel) {
            memcpy(text + t, m + p + 1, plen - 1);
            t += plen - 1;
        }
        at += 5 + len;
    }
    pthread_mutex_unlock(&f_lock);
    text[t] = '\0';
    return text;
}

static void reset(void)
{
    fc_console_stop();
    pthread_mutex_lock(&f_lock);
    f_opened = f_closed = f_ended = f_server_gone = 0;
    f_out_len = f_in_len = f_in_at = 0;
    f_headers[0] = '\0';
    pthread_mutex_unlock(&f_lock);
    pthread_mutex_lock(&s_lock);
    memset(s_seen, 0, sizeof(s_seen));
    s_seen_next = 0;
    s_seen_loaded = 0;
    pthread_mutex_unlock(&s_lock);
    stub_reset();
    fc_console_set_device("dev-1");
}

static int session(const char *id, const char *metadata)
{
    char json[600];
    cJSON *meta = cJSON_CreateString(metadata);
    char *quoted = cJSON_PrintUnformatted(meta);
    snprintf(json, sizeof(json), "[{\"sessionID\": \"%s\", \"sessionMetadata\": %s}]", id, quoted);
    free(quoted);
    cJSON_Delete(meta);
    cJSON *list = cJSON_Parse(json);
    fc_http_target_t t = {.host = "srv", .port = 443};
    int rc = fc_console_sync(list, &t);
    cJSON_Delete(list);
    return rc;
}

static int wait_closed(int n)
{
    for (int i = 0; i < 300; i++) {
        pthread_mutex_lock(&f_lock);
        int c = f_closed;
        pthread_mutex_unlock(&f_lock);
        if (c >= n) {
            return 1;
        }
        usleep(10 * 1000);
    }
    return 0;
}

#define V5 "\"protocols\": [\"v5.channel.k8s.io\"]"

/* ---- tests --------------------------------------------------------------------------- */

static void one_command_runs_and_exits(void)
{
    reset();
    CHECK_INT(session("s1", "{\"command\": {\"command\": \"status\"}, " V5 "}"), 1);
    CHECK(wait_closed(1));
    CHECK(strstr(f_headers, "/flightctl.v1.RouterService/Stream application/grpc") != NULL);
    CHECK(strstr(f_headers, "session-id=s1 client-name=dev-1 selected-protocol=v5.channel.k8s.io") != NULL);
    CHECK(strstr(sent(CH_STDOUT), "device     dev-1") != NULL);
    CHECK_STR(sent(CH_ERROR), "{\"metadata\":{},\"status\":\"Success\"}");
    CHECK_INT(f_ended, 1);
    CHECK(strchr(sent(CH_STDOUT), '\r') == NULL); /* no terminal: plain newlines */
}

static void an_unknown_command_fails_with_127(void)
{
    reset();
    session("s2", "{\"command\": {\"command\": \"journalctl\", \"args\": [\"-f\"]}, " V5 "}");
    CHECK(wait_closed(1));
    CHECK(strstr(sent(CH_STDOUT), "journalctl -f: not a command of this device") != NULL);
    CHECK_STR(sent(CH_ERROR), "{\"metadata\":{},\"status\":\"Failure\",\"reason\":\"NonZeroExitCode\","
                              "\"details\":{\"causes\":[{\"reason\":\"ExitCode\",\"message\":\"127\"}]},\"code\":127}");
}

static void a_terminal_gets_a_prompt_and_line_editing(void)
{
    reset();
    session("s3", "{\"tty\": true, " V5 "}");
    type_in("hepl\x7f\x7flp\r"); /* typed wrong, corrected */
    type_in("apps\r");
    type_in("\x1b[Aexit\r"); /* an arrow key, ignored */
    CHECK(wait_closed(1));
    const char *o = sent(CH_STDOUT);
    CHECK(strstr(o, "console on dev-1") != NULL);
    CHECK(strstr(o, "fc> ") != NULL);
    CHECK(strstr(o, "\b \b") != NULL);
    CHECK(strstr(o, "reboot   restart the device\r\n") != NULL);
    CHECK(strstr(o, "alarm            Running    ok\r\n") != NULL);
    CHECK_STR(sent(CH_ERROR), "{\"metadata\":{},\"status\":\"Success\"}");
}

static void a_session_seen_is_not_run_again_even_after_a_restart(void)
{
    reset();
    session("s4", "{\"command\": {\"command\": \"help\"}, " V5 "}");
    CHECK(wait_closed(1));
    CHECK_INT(session("s4", "{\"command\": {\"command\": \"help\"}, " V5 "}"), 1); /* listed, */
    CHECK_INT(f_opened, 1);                                                         /* not run */
    pthread_mutex_lock(&s_lock);
    memset(s_seen, 0, sizeof(s_seen));
    s_seen_loaded = 0; /* a restart: what was seen comes from storage */
    pthread_mutex_unlock(&s_lock);
    session("s4", "{\"command\": {\"command\": \"help\"}, " V5 "}");
    CHECK_INT(f_opened, 1);
    CHECK_STR(stub_kv(KV_SEEN), "s4");
}

static void a_second_session_is_refused(void)
{
    reset();
    session("s5", "{\"tty\": true, " V5 "}");
    session("s6", "{\"command\": {\"command\": \"status\"}, " V5 "}");
    CHECK(wait_closed(1)); /* the second, at once */
    CHECK(strstr(sent(CH_STDOUT), "another console session is open") != NULL);
    pthread_mutex_lock(&f_lock);
    f_server_gone = 1; /* the user of the first leaves */
    pthread_mutex_unlock(&f_lock);
    CHECK(wait_closed(2));
}

static void reboot_restarts_after_the_status(void)
{
    reset();
    session("s7", "{\"command\": {\"command\": \"reboot\"}, " V5 "}");
    CHECK(wait_closed(1));
    for (int i = 0; i < 300 && stub_reboots == 0; i++) {
        usleep(10 * 1000);
    }
    CHECK_INT(stub_reboots, 1);
    CHECK_STR(sent(CH_ERROR), "{\"metadata\":{},\"status\":\"Success\"}");
    CHECK_STR(stub_kv(KV_SEEN), "s7"); /* kept before it ran */
}

static void no_protocol_no_selected_protocol(void)
{
    reset();
    session("s8", "{\"command\": {\"command\": \"help\"}}");
    CHECK(wait_closed(1));
    CHECK(strstr(f_headers, "selected-protocol") == NULL);
}

static void bad_messages_end_the_session(void)
{
    reset();
    session("s9", "{\"tty\": true, " V5 "}");
    uint8_t compressed[] = {1, 0, 0, 0, 1, 0x00};
    raw_in(compressed, sizeof(compressed));
    CHECK(wait_closed(1));
    reset();
    session("s10", "{\"tty\": true, " V5 "}");
    uint8_t huge[] = {0, 0, 0x10, 0, 0}; /* 1 MB announced */
    raw_in(huge, sizeof(huge));
    CHECK(wait_closed(1));
    reset();
    session("s11", "{\"tty\": true, " V5 "}");
    uint8_t cut[] = {0, 0, 0, 0, 3, 0x0a, 0x7f, 0x00}; /* payload of 127 bytes, 1 there */
    raw_in(cut, sizeof(cut));
    CHECK(wait_closed(1));
    reset();
}

int main(void)
{
    RUN(one_command_runs_and_exits);
    RUN(an_unknown_command_fails_with_127);
    RUN(a_terminal_gets_a_prompt_and_line_editing);
    RUN(a_session_seen_is_not_run_again_even_after_a_restart);
    RUN(a_second_session_is_refused);
    RUN(reboot_restarts_after_the_status);
    RUN(no_protocol_no_selected_protocol);
    RUN(bad_messages_end_the_session);
    return TEST_RESULT();
}
