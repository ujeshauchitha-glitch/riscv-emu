#pragma once

#include <unordered_map>

#include "bus.hpp"
#include "csr.hpp"
#include "types.hpp"

// ---------------------------------------------------------------------------
// Sv39 virtual memory.
//
// This is what lets a kernel give every process its own address space, and what
// makes a page fault - rather than a crash - the response to touching an
// unmapped address.
//
// Sv39 gives a 39-bit virtual address space (512 GiB) through three levels of
// page table. A virtual address splits into three 9-bit indexes and an offset:
//
//   38      30 29      21 20      12 11         0
//  +----------+----------+----------+------------+
//  |  VPN[2]  |  VPN[1]  |  VPN[0]  |   offset   |
//  +----------+----------+----------+------------+
//
// Each index selects one of 512 entries in a table (512 x 8 bytes = one 4 KiB
// page - the sizes are chosen so a page table is exactly one page). The walk
// starts at the root table named by satp, and each level either points at the
// next table or is a *leaf* that completes the translation.
//
// A leaf found early is a superpage: at level 1 it maps 2 MiB, at level 2 it
// maps 1 GiB. That is how a kernel maps its own large regions without building
// thousands of leaf entries.
//
// Bits 63:39 of a virtual address must all equal bit 38 - the address must be
// "sign-extended" into the unused top. That is what creates the familiar split
// between low user addresses and high kernel addresses with a vast hole
// between: the hole is not a convention, it is unaddressable.
// ---------------------------------------------------------------------------

// Page table entry bits.
namespace pte {
constexpr u64 V = 1ull << 0;   // valid
constexpr u64 R = 1ull << 1;   // readable
constexpr u64 W = 1ull << 2;   // writable
constexpr u64 X = 1ull << 3;   // executable
constexpr u64 U = 1ull << 4;   // accessible from user mode
constexpr u64 G = 1ull << 5;   // global (present in every address space)
constexpr u64 A = 1ull << 6;   // accessed
constexpr u64 D = 1ull << 7;   // dirty
constexpr int PPN_SHIFT = 10;  // the physical page number starts here
}  // namespace pte

constexpr u64 PAGE_SIZE  = 4096;
constexpr u64 PAGE_SHIFT = 12;
constexpr int SV39_LEVELS = 3;

class Mmu {
public:
    explicit Mmu(Bus& bus) : bus_(bus) {}

    // Translate a virtual address, or return the page fault it caused.
    //
    // `priv` is the *effective* privilege for this access - the caller applies
    // mstatus.MPRV before calling, because that only affects loads and stores,
    // never instruction fetch.
    Result<u64> translate(u64 vaddr, AccessType type, u32 priv, CsrFile& csrs);

    // Discard cached translations. Called on SFENCE.VMA and whenever satp
    // changes, because either can invalidate what the TLB remembers.
    void flush() { tlb_.clear(); }

    // Statistics, useful when judging whether the TLB is doing anything.
    u64 tlb_hits() const   { return hits_; }
    u64 tlb_misses() const { return misses_; }

private:
    Bus& bus_;

    // A cached translation. Real hardware caches at page granularity and keys
    // by ASID so a context switch need not flush everything; the same shape is
    // used here.
    struct Entry {
        u64 ppn;       // physical page number of the mapped page
        u64 perms;     // the leaf PTE's permission bits
        u64 pte_addr;  // where that PTE lives, so D can be set on a later store
    };
    std::unordered_map<u64, Entry> tlb_;
    u64 hits_ = 0;
    u64 misses_ = 0;

    static u64 tlb_key(u64 vpn, u64 asid) { return (asid << 44) | vpn; }

    // The page-fault cause matching this access type.
    static Exception page_fault_for(AccessType type);

    // Does `perms` permit this access at this privilege?
    static bool permitted(u64 perms, AccessType type, u32 priv, const CsrFile& csrs);
};
