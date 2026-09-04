#include "elf_loader.hpp"

#include <cstring>

namespace {

// Just enough of the ELF64 format to load an image. Field offsets are from the
// ELF specification; the file is little-endian for RISC-V.
constexpr u64 EI_NIDENT = 16;
constexpr u32 PT_LOAD   = 1;
constexpr u16 EM_RISCV  = 243;
constexpr u8  ELFCLASS64 = 2;
constexpr u8  ELFDATA2LSB = 1;

u16 rd16(const std::vector<u8>& b, u64 off) {
    return static_cast<u16>(b[off]) | (static_cast<u16>(b[off + 1]) << 8);
}
u32 rd32(const std::vector<u8>& b, u64 off) {
    u32 v = 0;
    for (int i = 0; i < 4; ++i) v |= static_cast<u32>(b[off + i]) << (8 * i);
    return v;
}
u64 rd64(const std::vector<u8>& b, u64 off) {
    u64 v = 0;
    for (int i = 0; i < 8; ++i) v |= static_cast<u64>(b[off + i]) << (8 * i);
    return v;
}

LoadedImage fail(const std::string& msg) {
    LoadedImage r;
    r.ok = false;
    r.error = msg;
    return r;
}

}  // namespace

bool is_elf(const std::vector<u8>& b) {
    return b.size() >= 4 && b[0] == 0x7f && b[1] == 'E' && b[2] == 'L' && b[3] == 'F';
}

LoadedImage load_elf(const std::vector<u8>& b, Bus& bus) {
    if (!is_elf(b)) return fail("not an ELF file");
    if (b.size() < 64) return fail("truncated ELF header");

    if (b[4] != ELFCLASS64)   return fail("not ELF64 (32-bit images unsupported)");
    if (b[5] != ELFDATA2LSB)  return fail("not little-endian");

    const u16 e_machine = rd16(b, 18);
    if (e_machine != EM_RISCV) {
        return fail("not a RISC-V image (e_machine=" + std::to_string(e_machine) + ")");
    }

    const u64 e_entry  = rd64(b, 24);
    const u64 e_phoff  = rd64(b, 32);
    const u16 e_phentsize = rd16(b, 54);
    const u16 e_phnum  = rd16(b, 56);

    if (e_phnum == 0) return fail("no program headers");

    unsigned loaded = 0;
    for (u16 i = 0; i < e_phnum; ++i) {
        const u64 ph = e_phoff + static_cast<u64>(i) * e_phentsize;
        if (ph + 56 > b.size()) return fail("program header out of range");

        if (rd32(b, ph + 0) != PT_LOAD) continue;

        const u64 p_offset = rd64(b, ph + 8);
        const u64 p_paddr  = rd64(b, ph + 24);
        const u64 p_filesz = rd64(b, ph + 32);
        const u64 p_memsz  = rd64(b, ph + 40);

        if (p_offset + p_filesz > b.size()) return fail("segment extends past end of file");
        if (p_memsz < p_filesz) return fail("segment memsz < filesz");

        // Copy the bytes present in the file...
        for (u64 j = 0; j < p_filesz; ++j) {
            Status st = bus.store(p_paddr + j, 1, b[p_offset + j]);
            if (!st) {
                return fail("segment does not fit in mapped memory at 0x" +
                            std::to_string(p_paddr));
            }
        }
        // ...then zero the rest. This is .bss: the program expects it to exist
        // and to be zero, but it takes no space in the file.
        for (u64 j = p_filesz; j < p_memsz; ++j) {
            Status st = bus.store(p_paddr + j, 1, 0);
            if (!st) return fail("bss does not fit in mapped memory");
        }
        ++loaded;
    }

    if (loaded == 0) return fail("no PT_LOAD segments");

    LoadedImage r;
    r.ok = true;
    r.entry = e_entry;
    return r;
}
