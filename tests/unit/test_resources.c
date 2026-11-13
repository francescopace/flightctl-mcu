/*
 * Resource monitors: spec.resources, the defaults, alert rules with their
 * durations, CPU from two readings, status.resources and the summary.
 * Time is passed in, so the durations run without waiting.
 */
#include "../../core/src/fc_resources.c"

#include "port_stub.h"
#include "test.h"

static cJSON *parse(const char *json)
{
    cJSON *j = cJSON_Parse(json);
    if (j == NULL) {
        fprintf(stderr, "bad test JSON: %s\n", json);
    }
    return j;
}

/* status.resources.<key> and the summary level, as text "cpu memory disk level". */
static const char *state(void)
{
    static char out[96];
    char info[80];
    cJSON *r = cJSON_CreateObject();
    int level = fc_res_status(r, info, sizeof(info));
    snprintf(out, sizeof(out), "%s %s %s %d", cJSON_GetStringValue(cJSON_GetObjectItem(r, "cpu")),
             cJSON_GetStringValue(cJSON_GetObjectItem(r, "memory")),
             cJSON_GetStringValue(cJSON_GetObjectItem(r, "disk")), level);
    cJSON_Delete(r);
    return out;
}

static fc_sysinfo_t si(int mem_used, int disk_used)
{
    fc_sysinfo_t s = {0};
    s.mem_total = 1000;
    s.mem_free = 1000 - (uint64_t)mem_used * 10;
    s.disk_total = 1000;
    s.disk_free = 1000 - (uint64_t)disk_used * 10;
    return s;
}

static void test_durations(void)
{
    CHECK_INT(parse_duration("30m"), 1800);
    CHECK_INT(parse_duration("1h"), 3600);
    CHECK_INT(parse_duration("10s"), 10);
    CHECK_INT(parse_duration("0s"), 0);
    CHECK_INT(parse_duration("1h30m"), 5400);
    CHECK_INT(parse_duration(""), -1);
    CHECK_INT(parse_duration("5"), -1);
    CHECK_INT(parse_duration("5d"), -1);
    CHECK_INT(parse_duration("m"), -1);
    CHECK_INT(parse_duration(NULL), -1);
}

/* Without spec.resources: CPU and disk as the Linux agent, memory under 10% free. */
static void test_defaults(void)
{
    long long t = 1000;
    fc_res_reset(t);
    CHECK_INT(fc_res_next_sample(), t);
    fc_sysinfo_t s = si(50, 85);
    CHECK_INT(fc_res_sample(&s, t), 0);
    CHECK_STR(state(), "Healthy Healthy Healthy 0");
    /* disk above 80% for 30 minutes: Warning */
    for (t += 60; t <= 1000 + 1800; t += 60) {
        fc_res_sample(&s, t);
    }
    CHECK_STR(state(), "Healthy Healthy Warning 1");
    /* memory above 90%: Warning at the next sample */
    s = si(95, 85);
    CHECK_INT(fc_res_sample(&s, t), 1);
    CHECK_STR(state(), "Healthy Warning Warning 1");
    /* back under: cleared at once */
    s = si(50, 50);
    CHECK_INT(fc_res_sample(&s, t + 60), 1);
    CHECK_STR(state(), "Healthy Healthy Healthy 0");
}

static void test_spec_rules(void)
{
    long long t = 5000;
    fc_res_reset(t);
    char err[160] = "";
    cJSON *r = parse("[{\"monitorType\":\"Memory\",\"samplingInterval\":\"5s\",\"alertRules\":["
                     "{\"severity\":\"Warning\",\"duration\":\"10s\",\"percentage\":50,\"description\":\"\"},"
                     "{\"severity\":\"Critical\",\"duration\":\"20s\",\"percentage\":70,\"description\":\"heap low\"}]}]");
    CHECK_INT(fc_res_apply(r, t, err, sizeof(err)), 0);
    cJSON_Delete(r);
    fc_sysinfo_t s = si(80, 0);
    fc_res_sample(&s, t);
    CHECK_STR(state(), "Healthy Healthy Healthy 0");
    CHECK_INT(fc_res_next_sample(), t + 5);
    /* not due yet: nothing changes */
    CHECK_INT(fc_res_sample(&s, t + 4), 0);
    fc_res_sample(&s, t + 5);
    CHECK_INT(fc_res_sample(&s, t + 10), 1);
    CHECK_STR(state(), "Healthy Warning Healthy 1");
    fc_res_sample(&s, t + 15);
    CHECK_INT(fc_res_sample(&s, t + 20), 1);
    CHECK_STR(state(), "Healthy Critical Healthy 2");
    char info[80];
    cJSON *o = cJSON_CreateObject();
    CHECK_INT(fc_res_status(o, info, sizeof(info)), 2);
    CHECK_STR(info, "Critical resource alert: Memory");
    cJSON_Delete(o);

    /* the same spec again keeps the rules' state */
    r = parse("[{\"monitorType\":\"Memory\",\"samplingInterval\":\"5s\",\"alertRules\":["
              "{\"severity\":\"Warning\",\"duration\":\"10s\",\"percentage\":50,\"description\":\"\"},"
              "{\"severity\":\"Critical\",\"duration\":\"20s\",\"percentage\":70,\"description\":\"heap low\"}]}]");
    fc_res_apply(r, t + 21, err, sizeof(err));
    cJSON_Delete(r);
    CHECK_STR(state(), "Healthy Critical Healthy 2");

    /* No resources in the spec: the defaults again (memory Warning above
     * 90%). The Warning rule stays firing until the next sample, as a rule
     * updated in the Linux agent does; the Critical one is gone. */
    fc_res_apply(NULL, t + 30, err, sizeof(err));
    CHECK_STR(state(), "Healthy Warning Healthy 1");
    CHECK_INT(fc_res_sample(&s, t + 30), 1);
    CHECK_STR(state(), "Healthy Healthy Healthy 0");
}

static void test_invalid(void)
{
    fc_res_reset(0);
    char err[160] = "";
    cJSON *r = parse("[{\"monitorType\":\"GPU\",\"samplingInterval\":\"5s\",\"alertRules\":[]},"
                     "{\"monitorType\":\"Disk\",\"samplingInterval\":\"5s\",\"path\":\"/\",\"alertRules\":["
                     "{\"severity\":\"Bad\",\"duration\":\"10s\",\"percentage\":1}]},"
                     "{\"monitorType\":\"CPU\",\"samplingInterval\":\"5s\",\"alertRules\":[]}]");
    CHECK_INT(fc_res_apply(r, 0, err, sizeof(err)), -1);
    CHECK_CONTAINS(err, "Disk");
    cJSON_Delete(r);
    /* CPU with no rules: never sampled; the disk keeps its defaults */
    fc_sysinfo_t s = si(0, 0);
    s.cpu_total = 100;
    fc_res_sample(&s, 0);
    CHECK_INT(fc_res_cpu_percent(), -1);
    CHECK_INT(s_mon[RES_DISK].rule[SEV_WARNING].set, 1);
}

static void test_cpu(void)
{
    long long t = 0;
    fc_res_reset(t);
    char err[160] = "";
    cJSON *r = parse("[{\"monitorType\":\"CPU\",\"samplingInterval\":\"1s\",\"alertRules\":["
                     "{\"severity\":\"Warning\",\"duration\":\"2s\",\"percentage\":60,\"description\":\"\"}]}]");
    fc_res_apply(r, t, err, sizeof(err));
    cJSON_Delete(r);
    fc_sysinfo_t s = si(0, 0);
    /* the port cannot tell: nothing */
    fc_res_sample(&s, t++);
    CHECK_INT(fc_res_cpu_percent(), -1);
    /* first reading: no figure yet */
    s.cpu_busy = 1000, s.cpu_total = 10000;
    fc_res_sample(&s, t++);
    CHECK_INT(fc_res_cpu_percent(), -1);
    /* 750 busy out of 1000 */
    s.cpu_busy = 1750, s.cpu_total = 11000;
    fc_res_sample(&s, t++);
    CHECK_INT(fc_res_cpu_percent(), 75);
    s.cpu_busy = 2500, s.cpu_total = 12000;
    fc_res_sample(&s, t++);
    s.cpu_busy = 3250, s.cpu_total = 13000;
    CHECK_INT(fc_res_sample(&s, t++), 1);
    CHECK_STR(state(), "Warning Healthy Healthy 1");
    /* idle */
    s.cpu_busy = 3260, s.cpu_total = 14000;
    CHECK_INT(fc_res_sample(&s, t++), 1);
    CHECK_INT(fc_res_cpu_percent(), 1);
    CHECK_STR(state(), "Healthy Healthy Healthy 0");
}

int main(void)
{
    RUN(test_durations);
    RUN(test_defaults);
    RUN(test_spec_rules);
    RUN(test_invalid);
    RUN(test_cpu);
    return TEST_RESULT();
}
