#include "fc_resources.h"

#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

enum { RES_CPU, RES_MEM, RES_DISK, RES_N };
enum { SEV_INFO, SEV_WARNING, SEV_CRITICAL, SEV_N };

static const char *const k_type[RES_N] = {"CPU", "Memory", "Disk"};
static const char *const k_key[RES_N] = {"cpu", "memory", "disk"};
static const char *const k_sev[SEV_N] = {"Info", "Warning", "Critical"};

typedef struct {
    int set;
    double percentage;
    long long duration;
    char duration_text[12];
    char description[96];
    int above;       /* use was above the percentage at the last sample */
    long long since; /* ... since then */
    int firing;
} rule_t;

typedef struct {
    long long interval;
    long long next;
    rule_t rule[SEV_N];
} monitor_t;

static pthread_mutex_t s_lock = PTHREAD_MUTEX_INITIALIZER;
static monitor_t s_mon[RES_N];
static int s_cpu_have, s_cpu_pct = -1;
static uint64_t s_cpu_busy, s_cpu_total;

long long fc_res_now(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long)ts.tv_sec;
}

/* Flight Control's format: digits and a unit (s, m, h), e.g. "30m". A
 * sequence ("1h30m") is taken too, as Go's time.ParseDuration does. */
static long long parse_duration(const char *s)
{
    if (s == NULL || *s == '\0') {
        return -1;
    }
    long long total = 0;
    while (*s) {
        if (*s < '0' || *s > '9') {
            return -1;
        }
        long long n = 0;
        while (*s >= '0' && *s <= '9') {
            n = n * 10 + (*s++ - '0');
            if (n > 100000000LL) {
                return -1;
            }
        }
        switch (*s++) {
        case 'h': total += n * 3600; break;
        case 'm': total += n * 60; break;
        case 's': total += n; break;
        default: return -1;
        }
    }
    return total;
}

/* The rules a monitor will have: a rule kept under the same severity keeps
 * its state (how long the use has been above), as in the Linux agent. */
static void set_rules(monitor_t *m, const rule_t *rules, long long interval, long long now)
{
    int had = 0, has = 0;
    for (int s = 0; s < SEV_N; s++) {
        had |= m->rule[s].set;
        has |= rules[s].set;
        if (!rules[s].set) {
            memset(&m->rule[s], 0, sizeof(m->rule[s]));
            continue;
        }
        rule_t r = rules[s];
        if (m->rule[s].set) {
            r.above = m->rule[s].above;
            r.since = m->rule[s].since;
            r.firing = m->rule[s].firing;
        }
        m->rule[s] = r;
    }
    if (interval < 1) {
        interval = 1;
    }
    /* First sample at once (the CPU needs two); a shorter interval from now. */
    if (has && !had) {
        m->next = now;
    } else if (interval < m->interval && now + interval < m->next) {
        m->next = now + interval;
    }
    m->interval = interval;
}

static void rule_init(rule_t *r, double pct, const char *duration)
{
    memset(r, 0, sizeof(*r));
    r->set = 1;
    r->percentage = pct;
    r->duration = parse_duration(duration);
    snprintf(r->duration_text, sizeof(r->duration_text), "%s", duration);
}

static void defaults(int type, rule_t *rules)
{
    memset(rules, 0, sizeof(rule_t) * SEV_N);
    switch (type) {
    case RES_CPU:
        rule_init(&rules[SEV_CRITICAL], 90, "30m");
        rule_init(&rules[SEV_WARNING], 80, "1h");
        break;
    case RES_DISK:
        rule_init(&rules[SEV_CRITICAL], 90, "10m");
        rule_init(&rules[SEV_WARNING], 80, "30m");
        break;
    case RES_MEM:
        rule_init(&rules[SEV_WARNING], 90, "0s");
        break;
    }
}

void fc_res_reset(long long now)
{
    pthread_mutex_lock(&s_lock);
    for (int t = 0; t < RES_N; t++) {
        rule_t rules[SEV_N];
        defaults(t, rules);
        set_rules(&s_mon[t], rules, 60, now);
    }
    pthread_mutex_unlock(&s_lock);
}

int fc_res_apply(const cJSON *resources, long long now, char *err, size_t err_len)
{
    if (!cJSON_IsArray(resources)) {
        fc_res_reset(now);
        return 0;
    }
    int rc = 0;
    const cJSON *mon;
    cJSON_ArrayForEach(mon, resources) {
        const char *type = cJSON_GetStringValue(cJSON_GetObjectItem(mon, "monitorType"));
        int t = 0;
        while (t < RES_N && (type == NULL || strcmp(type, k_type[t]) != 0)) {
            t++;
        }
        long long interval = parse_duration(cJSON_GetStringValue(cJSON_GetObjectItem(mon, "samplingInterval")));
        if (t == RES_N || interval < 1) {
            snprintf(err, err_len, "resource monitor %s: unknown type or sampling interval", type ? type : "?");
            rc = -1;
            continue;
        }
        rule_t rules[SEV_N] = {0};
        int ok = 1;
        const cJSON *ar;
        cJSON_ArrayForEach(ar, cJSON_GetObjectItem(mon, "alertRules")) {
            const char *sev = cJSON_GetStringValue(cJSON_GetObjectItem(ar, "severity"));
            const char *dur = cJSON_GetStringValue(cJSON_GetObjectItem(ar, "duration"));
            const cJSON *pct = cJSON_GetObjectItem(ar, "percentage");
            int s = 0;
            while (s < SEV_N && (sev == NULL || strcmp(sev, k_sev[s]) != 0)) {
                s++;
            }
            if (s == SEV_N || !cJSON_IsNumber(pct) || parse_duration(dur) < 0 || strlen(dur) >= 12) {
                ok = 0;
                break;
            }
            rule_init(&rules[s], pct->valuedouble, dur);
            const char *desc = cJSON_GetStringValue(cJSON_GetObjectItem(ar, "description"));
            snprintf(rules[s].description, sizeof(rules[s].description), "%s", desc ? desc : "");
        }
        if (!ok) {
            snprintf(err, err_len, "resource monitor %s: invalid alert rule", type);
            rc = -1;
            continue;
        }
        pthread_mutex_lock(&s_lock);
        set_rules(&s_mon[t], rules, interval, now);
        if (t == RES_CPU && !s_mon[t].rule[0].set && !s_mon[t].rule[1].set && !s_mon[t].rule[2].set) {
            s_cpu_have = 0;
            s_cpu_pct = -1;
        }
        pthread_mutex_unlock(&s_lock);
    }
    return rc;
}

static int has_rules(const monitor_t *m)
{
    return m->rule[0].set || m->rule[1].set || m->rule[2].set;
}

long long fc_res_next_sample(void)
{
    long long next = -1;
    pthread_mutex_lock(&s_lock);
    for (int t = 0; t < RES_N; t++) {
        if (has_rules(&s_mon[t]) && (next < 0 || s_mon[t].next < next)) {
            next = s_mon[t].next;
        }
    }
    pthread_mutex_unlock(&s_lock);
    return next;
}

static int used_percent(uint64_t total, uint64_t free_)
{
    if (total == 0) {
        return -1;
    }
    if (free_ > total) {
        free_ = total;
    }
    return (int)((double)(total - free_) * 100.0 / (double)total);
}

static void alert_text(int t, int s, const rule_t *r, char *out, size_t len)
{
    if (r->description[0]) {
        snprintf(out, len, "%s: %s", k_sev[s], r->description);
    } else {
        snprintf(out, len, "%s: %s usage is above %d %% for more than %s", k_sev[s], k_type[t], (int)r->percentage,
                 r->duration_text);
    }
}

int fc_res_sample(const fc_sysinfo_t *si, long long now)
{
    int changed = 0;
    pthread_mutex_lock(&s_lock);
    for (int t = 0; t < RES_N; t++) {
        monitor_t *m = &s_mon[t];
        if (!has_rules(m) || now < m->next) {
            continue;
        }
        m->next = now + m->interval;
        int use = -1;
        if (t == RES_CPU) {
            if (si->cpu_total == 0) {
                continue;
            }
            if (s_cpu_have && si->cpu_total > s_cpu_total && si->cpu_busy >= s_cpu_busy) {
                double pct = (double)(si->cpu_busy - s_cpu_busy) * 100.0 / (double)(si->cpu_total - s_cpu_total);
                use = pct > 100 ? 100 : (int)pct;
                s_cpu_pct = use;
            }
            s_cpu_have = 1;
            s_cpu_busy = si->cpu_busy;
            s_cpu_total = si->cpu_total;
        } else if (t == RES_MEM) {
            use = used_percent(si->mem_total, si->mem_free);
        } else {
            use = used_percent(si->disk_total, si->disk_free);
        }
        if (use < 0) {
            continue;
        }
        for (int s = 0; s < SEV_N; s++) {
            rule_t *r = &m->rule[s];
            if (!r->set) {
                continue;
            }
            if (use > (int)r->percentage) {
                if (!r->above) {
                    r->above = 1;
                    r->since = now;
                }
            } else {
                r->above = 0;
            }
            int firing = r->above && now - r->since >= r->duration;
            if (firing != r->firing) {
                r->firing = firing;
                changed = 1;
                char text[160];
                alert_text(t, s, r, text, sizeof(text));
                fc_port_log(firing ? FC_LOG_WARN : FC_LOG_INFO, "resource alert %s (%s at %d%%)", text,
                            firing ? "firing" : "cleared", use);
            }
        }
    }
    pthread_mutex_unlock(&s_lock);
    return changed;
}

int fc_res_status(cJSON *resources, char *info, size_t info_len)
{
    static const char *const status[] = {"Healthy", "Healthy", "Warning", "Critical"};
    int worst = 0;
    char names[2][40] = {"", ""};
    pthread_mutex_lock(&s_lock);
    for (int t = 0; t < RES_N; t++) {
        int level = 0; /* 0 none, 1 + severity */
        for (int s = 0; s < SEV_N; s++) {
            if (s_mon[t].rule[s].set && s_mon[t].rule[s].firing) {
                level = 1 + s;
            }
        }
        cJSON_AddStringToObject(resources, k_key[t], status[level]);
        if (level >= 2) {
            char *n = names[level - 2];
            size_t used = strlen(n);
            snprintf(n + used, sizeof(names[0]) - used, "%s%s", used ? ", " : "", k_type[t]);
            if (level - 1 > worst) {
                worst = level - 1;
            }
        }
    }
    pthread_mutex_unlock(&s_lock);
    if (worst == 2) {
        snprintf(info, info_len, "Critical resource alert: %s", names[1]);
    } else if (worst == 1) {
        snprintf(info, info_len, "Degraded resource alert: %s", names[0]);
    } else if (info_len > 0) {
        info[0] = '\0';
    }
    return worst;
}

int fc_res_cpu_percent(void)
{
    pthread_mutex_lock(&s_lock);
    int p = s_cpu_pct;
    pthread_mutex_unlock(&s_lock);
    return p;
}
