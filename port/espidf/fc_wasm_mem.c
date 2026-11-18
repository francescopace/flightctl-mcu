/*
 * WASM apps' linear memories, each in a block of exactly its size.
 *
 * WAMR's os_mmap asks the heap for the memory plus 8 bytes (4 of alignment,
 * the block's start, which its os_munmap reads back). ESP-IDF's heap (TLSF)
 * looks for a request in the size class above it, so that any block there
 * fits: in a heap region of 256 KB or less the classes are 4 KB apart
 * between 64 and 128 KB, and 65,536 + 8 bytes needed a free block of 69,632.
 * On an ESP32-C5 at regime CoreMark's 64 KB failed with "largest free block
 * 64 KB" (v3.14-rc10), a block that holds 65,536 bytes; on the classic ESP32
 * those 4 KB were missing from the agent's heap.
 *
 * A memory of 16 KB or more is therefore allocated here on its own, rounded
 * up to a class boundary (64 KB multiples already are), with nothing in
 * front: the search then takes any block of the class it starts, and the
 * block an instance gives back fits the next one of the same size. Its start
 * is 4-byte aligned, not 8: WAMR's loads and stores on Xtensa and RISC-V
 * (WASM_CPU_SUPPORTS_UNALIGNED_ADDR_ACCESS 0) take any alignment, 64-bit ones
 * in two 32-bit halves. os_munmap is wrapped too, for these blocks; anything
 * else goes to WAMR's own. os_mremap (memory.grow) as well: WAMR's calls the
 * two from inside its own file, where a wrap does not reach. A request that
 * fails is kept, for the app to wait for a block of that size
 * (fc_wasm_mem_failed). Linked with --wrap=os_mmap, os_munmap and os_mremap
 * (components/fc_core/CMakeLists.txt): WAMR is not patched.
 */
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"

void *__real_os_mmap(void *hint, size_t size, int prot, int flags, int file);
void __real_os_munmap(void *addr, size_t size);

#define ROUND_FROM (16 * 1024)
#define EXACT_MAX  8 /* memories at a time: a few apps, each with one, and one being replaced */
#define PROT_EXEC  4 /* MMAP_PROT_EXEC: AOT code, not used here */

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static void *s_exact[EXACT_MAX];
static size_t s_failed;

/* TLSF's second level: 16 classes per power of two (a 4 KB step from 64 KB). */
static size_t class_round(size_t size)
{
    int fls = 31 - __builtin_clz((unsigned)size);
    size_t step = (size_t)1 << (fls - 4);
    return (size + step - 1) & ~(step - 1);
}

static int exact_slot(void *p, void *put)
{
    int found = -1;
    taskENTER_CRITICAL(&s_lock);
    for (int i = 0; i < EXACT_MAX && found < 0; i++) {
        if (s_exact[i] == p) {
            s_exact[i] = put;
            found = i;
        }
    }
    taskEXIT_CRITICAL(&s_lock);
    return found;
}

void *__wrap_os_mmap(void *hint, size_t size, int prot, int flags, int file)
{
    if (size < ROUND_FROM || (prot & PROT_EXEC)) {
        return __real_os_mmap(hint, size, prot, flags, file);
    }
    size_t block = class_round(size);
    /* As WAMR's: internal RAM, or PSRAM where the chip has some and no
     * internal block is large enough (MALLOC_CAP_8BIT). */
    void *p = heap_caps_malloc(block, MALLOC_CAP_8BIT);
    if (p == NULL) {
        __atomic_store_n(&s_failed, block, __ATOMIC_RELAXED);
        return NULL;
    }
    if (exact_slot(NULL, p) < 0) {
        heap_caps_free(p); /* no slot: WAMR's way, 8 bytes more */
        p = __real_os_mmap(hint, block, prot, flags, file);
        if (p == NULL) {
            __atomic_store_n(&s_failed, block + 8, __ATOMIC_RELAXED);
        }
        return p;
    }
    memset(p, 0, size);
    return p;
}

void __wrap_os_munmap(void *addr, size_t size)
{
    if (addr != NULL && exact_slot(addr, NULL) >= 0) {
        heap_caps_free(addr);
        return;
    }
    __real_os_munmap(addr, size);
}

/* WAMR's os_mremap_slow, with the wrapped pair. */
void *__wrap_os_mremap(void *old_addr, size_t old_size, size_t new_size)
{
    void *p = __wrap_os_mmap(NULL, new_size, 1 | 2 /* MMAP_PROT_READ | MMAP_PROT_WRITE */, 0, -1);
    if (p == NULL) {
        return NULL;
    }
    memcpy(p, old_addr, new_size < old_size ? new_size : old_size);
    __wrap_os_munmap(old_addr, old_size);
    return p;
}

long fc_wasm_mem_failed(void)
{
    return (long)__atomic_exchange_n(&s_failed, 0, __ATOMIC_RELAXED);
}
