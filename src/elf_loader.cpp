#include "elf_loader.hpp"

#include <cstring>

namespace {

// Does [offset, offset + length) lie inside the file?
//
// Written as a subtraction because the obvious `offset + length > size` can
// overflow: a crafted header with p_offset near 2^64 and a small p_filesz makes
// the sum wrap to something small, the check passes, and the copy loop
// dereferences wildly out of bounds. An ELF file is untrusted input - it is
// the one thing here that arrives from outside - so every bound is checked this
// way.
bool fits(const std::vector<u8>& b, u64 offset, u64 length) {
    return length <= b.size() && offset <= b.size() - length;
}

// Just enough of the ELF64 format to load an image. Field offsets are from the
// ELF specification; the file is little-endian for RISC-V.
constexpr u64 EI_NIDENT = 16;
constexpr u32 PT_LOAD   = 1;
constexpr u16 EM_RISCV  = 243;
constexpr u8  ELFCLASS64 = 2;
constexpr u8  ELFDATA2LSB = 1;
constexpr u32 SHT_SYMTAB = 2;
constexpr u32 SHT_STRTAB = 3;

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

// Look up a symbol's address in the ELF symbol table.
//
// Walks the section headers for a SYMTAB, reads its linked string table, and
// compares names. Returns 0 when the symbol (or the symbol table itself) is
// absent, which is the normal case for a stripped or hand-assembled image.
u64 find_symbol(const std::vector<u8>& b, u64 shoff, u16 shentsize, u16 shnum,
                const char* want) {
    if (shoff == 0 || shnum == 0) return 0;

    for (u16 i = 0; i < shnum; ++i) {
        const u64 sh = shoff + static_cast<u64>(i) * shentsize;
        if (!fits(b, sh, 64)) return 0;
        if (rd32(b, sh + 4) != SHT_SYMTAB) continue;

        const u64 sym_off  = rd64(b, sh + 24);
        const u64 sym_size = rd64(b, sh + 32);
        const u32 strtab_i = rd32(b, sh + 40);   // sh_link: the string table
        const u64 sym_entsize = rd64(b, sh + 56);
        if (sym_entsize == 0) continue;

        // The linked string table holds the symbol names.
        const u64 str_sh = shoff + static_cast<u64>(strtab_i) * shentsize;
        if (!fits(b, str_sh, 64)) continue;
        if (rd32(b, str_sh + 4) != SHT_STRTAB) continue;
        const u64 str_off  = rd64(b, str_sh + 24);
        const u64 str_size = rd64(b, str_sh + 32);

        for (u64 s = 0; s + sym_entsize <= sym_size; s += sym_entsize) {
            const u64 ent = sym_off + s;
            if (!fits(b, ent, 24)) break;

            const u32 name_off = rd32(b, ent + 0);
            if (name_off == 0 || name_off >= str_size) continue;
            if (!fits(b, str_off, name_off + 1)) continue;

            const char* name = reinterpret_cast<const char*>(b.data() + str_off + name_off);
            const u64 max_len = b.size() - (str_off + name_off);
            if (std::strncmp(name, want, max_len) == 0) {
                return rd64(b, ent + 8);   // st_value
            }
        }
    }
    return 0;
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
    const u64 e_shoff  = rd64(b, 40);
    const u16 e_phentsize = rd16(b, 54);
    const u16 e_phnum  = rd16(b, 56);
    const u16 e_shentsize = rd16(b, 58);
    const u16 e_shnum  = rd16(b, 60);

    if (e_phnum == 0) return fail("no program headers");

    unsigned loaded = 0;
    for (u16 i = 0; i < e_phnum; ++i) {
        const u64 ph = e_phoff + static_cast<u64>(i) * e_phentsize;
        if (!fits(b, ph, 56)) return fail("program header out of range");

        if (rd32(b, ph + 0) != PT_LOAD) continue;

        const u64 p_offset = rd64(b, ph + 8);
        const u64 p_paddr  = rd64(b, ph + 24);
        const u64 p_filesz = rd64(b, ph + 32);
        const u64 p_memsz  = rd64(b, ph + 40);

        if (!fits(b, p_offset, p_filesz)) return fail("segment extends past end of file");
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
    r.tohost = find_symbol(b, e_shoff, e_shentsize, e_shnum, "tohost");
    return r;
}
