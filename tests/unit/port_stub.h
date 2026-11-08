#pragma once

#include "fc_port.h"

/* State of the in-memory port (port_stub.c). */
extern int stub_reboots;
extern int stub_verbose; /* 1: print the agent's log lines */
void stub_reset(void);
const char *stub_kv(const char *key);     /* NULL if missing */
const char *stub_file(const char *path);  /* NULL if never written */
