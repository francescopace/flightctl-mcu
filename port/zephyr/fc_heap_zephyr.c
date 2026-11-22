/*
 * The C heap (malloc and friends), on a heap this port owns, so that it can
 * say how much of it is free and how low it has been: Zephyr's own malloc
 * does not give that to applications (in 4.5, only through a header private
 * to its libc). Built instead of it (CONFIG_COMMON_LIBC_MALLOC=n),
 * and written like it: a sys_heap behind a mutex.
 */
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/math_extras.h>
#include <zephyr/sys/mutex.h>
#include <zephyr/sys/sys_heap.h>

#include "fc_port_zephyr.h"

#define HEAP_ALIGN __alignof__(z_max_align_t)

static unsigned char __aligned(HEAP_ALIGN) s_arena[CONFIG_FC_HEAP_SIZE];
static struct sys_heap s_heap;
static SYS_MUTEX_DEFINE(s_lock);
static size_t s_boot_low = SIZE_MAX;
static size_t s_watch_low = SIZE_MAX;

#ifdef CONFIG_FC_ESP_INTERNAL_HEAPS
#include <zephyr/sys/multi_heap.h>
#include <soc/soc.h>

static struct sys_multi_heap s_multi;
#if defined(CONFIG_SOC_SERIES_ESP32)
static struct sys_heap s_extra[4];
static struct sys_heap *const heap_order[] = { &s_extra[0], &s_extra[1], &s_extra[2], &s_extra[3], &s_heap };
static unsigned char __noinit __aligned(HEAP_ALIGN) bank1_arena[17 * 1024];
static unsigned char __attribute__((section(".fc_heap.rtc_fast"))) __aligned(HEAP_ALIGN) rtc_arena[8 * 1024];
extern char procpu_dram1_org[];
#else
static struct sys_heap s_extra[2];
/* C5: use spare HP SRAM, then the main HP arena. Keep RTC RAM as a fallback
 * so frequently accessed WAMR bytecode and instance data stay in fast SRAM. */
static struct sys_heap *const heap_order[] = { &s_extra[1], &s_heap, &s_extra[0] };
static unsigned char __attribute__((section(".rtc_noinit"))) __aligned(HEAP_ALIGN) rtc_arena[15 * 1024];
extern char user_sram_end[];
#endif

static void *choose_heap(struct sys_multi_heap *multi, void *cfg, size_t alignment, size_t size)
{
    ARG_UNUSED(multi);
    ARG_UNUSED(cfg);
    for (size_t i = 0; i < ARRAY_SIZE(heap_order); i++) {
        void *p = sys_heap_aligned_alloc(heap_order[i], alignment, size);
        if (p != NULL) {
            return p;
        }
    }
    return NULL;
}

static void add_bank(size_t index, uintptr_t start, uintptr_t end)
{
    sys_heap_init(&s_extra[index], (void *)start, end - start);
    sys_multi_heap_add_heap(&s_multi, &s_extra[index], NULL);
}

static void init_extra_heaps(void)
{
    sys_multi_heap_init(&s_multi, choose_heap);
    sys_multi_heap_add_heap(&s_multi, &s_heap, NULL);
    add_bank(0, (uintptr_t)rtc_arena, (uintptr_t)rtc_arena + sizeof(rtc_arena));
#if defined(CONFIG_SOC_SERIES_ESP32)
    /* Espressif ROM reserves PRO data below 0x3ffe0440, APP data and
     * Bluetooth shared buffers from 0x3ffe3f20 to 0x3ffe5230. Zephyr's
     * linked bank 1 starts after MCUboot's last loader segment. */
    add_bank(1, 0x3ffe0440, SOC_ROM_STACK_START);
    add_bank(2, 0x3ffe5230, (uintptr_t)procpu_dram1_org);
    add_bank(3, (uintptr_t)bank1_arena, (uintptr_t)bank1_arena + sizeof(bank1_arena));
#else
    /* Linker limit of the application up to the original CPU stack top.
     * ROM static data above the stack stays reserved. */
    add_bank(1, (uintptr_t)user_sram_end, SOC_ROM_STACK_START);
#endif
}
#endif

static struct sys_memory_stats heap_stats(void)
{
    struct sys_memory_stats st;
    sys_heap_runtime_stats_get(&s_heap, &st);
#ifdef CONFIG_FC_ESP_INTERNAL_HEAPS
    for (size_t i = 0; i < ARRAY_SIZE(s_extra); i++) {
        struct sys_memory_stats bank;
        sys_heap_runtime_stats_get(&s_extra[i], &bank);
        st.free_bytes += bank.free_bytes;
        st.allocated_bytes += bank.allocated_bytes;
    }
#endif
    return st;
}

/* Under s_lock. Splitting or joining chunks changes the space spent on
 * headers, so today's free + allocated can be below an earlier allocated
 * peak. Track the actual free payload after allocator calls instead. */
static void record_free(void)
{
    struct sys_memory_stats st = heap_stats();
    if (st.free_bytes < s_boot_low) {
        s_boot_low = st.free_bytes;
    }
    if (st.free_bytes < s_watch_low) {
        s_watch_low = st.free_bytes;
    }
}

static int heap_init(void)
{
    sys_heap_init(&s_heap, s_arena, sizeof(s_arena));
#ifdef CONFIG_FC_ESP_INTERNAL_HEAPS
    init_extra_heaps();
#endif
    record_free();
    return 0;
}
SYS_INIT(heap_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_LIBC);

void *aligned_alloc(size_t alignment, size_t size)
{
    sys_mutex_lock(&s_lock, K_FOREVER);
#ifdef CONFIG_FC_ESP_INTERNAL_HEAPS
    void *p = choose_heap(&s_multi, NULL, alignment, size);
#else
    void *p = sys_heap_aligned_alloc(&s_heap, alignment, size);
#endif
    record_free();
    sys_mutex_unlock(&s_lock);
    if (p == NULL && size != 0) {
        errno = ENOMEM;
    }
    return p;
}

void *malloc(size_t size)
{
    return aligned_alloc(HEAP_ALIGN, size);
}

/* The C library's own calloc does not clear what this malloc returns. */
void *calloc(size_t n, size_t size)
{
    size_t bytes;
    if (size_mul_overflow(n, size, &bytes)) {
        errno = ENOMEM;
        return NULL;
    }
    void *p = malloc(bytes);
    if (p != NULL) {
        memset(p, 0, bytes);
    }
    return p;
}

void *realloc(void *ptr, size_t size)
{
    sys_mutex_lock(&s_lock, K_FOREVER);
#ifdef CONFIG_FC_ESP_INTERNAL_HEAPS
    void *p = sys_multi_heap_aligned_realloc(&s_multi, NULL, ptr, HEAP_ALIGN, size);
#else
    void *p = sys_heap_aligned_realloc(&s_heap, ptr, HEAP_ALIGN, size);
#endif
    record_free();
    sys_mutex_unlock(&s_lock);
    if (p == NULL && size != 0) {
        errno = ENOMEM;
    }
    return p;
}

void free(void *ptr)
{
    sys_mutex_lock(&s_lock, K_FOREVER);
#ifdef CONFIG_FC_ESP_INTERNAL_HEAPS
    sys_multi_heap_free(&s_multi, ptr);
#else
    sys_heap_free(&s_heap, ptr);
#endif
    sys_mutex_unlock(&s_lock);
}

void fc_heap_stats(size_t *total, size_t *free_now, size_t *free_min)
{
    sys_mutex_lock(&s_lock, K_FOREVER);
    struct sys_memory_stats st = heap_stats();
    *free_min = s_boot_low;
    sys_mutex_unlock(&s_lock);
    *total = st.free_bytes + st.allocated_bytes;
    *free_now = st.free_bytes;
}

long fc_port_heap_largest(void)
{
    size_t bytes;
    sys_mutex_lock(&s_lock, K_FOREVER);
    int rc = sys_heap_get_largest_free_block(&s_heap, &bytes);
#ifdef CONFIG_FC_ESP_INTERNAL_HEAPS
    for (size_t i = 0; i < ARRAY_SIZE(s_extra); i++) {
        size_t bank;
        if (sys_heap_get_largest_free_block(&s_extra[i], &bank) == 0 && (rc != 0 || bank > bytes)) {
            bytes = bank;
            rc = 0;
        }
    }
#endif
    sys_mutex_unlock(&s_lock);
    return rc == 0 ? (long)bytes : -1;
}

long fc_heap_watch(int start)
{
    sys_mutex_lock(&s_lock, K_FOREVER);
    if (start) {
        struct sys_memory_stats st = heap_stats();
        s_watch_low = st.free_bytes;
    }
    long low = (long)s_watch_low;
    sys_mutex_unlock(&s_lock);
    return start ? 0 : low;
}

#if defined(FC_TLS_POOL_PORT_STORAGE) && defined(CONFIG_SOC_SERIES_ESP32)
#include "fc_tls_pool.h"
#include <zephyr/sys/atomic.h>

/* Zephyr places .noinit in SRAM bank 1 on the classic DevKitC. Keep the TLS
 * receive records there so a WASM page can still fit in the main C heap. */
#ifdef CONFIG_FC_ESP_INTERNAL_HEAPS
#define STATIC_TLS_RECORDS FC_TLS_POOL_SLOTS
#else
#define STATIC_TLS_RECORDS FC_TLS_POOL_MAX
#endif
static unsigned char __noinit __aligned(HEAP_ALIGN) tls_records[STATIC_TLS_RECORDS][FC_TLS_POOL_SLOT_SIZE];
static ATOMIC_DEFINE(tls_allocated, STATIC_TLS_RECORDS);

void *fc_tls_pool_storage_alloc(void)
{
    for (int i = 0; i < STATIC_TLS_RECORDS; i++) {
        if (!atomic_test_and_set_bit(tls_allocated, i)) {
            return memset(tls_records[i], 0, FC_TLS_POOL_SLOT_SIZE);
        }
    }
    return calloc(1, FC_TLS_POOL_SLOT_SIZE);
}

void fc_tls_pool_storage_free(void *ptr)
{
    for (int i = 0; i < STATIC_TLS_RECORDS; i++) {
        if (ptr == tls_records[i]) {
            atomic_clear_bit(tls_allocated, i);
            return;
        }
    }
    free(ptr);
}
#endif

#if defined(CONFIG_SHARED_MULTI_HEAP)
#include <zephyr/multi_heap/shared_multi_heap.h>

/* WAMR calls this allocator for app linear memory through os_mmap.
 * Keep eight entries for the apps even when TLS uses the same PSRAM heap. */
#define WASM_MEM_PSRAM_MIN (16 * 1024)
#ifdef FC_TLS_POOL_PORT_STORAGE
#include "fc_tls_pool.h"
#define PSRAM_BLOCKS_MAX (8 + FC_TLS_POOL_MAX)
#else
#define PSRAM_BLOCKS_MAX 8
#endif

extern void set_exec_mem_alloc_func(void *(*alloc_func)(unsigned int size), void (*free_func)(void *addr));

static void *s_psram_blocks[PSRAM_BLOCKS_MAX];
static SYS_MUTEX_DEFINE(s_psram_lock);

static void *prefer_psram_alloc(unsigned int size)
{
    void *p = NULL;
    if (size >= WASM_MEM_PSRAM_MIN) {
        sys_mutex_lock(&s_psram_lock, K_FOREVER);
        for (int i = 0; i < PSRAM_BLOCKS_MAX; i++) {
            if (s_psram_blocks[i] == NULL) {
                p = shared_multi_heap_aligned_alloc(SMH_REG_ATTR_EXTERNAL, 8, size);
                s_psram_blocks[i] = p;
                break;
            }
        }
        sys_mutex_unlock(&s_psram_lock);
    }
    return p != NULL ? p : malloc(size);
}

static void prefer_psram_free(void *addr)
{
    if (addr == NULL) {
        return;
    }
    sys_mutex_lock(&s_psram_lock, K_FOREVER);
    for (int i = 0; i < PSRAM_BLOCKS_MAX; i++) {
        if (s_psram_blocks[i] == addr) {
            s_psram_blocks[i] = NULL;
            sys_mutex_unlock(&s_psram_lock);
            shared_multi_heap_free(addr);
            return;
        }
    }
    sys_mutex_unlock(&s_psram_lock);
    free(addr);
}

#if defined(FC_TLS_POOL_PORT_STORAGE) && !defined(CONFIG_SOC_SERIES_ESP32)
void *fc_tls_pool_storage_alloc(void)
{
    void *ptr = prefer_psram_alloc(FC_TLS_POOL_SLOT_SIZE);
    if (ptr != NULL) {
        memset(ptr, 0, FC_TLS_POOL_SLOT_SIZE);
    }
    return ptr;
}

void fc_tls_pool_storage_free(void *ptr)
{
    prefer_psram_free(ptr);
}
#endif

static int wasm_mem_init(void)
{
    set_exec_mem_alloc_func(prefer_psram_alloc, prefer_psram_free);
    return 0;
}
SYS_INIT(wasm_mem_init, APPLICATION, 0);
#endif
