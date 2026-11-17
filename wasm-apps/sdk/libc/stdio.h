/* Formatting (fc_libc.h). printf, puts and putchar write to the app's log at
 * INFO, a line at a time, as fc_log does; a line longer than 96 characters is
 * split, and one printf writes at most 127. No vsnprintf, no files. */
#pragma once
#include "fc_libc.h"
FC_LIBC(printf) int printf(const char *fmt, ...);
FC_LIBC(sprintf) int sprintf(char *str, const char *fmt, ...);
FC_LIBC(snprintf) int snprintf(char *str, size_t size, const char *fmt, ...);
FC_LIBC(puts) int puts(const char *s);
FC_LIBC(putchar) int putchar(int c);
