#pragma once
#include <stdint.h>

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);

/* Find the /memory bank (as patched by ABL) that contains addr. Returns 0 if none. */
int fdt_memory_bank(uintptr_t fdt, uint64_t addr, uint64_t *base, uint64_t *size);
