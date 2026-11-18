/*
 * The remote console (fc_console.h): a gRPC stream on the agent's
 * connection, Kubernetes' remote command protocol in it, and a small shell
 * for what a Linux device's would be: the device's own commands, and the few
 * of a shell's that Flight Control's UI sends for its Logs tab (journalctl
 * piped to tail, printf, the test of a file), answered from the log kept in
 * RAM (fc_log.h).
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
#include "fc_log.h"
#include "fc_port.h"

#define PROTOCOL    "v5.channel.k8s.io"
#define STREAM_PATH "/flightctl.v1.RouterService/Stream"
#define CH_STDIN  0
#define CH_STDOUT 1
#define CH_ERROR  3 /* the exit status, as Kubernetes' metav1.Status */
#define CH_RESIZE 4
#define CH_CLOSE  255

#define IDLE_S       600  /* a session with no input for this long ends, unless it follows the log */
#ifndef NO_USER_S
#define NO_USER_S    60   /* nothing at all from the user's side since it opened: nobody is there */
#endif
#define MSG_MAX      2048 /* the largest gRPC message taken from the server */
#define CONSOLE_LINE 160  /* a line typed at the prompt */
#define SCRIPT_MAX   2048 /* a line of a script read from stdin (bash -s): the Logs tab's fit */
#define OUT_PIECE    768  /* stdout bytes per message */
/* Flight Control lists a session until the device's stream for it closes:
 * one whose stream died with a restart stays listed, beside the new ones,
 * and one whose user left before the device came stays listed for good
 * (docs/FLIGHTCTL-LIMITATIONS.md, 15). The device remembers a hash of each session it
 * opened, kept across restarts, so that it never opens an old one again:
 * more than the server has been seen to list, in less RAM than 16 IDs took.
 * One it forgot would be opened again at each poll, with its stream and
 * buffers. */
#define SEEN_MAX     64
#define KV_SEEN      "console_seen"
/* The session's thread: its deepest call is a command's output going out
 * through TLS (send_payload, the stream, mbedTLS); the buffers of a
 * command's output are on the heap (obuf_t). The stack it never used is
 * logged when a session ends. */
#if defined(ESP_PLATFORM) || (defined(__ZEPHYR__) && !defined(CONFIG_ARCH_POSIX))
#define THREAD_STACK (8 * 1024)
#else
#define THREAD_STACK (64 * 1024) /* a host's libc (time zones, under sanitizers) takes more */
#endif

#define TOK_MAX    96 /* words and operators in a line */
#define ARGV_MAX   32
#define DEPTH_MAX  3    /* bash -c in bash -c... */
#define EXIT_MARK  '\x01' /* where $? goes, expanded as the command runs */
#define CLOCK_SET  1577836800 /* 2020-01-01: before it, the clock was not set */
#define AGENT_UNIT "flightctl-agent.service"

/* What journalctl was asked for. */
typedef struct {
    char unit[2][128]; /* -u: globs, any of them */
    int units;
    int prio_min, prio_max; /* syslog priorities: 0 emerg ... 7 debug */
    int none;               /* a boot not kept: nothing */
    int boot;               /* -1 every boot kept, 0 this one, 1 the previous one */
    int kernel;             /* -k: the system's lines only */
    int since_now;
    int has_since, has_until;
    time_t since, until;
    long lines; /* the last so many, or -1: all */
    int follow;
} jfilter_t;

typedef struct {
    fc_http_stream_t *st;
    int tty;
    int script;    /* commands on stdin, as to `bash -s` (the UI's Logs tab) */
    char *command; /* the session's one command, or NULL */
    char line[CONSOLE_LINE];
    size_t line_len;
    int esc;      /* inside an escape sequence: 1 after ESC, 2 after ESC [ */
    char *sbuf;   /* a script's line being read (SCRIPT_MAX + 1) */
    size_t slen;
    int sq, sesc, sskip; /* the quote it is in; after a backslash; a line too long */
    uint8_t in[MSG_MAX + 5];
    size_t in_len;
    int done;     /* nothing more to read: the user left, or ended it */
    int finished; /* the exit status and the end of the stream went out */
    int exit_code;
    int reboot;
    int heard;     /* anything came from the user's side */
    time_t opened;
    int follow;    /* following the log (journalctl -f, log -f) */
    uint32_t fseq; /* the next line to follow */
    jfilter_t jf;
    time_t last_input;
} session_t;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static char s_device[64];
static uint32_t s_seen[SEEN_MAX]; /* id_hash of the sessions opened; 0 empty */
static int s_seen_next, s_seen_loaded;
enum { CONSOLE_IDLE, CONSOLE_RUNNING, CONSOLE_FINISHED };
static int s_thread_state; /* also read by the spec long-poll */
static int s_stop;
static pthread_t s_thread;
static pthread_attr_t s_attr; /* until joined: on Zephyr it owns the stack */

void fc_console_set_device(const char *name)
{
    pthread_mutex_lock(&s_lock);
    snprintf(s_device, sizeof(s_device), "%s", name ? name : "");
    pthread_mutex_unlock(&s_lock);
}

/* ---- the sessions seen, kept across restarts ----------------------------------- */

/* FNV-1a, never 0 (an empty slot). */
static uint32_t id_hash(const char *id)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)id; *p; p++) {
        h = (h ^ *p) * 16777619u;
    }
    return h ? h : 1;
}

/* Stored as 8 hex digits each, comma-separated. A firmware before kept the
 * IDs themselves: those are hashed as they come. */
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
        int hex = strlen(id) == 8 && strspn(id, "0123456789abcdef") == 8;
        uint32_t h = hex ? (uint32_t)strtoul(id, NULL, 16) : id_hash(id);
        s_seen[s_seen_next++] = h ? h : 1;
    }
    s_seen_next %= SEEN_MAX;
    free(v);
}

/* The slot that holds h, or -1. */
static int seen_locked(uint32_t h)
{
    for (int i = 0; i < SEEN_MAX; i++) {
        if (s_seen[i] == h) {
            return i;
        }
    }
    return -1;
}

/* Into the oldest slot not in `keep` (the sessions listed now), so that one
 * still listed is forgotten only when more than SEEN_MAX are. Returns it. */
static int remember_locked(uint32_t h, uint64_t keep)
{
    int slot = s_seen_next;
    for (int k = 0; k < SEEN_MAX; k++) {
        int i = (s_seen_next + k) % SEEN_MAX;
        if (!(keep >> i & 1)) {
            slot = i;
            break;
        }
    }
    s_seen[slot] = h;
    s_seen_next = (slot + 1) % SEEN_MAX;
    char all[SEEN_MAX * 9];
    size_t n = 0;
    for (int i = 0; i < SEEN_MAX; i++) {
        if (s_seen[i] != 0) {
            n += (size_t)snprintf(all + n, sizeof(all) - n, "%s%08" PRIx32, n ? "," : "", s_seen[i]);
        }
    }
    fc_port_kv_set(KV_SEEN, all, n);
    return slot;
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
        if (k > 0) {
            memcpy(msg + n, p, k);
        }
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

/* Lines gathered into fewer messages, in a buffer on the heap: the
 * session's stack is small. */
typedef struct {
    session_t *s;
    size_t n;
    char *buf; /* OUT_PIECE */
} obuf_t;

/* 0, or -1 (said to the user) when there is no memory for it. */
static int ob_open(obuf_t *o, session_t *s)
{
    o->s = s;
    o->n = 0;
    o->buf = malloc(OUT_PIECE);
    if (o->buf == NULL) {
        out(s, "out of memory\n");
        return -1;
    }
    return 0;
}

static void ob_flush(obuf_t *o)
{
    if (o->n > 0) {
        o->buf[o->n] = '\0';
        out(o->s, o->buf);
        o->n = 0;
    }
}

static void ob_close(obuf_t *o)
{
    ob_flush(o);
    free(o->buf);
    o->buf = NULL;
}

static void ob_put(obuf_t *o, const char *text, size_t len)
{
    if (o->n + len >= OUT_PIECE) {
        ob_flush(o);
    }
    if (len >= OUT_PIECE) {
        while (len > 0) {
            size_t k = len < OUT_PIECE - 1 ? len : OUT_PIECE - 1;
            memcpy(o->buf, text, k);
            o->n = k;
            ob_flush(o);
            text += k;
            len -= k;
        }
        return;
    }
    memcpy(o->buf + o->n, text, len);
    o->n += len;
}

static void ob_puts(obuf_t *o, const char *text)
{
    ob_put(o, text, strlen(text));
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

/* ---- the device's commands ----------------------------------------------------------- */

static void cmd_help(session_t *s)
{
    out(s, "help     these commands\n"
           "status   the device: firmware, memory, storage, network\n"
           "apps     the WASM apps and their state\n"
           "log      the device's log: -n lines, -p level (err, warning, info), -f to follow\n"
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

/* ---- the log: journalctl, as the UI's Logs tab runs it -------------------------------- */

static const int s_prio[] = {3, 4, 6, 7}; /* fc_log_level_t to syslog: err, warning, info, debug */
static const char s_level[] = "EWID";

static int is_digits(const char *v)
{
    if (*v == '\0') {
        return 0;
    }
    for (; *v; v++) {
        if (*v < '0' || *v > '9') {
            return 0;
        }
    }
    return 1;
}

/* A [...] class at p, for c: 1 matched, 0 not, -1 not a class (no "]"). */
static int glob_class(const char *p, char c, const char **end)
{
    const char *q = p + 1;
    int neg = *q == '!' || *q == '^';
    q += neg;
    const char *first = q;
    int hit = 0;
    while (*q && (*q != ']' || q == first)) {
        if (q[1] == '-' && q[2] && q[2] != ']') {
            hit |= c >= q[0] && c <= q[2];
            q += 3;
        } else {
            hit |= c == *q;
            q++;
        }
    }
    if (*q != ']') {
        return -1;
    }
    *end = q + 1;
    return hit != neg;
}

/* A glob as systemd takes them (*, ?, [...]), against a whole string; the
 * last * is backtracked to, so a pattern takes no more than its length
 * times the string's. */
static int glob_match(const char *p, const char *s)
{
    const char *star = NULL, *star_s = NULL;
    while (*s) {
        const char *next = NULL;
        if (*p == '*') {
            star = p++;
            star_s = s;
            continue;
        }
        if (*p == '?') {
            next = p + 1;
        } else if (*p == '[') {
            const char *end;
            int r = glob_class(p, *s, &end);
            if (r == 1) {
                next = end;
            } else if (r == -1 && *s == '[') {
                next = p + 1;
            }
        } else if (*p != '\0') {
            const char *c = *p == '\\' && p[1] ? p + 1 : p;
            if (*c == *s) {
                next = c + 1;
            }
        }
        if (next != NULL) {
            p = next;
            s++;
        } else if (star != NULL) {
            p = star + 1;
            s = ++star_s;
        } else {
            return 0;
        }
    }
    while (*p == '*') {
        p++;
    }
    return *p == '\0';
}

/* -u PATTERN against a unit: "x" also means "x.service", as for systemd. */
static int unit_is(const char *pattern, const char *unit)
{
    if (glob_match(pattern, unit)) {
        return 1;
    }
    size_t n = strlen(unit);
    if (strchr(pattern, '.') == NULL && n > 8 && strcmp(unit + n - 8, ".service") == 0) {
        char u[96];
        snprintf(u, sizeof(u), "%.*s", (int)(n - 8), unit);
        return glob_match(pattern, u);
    }
    return 0;
}

static int jmatch(const jfilter_t *f, const fc_log_entry_t *e, time_t now, uint32_t up)
{
    if (f->none || (f->boot == 0 && e->previous) || (f->boot == 1 && !e->previous) || (f->kernel && !e->system)) {
        return 0;
    }
    int prio = s_prio[e->level & 3];
    if (prio < f->prio_min || prio > f->prio_max) {
        return 0;
    }
    if (f->units > 0) {
        /* The system's lines are no unit's (they are the kernel's, -k). The
         * others are the agent's; an app's ("app NAME: ...") is also
         * NAME.service's. */
        if (e->system) {
            return 0;
        }
        char app[80] = "";
        const char *colon = strncmp(e->text, "app ", 4) == 0 ? strchr(e->text + 4, ':') : NULL;
        if (colon != NULL && colon - e->text - 4 < 64) {
            snprintf(app, sizeof(app), "%.*s.service", (int)(colon - e->text - 4), e->text + 4);
        }
        int hit = 0;
        for (int i = 0; i < f->units && !hit; i++) {
            hit = unit_is(f->unit[i], AGENT_UNIT) || (app[0] && unit_is(f->unit[i], app));
        }
        if (!hit) {
            return 0;
        }
    }
    if (now >= CLOCK_SET && (f->has_since || f->has_until)) {
        /* A line of the previous boot from before its clock was set: out of
         * any range. */
        time_t t = (time_t)e->when;
        if (t == 0 || (f->has_since && t < f->since) || (f->has_until && t > f->until)) {
            return 0;
        }
    }
    (void)up;
    return 1;
}

/* A line as journalctl's short format would give it, with the level the
 * serial port shows: "Nov 16 10:34:54 W fc: text". Before the clock is set,
 * the uptime. */
static size_t jline(char *line, size_t cap, const fc_log_entry_t *e, time_t now, uint32_t up)
{
    char when[24];
    (void)now;
    (void)up;
    if (e->when != 0) {
        time_t t = (time_t)e->when;
        struct tm tm;
        gmtime_r(&t, &tm);
        strftime(when, sizeof(when), "%b %d %H:%M:%S", &tm);
    } else {
        snprintf(when, sizeof(when), "uptime %" PRIu32 "s", e->up_s);
    }
    /* The system's lines say whose they are ("wifi: ..."); the others are fc's. */
    int n = snprintf(line, cap, "%s %c %s%s\n", when, s_level[e->level & 3], e->system ? "" : "fc: ", e->text);
    return n < 0 ? 0 : (size_t)n < cap ? (size_t)n : cap - 1;
}

static time_t civil(long y, int m, int d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    long yoe = y - era * 400;
    long doy = (153L * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return (time_t)(era * 146097 + doe - 719468) * 86400;
}

static int num(const char *p, int digits, int *out)
{
    *out = 0;
    for (int i = 0; i < digits; i++) {
        if (p[i] < '0' || p[i] > '9') {
            return -1;
        }
        *out = *out * 10 + (p[i] - '0');
    }
    return 0;
}

/* A time as journalctl takes it: now, today, yesterday, @EPOCH, YYYY-MM-DD
 * [HH:MM[:SS]] (UTC: the device's zone), "N UNIT ago", -N UNIT, +N UNIT. */
static int parse_when(const char *v, time_t now, time_t *out, int *is_now)
{
    *is_now = 0;
    if (strcmp(v, "now") == 0) {
        *out = now;
        *is_now = 1;
        return 0;
    }
    time_t day = now - now % 86400;
    if (strcmp(v, "today") == 0 || strcmp(v, "yesterday") == 0 || strcmp(v, "tomorrow") == 0) {
        *out = day + (v[0] == 'y' ? -86400 : v[2] == 'm' ? 86400 : 0);
        return 0;
    }
    if (v[0] == '@') {
        if (!is_digits(v + 1) || strlen(v + 1) > 12) {
            return -1;
        }
        *out = (time_t)strtoll(v + 1, NULL, 10);
        return 0;
    }
    int y, mo, d, h = 0, mi = 0, sec = 0;
    if (strlen(v) >= 10 && v[4] == '-' && v[7] == '-' && num(v, 4, &y) == 0 && num(v + 5, 2, &mo) == 0 &&
        num(v + 8, 2, &d) == 0) {
        const char *t = v + 10;
        if (*t == ' ' || *t == 'T') {
            if (strlen(t + 1) < 5 || t[3] != ':' || num(t + 1, 2, &h) != 0 || num(t + 4, 2, &mi) != 0) {
                return -1;
            }
            t += 6;
            if (*t == ':') {
                if (num(t + 1, 2, &sec) != 0) {
                    return -1;
                }
                t += 3;
            }
        }
        if (*t != '\0' || mo < 1 || mo > 12 || d < 1 || d > 31 || h > 23 || mi > 59 || sec > 60) {
            return -1;
        }
        *out = civil(y, mo, d) + h * 3600 + mi * 60 + sec;
        return 0;
    }
    /* Relative. */
    const char *p = v;
    int sign = 0;
    if (*p == '-' || *p == '+') {
        sign = *p++ == '-' ? -1 : 1;
    }
    if (*p < '0' || *p > '9') {
        return -1;
    }
    long n = 0;
    while (*p >= '0' && *p <= '9' && n < 1000000) {
        n = n * 10 + (*p++ - '0');
    }
    while (*p == ' ') {
        p++;
    }
    const char *unit = p;
    while (*p && *p != ' ') {
        p++;
    }
    size_t ulen = (size_t)(p - unit);
    while (*p == ' ') {
        p++;
    }
    if (strcmp(p, "ago") == 0) {
        if (sign != 0) {
            return -1;
        }
        sign = -1;
    } else if (*p != '\0' || sign == 0) {
        return -1;
    }
    static const struct {
        const char *name;
        long s;
    } units[] = {{"s", 1},          {"sec", 1},        {"second", 1},     {"seconds", 1},  {"m", 60},
                 {"min", 60},       {"minute", 60},    {"minutes", 60},   {"h", 3600},     {"hour", 3600},
                 {"hours", 3600},   {"d", 86400},      {"day", 86400},    {"days", 86400}, {"w", 604800},
                 {"week", 604800},  {"weeks", 604800}, {"month", 2592000}, {"months", 2592000},
                 {"y", 31536000},   {"year", 31536000}, {"years", 31536000}};
    for (size_t i = 0; i < sizeof(units) / sizeof(units[0]); i++) {
        if (strlen(units[i].name) == ulen && strncmp(units[i].name, unit, ulen) == 0) {
            *out = now + (time_t)sign * n * units[i].s;
            return 0;
        }
    }
    return -1;
}

static int parse_prio(const char *v, int *out)
{
    static const char *names[] = {"emerg", "alert", "crit", "err", "warning", "notice", "info", "debug"};
    if (v[0] >= '0' && v[0] <= '7' && v[1] == '\0') {
        *out = v[0] - '0';
        return 0;
    }
    for (int i = 0; i < 8; i++) {
        if (strcmp(v, names[i]) == 0) {
            *out = i;
            return 0;
        }
    }
    return -1;
}

static int bad(session_t *s, const char *fmt, const char *what)
{
    char msg[200];
    snprintf(msg, sizeof(msg), fmt, what);
    out(s, msg);
    return 1;
}

/* journalctl's options (and log's, the same with -n 50 by default). */
static int journal_args(session_t *s, char **argv, int argc, jfilter_t *f, int is_log)
{
    memset(f, 0, sizeof(*f));
    f->prio_max = 7;
    f->lines = is_log ? 50 : -1;
    f->boot = -1; /* every boot kept, as journalctl without -b */
    int lines_given = 0, boot_given = 0;
    time_t now = time(NULL);
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = NULL;
        char name[24];
        const char *eq = a[0] == '-' && a[1] == '-' ? strchr(a, '=') : NULL;
        if (eq != NULL) {
            snprintf(name, sizeof(name), "%.*s", (int)(eq - a), a);
            v = eq + 1;
        } else if (a[0] == '-' && a[1] != '-' && a[1] != '\0' && a[2] != '\0' && strchr("upSUnob", a[1])) {
            snprintf(name, sizeof(name), "-%c", a[1]); /* -n100, -uNAME */
            v = a + 2;
        } else {
            snprintf(name, sizeof(name), "%s", a);
        }
#define IS(x) (strcmp(name, x) == 0)
#define NEED()                                                         \
    do {                                                               \
        if (v == NULL && i + 1 < argc) {                               \
            v = argv[++i];                                             \
        }                                                              \
        if (v == NULL) {                                               \
            return bad(s, "journalctl: option '%s' needs a value\n", a); \
        }                                                              \
    } while (0)
        if (IS("-u") || IS("--unit") || IS("--user-unit")) {
            NEED();
            if (f->units < 2) {
                snprintf(f->unit[f->units++], sizeof(f->unit[0]), "%s", v);
            }
        } else if (IS("-p") || IS("--priority")) {
            NEED();
            char lo[16];
            const char *dots = strstr(v, "..");
            snprintf(lo, sizeof(lo), "%.*s", dots ? (int)(dots - v) : (int)strlen(v), v);
            int a_p, b_p;
            if (parse_prio(lo, &a_p) != 0 || (dots && parse_prio(dots + 2, &b_p) != 0)) {
                return bad(s, "journalctl: unknown priority '%s'\n", v);
            }
            f->prio_min = dots ? a_p : 0;
            f->prio_max = dots ? b_p : a_p;
        } else if (IS("-b") || IS("--boot")) {
            if (v == NULL && i + 1 < argc && (is_digits(argv[i + 1]) || (argv[i + 1][0] == '-' && is_digits(argv[i + 1] + 1)))) {
                v = argv[++i];
            }
            /* Two boots are kept at most: 0 (or none given) is this one, -1
             * the one before; any other is empty. */
            if (v == NULL || v[0] == '\0' || strcmp(v, "0") == 0 || strcmp(v, "-0") == 0) {
                f->boot = 0;
            } else if (strcmp(v, "-1") == 0) {
                f->boot = 1;
            } else {
                f->none = 1;
            }
            boot_given = 1;
        } else if (IS("-S") || IS("--since") || IS("-U") || IS("--until")) {
            NEED();
            time_t t;
            int is_now;
            if (parse_when(v, now, &t, &is_now) != 0) {
                return bad(s, "journalctl: failed to parse timestamp: %s\n", v);
            }
            if (name[1] == 'S' || name[2] == 's') {
                f->has_since = 1;
                f->since = t;
                f->since_now = is_now;
            } else {
                f->has_until = 1;
                f->until = t;
            }
        } else if (IS("-n") || IS("--lines")) {
            if (v == NULL && i + 1 < argc && is_digits(argv[i + 1])) {
                v = argv[++i];
            }
            lines_given = 1;
            f->lines = v == NULL ? 10 : strcmp(v, "all") == 0 ? -1 : is_digits(v) ? atol(v) : -2;
            if (f->lines == -2) {
                return bad(s, "journalctl: bad number of lines '%s'\n", v);
            }
        } else if (IS("-f") || IS("--follow")) {
            f->follow = 1;
        } else if (IS("-o") || IS("--output")) {
            NEED(); /* one format: short */
        } else if (IS("-k") || IS("--dmesg")) {
            f->kernel = 1; /* the system's lines, of this boot unless -b says */
        } else if (IS("-e") || IS("--pager-end")) {
            if (!lines_given) {
                f->lines = 1000;
            }
        } else if (a[0] == '-' && !IS("--no-pager") && !IS("-q") && !IS("--quiet") && !IS("-a") && !IS("--all") &&
                   !IS("-x") && !IS("--catalog") && !IS("-l") && !IS("--full") && !IS("-r") && !IS("--reverse") &&
                   !IS("--no-hostname") && !IS("--utc") && !IS("--system") && !IS("-m") && !IS("--merge")) {
            return bad(s, "journalctl: unrecognized option '%s'\n", a);
        }
        /* FIELD=VALUE matches: every line passes. */
#undef IS
#undef NEED
    }
    if (f->kernel && !boot_given) {
        f->boot = 0;
    }
    if (f->follow && !lines_given && !is_log) {
        f->lines = 10; /* as journalctl -f */
    }
    return 0;
}

/* A line being read and written, on the heap. */
typedef struct {
    fc_log_entry_t e;
    char line[FC_LOG_TEXT_MAX + 48];
} jwork_t;

/* journalctl or log: the lines kept, the last `limit` of them (a tail -n
 * after it), then, with -f, the session follows the new ones. */
static int cmd_journal(session_t *s, char **argv, int argc, long limit, int is_log)
{
    jfilter_t f;
    int rc = journal_args(s, argv, argc, &f, is_log);
    if (rc != 0) {
        return rc;
    }
    if (limit >= 0 && !f.follow && (f.lines < 0 || limit < f.lines)) {
        f.lines = limit;
    }
    uint32_t end = fc_log_end();
    if (!f.since_now) {
        time_t now = time(NULL);
        uint32_t up = fc_log_uptime();
        jwork_t *w = malloc(sizeof(*w));
        obuf_t o;
        if (w == NULL || ob_open(&o, s) != 0) {
            free(w);
            return 1;
        }
        long count = 0;
        uint32_t seq = end - (uint32_t)(FC_LOG_RING); /* before the oldest */
        while (fc_log_next(&seq, &w->e) == 0 && (int32_t)(w->e.seq - end) < 0) {
            count += jmatch(&f, &w->e, now, up);
        }
        long skip = f.lines >= 0 && count > f.lines ? count - f.lines : 0;
        seq = end - (uint32_t)(FC_LOG_RING);
        while (!s->done && fc_log_next(&seq, &w->e) == 0 && (int32_t)(w->e.seq - end) < 0) {
            if (jmatch(&f, &w->e, now, up) && skip-- <= 0) {
                ob_put(&o, w->line, jline(w->line, sizeof(w->line), &w->e, now, up));
            }
        }
        if (is_log && count == 0) {
            ob_puts(&o, "no lines\n");
        }
        ob_close(&o);
        free(w);
    }
    if (f.follow) {
        s->jf = f;
        s->fseq = end;
        s->follow = 1;
    }
    return 0;
}

/* The lines written since the last call, while the session follows. */
static void follow_some(session_t *s)
{
    if (s->fseq == fc_log_end()) {
        return; /* nothing new: nothing taken */
    }
    jwork_t *w = malloc(sizeof(*w));
    obuf_t o;
    if (w == NULL || ob_open(&o, s) != 0) {
        free(w);
        return; /* tried again at the next round */
    }
    time_t now = time(NULL);
    uint32_t up = fc_log_uptime();
    while (!s->done && fc_log_next(&s->fseq, &w->e) == 0) {
        if (jmatch(&s->jf, &w->e, now, up)) {
            ob_put(&o, w->line, jline(w->line, sizeof(w->line), &w->e, now, up));
        }
    }
    ob_close(&o);
    free(w);
}

/* ---- a shell's worth of the rest ---------------------------------------------------- */

static int cmd_printf(session_t *s, char **argv, int argc)
{
    if (argc < 2) {
        out(s, "printf: usage: printf format [arguments]\n");
        return 2;
    }
    obuf_t o;
    if (ob_open(&o, s) != 0) {
        return 1;
    }
    int ai = 2, used;
    do { /* the format again, for the arguments left */
        used = 0;
        for (const char *p = argv[1]; *p; p++) {
            if (*p == '\\' && p[1]) {
                p++;
                char c = *p == 'n' ? '\n' : *p == 't' ? '\t' : *p == 'r' ? '\r' : *p;
                ob_put(&o, &c, 1);
            } else if (*p == '%' && p[1]) {
                p++;
                while (p[1] && strchr("-+ #0123456789.", *p)) {
                    p++;
                }
                if (*p == '%') {
                    ob_put(&o, "%", 1);
                    continue;
                }
                const char *arg = ai < argc ? argv[ai++] : "";
                used = 1;
                if (*p == 'd' || *p == 'i' || *p == 'u') {
                    char n[24];
                    snprintf(n, sizeof(n), "%ld", strtol(arg, NULL, 10));
                    ob_puts(&o, n);
                } else if (*p == 'c') {
                    ob_put(&o, arg, arg[0] ? 1 : 0);
                } else {
                    ob_puts(&o, arg);
                }
            } else {
                ob_put(&o, p, 1);
            }
        }
    } while (used && ai < argc);
    ob_close(&o);
    return 0;
}

static int cmd_echo(session_t *s, char **argv, int argc)
{
    obuf_t o;
    if (ob_open(&o, s) != 0) {
        return 1;
    }
    int i = 1, newline = 1;
    for (; i < argc && (strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "-e") == 0); i++) {
        newline &= argv[i][1] != 'n';
    }
    for (int first = i; i < argc; i++) {
        if (i > first) {
            ob_put(&o, " ", 1);
        }
        ob_puts(&o, argv[i]);
    }
    if (newline) {
        ob_put(&o, "\n", 1);
    }
    ob_close(&o);
    return 0;
}

/* test, [ and [[: strings and numbers; a file never exists, the device has
 * no file system to show. */
static int cmd_test(char **argv, int argc, int closer)
{
    if (closer) {
        argc--; /* the ] or ]] */
    }
    int i = 1, neg = 0;
    if (i < argc && strcmp(argv[i], "!") == 0) {
        neg = 1;
        i++;
    }
    int n = argc - i, r = 0;
    if (n == 1) {
        r = argv[i][0] != '\0';
    } else if (n == 2) {
        r = strcmp(argv[i], "-n") == 0 ? argv[i + 1][0] != '\0' : strcmp(argv[i], "-z") == 0 ? argv[i + 1][0] == '\0' : 0;
    } else if (n == 3) {
        const char *op = argv[i + 1];
        long x = strtol(argv[i], NULL, 10), y = strtol(argv[i + 2], NULL, 10);
        r = strcmp(op, "=") == 0 || strcmp(op, "==") == 0 ? strcmp(argv[i], argv[i + 2]) == 0
            : strcmp(op, "!=") == 0                       ? strcmp(argv[i], argv[i + 2]) != 0
            : strcmp(op, "-eq") == 0                      ? x == y
            : strcmp(op, "-ne") == 0                      ? x != y
            : strcmp(op, "-lt") == 0                      ? x < y
            : strcmp(op, "-le") == 0                      ? x <= y
            : strcmp(op, "-gt") == 0                      ? x > y
            : strcmp(op, "-ge") == 0                      ? x >= y
                                                          : 0;
    }
    return (r != neg) ? 0 : 1;
}

/* tail, cat, head... on a file: there are none. */
static int cmd_file(session_t *s, char **argv, int argc, const char *name)
{
    for (int i = 1; i < argc; i++) {
        if (argv[i][0] == '-') {
            if ((strcmp(argv[i], "-n") == 0 || strcmp(argv[i], "-c") == 0) && i + 1 < argc) {
                i++;
            }
            continue;
        }
        char msg[200];
        snprintf(msg, sizeof(msg), "%s: cannot open '%.120s' for reading: No such file or directory\n", name, argv[i]);
        out(s, msg);
        return 1;
    }
    return 0;
}

static const char *base(const char *path)
{
    const char *b = strrchr(path, '/');
    return b ? b + 1 : path;
}

static int is_shell(const char *b)
{
    return strcmp(b, "bash") == 0 || strcmp(b, "sh") == 0 || strcmp(b, "dash") == 0 || strcmp(b, "ash") == 0;
}

static int is_assignment(const char *w)
{
    if (!((*w >= 'a' && *w <= 'z') || (*w >= 'A' && *w <= 'Z') || *w == '_')) {
        return 0;
    }
    for (w++; *w && *w != '='; w++) {
        if (!((*w >= 'a' && *w <= 'z') || (*w >= 'A' && *w <= 'Z') || (*w >= '0' && *w <= '9') || *w == '_')) {
            return 0;
        }
    }
    return *w == '=';
}

static int exec_script(session_t *s, const char *src, int depth);

/* One command, its words expanded: what it printed went out; its exit code.
 * `limit` is the line count of a tail -n it was piped to, or -1. */
static int command(session_t *s, char **argv, int argc, long limit, int depth)
{
    for (;;) {
        while (argc > 0 && is_assignment(argv[0])) {
            argv++;
            argc--;
        }
        if (argc > 0 && strcmp(base(argv[0]), "sudo") == 0) {
            argv++;
            argc--;
            while (argc > 0 && argv[0][0] == '-') {
                int takes = strchr("ugCDhprT", argv[0][1]) != NULL && argv[0][1] && argv[0][2] == '\0';
                int dashdash = strcmp(argv[0], "--") == 0;
                argv++;
                argc--;
                if (dashdash) {
                    break;
                }
                if (takes && argc > 0) {
                    argv++;
                    argc--;
                }
            }
        } else if (argc > 0 && (strcmp(argv[0], "exec") == 0 || strcmp(argv[0], "command") == 0 ||
                                strcmp(base(argv[0]), "env") == 0 || strcmp(argv[0], "nohup") == 0)) {
            argv++;
            argc--;
        } else {
            break;
        }
    }
    if (argc == 0) {
        return 0;
    }
    const char *b = base(argv[0]);
    if (is_shell(b)) {
        for (int i = 1; i < argc && argv[i][0] == '-'; i++) {
            if (argv[i][1] != '-' && strchr(argv[i], 'c') != NULL) {
                if (i + 1 >= argc) {
                    out(s, "bash: -c: option requires an argument\n");
                    return 2;
                }
                return exec_script(s, argv[i + 1], depth + 1);
            }
        }
        return 0; /* a shell reading stdin: this one is it already */
    }
    if (strcmp(b, "help") == 0) {
        cmd_help(s);
    } else if (strcmp(b, "status") == 0) {
        cmd_status(s);
    } else if (strcmp(b, "apps") == 0) {
        cmd_apps(s);
    } else if (strcmp(b, "log") == 0 || strcmp(b, "journalctl") == 0) {
        return cmd_journal(s, argv, argc, limit, b[0] == 'l');
    } else if (strcmp(b, "reboot") == 0) {
        out(s, "restarting\n");
        s->reboot = 1;
        finish(s, 0);
    } else if (strcmp(b, "exit") == 0 || strcmp(b, "quit") == 0 || strcmp(b, "logout") == 0) {
        finish(s, argc > 1 ? atoi(argv[1]) : s->exit_code);
    } else if (strcmp(b, "printf") == 0) {
        return cmd_printf(s, argv, argc);
    } else if (strcmp(b, "echo") == 0) {
        return cmd_echo(s, argv, argc);
    } else if (strcmp(b, "test") == 0 || strcmp(b, "[") == 0 || strcmp(b, "[[") == 0) {
        int closer = b[0] == '[' && argc > 1 && strcmp(argv[argc - 1], b[1] ? "]]" : "]") == 0;
        return cmd_test(argv, argc, closer);
    } else if (strcmp(b, "true") == 0 || strcmp(b, ":") == 0 || strcmp(b, "set") == 0 || strcmp(b, "export") == 0 ||
               strcmp(b, "cd") == 0 || strcmp(b, "umask") == 0 || strcmp(b, "trap") == 0) {
        return 0;
    } else if (strcmp(b, "false") == 0) {
        return 1;
    } else if (strcmp(b, "tail") == 0 || strcmp(b, "head") == 0 || strcmp(b, "cat") == 0 ||
               strcmp(b, "less") == 0 || strcmp(b, "stat") == 0 || strcmp(b, "file") == 0) {
        return cmd_file(s, argv, argc, b);
#ifdef FC_CONSOLE_CRASH
    } else if (strcmp(b, "crash") == 0) {
        /* Test builds only: the fault a bug makes, a write to address 0, to
         * see what the device keeps of it (fc_log.h). */
        fc_port_log(FC_LOG_WARN, "console: crashing on purpose");
        *(volatile int *)0 = 1;
        return 1;
#endif
    } else {
        char msg[160];
        snprintf(msg, sizeof(msg), "%.60s: not a command of this device (help lists them)\n", argv[0]);
        out(s, msg);
        return 127;
    }
    return 0;
}

/* ---- lines: words, quotes, operators --------------------------------------------- */

enum { T_WORD, T_SEMI, T_AND, T_OR, T_PIPE };

typedef struct {
    int n;
    uint8_t type[TOK_MAX];
    char *word[TOK_MAX];
    char *store;
} toks_t;

/* After a $: $? becomes EXIT_MARK; $(...), `...`, ${...} and variables
 * become nothing (no command is run inside, no variable is set). */
static void dollar(const char **pp, char **oo)
{
    const char *p = *pp;
    if (*p == '`') {
        p = strchr(p + 1, '`');
        *pp = p ? p + 1 : *pp + strlen(*pp);
        return;
    }
    p++;
    if (*p == '?') {
        *(*oo)++ = EXIT_MARK;
        p++;
    } else if (*p == '(' || *p == '{') {
        char open = *p, close = open == '(' ? ')' : '}';
        int depth = 0, q = 0;
        for (; *p; p++) {
            if (q) {
                q = *p == q ? 0 : q;
            } else if (*p == '\'' || *p == '"') {
                q = *p;
            } else if (*p == open) {
                depth++;
            } else if (*p == close && --depth == 0) {
                p++;
                break;
            }
        }
    } else if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_') {
        while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || *p == '_') {
            p++;
        }
    } else if ((*p >= '0' && *p <= '9') || (*p && strchr("@*#$!-", *p))) {
        p++;
    } else {
        *(*oo)++ = '$';
    }
    *pp = p;
}

static int tokenize(const char *src, toks_t *t)
{
    t->n = 0;
    t->store = malloc(2 * strlen(src) + 2);
    if (t->store == NULL) {
        return -1;
    }
    char *o = t->store;
    const char *p = src;
    for (;;) {
        while (*p == ' ' || *p == '\t' || *p == '\r') {
            p++;
        }
        if (*p == '\0') {
            return 0;
        }
        if (*p == '#') {
            while (*p && *p != '\n') {
                p++;
            }
            continue;
        }
        if (t->n == TOK_MAX) {
            return -1;
        }
        int type = -1;
        if (*p == ';' || *p == '\n') {
            type = T_SEMI;
            p++;
        } else if (p[0] == '&' && p[1] == '&') {
            type = T_AND;
            p += 2;
        } else if (p[0] == '|' && p[1] == '|') {
            type = T_OR;
            p += 2;
        } else if (*p == '|') {
            type = T_PIPE;
            p++;
        } else if (*p == '&' && p[1] != '>') {
            type = T_SEMI; /* in the background: here, in turn */
            p++;
        }
        if (type >= 0) {
            t->type[t->n] = (uint8_t)type;
            t->word[t->n++] = NULL;
            continue;
        }
        /* A redirection (2>&1, >/dev/null, &>x, <x) is dropped with its target. */
        const char *q = p;
        while (*q >= '0' && *q <= '9') {
            q++;
        }
        int redirect = *q == '>' || *q == '<' || (q == p && q[0] == '&' && q[1] == '>');
        if (redirect) {
            q += *q == '&';
            q++;
            q += *q == '>' || *q == '&';
            p = q;
            while (*p == ' ' || *p == '\t') {
                p++;
            }
        }
        char *w = o;
        int quoted = 0;
        for (;;) {
            char c = *p;
            if (c == '\0' || c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == ';' || c == '&' || c == '|') {
                break;
            }
            if (c == '\'') {
                quoted = 1;
                for (p++; *p && *p != '\''; p++) {
                    *o++ = *p;
                }
                p += *p != '\0';
            } else if (c == '"') {
                quoted = 1;
                p++;
                while (*p && *p != '"') {
                    if (*p == '\\' && p[1] && strchr("\"\\$`\n", p[1])) {
                        p++;
                        if (*p != '\n') {
                            *o++ = *p;
                        }
                        p++;
                    } else if (*p == '$' || *p == '`') {
                        dollar(&p, &o);
                    } else {
                        *o++ = *p++;
                    }
                }
                p += *p != '\0';
            } else if (c == '\\') {
                p++;
                if (*p) {
                    if (*p != '\n') {
                        *o++ = *p;
                    }
                    p++;
                }
            } else if (c == '$' || c == '`') {
                dollar(&p, &o);
            } else {
                *o++ = *p++;
            }
        }
        *o++ = '\0';
        if (redirect || (o - w == 1 && !quoted)) {
            o = w; /* a redirection's target, or a word that expanded to nothing */
            continue;
        }
        t->type[t->n] = T_WORD;
        t->word[t->n++] = w;
    }
}

/* A word with $? in it, the exit code put in: a new string, or NULL. */
static char *expand(const char *w, int code)
{
    char num[16];
    int nl = snprintf(num, sizeof(num), "%d", code);
    size_t marks = 0;
    for (const char *c = w; *c; c++) {
        marks += *c == EXIT_MARK;
    }
    char *x = malloc(strlen(w) + marks * (size_t)nl + 1), *o = x;
    if (x == NULL) {
        return NULL;
    }
    for (const char *c = w; *c; c++) {
        if (*c == EXIT_MARK) {
            memcpy(o, num, (size_t)nl);
            o += nl;
        } else {
            *o++ = *c;
        }
    }
    *o = '\0';
    return x;
}

/* A pipeline, words [a, b): its first command runs, with the -n of a tail
 * or head after it (journalctl ... | tail -n N); other filters are not had. */
static int pipeline(session_t *s, toks_t *t, int a, int b, int depth)
{
    int end = a;
    while (end < b && t->type[end] != T_PIPE) {
        end++;
    }
    long limit = -1;
    for (int i = end; i < b;) {
        int j = ++i;
        while (j < b && t->type[j] != T_PIPE) {
            j++;
        }
        if (j > i && t->word[i] != NULL &&
            (strcmp(base(t->word[i]), "tail") == 0 || strcmp(base(t->word[i]), "head") == 0)) {
            long n = 10;
            for (int k = i + 1; k < j; k++) {
                const char *w = t->word[k];
                if (strcmp(w, "-n") == 0 && k + 1 < j) {
                    n = atol(t->word[++k]);
                } else if (strncmp(w, "-n", 2) == 0 && is_digits(w + 2)) {
                    n = atol(w + 2);
                } else if (strncmp(w, "--lines=", 8) == 0) {
                    n = atol(w + 8);
                }
            }
            limit = limit < 0 || n < limit ? n : limit;
        }
        i = j;
    }
    char *argv[ARGV_MAX + 1], *owned[ARGV_MAX];
    int argc = 0, nown = 0;
    for (int i = a; i < end && argc < ARGV_MAX; i++) {
        char *w = t->word[i];
        if (strchr(w, EXIT_MARK) != NULL) {
            char *x = expand(w, s->exit_code);
            if (x != NULL) {
                owned[nown++] = x;
                w = x;
            }
        }
        argv[argc++] = w;
    }
    argv[argc] = NULL;
    int rc = command(s, argv, argc, limit, depth);
    for (int i = 0; i < nown; i++) {
        free(owned[i]);
    }
    return rc;
}

/* A line, or a script given to bash -c: its exit code, also in $?. */
static int exec_script(session_t *s, const char *src, int depth)
{
    if (depth > DEPTH_MAX) {
        out(s, "bash: shells nested too deep\n");
        return 2;
    }
    toks_t *t = malloc(sizeof(*t));
    if (t == NULL || tokenize(src, t) != 0) {
        out(s, t == NULL || t->store == NULL ? "bash: out of memory\n" : "bash: a line too long\n");
        if (t != NULL) {
            free(t->store);
        }
        free(t);
        return 2;
    }
    int status = s->exit_code, skip = 0;
    for (int i = 0; i < t->n && !s->done && !s->follow;) {
        int start = i;
        while (i < t->n && t->type[i] != T_SEMI && t->type[i] != T_AND && t->type[i] != T_OR) {
            i++;
        }
        if (!skip && i > start) {
            if (t->type[start] != T_WORD) {
                out(s, "bash: syntax error near '|'\n");
                status = 2;
            } else {
                status = pipeline(s, t, start, i, depth);
            }
            s->exit_code = status;
        }
        if (i < t->n) {
            int op = t->type[i++];
            skip = op == T_AND ? status != 0 : op == T_OR ? status == 0 : 0;
        }
    }
    free(t->store);
    free(t);
    return status;
}

static int run(session_t *s, const char *line)
{
    return exec_script(s, line, 0);
}

/* ---- input ---------------------------------------------------------------------------- */

static void prompt(session_t *s)
{
    if (s->tty && !s->done) {
        out(s, "fc> ");
    }
}

/* A byte typed at the prompt. */
static void typed(session_t *s, uint8_t c)
{
    if (s->follow) { /* log -f: Ctrl-C ends it, the rest waits */
        if (c == 0x03) {
            s->follow = 0;
            out(s, "^C\n");
            prompt(s);
        }
        return;
    }
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
        if (!s->follow) {
            prompt(s);
        }
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

/* Bytes of a script on stdin (bash -s): each line, once whole (a newline
 * outside quotes), runs; a line over SCRIPT_MAX does not. */
static void script_in(session_t *s, const uint8_t *b, size_t n)
{
    for (size_t i = 0; i < n && !s->done && !s->follow; i++) {
        char c = (char)b[i];
        if (s->sskip) {
            if (c == '\n') {
                s->sskip = 0;
                out(s, "bash: a line over 2048 bytes: not run\n");
                s->exit_code = 2;
            }
            continue;
        }
        if (c == '\0') {
            continue;
        }
        if (s->slen >= SCRIPT_MAX) {
            s->sskip = 1;
            s->slen = 0;
            s->sq = s->sesc = 0;
            continue;
        }
        s->sbuf[s->slen++] = c;
        if (s->sesc) {
            s->sesc = 0;
        } else if (s->sq == '\'') {
            s->sq = c == '\'' ? 0 : s->sq;
        } else if (c == '\\') {
            s->sesc = 1;
        } else if (s->sq == '"') {
            s->sq = c == '"' ? 0 : s->sq;
        } else if (c == '\'' || c == '"') {
            s->sq = c;
        } else if (c == '\n') {
            s->sbuf[s->slen] = '\0';
            s->slen = 0;
            s->exit_code = run(s, s->sbuf);
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
    s->heard = 1;
    s->last_input = time(NULL);
    uint8_t ch = payload[0];
    if (ch == CH_STDIN) {
        if (s->script) {
            script_in(s, payload + 1, plen - 1);
        } else {
            for (size_t i = 1; i < plen && !s->done; i++) {
                typed(s, payload[i]);
            }
        }
    } else if (ch == CH_CLOSE && plen >= 2 && payload[1] == CH_STDIN && !s->tty) {
        /* Input over: a last line without its newline, then the end, unless
         * the log is followed (until the user leaves). */
        if (s->script && s->slen > 0 && !s->follow && !s->sskip) {
            s->sbuf[s->slen] = '\0';
            s->slen = 0;
            s->exit_code = run(s, s->sbuf);
        }
        if (!s->follow) {
            finish(s, s->exit_code);
        }
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

static void session_free(session_t *s)
{
    free(s->command);
    free(s->sbuf);
    free(s);
}

static void *session_thread(void *arg)
{
    session_t *s = arg;
    s->last_input = s->opened = time(NULL);
    if (s->command != NULL) {
        s->exit_code = run(s, s->command);
        if (!s->follow) {
            finish(s, s->exit_code);
        }
    } else if (s->tty) {
        char hello[128];
        pthread_mutex_lock(&s_lock);
        snprintf(hello, sizeof(hello), "flightctl-mcu console on %s: help lists the commands\n", s_device);
        pthread_mutex_unlock(&s_lock);
        out(s, hello);
        prompt(s);
    }
    uint8_t buf[256];
    while (!s->done && !__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE)) {
        int n = fc_http_stream_read(s->st, buf, sizeof(buf), 200);
        if (n < 0) {
            break; /* the server or the connection ended it */
        }
        if (n > 0) {
            on_bytes(s, buf, (size_t)n);
        } else if (!s->follow && time(NULL) - s->last_input > IDLE_S) {
            out(s, "\nno input for 10 minutes: the session ends\n");
            finish(s, 0);
        } else if (!s->heard && s->command == NULL && time(NULL) - s->opened > NO_USER_S) {
            /* A session left listed by the server (its user gone before the
             * device came): ended, so that the server lets go of it. */
            fc_port_log(FC_LOG_INFO, "console: nobody on the session after %d s: it ends", NO_USER_S);
            finish(s, 0);
        }
        if (s->follow && !s->done) {
            follow_some(s);
        }
    }
    if (__atomic_load_n(&s_stop, __ATOMIC_ACQUIRE) && !s->finished) {
        out(s, "\nthe device needs its connection: the session ends\n");
    }
    finish(s, s->exit_code);
    /* What finish sent goes out while the stream is still open. */
    for (int i = 0; i < 5 && fc_http_stream_read(s->st, buf, sizeof(buf), 100) >= 0; i++) {
    }
    fc_http_stream_close(s->st);
    int reboot = s->reboot;
    session_free(s);
    long unused = fc_port_stack_unused();
    if (unused >= 0) {
        fc_port_log(FC_LOG_INFO, "console: session ended (stack never used: %ld of %d bytes)", unused, THREAD_STACK);
    } else {
        fc_port_log(FC_LOG_INFO, "console: session ended");
    }
    if (reboot) {
        fc_port_log(FC_LOG_WARN, "console: restarting, as asked");
        sleep(1);
        fc_port_reboot();
    }
    int running = CONSOLE_RUNNING;
    /* A concurrent stop may already have claimed the thread for joining. */
    __atomic_compare_exchange_n(&s_thread_state, &running, CONSOLE_FINISHED, 0, __ATOMIC_RELEASE, __ATOMIC_RELAXED);
    return NULL;
}

/* A word as a shell reads it back: quoted unless plain. */
static size_t quote_into(char *o, const char *w)
{
    size_t n = 0;
    int plain = *w != '\0';
    for (const char *c = w; *c; c++) {
        plain &= (*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') ||
                 strchr("_./=:,@%+-", *c) != NULL;
    }
    if (plain) {
        n = strlen(w);
        memcpy(o, w, n);
        return n;
    }
    o[n++] = '\'';
    for (const char *c = w; *c; c++) {
        if (*c == '\'') {
            memcpy(o + n, "'\\''", 4);
            n += 4;
        } else {
            o[n++] = *c;
        }
    }
    o[n++] = '\'';
    return n;
}

/* "command arg..." of the session's metadata, quoted as a shell would need,
 * or NULL. A shell that reads stdin (bash -s, as the Logs tab opens) is
 * NULL too: the session then reads its commands. */
static char *command_of(const cJSON *meta)
{
    const cJSON *c = cJSON_GetObjectItem(meta, "command");
    const char *cmd = cJSON_GetStringValue(cJSON_GetObjectItem(c, "command"));
    if (cmd == NULL || cmd[0] == '\0') {
        return NULL;
    }
    const cJSON *args = cJSON_GetObjectItem(c, "args"), *a;
    int reads_stdin = is_shell(base(cmd));
    size_t cap = 4 * strlen(cmd) + 3;
    cJSON_ArrayForEach(a, args) {
        if (cJSON_IsString(a)) {
            cap += 4 * strlen(a->valuestring) + 3;
            reads_stdin &= a->valuestring[0] == '-' && strchr(a->valuestring, 'c') == NULL;
        }
    }
    if (reads_stdin) {
        return NULL;
    }
    char *buf = malloc(cap);
    if (buf == NULL) {
        return NULL;
    }
    size_t n = quote_into(buf, cmd);
    cJSON_ArrayForEach(a, args) {
        if (cJSON_IsString(a)) {
            buf[n++] = ' ';
            n += quote_into(buf + n, a->valuestring);
        }
    }
    buf[n] = '\0';
    return buf;
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

static void start(const char *id, const char *metadata, const fc_http_target_t *t, int busy, uint64_t *keep)
{
    /* Reclaim a finished console stack before allocating its replacement. */
    if (!busy) {
        fc_console_stop();
    }
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
    /* An unopened stream cannot execute anything and can be tried again.
     * Record an opened session before a command can request a reboot. */
    pthread_mutex_lock(&s_lock);
    *keep |= (uint64_t)1 << remember_locked(id_hash(id), *keep);
    pthread_mutex_unlock(&s_lock);
    s->tty = cJSON_IsTrue(cJSON_GetObjectItem(meta, "tty"));
    s->command = command_of(meta);
    cJSON_Delete(meta);
    s->script = !s->tty && s->command == NULL;
    if (busy || (s->script && (s->sbuf = malloc(SCRIPT_MAX + 1)) == NULL)) {
        out(s, busy ? "another console session is open on this device\n" : "out of memory\n");
        finish(s, 1);
        fc_http_stream_close(s->st);
        session_free(s);
        return;
    }
    fc_port_log(FC_LOG_INFO, "console: session %s%s", id, s->tty ? " (terminal)" : s->script ? " (commands on stdin)" : "");
    pthread_mutex_lock(&s_lock);
    __atomic_store_n(&s_stop, 0, __ATOMIC_RELEASE);
    pthread_attr_init(&s_attr);
    pthread_attr_setstacksize(&s_attr, THREAD_STACK);
    __atomic_store_n(&s_thread_state, CONSOLE_RUNNING, __ATOMIC_RELEASE);
    if (pthread_create(&s_thread, &s_attr, session_thread, s) != 0) {
        __atomic_store_n(&s_thread_state, CONSOLE_IDLE, __ATOMIC_RELEASE);
        pthread_mutex_unlock(&s_lock);
        pthread_attr_destroy(&s_attr);
        fc_port_log(FC_LOG_WARN, "console %s: no thread for it", id);
        out(s, "could not start the console thread\n");
        finish(s, 1);
        fc_http_stream_close(s->st);
        session_free(s);
        return;
    }
    pthread_mutex_unlock(&s_lock);
}

int fc_console_sync(const cJSON *consoles, const fc_http_target_t *t)
{
    int listed = 0;
    uint64_t keep = 0; /* the slots of the sessions listed now */
    const cJSON *c;
    pthread_mutex_lock(&s_lock);
    seen_load_locked();
    cJSON_ArrayForEach(c, consoles) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(c, "sessionID"));
        int slot = id != NULL && id[0] ? seen_locked(id_hash(id)) : -1;
        if (slot >= 0) {
            keep |= (uint64_t)1 << slot;
        }
    }
    pthread_mutex_unlock(&s_lock);
    cJSON_ArrayForEach(c, consoles) {
        const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(c, "sessionID"));
        const char *meta = cJSON_GetStringValue(cJSON_GetObjectItem(c, "sessionMetadata"));
        if (id == NULL || id[0] == '\0') {
            continue;
        }
        uint32_t h = id_hash(id);
        listed = listed ? listed : 1;
        pthread_mutex_lock(&s_lock);
        int busy = __atomic_load_n(&s_thread_state, __ATOMIC_ACQUIRE) == CONSOLE_RUNNING;
        if (seen_locked(h) >= 0) {
            listed = busy ? 2 : listed;
            pthread_mutex_unlock(&s_lock);
            continue;
        }
        listed = 2;
        pthread_mutex_unlock(&s_lock);
        start(id, meta, t, busy, &keep);
    }
    return listed;
}

int fc_console_finished(void)
{
    return __atomic_load_n(&s_thread_state, __ATOMIC_ACQUIRE) == CONSOLE_FINISHED;
}

void fc_console_stop(void)
{
    pthread_mutex_lock(&s_lock);
    int joinable = __atomic_exchange_n(&s_thread_state, CONSOLE_IDLE, __ATOMIC_ACQ_REL) != CONSOLE_IDLE;
    __atomic_store_n(&s_stop, 1, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&s_lock);
    if (joinable) {
        pthread_join(s_thread, NULL);
        pthread_attr_destroy(&s_attr);
    }
}
