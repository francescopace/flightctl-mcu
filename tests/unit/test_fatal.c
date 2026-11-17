/*
 * What a crash was (fc_fatal.h): the record a fault handler leaves across the
 * restart, checked before it is believed, and the text of lastFatalError, the
 * same on Zephyr and ESP-IDF.
 */
#include "fc_fatal.h"

#include "test.h"

static fc_fatal_t rec(int kind)
{
    fc_fatal_t f;
    memset(&f, 0, sizeof(f));
    f.code = 28;
    f.uptime_ms = 30512;
    f.pc = 0x42008a1e;
    f.kind = (uint8_t)kind;
    fc_fatal_copy(f.reason, sizeof(f.reason), "LoadProhibited");
    fc_fatal_copy(f.thread, sizeof(f.thread), "fc_fault");
    fc_fatal_copy(f.firmware, sizeof(f.firmware), "v3.14");
    fc_fatal_seal(&f);
    return f;
}

static void test_zephyr_format(void)
{
    /* Zephyr's port (port/zephyr/fc_port_zephyr.c) formats with fc_fatal.h
     * too. On Arm: the pc and lr, as before the format was shared. */
    fc_fatal_t f = rec(FC_FATAL_LR);
    fc_fatal_copy(f.reason, sizeof(f.reason), "CPU exception");
    f.code = 0;
    f.uptime_ms = 1234;
    f.pc = 0x00001f2a;
    f.addrs[f.naddrs++] = 0x00001e01;
    fc_fatal_copy(f.thread, sizeof(f.thread), "fc_agent");
    fc_fatal_seal(&f);
    char buf[272];
    fc_fatal_format(&f, "v3.14", buf, sizeof(buf));
    CHECK_STR(buf, "CPU exception (0) in thread \"fc_agent\", 1234 ms after boot, pc 0x00001f2a lr 0x00001e01");
    /* Where the port reads no registers (native_sim), or the frame did not
     * check out: the pc 0 and no addresses. */
    f.naddrs = 0;
    f.pc = 0;
    fc_fatal_seal(&f);
    fc_fatal_format(&f, "v3.14", buf, sizeof(buf));
    CHECK_STR(buf, "CPU exception (0) in thread \"fc_agent\", 1234 ms after boot, pc 0x00000000");
    /* A thread name longer than the record keeps: cut. */
    fc_fatal_copy(f.thread, sizeof(f.thread), "a_thread_name_of_31_characters_");
    CHECK_STR(f.thread, "a_thread_name_of_");
}

static void test_backtrace(void)
{
    fc_fatal_t f = rec(FC_FATAL_BACKTRACE);
    f.addrs[f.naddrs++] = 0x42008a3b;
    f.addrs[f.naddrs++] = 0x4037d2c1;
    fc_fatal_seal(&f);
    CHECK(fc_fatal_valid(&f));
    char buf[272];
    fc_fatal_format(&f, "v3.14", buf, sizeof(buf));
    CHECK_STR(buf, "LoadProhibited (28) in thread \"fc_fault\", 30512 ms after boot, pc 0x42008a1e "
                   "backtrace 0x42008a3b 0x4037d2c1");
}

static void test_ra_stack(void)
{
    fc_fatal_t f = rec(FC_FATAL_RA_STACK);
    f.addrs[f.naddrs++] = 0x42001234;
    f.addrs[f.naddrs++] = 0x42005678;
    f.addrs[f.naddrs++] = 0x4038abcd;
    fc_fatal_seal(&f);
    char buf[272];
    fc_fatal_format(&f, "v3.14", buf, sizeof(buf));
    CHECK_STR(buf, "LoadProhibited (28) in thread \"fc_fault\", 30512 ms after boot, pc 0x42008a1e "
                   "ra 0x42001234, stack 0x42005678 0x4038abcd");
    /* ra alone */
    f.naddrs = 1;
    fc_fatal_seal(&f);
    fc_fatal_format(&f, "v3.14", buf, sizeof(buf));
    CHECK_STR(buf, "LoadProhibited (28) in thread \"fc_fault\", 30512 ms after boot, pc 0x42008a1e ra 0x42001234");
}

static void test_other_firmware(void)
{
    /* A rollback after the crash: the addresses are the other ELF's. */
    fc_fatal_t f = rec(FC_FATAL_BACKTRACE);
    char buf[272];
    fc_fatal_format(&f, "v3.13", buf, sizeof(buf));
    CHECK_STR(buf, "LoadProhibited (28) in thread \"fc_fault\", 30512 ms after boot, pc 0x42008a1e, firmware v3.14");
    fc_fatal_format(&f, NULL, buf, sizeof(buf));
    CHECK_STR(buf, "LoadProhibited (28) in thread \"fc_fault\", 30512 ms after boot, pc 0x42008a1e");
}

static void test_not_believed(void)
{
    fc_fatal_t f;
    memset(&f, 0xa5, sizeof(f)); /* RAM after a power cut */
    CHECK(!fc_fatal_valid(&f));
    f = rec(FC_FATAL_BACKTRACE);
    CHECK(fc_fatal_valid(&f));
    f.pc ^= 4; /* changed after it was sealed */
    CHECK(!fc_fatal_valid(&f));
    f = rec(FC_FATAL_BACKTRACE);
    f.naddrs = FC_FATAL_ADDRS + 1;
    fc_fatal_seal(&f);
    CHECK(!fc_fatal_valid(&f));
    f = rec(7);
    CHECK(!fc_fatal_valid(&f));
    f = rec(FC_FATAL_BACKTRACE);
    f.magic = 0; /* cleared at boot once read */
    CHECK(!fc_fatal_valid(&f));
}

static void test_strings_from_the_handler(void)
{
    /* Bounded, quotes and control characters replaced, NULL taken. */
    char s[8];
    fc_fatal_copy(s, sizeof(s), "a\"b\nc0123456789");
    CHECK_STR(s, "a?b?c01");
    fc_fatal_copy(s, sizeof(s), NULL);
    CHECK_STR(s, "");
    /* A handler that could not read the reason (flash cache off). */
    fc_fatal_t f = rec(FC_FATAL_BACKTRACE);
    f.reason[0] = '\0';
    f.thread[0] = '\0';
    f.uptime_ms = 0;
    fc_fatal_seal(&f);
    char buf[272];
    fc_fatal_format(&f, "v3.14", buf, sizeof(buf));
    CHECK_STR(buf, "fatal error (28) in thread \"\", 0 ms after boot, pc 0x42008a1e");
    /* The longest record fits the buffer the ports give it. */
    memset(f.reason, 'r', sizeof(f.reason) - 1);
    memset(f.thread, 't', sizeof(f.thread) - 1);
    memset(f.firmware, 'f', sizeof(f.firmware) - 1);
    f.code = 0xffffffffu;
    f.uptime_ms = 0xffffffffu;
    f.kind = FC_FATAL_RA_STACK;
    f.naddrs = FC_FATAL_ADDRS;
    for (int i = 0; i < FC_FATAL_ADDRS; i++) {
        f.addrs[i] = 0xffffffffu;
    }
    fc_fatal_seal(&f);
    fc_fatal_format(&f, "v3.14", buf, sizeof(buf));
    CHECK(strlen(buf) < sizeof(buf) - 1);
    CHECK_CONTAINS(buf, ", firmware fffffff");
    /* and a short buffer cuts it, without overrunning */
    char small[40];
    fc_fatal_format(&f, "v3.14", small, sizeof(small));
    CHECK_INT(strlen(small), sizeof(small) - 1);
}

int main(void)
{
    RUN(test_zephyr_format);
    RUN(test_backtrace);
    RUN(test_ra_stack);
    RUN(test_other_firmware);
    RUN(test_not_believed);
    RUN(test_strings_from_the_handler);
    return TEST_RESULT();
}
