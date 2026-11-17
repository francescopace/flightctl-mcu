/*
 * WASM apps' linear memories, sized so that the block an instance gives back
 * fits the next one.
 *
 * WAMR's os_mmap asks the heap for the memory plus 8 bytes (4 of alignment,
 * the block's start). ESP-IDF's heap (TLSF) rounds a request up to its size
 * class, in a heap region of 256 KB or less to a multiple of 4 KB between 64
 * and 128 KB: 65,536 + 8 bytes takes a block of 69,632. The block the
 * instance gives back when it ends is the size it asked for, 65,544, under
 * the next class: the next instance, which asks the same, did not fit in it.
 * On an ESP32-C5, CoreMark's 64 KB failed at a restart with "largest free
 * block 64 KB". The request is therefore rounded up here to what the heap
 * would take anyway: the first instance needs no more, and what it gives back
 * fits the next. A request that fails is kept, for the app to wait for a
 * block of that size (fc_wasm_mem_failed). Linked with -Wl,--wrap=os_mmap
 * (components/fc_core/CMakeLists.txt): WAMR is not patched.
 */
#include <stddef.h>
#include <stdint.h>

void *__real_os_mmap(void *hint, size_t size, int prot, int flags, int file);

#define WAMR_MMAP_EXTRA (4 + sizeof(uintptr_t)) /* what WAMR's os_mmap adds */
#define ROUND_FROM (16 * 1024)

/* The block the last memory that did not fit asked for (fc_wasm_mem_failed). */
static size_t s_failed;

void *__wrap_os_mmap(void *hint, size_t size, int prot, int flags, int file)
{
    if (size >= ROUND_FROM && !(prot & 4 /* MMAP_PROT_EXEC: AOT code, not used here */)) {
        size_t total = size + WAMR_MMAP_EXTRA;
        /* TLSF's second level in regions of 256 KB or less: 16 classes per
         * power of two, a 4 KB step from 64 KB (and a multiple of the 2 KB
         * step of larger regions). */
        int fls = 31 - __builtin_clz((unsigned)total);
        size_t step = (size_t)1 << (fls - 4);
        size = ((total + step - 1) & ~(step - 1)) - WAMR_MMAP_EXTRA;
    }
    void *p = __real_os_mmap(hint, size, prot, flags, file);
    if (p == NULL) {
        __atomic_store_n(&s_failed, size + WAMR_MMAP_EXTRA, __ATOMIC_RELAXED);
    }
    return p;
}

/* For an app whose memory did not fit (fc_apps.c): the block it needs, which
 * it waits for before it is started again. 0: none failed since the last
 * call. */
long fc_wasm_mem_failed(void)
{
    return (long)__atomic_exchange_n(&s_failed, 0, __ATOMIC_RELAXED);
}
