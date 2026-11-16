/*
 * The remote console (fc_console.h): a gRPC stream on the agent's
 * connection, Kubernetes' remote command protocol in it, and a closed set of
 * commands for what a shell would be.
 */
#include "fc_console.h"

#include <inttypes.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "fc_apps.h"
#include "fc_port.h"

#define PROTOCOL    "v5.channel.k8s.io"
#define STREAM_PATH "/flightctl.v1.RouterService/Stream"
#define CH_STDIN  0
#define CH_STDOUT 1
#define CH_ERROR  3 /* the exit status, as Kubernetes' metav1.Status */
#define CH_RESIZE 4
#define CH_CLOSE  255

#define IDLE_S       600  /* a session with no input for this long ends */
#define MSG_MAX      1024 /* the largest gRPC message taken from the server */
#define CONSOLE_LINE     96
#define OUT_PIECE    768  /* stdout bytes per message */
#define SEEN_MAX     4
#define SEEN_ID_MAX  40
#define KV_SEEN      "console_seen"
#define THREAD_STACK (6 * 1024)

typedef struct {
    fc_http_stream_t *st;
    int tty;
    char *command; /* the session's one command, or NULL: a prompt */
    char line[CONSOLE_LINE];
    size_t line_len;
    int esc;      /* inside an escape sequence: 1 after ESC, 2 after ESC [ */
    uint8_t in[MSG_MAX + 5];
    size_t in_len;
    int done;     /* nothing more to read: the user left, or ended it */
    int finished; /* the exit status and the end of the stream went out */
    int exit_code;
    int reboot;
    time_t last_input;
} session_t;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static char s_device[64];
static char s_seen[SEEN_MAX][SEEN_ID_MAX];
static int s_seen_next, s_seen_loaded;
static int s_running;  /* the session thread runs */
static int s_joinable; /* it ran, and was not joined */
static volatile int s_stop;
static pthread_t s_thread;
static pthread_attr_t s_attr; /* until joined: on Zephyr it owns the stack */

void fc_console_set_device(const char *name)
{
    pthread_mutex_lock(&s_lock);
    snprintf(s_device, sizeof(s_device), "%s", name ? name : "");
    pthread_mutex_unlock(&s_lock);
}

/* ---- the sessions seen, kept across restarts ----------------------------------- */

static void seen_load_locked(void)
{
    if (s_seen_loaded) {
        return;
    }
    s_seen_loaded = 1;
    char *v = NULL;
    size_t len = 0;
    if (fc_port_kv_get(KV_SEEN, &v, &len) != 0 || v == NULL) {
        return;
    }
    char *save = NULL;
    for (char *id = strtok_r(v, ",", &save); id != NULL && s_seen_next < SEEN_MAX; id = strtok_r(NULL, ",", &save)) {
        snprintf(s_seen[s_seen_next++], SEEN_ID_MAX, "%s", id);
    }
    s_seen_next %= SEEN_MAX;
    free(v);
}

static int seen_locked(const char *id)
{
    for (int i = 0; i < SEEN_MAX; i++) {
        if (strcmp(s_seen[i], id) == 0) {
            return 1;
        }
    }
    return 0;
}

static void remember_locked(const char *id)
{
    snprintf(s_seen[s_seen_next], SEEN_ID_MAX, "%s", id);
    s_seen_next = (s_seen_next + 1) % SEEN_MAX;
    char all[SEEN_MAX * SEEN_ID_MAX + SEEN_MAX];
    size_t n = 0;
    for (int i = 0; i < SEEN_MAX; i++) {
        if (s_seen[i][0]) {
            n += (size_t)snprintf(all + n, sizeof(all) - n, "%s%s", n ? "," : "", s_seen[i]);
        }
    }
    fc_port_kv_set(KV_SEEN, all, n);
}

/* ---- the wire: gRPC messages, protobuf ----------------------------------------------- */

/* Sends StreamRequest{payload: channel, data}. */
static int send_payload(session_t *s, uint8_t channel, const void *data, size_t len)
{
    uint8_t msg[5 + 3 + 1 + OUT_PIECE];
    const uint8_t *p = data;
    do {
        size_t k = len < OUT_PIECE ? len : OUT_PIECE;
        size_t plen = 1 + k, n = 5;
        msg[n++] = 0x0a; /* field 1, bytes */
        if (plen >= 0x80) {
            msg[n++] = (uint8_t)(plen | 0x80);
            msg[n++] = (uint8_t)(plen >> 7);
        } else {
            msg[n++] = (uint8_t)plen;
        }
        msg[n++] = channel;
        memcpy(msg + n, p, k);
        n += k;
        uint32_t body = (uint32_t)(n - 5);
        msg[0] = 0; /* not compressed */
        msg[1] = (uint8_t)(body >> 24);
        msg[2] = (uint8_t)(body >> 16);
        msg[3] = (uint8_t)(body >> 8);
        msg[4] = (uint8_t)body;
        if (fc_http_stream_write(s->st, msg, n, 0, 2000) != 0) {
            s->done = 1;
            return -1;
        }
        p += k;
        len -= k;
    } while (len > 0);
    return 0;
}

/* Text on stdout; at a terminal, "\n" goes as "\r\n". */
static void out(session_t *s, const char *text)
{
    if (!s->tty) {
        send_payload(s, CH_STDOUT, text, strlen(text));
        return;
    }
    char buf[OUT_PIECE];
    size_t n = 0;
    for (const char *c = text; *c; c++) {
        if (n + 2 > sizeof(buf)) {
            send_payload(s, CH_STDOUT, buf, n);
            n = 0;
        }
        if (*c == '\n') {
            buf[n++] = '\r';
        }
        buf[n++] = *c;
    }
    if (n > 0) {
        send_payload(s, CH_STDOUT, buf, n);
    }
}

/* The exit status, as the Kubernetes API server words it, then the end of
 * the stream from this side. */
static void finish(session_t *s, int code)
{
    if (s->finished) {
        return;
    }
    s->finished = 1;
    s->done = 1;
    char st[200];
    if (code == 0) {
        snprintf(st, sizeof(st), "{\"metadata\":{},\"status\":\"Success\"}");
    } else {
        snprintf(st, sizeof(st),
                 "{\"metadata\":{},\"status\":\"Failure\",\"reason\":\"NonZeroExitCode\","
                 "\"details\":{\"causes\":[{\"reason\":\"ExitCode\",\"message\":\"%d\"}]},\"code\":%d}",
                 code, code);
    }
    if (send_payload(s, CH_ERROR, st, strlen(st)) == 0) {
        fc_http_stream_write(s->st, "", 0, 1, 2000);
    }
}

/* ---- commands --------------------------------------------------------------------- */

static void cmd_help(session_t *s)
{
    out(s, "help     these commands\n"
           "status   the device: firmware, memory, storage, network\n"
           "apps     the WASM apps and their state\n"
           "reboot   restart the device\n"
           "exit     end the session\n");
}

static void put_value(char *buf, size_t cap, const cJSON *v)
{
    if (cJSON_IsString(v)) {
        snprintf(buf, cap, "%s", v->valuestring);
    } else if (cJSON_IsNumber(v)) {
        snprintf(buf, cap, "%.0f", v->valuedouble);
    } else {
        snprintf(buf, cap, "-");
    }
}

static void cmd_status(session_t *s)
{
    fc_sysinfo_t si;
    memset(&si, 0, sizeof(si));
    fc_port_sysinfo(&si);
    char line[160];
    pthread_mutex_lock(&s_lock);
    snprintf(line, sizeof(line), "device     %s (%s)\n", s_device, si.hostname);
    pthread_mutex_unlock(&s_lock);
    out(s, line);
    snprintf(line, sizeof(line), "system     %s %s on %s\n", si.operating_system, si.os_version, si.architecture);
    out(s, line);
    snprintf(line, sizeof(line), "memory     %" PRIu64 " of %" PRIu64 " bytes free\n", si.mem_free, si.mem_total);
    out(s, line);
    snprintf(line, sizeof(line), "storage    %" PRIu64 " of %" PRIu64 " bytes free\n", si.disk_free, si.disk_total);
    out(s, line);
    cJSON *ci = cJSON_CreateObject();
    if (ci != NULL) {
        fc_port_custom_info(ci);
        const cJSON *item;
        cJSON_ArrayForEach(item, ci) {
            char v[96];
            put_value(v, sizeof(v), item);
            snprintf(line, sizeof(line), "%-22s %s\n", item->string, v);
            out(s, line);
        }
        cJSON_Delete(ci);
    }
}

static void cmd_apps(session_t *s)
{
    cJSON *apps = cJSON_CreateArray(), *summary = cJSON_CreateObject(), *ci = cJSON_CreateObject();
    if (apps == NULL || summary == NULL || ci == NULL) {
        out(s, "out of memory\n");
    } else {
        fc_apps_status(apps, summary, ci);
        if (cJSON_GetArraySize(apps) == 0) {
            out(s, "no apps\n");
        }
        const cJSON *app;
        cJSON_ArrayForEach(app, apps) {
            const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(app, "name"));
            const char *st = cJSON_GetStringValue(cJSON_GetObjectItem(app, "status"));
            char key[80], state[96], line[220];
            snprintf(key, sizeof(key), "app.%s", name ? name : "");
            put_value(state, sizeof(state), cJSON_GetObjectItem(ci, key));
            snprintf(line, sizeof(line), "%-16s %-10s %s\n", name ? name : "?", st ? st : "?", state);
            out(s, line);
        }
    }
    cJSON_Delete(apps);
    cJSON_Delete(summary);
    cJSON_Delete(ci);
}

/* Runs one command line: its exit code. */
static int run(session_t *s, char *line)
{
    char *cmd = line;
    while (*cmd == ' ' || *cmd == '\t') {
        cmd++;
    }
    size_t n = strlen(cmd);
    while (n > 0 && (cmd[n - 1] == ' ' || cmd[n - 1] == '\t' || cmd[n - 1] == '\r' || cmd[n - 1] == '\n')) {
        cmd[--n] = '\0';
    }
    if (n == 0) {
        return 0;
    }
    if (strcmp(cmd, "help") == 0) {
        cmd_help(s);
    } else if (strcmp(cmd, "status") == 0) {
        cmd_status(s);
    } else if (strcmp(cmd, "apps") == 0) {
        cmd_apps(s);
    } else if (strcmp(cmd, "reboot") == 0) {
        out(s, "restarting\n");
        s->reboot = 1;
        finish(s, 0);
    } else if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0 || strcmp(cmd, "logout") == 0) {
        finish(s, 0);
    } else {
        char msg[160];
        snprintf(msg, sizeof(msg), "%.60s: not a command of this device (help lists them)\n", cmd);
        out(s, msg);
        return 127;
    }
    return 0;
}

static void prompt(session_t *s)
{
    if (s->tty && !s->done) {
        out(s, "fc> ");
    }
}

/* A byte typed at the prompt. */
static void typed(session_t *s, uint8_t c)
{
    if (s->esc == 1) {
        s->esc = c == '[' ? 2 : 0;
        return;
    }
    if (s->esc == 2) {
        s->esc = c >= 0x40 && c <= 0x7e ? 0 : 2; /* the sequence's last byte */
        return;
    }
    if (c == '\r' || c == '\n') {
        if (c == '\n' && s->tty) {
            return; /* a terminal sends \r; \n after it is nothing more */
        }
        if (s->tty) {
            out(s, "\n");
        }
        s->line[s->line_len] = '\0';
        s->exit_code = run(s, s->line);
        s->line_len = 0;
        prompt(s);
    } else if (c == 0x7f || c == 0x08) {
        if (s->line_len > 0) {
            s->line_len--;
            if (s->tty) {
                send_payload(s, CH_STDOUT, "\b \b", 3);
            }
        }
    } else if (c == 0x03) { /* ^C: the line goes */
        s->line_len = 0;
        if (s->tty) {
            out(s, "^C\n");
        }
        prompt(s);
    } else if (c == 0x04) { /* ^D on an empty line: the end */
        if (s->line_len == 0) {
            if (s->tty) {
                out(s, "\n");
            }
            finish(s, 0);
        }
    } else if (c == 0x1b) {
        s->esc = 1;
    } else if (c >= 0x20 && c < 0x7f && s->line_len < CONSOLE_LINE - 1) {
        s->line[s->line_len++] = (char)c;
        if (s->tty) {
            send_payload(s, CH_STDOUT, &c, 1);
        }
    }
}

/* One StreamResponse: {payload: bytes = 1, closed: bool = 2}. */
static void on_message(session_t *s, const uint8_t *p, size_t n)
{
    const uint8_t *payload = NULL;
    size_t plen = 0;
    size_t off = 0;
    while (off < n) {
        uint64_t key = 0, v = 0;
        int shift = 0;
        while (off < n && shift < 64) {
            key |= (uint64_t)(p[off] & 0x7f) << shift;
            shift += 7;
            if (!(p[off++] & 0x80)) {
                break;
            }
        }
        int wire = (int)(key & 7), field = (int)(key >> 3);
        if (wire == 0 || wire == 2) {
            shift = 0;
            while (off < n && shift < 64) {
                v |= (uint64_t)(p[off] & 0x7f) << shift;
                shift += 7;
                if (!(p[off++] & 0x80)) {
                    break;
                }
            }
        }
        if (wire == 2) {
            if (v > n - off) {
                s->done = 1; /* cut short */
                return;
            }
            if (field == 1) {
                payload = p + off;
                plen = (size_t)v;
            }
            off += (size_t)v;
        } else if (wire == 0) {
            if (field == 2 && v != 0) {
                s->done = 1; /* the user's side ended */
            }
        } else if (wire == 1 || wire == 5) {
            off += wire == 1 ? 8 : 4;
        } else {
            s->done = 1;
            return;
        }
    }
    if (payload == NULL || plen == 0) {
        return;
    }
    s->last_input = time(NULL);
    uint8_t ch = payload[0];
    if (ch == CH_STDIN) {
        for (size_t i = 1; i < plen && !s->done; i++) {
            typed(s, payload[i]);
        }
    } else if (ch == CH_CLOSE && plen >= 2 && payload[1] == CH_STDIN && !s->tty) {
        /* Input over: a last line without its newline, then the end. */
        if (s->line_len > 0) {
            typed(s, '\n');
        }
        finish(s, s->exit_code);
    }
    /* CH_RESIZE: nothing to lay out. */
}

/* gRPC framing: a flag byte (0, not compressed), a 32-bit length, the message. */
static void on_bytes(session_t *s, const uint8_t *b, size_t n)
{
    while (n > 0 && !s->done) {
        size_t k = sizeof(s->in) - s->in_len;
        k = n < k ? n : k;
        memcpy(s->in + s->in_len, b, k);
        s->in_len += k;
        b += k;
        n -= k;
        while (s->in_len >= 5 && !s->done) {
            uint32_t len = (uint32_t)s->in[1] << 24 | (uint32_t)s->in[2] << 16 | (uint32_t)s->in[3] << 8 | s->in[4];
            if (s->in[0] != 0 || len > MSG_MAX) {
                fc_port_log(FC_LOG_WARN, "console: a message compressed or over %d bytes", MSG_MAX);
                s->done = 1;
                break;
            }
            if (s->in_len < 5 + len) {
                break;
            }
            on_message(s, s->in + 5, len);
            memmove(s->in, s->in + 5 + len, s->in_len - 5 - len);
            s->in_len -= 5 + len;
        }
    }
}

/* ---- sessions ------------------------------------------------------------------------- */

static void *session_thread(void *arg)
{
    session_t *s = arg;
    s->last_input = time(NULL);
    if (s->command != NULL) {
        int code = run(s, s->command);
        finish(s, code);
    } else if (s->tty) {
        char hello[128];
        pthread_mutex_lock(&s_lock);
        snprintf(hello, sizeof(hello), "flightctl-mcu console on %s: help lists the commands\n", s_device);
        pthread_mutex_unlock(&s_lock);
        out(s, hello);
        prompt(s);
    }
    uint8_t buf[256];
    while (!s->done && !s_stop) {
        int n = fc_http_stream_read(s->st, buf, sizeof(buf), 200);
        if (n < 0) {
            break; /* the server or the connection ended it */
        }
        if (n > 0) {
            on_bytes(s, buf, (size_t)n);
        } else if (time(NULL) - s->last_input > IDLE_S) {
            out(s, "\nno input for 10 minutes: the session ends\n");
            finish(s, 0);
        }
    }
    if (s_stop && !s->finished) {
        out(s, "\nthe device needs its connection: the session ends\n");
    }
    finish(s, s->exit_code);
    /* What finish sent goes out while the stream is still open. */
    for (int i = 0; i < 5 && fc_http_stream_read(s->st, buf, sizeof(buf), 100) >= 0; i++) {
    }
    fc_http_stream_close(s->st);
    int reboot = s->reboot;
    free(s->command);
    free(s);
    fc_port_log(FC_LOG_INFO, "console: session ended");
    pthread_mutex_lock(&s_lock);
    s_running = 0;
    pthread_mutex_unlock(&s_lock);
    if (reboot) {
        fc_port_log(FC_LOG_WARN, "console: restarting, as asked");
        sleep(1);
        fc_port_reboot();
    }
    return NULL;
}

/* "command arg..." of the session's metadata, or NULL. */
static char *command_of(const cJSON *meta)
{
    const cJSON *c = cJSON_GetObjectItem(meta, "command");
    const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItem(c, "command"));
    if (cmd == NULL || cmd[0] == '\0') {
        return NULL;
    }
    char buf[CONSOLE_LINE];
    size_t n = (size_t)snprintf(buf, sizeof(buf), "%s", cmd);
    const cJSON *a;
    cJSON_ArrayForEach(a, cJSON_GetObjectItem(c, "args")) {
        if (cJSON_IsString(a) && n < sizeof(buf)) {
            n += (size_t)snprintf(buf + n, sizeof(buf) - n, " %s", a->valuestring);
        }
    }
    return strdup(buf);
}

static int offers(const cJSON *meta, const char *protocol)
{
    const cJSON *p;
    cJSON_ArrayForEach(p, cJSON_GetObjectItem(meta, "protocols")) {
        if (cJSON_IsString(p) && strcmp(p->valuestring, protocol) == 0) {
            return 1;
        }
    }
    return 0;
}

static void start(const char *id, const char *metadata, const fc_http_target_t *t, int busy)
{
    cJSON *meta = cJSON_Parse(metadata ? metadata : "{}");
    char device[64];
    pthread_mutex_lock(&s_lock);
    snprintf(device, sizeof(device), "%s", s_device);
    pthread_mutex_unlock(&s_lock);
    /* Without the protocol the user's side speaks, no selected-protocol:
     * the server then refuses the session. */
    const char *headers[] = {"session-id", id, "client-name", device, "selected-protocol", PROTOCOL, NULL};
    if (!offers(meta, PROTOCOL)) {
        headers[4] = NULL;
        fc_port_log(FC_LOG_WARN, "console %s: the client does not speak %s", id, PROTOCOL);
    }
    session_t *s = calloc(1, sizeof(*s));
    if (s == NULL || (s->st = fc_http_stream_open(t, STREAM_PATH, "application/grpc", headers)) == NULL) {
        fc_port_log(FC_LOG_WARN, "console %s: cannot open its stream", id);
        free(s);
        cJSON_Delete(meta);
        return;
    }
    s->tty = cJSON_IsTrue(cJSON_GetObjectItem(meta, "tty"));
    s->command = command_of(meta);
    cJSON_Delete(meta);
    if (busy) {
        out(s, "another console session is open on this device\n");
        finish(s, 1);
        fc_http_stream_close(s->st);
        free(s->command);
        free(s);
        return;
    }
    fc_port_log(FC_LOG_INFO, "console: session %s%s", id, s->tty ? " (terminal)" : "");
    pthread_mutex_lock(&s_lock);
    if (s_joinable) {
        pthread_mutex_unlock(&s_lock);
        pthread_join(s_thread, NULL);
        pthread_attr_destroy(&s_attr);
        pthread_mutex_lock(&s_lock);
        s_joinable = 0;
    }
    s_stop = 0;
    pthread_attr_init(&s_attr);
    pthread_attr_setstacksize(&s_attr, THREAD_STACK);
    s_running = 1;
    if (pthread_create(&s_thread, &s_attr, session_thread, s) != 0) {
        s_running = 0;
        pthread_mutex_unlock(&s_lock);
        pthread_attr_destroy(&s_attr);
        fc_port_log(FC_LOG_WARN, "console %s: no thread for it", id);
        fc_http_stream_close(s->st);
        free(s->command);
        free(s);
        return;
    }
    s_joinable = 1;
    pthread_mutex_unlock(&s_lock);
}

int fc_console_sync(const cJSON *consoles, const fc_http_target_t *t)
{
    int listed = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, consoles) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(c, "sessionID"));
        const char *meta = cJSON_GetStringValue(cJSON_GetObjectItem(c, "sessionMetadata"));
        if (id == NULL || id[0] == '\0' || strlen(id) >= SEEN_ID_MAX || strchr(id, ',') != NULL) {
            continue;
        }
        listed = 1;
        pthread_mutex_lock(&s_lock);
        seen_load_locked();
        if (seen_locked(id)) {
            pthread_mutex_unlock(&s_lock);
            continue;
        }
        remember_locked(id); /* before it runs: a reboot it asks for is not asked again */
        int busy = s_running;
        pthread_mutex_unlock(&s_lock);
        start(id, meta, t, busy);
    }
    return listed;
}

void fc_console_stop(void)
{
    pthread_mutex_lock(&s_lock);
    int joinable = s_joinable;
    s_joinable = 0;
    s_stop = 1;
    pthread_mutex_unlock(&s_lock);
    if (joinable) {
        pthread_join(s_thread, NULL);
        pthread_attr_destroy(&s_attr);
    }
}
