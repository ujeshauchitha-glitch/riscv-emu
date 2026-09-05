#pragma once

#include <string>
#include <vector>

#include "types.hpp"

// ---------------------------------------------------------------------------
// The Flattened Device Tree.
//
// xv6 gets away without one: it is written for exactly this machine, with the
// UART's address compiled into it. Linux cannot be. The same kernel binary
// boots on a SiFive board, a QEMU virt machine and this emulator, so something
// has to tell it, at runtime, what hardware is present and where. That
// something is a device tree - a blob of structured data the bootloader leaves
// in memory and passes the address of in a1.
//
// The format is deliberately simple, because it has to be parsed by a kernel
// that has no allocator yet, no console, and no idea where anything is:
//
//   header      magic, sizes, and offsets to the three blocks below
//   memory      reservation map (regions the kernel must not use)
//   structure   a stream of tokens: BEGIN_NODE, PROP, END_NODE, END
//   strings     every property name, once, referenced by offset
//
// Everything is big-endian, including on a little-endian machine, which is a
// standing trap for anyone writing one of these by hand.
//
// This builder generates the tree directly rather than shelling out to `dtc`.
// That keeps the emulator dependency-free and, more usefully, keeps the
// description of the machine in the same place as the machine itself - the DTB
// is generated from the same constants in types.hpp that the devices are
// attached with, so it cannot drift out of agreement with them.
// ---------------------------------------------------------------------------

class Fdt {
public:
    // Build a device tree describing this machine: one hart, `dram_bytes` of
    // memory at DRAM_BASE, and the CLINT, PLIC, UART and virtio-mmio devices.
    //
    // `bootargs` becomes /chosen/bootargs - the kernel command line. Everything
    // Linux needs to find its console and its root filesystem goes there.
    static std::vector<u8> build(u64 dram_bytes, const std::string& bootargs,
                                 u64 initrd_start = 0, u64 initrd_end = 0);

private:
    std::vector<u8>          structure_;
    std::vector<u8>          strings_;
    std::vector<u8>          reservations_;

    // Property names are pooled: the same name used by twenty nodes is stored
    // once and referenced by offset. Real trees repeat "compatible" and "reg"
    // constantly, so this is most of why the format is compact.
    u32 intern(const std::string& name);

    void begin_node(const std::string& name);
    void end_node();
    void prop_empty(const std::string& name);
    void prop_u32(const std::string& name, u32 value);
    void prop_u64(const std::string& name, u64 value);
    void prop_string(const std::string& name, const std::string& value);
    void prop_stringlist(const std::string& name,
                         const std::vector<std::string>& values);
    void prop_cells(const std::string& name, const std::vector<u32>& cells);
    void prop_reg(const std::string& name, u64 address, u64 size);

    void align_structure();
    std::vector<u8> finish();
};
