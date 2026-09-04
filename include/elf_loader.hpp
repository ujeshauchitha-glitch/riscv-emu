#pragma once

#include <string>
#include <vector>

#include "bus.hpp"
#include "types.hpp"

// ---------------------------------------------------------------------------
// Minimal ELF64 loader.
//
// A kernel is not a flat blob: it is an ELF file describing where each piece of
// itself belongs in memory. Loading it means walking the program headers and
// copying each PT_LOAD segment to the physical address it asks for, then
// starting at the entry point the header names.
//
// Two details that matter for bare-metal images:
//
//   * We load at p_paddr, not p_vaddr. A kernel is linked for the virtual
//     addresses it will use *after* it turns on paging, but at boot there is no
//     MMU yet, so the physical address is the one that applies.
//
//   * p_memsz may exceed p_filesz. The difference is .bss - storage the program
//     expects to exist and to be zeroed, but which occupies no space in the
//     file. Skipping that zero-fill leaves a kernel's globals full of garbage.
// ---------------------------------------------------------------------------

struct LoadedImage {
    bool        ok = false;
    u64         entry = 0;
    std::string error;
};

// Load an ELF64 RISC-V image into the bus. `is_elf` can be checked first to
// decide between this and a flat binary.
LoadedImage load_elf(const std::vector<u8>& bytes, Bus& bus);

// True if the buffer starts with the ELF magic number.
bool is_elf(const std::vector<u8>& bytes);
