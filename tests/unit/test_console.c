/*
 * The remote console (fc_console.c) against a fake stream: the headers it
 * opens with, one command per session, the prompt with its line editing,
 * the exit status as Kubernetes words it, sessions remembered across a
 * restart, a second session refused, reboot, and bad input from the server.
 * And the Logs tab of Flight Control's UI: the commands it sends, built here
 * as its deviceLogCommandBuilder.ts builds them, answered from the log in RAM.
 */
#define NO_USER_S 1 /* the tests do not wait a minute */
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
static uint8_t f_in[16384]; /* what the server has for it */
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

/* The server sends a channel's bytes: StreamResponse{payload: channel, bytes}
 * in a gRPC frame. */
static void send_in(uint8_t channel, const void *data, size_t tl)
{
    size_t plen = 1 + tl;
    uint8_t m[2100];
    size_t n = 5;
    m[n++] = 0x0a;
    if (plen >= 0x80) {
        m[n++] = (uint8_t)(plen | 0x80);
        m[n++] = (uint8_t)(plen >> 7);
    } else {
        m[n++] = (uint8_t)plen;
    }
    m[n++] = channel;
    memcpy(m + n, data, tl);
    n += tl;
    size_t body = n - 5;
    m[0] = 0;
    m[1] = 0;
    m[2] = (uint8_t)(body >> 16);
    m[3] = (uint8_t)(body >> 8);
    m[4] = (uint8_t)body;
    pthread_mutex_lock(&f_lock);
    memcpy(f_in + f_in_len, m, n);
    f_in_len += n;
    pthread_mutex_unlock(&f_lock);
}

static void type_in(const char *text)
{
    send_in(CH_STDIN, text, strlen(text));
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
    for (int i = 0; i < 3000; i++) { /* 30 s at most, however slow the machine */
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
    CHECK_INT(session("s1", "{\"command\": {\"command\": \"status\"}, " V5 "}"), 2); /* new */
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
    session("s2", "{\"command\": {\"command\": \"uptime\", \"args\": [\"-p\"]}, " V5 "}");
    CHECK(wait_closed(1));
    CHECK(strstr(sent(CH_STDOUT), "uptime: not a command of this device") != NULL);
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
    for (int i = 0; i < 3000 && stub_reboots == 0; i++) {
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


/* ---- the Logs tab ------------------------------------------------------------------ */

/* quoteShellArg of deviceLogCommandBuilder.ts. */
static size_t ui_quote(char *o, const char *a)
{
    size_t n = 0;
    o[n++] = '\'';
    for (; *a; a++) {
        if (*a == '\'') {
            memcpy(o + n, "'\\''", 4);
            n += 4;
        } else {
            o[n++] = *a;
        }
    }
    o[n++] = '\'';
    o[n] = '\0';
    return n;
}

/* getRetrieveLogContentCommand for the journal: args are the filters, as
 * the builder puts them (unit, time, priority), live the follow mode. */
static void ui_journal(char *cmd, const char *const *args, const char *const *follow_args, int live)
{
    char journal[1024], inner[1500], quoted[2000];
    size_t n = (size_t)sprintf(journal, "journalctl");
    for (const char *const *a = args; *a; a++) {
        journal[n++] = ' ';
        n += ui_quote(journal + n, *a);
    }
    n += (size_t)sprintf(journal + n, " '--no-pager'");
    n = (size_t)sprintf(inner, "set -o pipefail; %s 2>&1 | tail -n %d", journal, live ? 10000 : 100000);
    if (live) {
        n += (size_t)sprintf(inner + n, " && journalctl");
        for (const char *const *a = follow_args; *a; a++) {
            inner[n++] = ' ';
            n += ui_quote(inner + n, *a);
        }
        sprintf(inner + n, " '--since' 'now' '--no-pager' -f");
    }
    ui_quote(quoted, inner);
    if (live) {
        sprintf(cmd, "sudo --non-interactive bash -c %s 2>&1\n", quoted);
    } else {
        sprintf(cmd, "sudo --non-interactive bash -c %s 2>&1\nprintf '%%s %%s\\n' '__FLIGHTCTL_DEVICE_LOGS_EOF__' \"$?\"\n",
                quoted);
    }
}

#define LOGS_META "{\"tty\": false, \"term\": \"xterm-256color\", \"command\": {\"command\": \"/bin/bash\", \"args\": [\"-s\"]}, " V5 "}"
#define FOOTER    "__FLIGHTCTL_DEVICE_LOGS_EOF__"

static int count_of(const char *hay, const char *needle)
{
    int c = 0;
    for (const char *p = strstr(hay, needle); p != NULL; p = strstr(p + 1, needle)) {
        c++;
    }
    return c;
}

/* Waits for the n-th footer; the output from the (n-1)-th to it. */
static const char *answer(int nth)
{
    static char one[16384];
    for (int i = 0; i < 3000 && count_of(sent(CH_STDOUT), FOOTER) < nth; i++) {
        usleep(10 * 1000);
    }
    const char *o = sent(CH_STDOUT);
    for (int i = 1; i < nth && o != NULL; i++) {
        o = strstr(o, FOOTER);
        o = o ? strchr(o, '\n') + 1 : NULL;
    }
    const char *end = o ? strstr(o, FOOTER) : NULL;
    end = end ? strchr(end, '\n') : NULL;
    snprintf(one, sizeof(one), "%.*s", end ? (int)(end + 1 - o) : 0, end ? o : "");
    return one;
}

static void the_logs_tab_gets_the_log_on_one_session(void)
{
    reset();
    fc_log_put(FC_LOG_INFO, "agent started L1");
    fc_log_put(FC_LOG_WARN, "app alarm: too hot L1");
    fc_log_put(FC_LOG_ERROR, "tls: handshake failed L1");
    CHECK_INT(session("l1", LOGS_META), 2);
    char cmd[2100];
    /* The agent's log, every level (the tab's first search). */
    static const char *const agent[] = {"-u", "flightctl-agent.service", NULL};
    ui_journal(cmd, agent, NULL, 0);
    CHECK(strstr(cmd, "sudo --non-interactive bash -c 'set -o pipefail; journalctl '\\''-u'\\'' "
                      "'\\''flightctl-agent.service'\\'' '\\''--no-pager'\\'' 2>&1 | tail -n 100000' 2>&1\n"
                      "printf '%s %s\\n' '__FLIGHTCTL_DEVICE_LOGS_EOF__' \"$?\"\n") == cmd);
    type_in(cmd);
    const char *o = answer(1);
    CHECK(strstr(o, " I fc: agent started L1\n") != NULL);
    CHECK(strstr(o, " W fc: app alarm: too hot L1\n") != NULL);
    CHECK(strstr(o, " E fc: tls: handshake failed L1\n") != NULL);
    CHECK(strstr(o, FOOTER " 0\n") != NULL);
    /* Warning and above, on the same session. */
    static const char *const warn[] = {"-u", "flightctl-agent.service", "-p", "warning", NULL};
    ui_journal(cmd, warn, NULL, 0);
    type_in(cmd);
    o = answer(2);
    CHECK(strstr(o, "agent started L1") == NULL);
    CHECK(strstr(o, "too hot L1") != NULL && strstr(o, "handshake failed L1") != NULL);
    /* A unit: an app's lines are its NAME.service's. */
    static const char *const app[] = {"-u", "alarm.service", NULL};
    ui_journal(cmd, app, NULL, 0);
    type_in(cmd);
    o = answer(3);
    CHECK(strstr(o, "too hot L1") != NULL && strstr(o, "handshake failed L1") == NULL);
    /* System, no unit: everything; a unit with a glob. */
    static const char *const glob[] = {"-u", "flightctl-*", NULL};
    ui_journal(cmd, glob, NULL, 0);
    type_in(cmd);
    o = answer(4);
    CHECK(strstr(o, "agent started L1") != NULL);
    /* Nothing: another unit, another boot, a range in the past. */
    static const char *const sshd[] = {"-u", "sshd.service", NULL};
    static const char *const prev[] = {"-u", "flightctl-agent.service", "-b", "-1", NULL};
    static const char *const past[] = {"--since", "2001-01-01", "--until", "2001-01-03", NULL};
    const char *const *empty[] = {sshd, prev, past};
    for (int i = 0; i < 3; i++) {
        ui_journal(cmd, empty[i], NULL, 0);
        type_in(cmd);
        CHECK_STR(answer(5 + i), FOOTER " 0\n");
    }
    /* This boot, the last hour, today. */
    char today[16];
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(today, sizeof(today), "%Y-%m-%d", &tm);
    static const char *const boot[] = {"-b", "0", NULL};
    static const char *const hour[] = {"--since", "1 hour ago", "-p", "err", NULL};
    const char *const day[] = {"--since", today, NULL};
    const char *const *some[] = {boot, hour, day};
    for (int i = 0; i < 3; i++) {
        ui_journal(cmd, some[i], NULL, 0);
        type_in(cmd);
        o = answer(8 + i);
        CHECK(strstr(o, "handshake failed L1") != NULL);
    }
    CHECK(strstr(answer(9), "agent started L1") == NULL); /* -p err */
    pthread_mutex_lock(&f_lock);
    CHECK_INT(f_closed, 0); /* the session stays for the next search */
    f_server_gone = 1;
    pthread_mutex_unlock(&f_lock);
    CHECK(wait_closed(1));
}

static void the_logs_tab_is_told_there_are_no_files(void)
{
    reset();
    session("l2", LOGS_META);
    /* getFileProbeCommand, then the file's tail. */
    char probe[600], path[200];
    ui_quote(path, "/var/log/messages");
    char script[400];
    ui_quote(script, "path=$1\n"
                     "[[ -e \"$path\" ]] && echo \"exists=1\" || echo \"exists=0\"\n"
                     "[[ -f \"$path\" ]] && echo \"regular=1\" || echo \"regular=0\"\n"
                     "echo \"size=$(stat -c %s -- \"$path\" 2>/dev/null)\"\n"
                     "echo \"mime=$(file -b --mime-type -- \"$path\" 2>/dev/null)\"");
    snprintf(probe, sizeof(probe),
             "sudo --non-interactive bash -c %s bash %s 2>&1\nprintf '%%s %%s\\n' '" FOOTER "' \"$?\"\n", script, path);
    type_in(probe);
    CHECK_STR(answer(1), "exists=0\nregular=0\nsize=\nmime=\n" FOOTER " 0\n");
    type_in("sudo --non-interactive 'tail' '-n' '100000' '/var/log/messages' 2>&1\n"
            "printf '%s %s\\n' '" FOOTER "' \"$?\"\n");
    CHECK_STR(answer(2), "tail: cannot open '/var/log/messages' for reading: No such file or directory\n" FOOTER " 1\n");
    reset();
}

static void live_logs_follow_until_the_user_leaves(void)
{
    reset();
    fc_log_put(FC_LOG_INFO, "before L3");
    session("l3", LOGS_META);
    static const char *const agent[] = {"-u", "flightctl-agent.service", NULL};
    char cmd[2100];
    ui_journal(cmd, agent, agent, 1);
    type_in(cmd);
    for (int i = 0; i < 3000 && strstr(sent(CH_STDOUT), "before L3") == NULL; i++) {
        usleep(10 * 1000);
    }
    /* The follow starts a moment after the first part's lines, and shows
     * only what comes after it: a line again and again until one is seen,
     * however slow the machine. */
    for (int i = 0; i < 3000 && strstr(sent(CH_STDOUT), "after L3") == NULL; i++) {
        if (i % 10 == 0) {
            fc_log_put(FC_LOG_WARN, "after L3");
        }
        usleep(10 * 1000);
    }
    const char *o = sent(CH_STDOUT);
    CHECK(strstr(o, "before L3") != NULL);
    CHECK(strstr(o, " W fc: after L3\n") != NULL);
    CHECK_INT(count_of(o, "before L3"), 1); /* --since now: not again */
    CHECK(strstr(o, FOOTER) == NULL);
    type_in("printf x\n"); /* while it follows, input waits */
    usleep(300 * 1000);
    CHECK(strstr(sent(CH_STDOUT), "x") == NULL || strstr(sent(CH_STDOUT), "\nx") == NULL);
    pthread_mutex_lock(&f_lock);
    CHECK_INT(f_closed, 0);
    f_server_gone = 1;
    pthread_mutex_unlock(&f_lock);
    CHECK(wait_closed(1));
}

/* One journalctl as a command of its own: its output. */
static const char *journal_of(const char *id, const char *args)
{
    static char out[8192];
    reset();
    char meta[300];
    snprintf(meta, sizeof(meta), "{\"command\": {\"command\": \"journalctl\", \"args\": [%s]}, " V5 "}", args);
    session(id, meta);
    CHECK(wait_closed(1));
    snprintf(out, sizeof(out), "%s", sent(CH_STDOUT));
    return out;
}

/* The system's lines (ESP-IDF's, Zephyr's) are the kernel's, no unit's;
 * a restart keeps the previous boot's lines, journalctl's boot -1, and the
 * one before it goes. */
static void the_previous_boot_and_the_systems_lines(void)
{
    fc_log_put(FC_LOG_INFO, "agent B1");
    fc_log_put_system(FC_LOG_WARN, "wifi: disconnected B1");
    fc_log_put_fatal("fatal: crashed B1");
    fc_log_restart();
    fc_log_put(FC_LOG_INFO, "agent B2");
    fc_log_put_system(FC_LOG_INFO, "wifi: connected B2");
    const char *o = journal_of("b1", "");
    CHECK(strstr(o, " W wifi: disconnected B1\n") != NULL);
    CHECK(strstr(o, " E fatal: crashed B1\n") != NULL);
    CHECK(strstr(o, " I fc: agent B2\n") != NULL);
    CHECK(strstr(o, "disconnected B1") < strstr(o, "agent B2"));
    o = journal_of("b2", "\"-b\", \"-1\"");
    CHECK(strstr(o, "agent B1") != NULL && strstr(o, "crashed B1") != NULL && strstr(o, "B2") == NULL);
    o = journal_of("b3", "\"-b\"");
    CHECK(strstr(o, "B1") == NULL && strstr(o, "agent B2") != NULL && strstr(o, "connected B2") != NULL);
    o = journal_of("b4", "\"-u\", \"flightctl-agent.service\", \"-b\", \"-1\"");
    CHECK(strstr(o, "agent B1") != NULL && strstr(o, "wifi") == NULL && strstr(o, "fatal") == NULL);
    o = journal_of("b5", "\"-k\"");
    CHECK(strstr(o, "connected B2") != NULL && strstr(o, "agent") == NULL && strstr(o, "B1") == NULL);
    o = journal_of("b6", "\"-b\", \"-2\"");
    CHECK(strstr(o, "B1") == NULL && strstr(o, "B2") == NULL);
    /* Another restart: B2 is the previous boot, B1 is gone. */
    fc_log_restart();
    fc_log_put(FC_LOG_INFO, "agent B3");
    o = journal_of("b7", "");
    CHECK(strstr(o, "B1") == NULL && strstr(o, "agent B2") != NULL && strstr(o, "agent B3") != NULL);
    o = journal_of("b8", "\"-b\", \"-1\"");
    CHECK(strstr(o, "agent B2") != NULL && strstr(o, "B3") == NULL);
    reset();
}

static void a_script_ends_with_its_input_and_its_last_code(void)
{
    reset();
    for (int i = 0; i < 5; i++) {
        char t[32];
        snprintf(t, sizeof(t), "line %d L4", i);
        fc_log_put(FC_LOG_INFO, t);
    }
    session("l4", LOGS_META);
    type_in("journalctl --no-pager | tail -n 2; echo done\n");
    type_in("false");
    send_in(CH_CLOSE, "\0", 1); /* stdin closed, the last line without its newline */
    CHECK(wait_closed(1));
    const char *o = sent(CH_STDOUT);
    CHECK(strstr(o, "line 2 L4") == NULL && strstr(o, "line 3 L4") != NULL && strstr(o, "line 4 L4\ndone\n") != NULL);
    CHECK(strstr(sent(CH_ERROR), "\"code\":1") != NULL);
}

static void log_at_the_prompt_and_as_one_command(void)
{
    reset();
    fc_log_put(FC_LOG_ERROR, "an error L5");
    fc_log_put(FC_LOG_INFO, "some info L5");
    session("l5", "{\"tty\": true, " V5 "}");
    type_in("log -n 1\r");
    type_in("exit\r");
    CHECK(wait_closed(1));
    const char *o = sent(CH_STDOUT);
    CHECK(strstr(o, " I fc: some info L5\r\n") != NULL && strstr(o, "an error L5") == NULL);
    reset();
    session("l6", "{\"command\": {\"command\": \"journalctl\", \"args\": [\"-p\", \"err\", \"--since\", \"10 min ago\"]}, " V5 "}");
    CHECK(wait_closed(1));
    o = sent(CH_STDOUT);
    CHECK(strstr(o, "an error L5") != NULL && strstr(o, "some info L5") == NULL);
    CHECK_STR(sent(CH_ERROR), "{\"metadata\":{},\"status\":\"Success\"}");
    reset();
    session("l7", "{\"command\": {\"command\": \"journalctl\", \"args\": [\"--since\", \"yesterday at noon\"]}, " V5 "}");
    CHECK(wait_closed(1));
    CHECK(strstr(sent(CH_STDOUT), "failed to parse timestamp: yesterday at noon") != NULL);
    CHECK(strstr(sent(CH_ERROR), "\"code\":1") != NULL);
}

static void a_shell_line_is_split_as_bash_would(void)
{
    reset();
    session("l8", LOGS_META);
    type_in("echo 'a;b' \"c d\" e\\ f; false || echo or-$?; true && echo and; echo \"$(date)x\" ${HOME}y # comment\n");
    type_in("printf '%s=%d\\n' k 1 j 2; [ a = a ] && echo eq; test -n '' || echo empty; nosuch 2>/dev/null\n");
    type_in("printf '%s\\n' \"$?\"\n");
    type_in("echo '");
    type_in("multi\nline'\n");
    for (int i = 0; i < 3000 && strstr(sent(CH_STDOUT), "multi\nline\n") == NULL; i++) {
        usleep(10 * 1000);
    }
    const char *o = sent(CH_STDOUT);
    CHECK(strstr(o, "a;b c d e f\nor-1\nand\nx y\nk=1\nj=2\neq\nempty\nnosuch: not a command") == o);
    CHECK(strstr(o, "(help lists them)\n127\nmulti\nline\n") != NULL);
    reset();
}

/* A session the server still lists after its user left (the device
 * restarted while it was open): nothing comes, and it ends on its own, so
 * that the server lets go of it and new sessions are not refused. Sixteen
 * are remembered, more than the server keeps listed. */
static void a_session_nobody_joins_ends(void)
{
    reset();
    session("n1", LOGS_META);
    CHECK(wait_closed(1));
    CHECK_STR(sent(CH_ERROR), "{\"metadata\":{},\"status\":\"Success\"}");
    reset();
    char id[8];
    for (int i = 0; i < 16; i++) {
        snprintf(id, sizeof(id), "m%d", i);
        session(id, "{\"command\": {\"command\": \"help\"}, " V5 "}");
        CHECK(wait_closed(i + 1));
    }
    session("m0", "{\"command\": {\"command\": \"help\"}, " V5 "}"); /* the oldest, still known */
    CHECK_INT(f_opened, 16);
}

static void journal_parts(void)
{
    CHECK(glob_match("flightctl-*.service", AGENT_UNIT));
    CHECK(glob_match("flightctl-agent@*.service", "flightctl-agent@x.service"));
    CHECK(glob_match("[fg]light?tl-agent.service", AGENT_UNIT));
    CHECK(!glob_match("[!f]light*", AGENT_UNIT));
    CHECK(!glob_match("*-agent", AGENT_UNIT));
    CHECK(unit_is("flightctl-agent", AGENT_UNIT));
    CHECK(!glob_match("a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*a*b", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"));
    time_t t;
    int now;
    CHECK_INT(parse_when("2026-11-16", 0, &t, &now), 0);
    CHECK_INT(t, 1794787200);
    CHECK_INT(parse_when("2026-11-16 10:34:54", 0, &t, &now), 0);
    CHECK_INT(t, 1794787200 + 10 * 3600 + 34 * 60 + 54);
    CHECK_INT(parse_when("7 days ago", 1000000, &t, &now), 0);
    CHECK_INT(t, 1000000 - 7 * 86400);
    CHECK_INT(parse_when("-1h", 10000, &t, &now), 0);
    CHECK_INT(t, 10000 - 3600);
    CHECK_INT(parse_when("now", 5, &t, &now), 0);
    CHECK_INT(now, 1);
    CHECK_INT(parse_when("1 fortnight ago", 5, &t, &now), -1);
    CHECK_INT(parse_when("2026-13-01", 5, &t, &now), -1);
    CHECK_INT(parse_when("3 hours", 5, &t, &now), -1);
}

static void the_ring_drops_its_oldest_lines(void)
{
    char big[400];
    memset(big, 'x', sizeof(big) - 1);
    big[sizeof(big) - 1] = '\0';
    uint32_t start = fc_log_end();
    for (int i = 0; i < 100; i++) {
        fc_log_put(FC_LOG_INFO, big);
    }
    fc_log_put(FC_LOG_INFO, "last\n");
    CHECK_INT(fc_log_end(), start + 101);
    uint32_t seq = start;
    fc_log_entry_t e;
    int kept = 0;
    uint32_t first = 0;
    while (fc_log_next(&seq, &e) == 0) {
        if (kept++ == 0) {
            first = e.seq;
        }
        if (e.seq != start + 100) {
            CHECK_INT(strlen(e.text), FC_LOG_TEXT_MAX); /* cut */
        }
    }
    CHECK_STR(e.text, "last"); /* without its newline */
    CHECK_INT(kept, (int)(start + 101 - first));
    CHECK(kept >= FC_LOG_RING / (FC_LOG_TEXT_MAX + 7) - 1 && kept <= FC_LOG_RING / (FC_LOG_TEXT_MAX + 7) + 1);
    CHECK_INT(fc_log_next(&seq, &e), -1);
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
    RUN(the_logs_tab_gets_the_log_on_one_session);
    RUN(the_logs_tab_is_told_there_are_no_files);
    RUN(live_logs_follow_until_the_user_leaves);
    RUN(a_script_ends_with_its_input_and_its_last_code);
    RUN(log_at_the_prompt_and_as_one_command);
    RUN(a_shell_line_is_split_as_bash_would);
    RUN(a_session_nobody_joins_ends);
    RUN(journal_parts);
    RUN(the_ring_drops_its_oldest_lines);
    RUN(the_previous_boot_and_the_systems_lines);
    return TEST_RESULT();
}
