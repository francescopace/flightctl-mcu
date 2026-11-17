/* Character classes, in the C locale (fc_libc.h). */
#pragma once
#include "fc_libc.h"
FC_LIBC(isalnum) int isalnum(int c);
FC_LIBC(isalpha) int isalpha(int c);
FC_LIBC(isdigit) int isdigit(int c);
FC_LIBC(isgraph) int isgraph(int c);
FC_LIBC(isprint) int isprint(int c);
FC_LIBC(isspace) int isspace(int c);
FC_LIBC(isupper) int isupper(int c);
FC_LIBC(isxdigit) int isxdigit(int c);
FC_LIBC(tolower) int tolower(int c);
FC_LIBC(toupper) int toupper(int c);
