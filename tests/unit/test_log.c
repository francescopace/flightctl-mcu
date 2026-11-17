/*
 * The device's log in RAM (fc_log.c): what a restart keeps, and a ring that
 * does not check out after it, which is started again empty.
 */
#include "../../core/src/fc_log.c"

#include "port_stub.h"
#include "test.h"

static int count(int previous)
{
    uint32_t seq = 0;
    fc_log_entry_t e;
    int n = 0;
    while (fc_log_next(&seq, &e) == 0) {
        n += e.previous == previous;
    }
    return n;
}

/* A line cut in the middle by a restart is not there; the ones before are. */
static void a_restart_keeps_the_lines_written(void)
{
    fc_log_put(FC_LOG_INFO, "one");
    fc_log_put(FC_LOG_WARN, "two");
    /* The next line's bytes written, its indices not yet moved. */
    uint32_t at = (s_mem.head + s_mem.used) % FC_LOG_RING;
    uint8_t h[HDR] = {5, 0, FC_LOG_INFO, 0, 0, 0, 0};
    put_bytes(at, h, HDR);
    put_bytes(at + HDR, "three", 5);
    fc_log_restart();
    CHECK_INT(count(1), 2);
    CHECK_INT(count(0), 0);
    uint32_t seq = 0;
    fc_log_entry_t e;
    CHECK_INT(fc_log_next(&seq, &e), 0);
    CHECK_STR(e.text, "one");
    CHECK_INT(e.previous, 1);
    CHECK_INT(e.system, 0);
}

/* The previous boot's time: from its clock, when it was set. */
static void the_previous_boot_keeps_its_time(void)
{
    fc_log_restart();
    fc_log_put(FC_LOG_INFO, "with a clock");
    int64_t epoch = s_mem.boot_epoch;
    CHECK(epoch > CLOCK_SET);
    fc_log_restart();
    uint32_t seq = 0;
    fc_log_entry_t e;
    while (fc_log_next(&seq, &e) == 0 && strcmp(e.text, "with a clock") != 0) {
    }
    CHECK_INT(e.previous, 1);
    CHECK(e.when == epoch + e.up_s);
}

/* Whatever RAM held at a power-on, or a ring of another layout: empty. */
static void a_ring_that_does_not_check_out_is_dropped(void)
{
    fc_log_put(FC_LOG_INFO, "kept until the damage");
    s_mem.ring[(s_mem.head + 0) % FC_LOG_RING] = 0xff; /* a first entry too long */
    fc_log_restart();
    CHECK_INT(fc_log_end() - s_mem.head_seq, 0);
    CHECK_INT(count(1) + count(0), 0);
    fc_log_put(FC_LOG_INFO, "x");
    s_mem.magic ^= 1;
    fc_log_restart();
    CHECK_INT(count(1) + count(0), 0);
    fc_log_put(FC_LOG_INFO, "y");
    s_mem.end_seq += 3; /* says it holds more lines than it does */
    fc_log_restart();
    CHECK_INT(count(1) + count(0), 0);
}

int main(void)
{
    RUN(a_restart_keeps_the_lines_written);
    RUN(the_previous_boot_keeps_its_time);
    RUN(a_ring_that_does_not_check_out_is_dropped);
    return TEST_RESULT();
}
