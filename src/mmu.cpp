#include "mmu.hpp"

Exception Mmu::page_fault_for(AccessType type) {
    switch (type) {
        case AccessType::Instruction: return Exception::InstructionPageFault;
        case AccessType::Load:        return Exception::LoadPageFault;
        case AccessType::Store:       return Exception::StoreAMOPageFault;
    }
    return Exception::LoadPageFault;
}

bool Mmu::permitted(u64 perms, AccessType type, u32 priv, const CsrFile& csrs) {
    // The U bit says which side of the user/supervisor divide a page is on.
    const bool user_page = (perms & pte::U) != 0;

    if (priv == PRIV_USER) {
        // User mode may only touch pages marked U.
        if (!user_page) return false;
    } else {
        // Supervisor touching a user page is normally forbidden. That is a
        // deliberate hardening measure: without it, a kernel tricked into
        // dereferencing a user-supplied pointer would happily read or write
        // user memory as itself. mstatus.SUM lifts the restriction for the
        // windows where a kernel genuinely means to do it (copyin/copyout).
        //
        // Note SUM applies to loads and stores only, never to instruction
        // fetch: the kernel must never execute user pages, whatever SUM says.
        if (user_page && (type == AccessType::Instruction || !csrs.mstatus_sum())) {
            return false;
        }
    }

    switch (type) {
        case AccessType::Instruction:
            return (perms & pte::X) != 0;

        case AccessType::Load:
            // MXR ("make executable readable") lets a load succeed on an
            // execute-only page. Kernels use it to inspect their own code.
            if (perms & pte::R) return true;
            return csrs.mstatus_mxr() && (perms & pte::X);

        case AccessType::Store:
            return (perms & pte::W) != 0;
    }
    return false;
}

Result<u64> Mmu::translate(u64 vaddr, AccessType type, u32 priv, CsrFile& csrs) {
    // Machine mode does not translate, and neither does anything when satp says
    // Bare. Translation is a supervisor/user facility.
    if (priv == PRIV_MACHINE || csrs.satp_mode() == csr::SATP_MODE_BARE) {
        return Result<u64>::good(vaddr);
    }

    // Bits 63:39 must all equal bit 38. An address that fails this is not
    // merely unmapped, it is unrepresentable - and rejecting it here is what
    // creates the unaddressable hole between user and kernel space.
    const u64 sign_bits = vaddr >> 38;
    if (sign_bits != 0 && sign_bits != 0x3ff'ffff) {
        return Result<u64>::bad(page_fault_for(type), vaddr);
    }

    const u64 vpn  = vaddr >> PAGE_SHIFT;
    const u64 asid = csrs.satp_asid();
    const u64 offset = vaddr & (PAGE_SIZE - 1);

    // The TLB caches whole translations. Without it every guest memory access
    // costs three extra memory reads for the walk, which is the difference
    // between a kernel booting in seconds and in minutes.
    const u64 key = tlb_key(vpn, asid);
    if (auto it = tlb_.find(key); it != tlb_.end()) {
        if (!permitted(it->second.perms, type, priv, csrs)) {
            return Result<u64>::bad(page_fault_for(type), vaddr);
        }

        // A hit still has to set the dirty bit.
        //
        // This is easy to miss, because the walk sets A and D and a hit skips
        // the walk entirely - so a page that is *read* first, filling the TLB,
        // and written afterwards would keep D = 0 forever. A kernel scanning
        // page tables for dirty pages (writeback, swap, msync) would then treat
        // modified pages as clean and drop the writes, which is data loss with
        // no error anywhere.
        //
        // Remembering the PTE's address in the entry is what makes this
        // possible without re-walking.
        if (type == AccessType::Store && (it->second.perms & pte::D) == 0) {
            it->second.perms |= pte::D;
            bus_.store(it->second.pte_addr, 8, it->second.perms);
        }

        ++hits_;
        return Result<u64>::good((it->second.ppn << PAGE_SHIFT) | offset);
    }
    ++misses_;

    // Walk the table, starting at the root satp names.
    u64 table = csrs.satp_ppn() << PAGE_SHIFT;
    u64 entry = 0;
    int level = SV39_LEVELS - 1;

    for (; level >= 0; --level) {
        // The 9 bits of the virtual address that index this level.
        const u64 index = (vaddr >> (PAGE_SHIFT + 9 * level)) & 0x1ff;
        const u64 addr  = table + index * 8;

        Result<u64> r = bus_.load(addr, 8, AccessType::Load);
        if (!r) {
            // The page table itself is unreachable - a corrupt satp, or a table
            // outside RAM. Report it as a page fault, which is what the guest
            // can act on.
            return Result<u64>::bad(page_fault_for(type), vaddr);
        }
        entry = r.value;

        if ((entry & pte::V) == 0) {
            return Result<u64>::bad(page_fault_for(type), vaddr);
        }
        // W without R is a reserved encoding, not a write-only page.
        if ((entry & pte::W) && !(entry & pte::R)) {
            return Result<u64>::bad(page_fault_for(type), vaddr);
        }

        if (entry & (pte::R | pte::X)) break;   // a leaf: the walk ends here

        // A pointer to the next level down.
        table = ((entry >> pte::PPN_SHIFT) << PAGE_SHIFT);
    }

    // Ran past the last level without finding a leaf.
    if (level < 0) {
        return Result<u64>::bad(page_fault_for(type), vaddr);
    }

    if (!permitted(entry, type, priv, csrs)) {
        return Result<u64>::bad(page_fault_for(type), vaddr);
    }

    u64 ppn = entry >> pte::PPN_SHIFT;

    // A leaf above level 0 is a superpage, and its physical address must be
    // aligned to the size it maps - the lower page-number fields have to be
    // zero. A misaligned superpage is a fault, not a silently truncated
    // mapping.
    if (level > 0) {
        const u64 low_bits = ppn & ((1ull << (9 * level)) - 1);
        if (low_bits != 0) {
            return Result<u64>::bad(page_fault_for(type), vaddr);
        }
        // The virtual address supplies the page-number bits the superpage does
        // not: a 2 MiB page is selected by VPN[2] and VPN[1], and VPN[0] picks
        // the 4 KiB page inside it.
        const u64 vpn_low = vpn & ((1ull << (9 * level)) - 1);
        ppn |= vpn_low;
    }

    // The accessed and dirty bits. The spec allows an implementation either to
    // fault so software can set them, or to set them itself; setting them is
    // simpler and is what most hardware does.
    const u64 pte_index = (vaddr >> (PAGE_SHIFT + 9 * level)) & 0x1ff;
    const u64 pte_addr  = table + pte_index * 8;

    u64 updated = entry | pte::A;
    if (type == AccessType::Store) updated |= pte::D;
    if (updated != entry) {
        bus_.store(pte_addr, 8, updated);
    }

    // Cache the *updated* permissions, not the ones read from memory: the
    // entry's D bit is what the hit path above tests, so caching the stale
    // value would make it write the PTE again on every store.
    tlb_[key] = Entry{ppn, updated, pte_addr};
    return Result<u64>::good((ppn << PAGE_SHIFT) | offset);
}
