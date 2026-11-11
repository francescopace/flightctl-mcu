#pragma once

#include "fc_port.h"

/* State of the in-memory port (port_stub.c). */
extern int stub_reboots;
extern int stub_verbose; /* 1: print the agent's log lines */
extern int stub_hwkey_bits;       /* > 0: a "hardware" key store, for RSA keys of this size or FC_HWKEY_ECDSA_P256 */
extern int stub_hwkey_seal_fails; /* 1: sealing fails */
extern int stub_hwkey_seals;      /* keys sealed since stub_reset */
void stub_reset(void);
const char *stub_kv(const char *key);     /* NULL if missing */
const char *stub_file(const char *path);  /* NULL if never written */
