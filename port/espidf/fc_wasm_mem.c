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
 * fits the next. Linked with -Wl,--wrap=os_mmap
 * (components/fc_core/CMakeLists.txt): WAMR is not patched.
 */
#include <stddef.h>
#include <stdint.h>

void *__real_os_mmap(void *hint, size_t size, int prot, int flags, int file);

#define WAMR_MMAP_EXTRA (4 + sizeof(uintptr_t)) /* what WAMR's os_mmap adds */
#define ROUND_FROM (16 * 1024)

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
    return __real_os_mmap(hint, size, prot, flags, file);
}
