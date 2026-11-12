/* Forced into WAMR 2.4.5's sources on ESP-IDF 6 (firmware/CMakeLists.txt):
 * what its ESP-IDF port takes from newlib's headers without including them,
 * and picolibc does not give. Only its file access for WASI needs them,
 * which this firmware does not build in. */
#ifndef __ASSEMBLER__
#include <sys/stat.h>
int renameat(int, const char *, int, const char *);
#endif
