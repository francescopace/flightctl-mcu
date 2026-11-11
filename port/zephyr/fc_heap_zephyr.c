/*
 * The C heap (malloc and friends), on a heap this port owns, so that it can
 * say how much of it is free and how low it has been: Zephyr 4.3's own
 * malloc keeps that to itself. Built instead of it (CONFIG_COMMON_LIBC_MALLOC=n),
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

void fc_heap_stats(size_t *total, size_t *free_now, size_t *free_min)
{
    struct sys_memory_stats st = {0};
    sys_mutex_lock(&s_lock, K_FOREVER);
    sys_heap_runtime_stats_get(&s_heap, &st);
    sys_mutex_unlock(&s_lock);
    *total = st.free_bytes + st.allocated_bytes;
    *free_now = st.free_bytes;
    *free_min = *total - st.max_allocated_bytes;
}
