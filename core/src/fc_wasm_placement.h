#pragma once

#include "wasm_export.h"

/* SRAM-only ESP32-C5/C6. The loader reports the declared size before it
 * decodes the functions. Once loaded, WAMR may have shrunk the memory. */
size_t fc_wasm_initial_memory(wasm_module_t module);
size_t fc_wasm_block_size(size_t bytes);

typedef struct {
    void *data;
    size_t size;
} fc_wasm_reserve_t;

fc_wasm_reserve_t fc_wasm_reserve_memory(wasm_module_t module);
void fc_wasm_release_memory(fc_wasm_reserve_t *reserve);
wasm_module_inst_t fc_wasm_instantiate_reserved(fc_wasm_reserve_t *reserve, wasm_module_t module,
                                                uint32_t stack, char *error, uint32_t error_size);
