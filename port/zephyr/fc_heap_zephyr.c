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

/* Under s_lock. Splitting or joining chunks changes the space spent on
 * headers, so today's free + allocated can be below an earlier allocated
 * peak. Track the actual free payload after allocator calls instead. */
static void record_free(void)
{
    struct sys_memory_stats st;
    sys_heap_runtime_stats_get(&s_heap, &st);
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
    record_free();
    return 0;
}
SYS_INIT(heap_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_LIBC);

void *aligned_alloc(size_t alignment, size_t size)
{
    sys_mutex_lock(&s_lock, K_FOREVER);
    void *p = sys_heap_aligned_alloc(&s_heap, alignment, size);
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
    void *p = sys_heap_aligned_realloc(&s_heap, ptr, HEAP_ALIGN, size);
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
    sys_heap_free(&s_heap, ptr);
    sys_mutex_unlock(&s_lock);
}

void fc_heap_stats(size_t *total, size_t *free_now, size_t *free_min)
{
    struct sys_memory_stats st = {0};
    sys_mutex_lock(&s_lock, K_FOREVER);
    sys_heap_runtime_stats_get(&s_heap, &st);
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
    sys_mutex_unlock(&s_lock);
    return rc == 0 ? (long)bytes : -1;
}

long fc_heap_watch(int start)
{
    sys_mutex_lock(&s_lock, K_FOREVER);
    if (start) {
        struct sys_memory_stats st;
        sys_heap_runtime_stats_get(&s_heap, &st);
        s_watch_low = st.free_bytes;
    }
    long low = (long)s_watch_low;
    sys_mutex_unlock(&s_lock);
    return start ? 0 : low;
}

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

#ifdef FC_TLS_POOL_PORT_STORAGE
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
