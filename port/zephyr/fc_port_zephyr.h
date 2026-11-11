/* Zephyr port: what the application calls before it starts the agent. */
#pragma once

#include <stddef.h>

/* Mounts the storage (LittleFS on the fc_storage partition). 0 or -1. */
int fc_port_zephyr_init(void);

/* The C heap (fc_heap_zephyr.c): its size, what is free, and the least that
 * has been free since boot. */
void fc_heap_stats(size_t *total, size_t *free_now, size_t *free_min);
