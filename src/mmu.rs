//! Sv39 virtual memory.
//!
//! This is what lets a kernel give every process its own address space, and what
//! makes a page fault - rather than a crash - the response to touching an unmapped
//! address.
//!
//! Sv39 gives a 39-bit virtual address space (512 GiB) through three levels of page
//! table. A virtual address splits into three 9-bit indexes and an offset:
//!
//! ```text
//!   38      30 29      21 20      12 11         0
//!  +----------+----------+----------+------------+
//!  |  VPN[2]  |  VPN[1]  |  VPN[0]  |   offset   |
//!  +----------+----------+----------+------------+
//! ```
//!
//! Each index selects one of 512 entries in a table (512 x 8 bytes = exactly one
//! 4 KiB page). The walk starts at the root table satp names, and each level either
//! points at the next table or is a *leaf* that completes the translation. A leaf
//! found early is a superpage: 2 MiB at level 1, 1 GiB at level 2.
//!
//! Bits 63:39 of a virtual address must all equal bit 38. That is what creates the
//! familiar split between low user addresses and high kernel addresses with a vast
//! unaddressable hole between.

use crate::bus::Bus;
use crate::csr::{csr, CsrFile};
use crate::device::AccessType;
use crate::trap::{Exception, Res, Trap};
use crate::types::{PRIV_MACHINE, PRIV_USER};

pub mod pte {
    pub const V: u64 = 1 << 0; // valid
    pub const R: u64 = 1 << 1; // readable
    pub const W: u64 = 1 << 2; // writable
    pub const X: u64 = 1 << 3; // executable
    pub const U: u64 = 1 << 4; // accessible from user mode
    pub const G: u64 = 1 << 5; // global
    pub const A: u64 = 1 << 6; // accessed
    pub const D: u64 = 1 << 7; // dirty
    pub const PPN_SHIFT: u32 = 10;
}

pub const PAGE_SIZE: u64 = 4096;
pub const PAGE_SHIFT: u32 = 12;
const SV39_LEVELS: i32 = 3;

const TLB_SLOTS: usize = 1024; // a power of two
/// A real key is at most 16 (asid) + 27 (vpn) bits wide, so all-ones can never be one.
const INVALID_TAG: u64 = !0;

#[derive(Clone, Copy)]
struct Entry {
    tag: u64,      // asid:vpn, or INVALID_TAG for an empty slot
    ppn: u64,      // physical page number of the mapped page
    perms: u64,    // the leaf PTE's permission bits
    pte_addr: u64, // where that PTE lives, so D can be set on a later store
}

const EMPTY: Entry = Entry { tag: INVALID_TAG, ppn: 0, perms: 0, pte_addr: 0 };

/// The TLB caches whole translations. Without it every guest memory access costs
/// three extra memory reads for the walk, which is the difference between a kernel
/// booting in seconds and in minutes. It is direct-mapped: a translation lives in
/// exactly one slot, chosen from the low bits of the virtual page number. Two hot
/// pages that collide evict each other and cost one walk, which is rare enough not
/// to matter with 1024 slots.
pub struct Mmu {
    tlb: Box<[Entry; TLB_SLOTS]>,
    hits: u64,
    misses: u64,
}

impl Default for Mmu {
    fn default() -> Self {
        Self::new()
    }
}

#[inline]
fn page_fault_for(ty: AccessType) -> Exception {
    match ty {
        AccessType::Instruction => Exception::InstructionPageFault,
        AccessType::Load => Exception::LoadPageFault,
        AccessType::Store => Exception::StoreAMOPageFault,
    }
}

/// Does `perms` permit this access at this privilege?
#[inline(always)]
fn permitted(perms: u64, ty: AccessType, priv_: u32, csrs: &CsrFile) -> bool {
    // The U bit says which side of the user/supervisor divide a page is on.
    let user_page = perms & pte::U != 0;

    if priv_ == PRIV_USER {
        // User mode may only touch pages marked U.
        if !user_page {
            return false;
        }
    } else if user_page && (ty == AccessType::Instruction || !csrs.mstatus_sum()) {
        // Supervisor touching a user page is normally forbidden - a deliberate
        // hardening measure. mstatus.SUM lifts it for the windows where a kernel
        // genuinely copies to or from user memory. SUM applies to loads and stores
        // only, never to instruction fetch.
        return false;
    }

    match ty {
        AccessType::Instruction => perms & pte::X != 0,
        // MXR lets a load succeed on an execute-only page.
        AccessType::Load => perms & pte::R != 0 || (csrs.mstatus_mxr() && perms & pte::X != 0),
        AccessType::Store => perms & pte::W != 0,
    }
}

impl Mmu {
    pub fn new() -> Mmu {
        Mmu { tlb: Box::new([EMPTY; TLB_SLOTS]), hits: 0, misses: 0 }
    }

    /// Discard cached translations. Called on SFENCE.VMA and whenever satp changes.
    pub fn flush(&mut self) {
        for e in self.tlb.iter_mut() {
            e.tag = INVALID_TAG;
        }
    }

    pub fn tlb_hits(&self) -> u64 {
        self.hits
    }
    pub fn tlb_misses(&self) -> u64 {
        self.misses
    }

    /// Translate a virtual address, or return the page fault it caused.
    ///
    /// `priv_` is the *effective* privilege for this access - the caller applies
    /// mstatus.MPRV before calling, because that only affects loads and stores,
    /// never instruction fetch.
    #[inline]
    pub fn translate(
        &mut self,
        bus: &Bus,
        csrs: &CsrFile,
        vaddr: u64,
        ty: AccessType,
        priv_: u32,
    ) -> Res<u64> {
        // Machine mode does not translate, and neither does anything when satp
        // says Bare. Translation is a supervisor/user facility.
        if priv_ == PRIV_MACHINE || csrs.satp_mode() == csr::SATP_MODE_BARE {
            return Ok(vaddr);
        }
        self.translate_paged(bus, csrs, vaddr, ty, priv_)
    }

    fn translate_paged(
        &mut self,
        bus: &Bus,
        csrs: &CsrFile,
        vaddr: u64,
        ty: AccessType,
        priv_: u32,
    ) -> Res<u64> {
        // An address that fails the sign-extension rule is not merely unmapped, it
        // is unrepresentable.
        let sign_bits = vaddr >> 38;
        if sign_bits != 0 && sign_bits != 0x3ff_ffff {
            return Err(Trap::new(page_fault_for(ty), vaddr));
        }

        let vpn = vaddr >> PAGE_SHIFT;
        let asid = csrs.satp_asid();
        let offset = vaddr & (PAGE_SIZE - 1);

        let key = (asid << 44) | vpn;
        let slot = &mut self.tlb[(vpn as usize) & (TLB_SLOTS - 1)];
        if slot.tag == key {
            if !permitted(slot.perms, ty, priv_, csrs) {
                return Err(Trap::new(page_fault_for(ty), vaddr));
            }

            // A hit still has to set the dirty bit. The walk sets A and D and a hit
            // skips the walk, so a page that is *read* first (filling the TLB) and
            // written afterwards would keep D = 0 forever; a kernel scanning page
            // tables for dirty pages would treat modified pages as clean and drop
            // the writes. Remembering the PTE's address makes this possible without
            // re-walking.
            if ty == AccessType::Store && slot.perms & pte::D == 0 {
                slot.perms |= pte::D;
                let _ = bus.store(slot.pte_addr, 8, slot.perms);
            }

            self.hits += 1;
            return Ok((slot.ppn << PAGE_SHIFT) | offset);
        }
        self.misses += 1;

        // Walk the table, starting at the root satp names.
        let mut table = csrs.satp_ppn() << PAGE_SHIFT;
        let mut entry = 0u64;
        let mut level = SV39_LEVELS - 1;

        while level >= 0 {
            // The 9 bits of the virtual address that index this level.
            let index = (vaddr >> (PAGE_SHIFT + 9 * level as u32)) & 0x1ff;
            let addr = table + index * 8;

            // The page table itself unreachable - a corrupt satp, or a table
            // outside RAM - is reported as a page fault, which the guest can act on.
            entry = match bus.load(addr, 8, AccessType::Load) {
                Ok(v) => v,
                Err(_) => return Err(Trap::new(page_fault_for(ty), vaddr)),
            };

            if entry & pte::V == 0 {
                return Err(Trap::new(page_fault_for(ty), vaddr));
            }
            // W without R is a reserved encoding, not a write-only page.
            if entry & pte::W != 0 && entry & pte::R == 0 {
                return Err(Trap::new(page_fault_for(ty), vaddr));
            }
            if entry & (pte::R | pte::X) != 0 {
                break; // a leaf: the walk ends here
            }
            // A pointer to the next level down.
            table = (entry >> pte::PPN_SHIFT) << PAGE_SHIFT;
            level -= 1;
        }

        // Ran past the last level without finding a leaf.
        if level < 0 {
            return Err(Trap::new(page_fault_for(ty), vaddr));
        }
        if !permitted(entry, ty, priv_, csrs) {
            return Err(Trap::new(page_fault_for(ty), vaddr));
        }

        let mut ppn = entry >> pte::PPN_SHIFT;

        // A leaf above level 0 is a superpage, and its physical address must be
        // aligned to the size it maps - the lower page-number fields have to be
        // zero. A misaligned superpage is a fault, not a silently truncated mapping.
        if level > 0 {
            let low_bits = ppn & ((1u64 << (9 * level as u32)) - 1);
            if low_bits != 0 {
                return Err(Trap::new(page_fault_for(ty), vaddr));
            }
            // The virtual address supplies the page-number bits the superpage does
            // not: a 2 MiB page is selected by VPN[2] and VPN[1], and VPN[0] picks
            // the 4 KiB page inside it.
            ppn |= vpn & ((1u64 << (9 * level as u32)) - 1);
        }

        // The accessed and dirty bits. The spec allows either faulting so software
        // can set them, or setting them in hardware; setting them is simpler and is
        // what most hardware does.
        let pte_index = (vaddr >> (PAGE_SHIFT + 9 * level as u32)) & 0x1ff;
        let pte_addr = table + pte_index * 8;

        let mut updated = entry | pte::A;
        if ty == AccessType::Store {
            updated |= pte::D;
        }
        if updated != entry {
            let _ = bus.store(pte_addr, 8, updated);
        }

        // Cache the *updated* permissions, not the ones read from memory: the
        // entry's D bit is what the hit path tests, so caching the stale value
        // would make it write the PTE again on every store.
        *slot = Entry { tag: key, ppn, perms: updated, pte_addr };
        Ok((ppn << PAGE_SHIFT) | offset)
    }
}
