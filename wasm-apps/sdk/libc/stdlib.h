/* Numbers from text, and abort (fc_libc.h). abort() stops the app as a trap
 * does. No malloc: an app has no heap. */
#pragma once
#include "fc_libc.h"
FC_LIBC(atoi) int atoi(const char *s);
FC_LIBC(strtol) long strtol(const char *s, char **end, int base);
FC_LIBC(strtoul) unsigned long strtoul(const char *s, char **end, int base);
FC_LIBC(abort) _Noreturn void abort(void);
