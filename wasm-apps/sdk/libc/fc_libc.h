/*
 * The C library functions the device has for apps (docs/APPS.md), imported
 * from the module "env". Included by this directory's string.h, stdio.h,
 * stdlib.h, ctype.h and math.h: build with -I wasm-apps/sdk/libc. Only
 * these, and no files. malloc is the app's own (stdlib.h).
 */
#pragma once
#include <stddef.h>
#define FC_LIBC(name) __attribute__((import_module("env"), import_name(#name)))
