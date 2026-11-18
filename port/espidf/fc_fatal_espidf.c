/*
 * What a crash was, on ESP-IDF: the panic handler's record (the exception,
 * the task, the pc and the return addresses), kept across the restart in RAM
 * that a restart does not clear, and given at the next boot as Zephyr's port
 * gives it: customInfo.lastFatalError, in fc_fatal.h's format.
 *
 * ESP-IDF's panic handler is wrapped (-Wl,--wrap=esp_panic_handler, as its own
 * panic tests do): the record is written, then ESP-IDF's handler prints its
 * report and restarts as before. The wrapper only copies: it formats nothing,
 * and it reads nothing in flash while the flash cache is off (a crash during a
 * flash write), when only the pc, the cause and the return addresses are kept.
 * A fault, an abort(), the interrupt watchdog and the task watchdog (when it
 * is set to panic) all come through here; a brownout does not. The same text
 * is the last line of the crashing boot's log (journalctl -b -1).
 */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_memory_utils.h"
#include "esp_private/cache_utils.h"
#include "esp_private/panic_internal.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "fc_fatal.h"
#include "fc_log.h"
#include "fc_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
#include "esp_core_dump.h"
#include "esp_partition.h"
#endif
#if __XTENSA__
#include "esp_cpu_utils.h"
#include "esp_debug_helpers.h"
#include "xtensa_context.h"
#else
#include "riscv/rvruntime-frames.h"
#endif

static __NOINIT_ATTR fc_fatal_t s_rec;
static char s_text[272];

size_t fc_port_coredump_size(void)
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    size_t address, size;
    if (esp_core_dump_image_get(&address, &size) == ESP_OK && esp_core_dump_image_check() == ESP_OK) {
        return size;
    }
#endif
    return 0;
}

void fc_port_coredump_erase(void)
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                           ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    if (part && esp_partition_erase_range(part, 0, part->size) != ESP_OK) {
        fc_port_log(FC_LOG_WARN, "could not erase the crash dump");
    }
#endif
}

int fc_port_coredump_read(size_t offset, void *buf, size_t len)
{
#if CONFIG_ESP_COREDUMP_ENABLE_TO_FLASH
    const esp_partition_t *part = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                           ESP_PARTITION_SUBTYPE_DATA_COREDUMP, NULL);
    size_t address, size;
    if (part && esp_core_dump_image_get(&address, &size) == ESP_OK && address == part->address &&
        size <= part->size && offset <= size && len <= size - offset) {
        return esp_partition_read(part, offset, buf, len) == ESP_OK ? 0 : -1;
    }
#endif
    return -1;
}

extern void __real_esp_panic_handler(panic_info_t *info);

/* "abort() was called at PC 0x42008a1e on core 0": the caller's pc. */
static IRAM_ATTR uint32_t abort_pc(const char *details)
{
    for (const char *p = details; p != NULL && *p != '\0'; p++) {
        if (p[0] == '0' && p[1] == 'x') {
            uint32_t v = 0;
            for (p += 2;; p++) {
                int d = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : -1;
                if (d < 0) {
                    return v;
                }
                v = v << 4 | (uint32_t)d;
            }
        }
    }
    return 0;
}

#if !__XTENSA__
/* An address in the firmware's code, in flash or in IRAM. Not
 * esp_ptr_executable: where the SRAM is one block for code and data (the C5,
 * the C6), pointers to the stack are executable too, and filled the record. */
static IRAM_ATTR int in_code(uint32_t a)
{
    extern char _iram_text_start[], _iram_text_end[], _text_start[], _text_end[];
    return (a >= (uint32_t)_iram_text_start && a < (uint32_t)_iram_text_end) ||
           (a >= (uint32_t)_text_start && a < (uint32_t)_text_end);
}
#endif

static IRAM_ATTR void add_addr(uint32_t a)
{
    if (s_rec.naddrs < FC_FATAL_ADDRS) {
        s_rec.addrs[s_rec.naddrs++] = a;
        fc_fatal_seal(&s_rec);
    }
}

void IRAM_ATTR __wrap_esp_panic_handler(panic_info_t *info)
{
    /* A fault in ESP-IDF's handler comes back here: the first one is the crash. */
    static DRAM_ATTR int s_entered;
    if (s_entered++) {
        __real_esp_panic_handler(info);
        return;
    }
    int cache = spi_flash_cache_enabled();
    extern bool g_panic_abort;
    extern char *g_panic_abort_details;
    extern bool g_twdt_isr __attribute__((weak)); /* task_wdt.c, when the task watchdog is in */
    /* Cleared word by word: memset may be in flash. */
    _Static_assert(sizeof(s_rec) % 4 == 0, "fc_fatal_t in words");
    for (volatile uint32_t *w = (volatile uint32_t *)&s_rec; w < (volatile uint32_t *)(&s_rec + 1); w++) {
        *w = 0;
    }
#if __XTENSA__
    const XtExcFrame *f = info->frame;
    /* The faulting instruction itself, as ESP-IDF's "PC:" (the backtrace's
     * entries are return addresses, moved back into their call). */
    s_rec.pc = f != NULL ? f->pc : (uint32_t)info->addr;
    s_rec.code = f != NULL ? f->exccause : 0;
    s_rec.kind = FC_FATAL_BACKTRACE;
#else
    const RvExcFrame *f = info->frame;
    s_rec.pc = f != NULL ? f->mepc : (uint32_t)info->addr;
    s_rec.code = f != NULL ? f->mcause : 0;
    s_rec.kind = FC_FATAL_RA_STACK;
#endif
    /* Strings, names and the uptime only while flash can be read: the
     * string literals are in flash too. Without them, "fatal error". */
    if (cache) {
        if (g_panic_abort && &g_twdt_isr != NULL && g_twdt_isr) {
            /* The task watchdog set to panic goes through abort()'s path, with
             * the frame of the task that was running: its pc is the one. */
            fc_fatal_copy(s_rec.reason, sizeof(s_rec.reason), "task watchdog");
        } else if (g_panic_abort) {
            /* The frame is panic_abort's own: the pc that matters is abort()'s caller. */
            fc_fatal_copy(s_rec.reason, sizeof(s_rec.reason), "abort()");
            uint32_t pc = abort_pc(g_panic_abort_details);
            if (pc != 0 && esp_ptr_executable((const void *)pc)) { /* not a number of an assert's text */
                s_rec.pc = pc;
            }
        } else if (info->reason != NULL) {
            fc_fatal_copy(s_rec.reason, sizeof(s_rec.reason), info->reason);
        } else {
            static const char *const kinds[] = {
                [PANIC_EXCEPTION_DEBUG] = "debug exception", [PANIC_EXCEPTION_IWDT] = "interrupt watchdog",
                [PANIC_EXCEPTION_TWDT] = "task watchdog",    [PANIC_EXCEPTION_ABORT] = "abort()",
                [PANIC_EXCEPTION_FAULT] = "fault",
            };
            fc_fatal_copy(s_rec.reason, sizeof(s_rec.reason),
                          (unsigned)info->exception <= PANIC_EXCEPTION_FAULT ? kinds[info->exception] : "panic");
        }
        TaskHandle_t t = xTaskGetCurrentTaskHandleForCore(info->core);
        fc_fatal_copy(s_rec.thread, sizeof(s_rec.thread), t != NULL ? pcTaskGetName(t) : "");
        fc_fatal_copy(s_rec.firmware, sizeof(s_rec.firmware), esp_app_get_description()->version);
        s_rec.uptime_ms = (uint32_t)(esp_timer_get_time() / 1000);
    }
    fc_fatal_seal(&s_rec);
    /* What was calling, sealed one address at a time: a fault while walking
     * the stack still leaves the record. */
    if (cache && f != NULL) {
#if __XTENSA__
        esp_backtrace_frame_t fr = {.pc = f->pc, .sp = f->a1, .next_pc = f->a0, .exc_frame = f};
        while (fr.next_pc != 0 && s_rec.naddrs < FC_FATAL_ADDRS && esp_stack_ptr_is_sane(fr.sp)) {
            int ok = esp_backtrace_get_next_frame(&fr);
            add_addr(esp_cpu_process_stack_pc(fr.pc));
            if (!ok) {
                break;
            }
        }
#else
        /* Without frame pointers there is no chain to follow: ra, then the
         * words of the stack that point into the firmware's code, as
         * ESP-IDF's stack dump shows them for idf.py monitor to decode. */
        add_addr(f->ra);
        const uint32_t *sp = (const uint32_t *)f->sp;
        for (int i = 0; i < 256 && s_rec.naddrs < FC_FATAL_ADDRS; i++) {
            if (!esp_ptr_byte_accessible(sp + i)) {
                break;
            }
            uint32_t w = sp[i];
            if (w != f->ra && w != f->mepc && (w & 1) == 0 && in_code(w)) {
                add_addr(w);
            }
        }
#endif
    }
    /* The last line of this boot's log, kept across the restart as Zephyr's
     * port keeps it (fc_log.h): formatting and the log are in flash. */
    if (cache) {
        static char line[sizeof(s_text) + 24];
        memcpy(line, "fatal: ", 7);
        fc_fatal_format(&s_rec, NULL, line + 7, sizeof(line) - 7);
        fc_log_put_fatal(line);
    }
    __real_esp_panic_handler(info);
}

/* At boot: the record, if this restart was a crash and the record checks out
 * (else it is another boot's, or RAM that was not kept); then cleared. */
void fc_espidf_fatal_boot(void)
{
    esp_reset_reason_t r = esp_reset_reason();
    int crash = r == ESP_RST_PANIC || r == ESP_RST_INT_WDT || r == ESP_RST_TASK_WDT || r == ESP_RST_WDT;
    if (crash && fc_fatal_valid(&s_rec)) {
        fc_fatal_format(&s_rec, esp_app_get_description()->version, s_text, sizeof(s_text));
        fc_port_log(FC_LOG_WARN, "last crash: %s", s_text);
    }
    memset(&s_rec, 0, sizeof(s_rec));
#if CONFIG_FC_TEST_FAULT_AFTER_S > 0
    extern void fc_fatal_test_start(void);
    if (!crash) {
        fc_fatal_test_start();
    }
#endif
}

/* customInfo.lastFatalError: NULL unless the last restart was a crash. */
const char *fc_espidf_last_fatal(void)
{
    return s_text[0] != '\0' ? s_text : NULL;
}

#if CONFIG_FC_TEST_FAULT_AFTER_S > 0
/* Test builds only (sdkconfig.faulttest): CONFIG_FC_TEST_FAULT_AFTER_S after
 * a boot that was not a crash, a task of its own writes to address 0, the
 * fault a bug makes, or calls abort() (sdkconfig.aborttest), or loops with
 * interrupts off until the interrupt watchdog panics (sdkconfig.iwdttest), or
 * loops above the idle task until the task watchdog does (sdkconfig.twdttest).
 * Its name and this function's are what the test looks for in
 * lastFatalError, decoded against the ELF. */
__attribute__((noinline)) void fc_fatal_test_fault(volatile int *p)
{
#if CONFIG_FC_TEST_FAULT_ABORT
    if (p == NULL) {
        abort(); /* as a failed assert or ESP_ERROR_CHECK does */
    }
#elif CONFIG_FC_TEST_FAULT_IWDT
    /* A driver stuck in a critical section: this core's tick stops. */
    portDISABLE_INTERRUPTS();
    while (p == NULL) {
        __asm__ volatile("nop");
    }
#elif CONFIG_FC_TEST_FAULT_TWDT
    /* A task that never blocks: the idle task of its core starves. */
    while (p == NULL) {
        __asm__ volatile("nop");
    }
#endif
    *p = 1;
}

static void fault_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(CONFIG_FC_TEST_FAULT_AFTER_S * 1000));
    fc_port_log(FC_LOG_WARN, "test build: faulting on purpose");
    fc_fatal_test_fault((volatile int *)arg);
    vTaskDelete(NULL);
}

void fc_fatal_test_start(void)
{
    xTaskCreate(fault_task, "fc_fault", 3072, NULL, 5, NULL);
}
#endif
