/* Built with the interpreter so the module layout matches its configuration. */
#include "../../third_party/wamr/core/iwasm/interpreter/wasm.h"
#include "../../core/src/fc_wasm_placement.h"

size_t fc_wasm_initial_memory(wasm_module_t module)
{
    const WASMModule *m = (const WASMModule *)module;
    if (m->memory_count != 1 || m->import_memory_count || (m->memories[0].flags & SHARED_MEMORY_FLAG)) {
        return 0;
    }
    const WASMMemory *mem = m->memories;
    uint64_t bytes = (uint64_t)mem->num_bytes_per_page * mem->init_page_count;
    return bytes <= SIZE_MAX ? (size_t)bytes : 0;
}
