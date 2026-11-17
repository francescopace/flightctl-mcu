/*
 * The C heap (malloc and friends), on a heap this port owns, so that it can
 * say how much of it is free and how low it has been: Zephyr's own malloc
 * does not give that to applications (in 4.5, only through a header private
 * to its libc). Built instead of it (CONFIG_COMMON_LIBC_MALLOC=n),
 * and written like it: a sys_heap behind a mutex.
 */
#include <errno.h>
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

static int heap_init(void)
{
    sys_heap_init(&s_heap, s_arena, sizeof(s_arena));
    return 0;
}
SYS_INIT(heap_init, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_LIBC);

void *aligned_alloc(size_t alignment, size_t size)
{
    sys_mutex_lock(&s_lock, K_FOREVER);
    void *p = sys_heap_aligned_alloc(&s_heap, alignment, size);
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

/* The most allocated at once before sys_heap's own count was last started
 * again (fc_heap_watch): the low since boot is the lower of the two. */
static size_t s_max_before;

void fc_heap_stats(size_t *total, size_t *free_now, size_t *free_min)
{
    struct sys_memory_stats st = {0};
    sys_mutex_lock(&s_lock, K_FOREVER);
    sys_heap_runtime_stats_get(&s_heap, &st);
    size_t max = st.max_allocated_bytes > s_max_before ? st.max_allocated_bytes : s_max_before;
    sys_mutex_unlock(&s_lock);
    *total = st.free_bytes + st.allocated_bytes;
    *free_now = st.free_bytes;
    *free_min = *total - max;
}

long fc_heap_watch(int start)
{
    struct sys_memory_stats st = {0};
    sys_mutex_lock(&s_lock, K_FOREVER);
    sys_heap_runtime_stats_get(&s_heap, &st);
    if (start) {
        if (st.max_allocated_bytes > s_max_before) {
            s_max_before = st.max_allocated_bytes;
        }
        sys_heap_runtime_stats_reset_max(&s_heap);
    }
    sys_mutex_unlock(&s_lock);
    return start ? 0 : (long)(st.free_bytes + st.allocated_bytes - st.max_allocated_bytes);
}

#if defined(CONFIG_SHARED_MULTI_HEAP)
#include <zephyr/multi_heap/shared_multi_heap.h>

/* WASM apps' linear memories in PSRAM, where the board has some (the
 * ESP32-S3 N8R8): WAMR asks its Zephyr platform for them through os_mmap,
 * which calls the allocator given with set_exec_mem_alloc_func. Blocks of
 * 16 KB or more come from the PSRAM heap, smaller ones and any the PSRAM
 * cannot give from the C heap, which the agent's TLS buffers need. */
#define WASM_MEM_PSRAM_MIN (16 * 1024)
#define WASM_MEM_PSRAM_MAX 8 /* blocks at a time: two apps, a memory each */

extern void set_exec_mem_alloc_func(void *(*alloc_func)(unsigned int size), void (*free_func)(void *addr));

static void *s_psram_blocks[WASM_MEM_PSRAM_MAX];
static SYS_MUTEX_DEFINE(s_psram_lock);

static void *wasm_mem_alloc(unsigned int size)
{
    void *p = NULL;
    if (size >= WASM_MEM_PSRAM_MIN) {
        sys_mutex_lock(&s_psram_lock, K_FOREVER);
        for (int i = 0; i < WASM_MEM_PSRAM_MAX; i++) {
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

static void wasm_mem_free(void *addr)
{
    sys_mutex_lock(&s_psram_lock, K_FOREVER);
    for (int i = 0; i < WASM_MEM_PSRAM_MAX; i++) {
        if (addr != NULL && s_psram_blocks[i] == addr) {
            s_psram_blocks[i] = NULL;
            sys_mutex_unlock(&s_psram_lock);
            shared_multi_heap_free(addr);
            return;
        }
    }
    sys_mutex_unlock(&s_psram_lock);
    free(addr);
}

static int wasm_mem_init(void)
{
    set_exec_mem_alloc_func(wasm_mem_alloc, wasm_mem_free);
    return 0;
}
SYS_INIT(wasm_mem_init, APPLICATION, 0);
#endif
