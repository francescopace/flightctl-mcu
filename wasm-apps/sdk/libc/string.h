/* The device's string functions (fc_libc.h). Native, so faster than C built
 * into the app; a pointer outside the app's memory stops the app. */
#pragma once
#include "fc_libc.h"
FC_LIBC(memcmp) int memcmp(const void *a, const void *b, size_t n);
FC_LIBC(memcpy) void *memcpy(void *dst, const void *src, size_t n);
FC_LIBC(memmove) void *memmove(void *dst, const void *src, size_t n);
FC_LIBC(memset) void *memset(void *s, int c, size_t n);
FC_LIBC(memchr) void *memchr(const void *s, int c, size_t n);
FC_LIBC(strchr) char *strchr(const char *s, int c);
FC_LIBC(strcmp) int strcmp(const char *a, const char *b);
FC_LIBC(strcpy) char *strcpy(char *dst, const char *src);
FC_LIBC(strlen) size_t strlen(const char *s);
FC_LIBC(strncmp) int strncmp(const char *a, const char *b, size_t n);
FC_LIBC(strncpy) char *strncpy(char *dst, const char *src, size_t n);
FC_LIBC(strncasecmp) int strncasecmp(const char *a, const char *b, size_t n);
FC_LIBC(strspn) size_t strspn(const char *s, const char *accept);
FC_LIBC(strcspn) size_t strcspn(const char *s, const char *reject);
FC_LIBC(strstr) char *strstr(const char *haystack, const char *needle);
