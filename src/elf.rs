//! A just-enough ELF64 loader.
//!
//! An ELF file is untrusted input - it is the one thing here that arrives from
//! outside - so every bound is checked, and checked in a form that cannot overflow:
//! `offset + length > size` wraps when a crafted header puts p_offset near 2^64, the
//! check passes, and the copy reads wildly out of bounds. `fits` subtracts instead.

use crate::bus::Bus;

#[derive(Debug, Default)]
pub struct LoadedImage {
    pub entry: u64,
    /// Address of the `tohost` symbol, or 0 if the image has none. riscv-tests
    /// programs report their result by writing here (the HTIF protocol).
    pub tohost: u64,
}

const PT_LOAD: u32 = 1;
const EM_RISCV: u16 = 243;
const ELFCLASS64: u8 = 2;
const ELFDATA2LSB: u8 = 1;
const SHT_SYMTAB: u32 = 2;
const SHT_STRTAB: u32 = 3;

/// Does [offset, offset + length) lie inside the file?
fn fits(b: &[u8], offset: u64, length: u64) -> bool {
    length <= b.len() as u64 && offset <= b.len() as u64 - length
}

fn rd16(b: &[u8], off: u64) -> u16 {
    u16::from_le_bytes(b[off as usize..off as usize + 2].try_into().unwrap())
}
fn rd32(b: &[u8], off: u64) -> u32 {
    u32::from_le_bytes(b[off as usize..off as usize + 4].try_into().unwrap())
}
fn rd64(b: &[u8], off: u64) -> u64 {
    u64::from_le_bytes(b[off as usize..off as usize + 8].try_into().unwrap())
}

pub fn is_elf(b: &[u8]) -> bool {
    b.len() >= 4 && b[..4] == [0x7f, b'E', b'L', b'F']
}

/// Look up a symbol's address in the ELF symbol table. Returns 0 when the symbol (or
/// the symbol table itself) is absent, the normal case for a stripped or
/// hand-assembled image.
fn find_symbol(b: &[u8], shoff: u64, shentsize: u64, shnum: u64, want: &str) -> u64 {
    if shoff == 0 || shnum == 0 {
        return 0;
    }
    for i in 0..shnum {
        let sh = shoff.wrapping_add(i.wrapping_mul(shentsize));
        if !fits(b, sh, 64) {
            return 0;
        }
        if rd32(b, sh + 4) != SHT_SYMTAB {
            continue;
        }
        let sym_off = rd64(b, sh + 24);
        let sym_size = rd64(b, sh + 32);
        let strtab_i = rd32(b, sh + 40) as u64; // sh_link: the string table
        let sym_entsize = rd64(b, sh + 56);
        if sym_entsize == 0 {
            continue;
        }
        let str_sh = shoff.wrapping_add(strtab_i.wrapping_mul(shentsize));
        if !fits(b, str_sh, 64) || rd32(b, str_sh + 4) != SHT_STRTAB {
            continue;
        }
        let str_off = rd64(b, str_sh + 24);
        let str_size = rd64(b, str_sh + 32);

        let mut s = 0u64;
        while s.checked_add(sym_entsize).map_or(false, |e| e <= sym_size) {
            let ent = sym_off.wrapping_add(s);
            s += sym_entsize;
            if !fits(b, ent, 24) {
                break;
            }
            let name_off = rd32(b, ent) as u64;
            if name_off == 0 || name_off >= str_size || !fits(b, str_off, name_off + 1) {
                continue;
            }
            let start = (str_off + name_off) as usize;
            let tail = &b[start..];
            let end = tail.iter().position(|&c| c == 0).unwrap_or(tail.len());
            if &tail[..end] == want.as_bytes() {
                return rd64(b, ent + 8); // st_value
            }
        }
    }
    0
}

/// Copy the loadable segments of an ELF64 RISC-V image into memory.
pub fn load_elf(b: &[u8], bus: &Bus) -> Result<LoadedImage, String> {
    if !is_elf(b) {
        return Err("not an ELF file".into());
    }
    if b.len() < 64 {
        return Err("truncated ELF header".into());
    }
    if b[4] != ELFCLASS64 {
        return Err("not ELF64 (32-bit images unsupported)".into());
    }
    if b[5] != ELFDATA2LSB {
        return Err("not little-endian".into());
    }
    let machine = rd16(b, 18);
    if machine != EM_RISCV {
        return Err(format!("not a RISC-V image (e_machine={machine})"));
    }

    let e_entry = rd64(b, 24);
    let e_phoff = rd64(b, 32);
    let e_shoff = rd64(b, 40);
    let e_phentsize = rd16(b, 54) as u64;
    let e_phnum = rd16(b, 56) as u64;
    let e_shentsize = rd16(b, 58) as u64;
    let e_shnum = rd16(b, 60) as u64;

    if e_phnum == 0 {
        return Err("no program headers".into());
    }

    let mut loaded = 0u32;
    for i in 0..e_phnum {
        let ph = e_phoff.wrapping_add(i.wrapping_mul(e_phentsize));
        if !fits(b, ph, 56) {
            return Err("program header out of range".into());
        }
        if rd32(b, ph) != PT_LOAD {
            continue;
        }
        let p_offset = rd64(b, ph + 8);
        let p_paddr = rd64(b, ph + 24);
        let p_filesz = rd64(b, ph + 32);
        let p_memsz = rd64(b, ph + 40);

        if !fits(b, p_offset, p_filesz) {
            return Err("segment extends past end of file".into());
        }
        if p_memsz < p_filesz {
            return Err("segment memsz < filesz".into());
        }

        // A segment must fit in RAM, memsz included, before anything is written. Without this a crafted
        // p_memsz of 2^60 would send the zero-fill loop around a million-byte chunk 2^40 times.
        let end = p_paddr.checked_add(p_memsz).ok_or("segment address overflows")?;
        if p_paddr < crate::types::DRAM_BASE || end > crate::types::DRAM_BASE + bus.ram.size() {
            return Err(format!("segment {p_paddr:#x}..{end:#x} does not fit in mapped memory"));
        }

        // Copy the bytes present in the file...
        let data = &b[p_offset as usize..(p_offset + p_filesz) as usize];
        if !bus.ram.write_bytes(p_paddr, data) {
            return Err(format!("segment does not fit in mapped memory at {p_paddr:#x}"));
        }
        // ...then zero the rest. This is .bss: the program expects it to exist and to
        // be zero, but it takes no space in the file.
        let bss = p_memsz - p_filesz;
        if bss > 0 {
            let start = p_paddr.checked_add(p_filesz).ok_or("bss does not fit in mapped memory")?;
            // Zero in bounded chunks so a huge memsz cannot demand one huge allocation.
            let zeros = vec![0u8; bss.min(1 << 20) as usize];
            let mut done = 0u64;
            while done < bss {
                let n = (bss - done).min(zeros.len() as u64) as usize;
                if !bus.ram.write_bytes(start + done, &zeros[..n]) {
                    return Err("bss does not fit in mapped memory".into());
                }
                done += n as u64;
            }
        }
        loaded += 1;
    }
    if loaded == 0 {
        return Err("no PT_LOAD segments".into());
    }

    Ok(LoadedImage { entry: e_entry, tohost: find_symbol(b, e_shoff, e_shentsize, e_shnum, "tohost") })
}
