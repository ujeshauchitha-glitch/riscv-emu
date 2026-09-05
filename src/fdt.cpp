#include "fdt.hpp"

#include <cstring>

namespace {

constexpr u32 FDT_MAGIC       = 0xd00dfeed;
constexpr u32 FDT_VERSION     = 17;
constexpr u32 FDT_COMPAT_VER  = 16;

// Structure-block tokens.
constexpr u32 FDT_BEGIN_NODE = 1;
constexpr u32 FDT_END_NODE   = 2;
constexpr u32 FDT_PROP       = 3;
constexpr u32 FDT_END        = 9;

// Interrupt controller phandles. A phandle is just a unique number a node
// carries so other nodes can refer to it - the device tree's equivalent of a
// pointer. Devices name the controller their interrupt line goes to.
constexpr u32 PHANDLE_PLIC = 1;
constexpr u32 PHANDLE_CPU0_INTC = 2;

// Everything in a device tree is big-endian, including on a little-endian
// machine. Writing these by hand without a helper is the single most common way
// to produce a blob a kernel silently refuses.
void push_be32(std::vector<u8>& out, u32 v) {
    out.push_back(static_cast<u8>(v >> 24));
    out.push_back(static_cast<u8>(v >> 16));
    out.push_back(static_cast<u8>(v >> 8));
    out.push_back(static_cast<u8>(v));
}

void push_be64(std::vector<u8>& out, u64 v) {
    push_be32(out, static_cast<u32>(v >> 32));
    push_be32(out, static_cast<u32>(v));
}

void pad_to_4(std::vector<u8>& out) {
    while (out.size() % 4 != 0) out.push_back(0);
}

}  // namespace

u32 Fdt::intern(const std::string& name) {
    // Look for the name already in the pool. A linear scan is fine: a tree this
    // size has a few dozen distinct property names.
    for (std::size_t i = 0; i + name.size() < strings_.size(); ++i) {
        if (std::memcmp(strings_.data() + i, name.c_str(), name.size() + 1) == 0) {
            return static_cast<u32>(i);
        }
    }
    const u32 offset = static_cast<u32>(strings_.size());
    strings_.insert(strings_.end(), name.begin(), name.end());
    strings_.push_back(0);
    return offset;
}

void Fdt::begin_node(const std::string& name) {
    push_be32(structure_, FDT_BEGIN_NODE);
    structure_.insert(structure_.end(), name.begin(), name.end());
    structure_.push_back(0);
    pad_to_4(structure_);
}

void Fdt::end_node() {
    push_be32(structure_, FDT_END_NODE);
}

void Fdt::prop_empty(const std::string& name) {
    // A property with no value. Its presence is the information - "ranges" on a
    // bus node means "addresses pass through unchanged", and there is nothing
    // more to say.
    push_be32(structure_, FDT_PROP);
    push_be32(structure_, 0);
    push_be32(structure_, intern(name));
}

void Fdt::prop_u32(const std::string& name, u32 value) {
    push_be32(structure_, FDT_PROP);
    push_be32(structure_, 4);
    push_be32(structure_, intern(name));
    push_be32(structure_, value);
}

void Fdt::prop_u64(const std::string& name, u64 value) {
    push_be32(structure_, FDT_PROP);
    push_be32(structure_, 8);
    push_be32(structure_, intern(name));
    push_be64(structure_, value);
}

void Fdt::prop_string(const std::string& name, const std::string& value) {
    push_be32(structure_, FDT_PROP);
    push_be32(structure_, static_cast<u32>(value.size() + 1));
    push_be32(structure_, intern(name));
    structure_.insert(structure_.end(), value.begin(), value.end());
    structure_.push_back(0);
    pad_to_4(structure_);
}

void Fdt::prop_stringlist(const std::string& name,
                          const std::vector<std::string>& values) {
    // A list is just several NUL-terminated strings back to back. "compatible"
    // uses this to name a device from most specific to least, so a kernel can
    // fall back to a generic driver when it has no exact match.
    u32 length = 0;
    for (const auto& v : values) length += static_cast<u32>(v.size() + 1);

    push_be32(structure_, FDT_PROP);
    push_be32(structure_, length);
    push_be32(structure_, intern(name));
    for (const auto& v : values) {
        structure_.insert(structure_.end(), v.begin(), v.end());
        structure_.push_back(0);
    }
    pad_to_4(structure_);
}

void Fdt::prop_cells(const std::string& name, const std::vector<u32>& cells) {
    push_be32(structure_, FDT_PROP);
    push_be32(structure_, static_cast<u32>(cells.size() * 4));
    push_be32(structure_, intern(name));
    for (u32 c : cells) push_be32(structure_, c);
}

void Fdt::prop_reg(const std::string& name, u64 address, u64 size) {
    // With #address-cells = 2 and #size-cells = 2, a reg entry is four 32-bit
    // cells: the address in two, the size in two. That is what lets a 64-bit
    // machine describe memory above 4 GiB in a format designed for 32-bit ones.
    push_be32(structure_, FDT_PROP);
    push_be32(structure_, 16);
    push_be32(structure_, intern(name));
    push_be64(structure_, address);
    push_be64(structure_, size);
}

std::vector<u8> Fdt::finish() {
    push_be32(structure_, FDT_END);

    // The reservation block always ends with an all-zero entry, even when there
    // is nothing reserved. A parser reads entries until it sees that.
    push_be64(reservations_, 0);
    push_be64(reservations_, 0);

    constexpr u32 header_size = 40;
    const u32 off_reservations = header_size;
    const u32 off_structure    = off_reservations +
                                 static_cast<u32>(reservations_.size());
    const u32 off_strings      = off_structure +
                                 static_cast<u32>(structure_.size());
    const u32 total            = off_strings + static_cast<u32>(strings_.size());

    std::vector<u8> out;
    out.reserve(total);
    push_be32(out, FDT_MAGIC);
    push_be32(out, total);
    push_be32(out, off_structure);
    push_be32(out, off_strings);
    push_be32(out, off_reservations);
    push_be32(out, FDT_VERSION);
    push_be32(out, FDT_COMPAT_VER);
    push_be32(out, 0);                                    // boot cpuid
    push_be32(out, static_cast<u32>(strings_.size()));
    push_be32(out, static_cast<u32>(structure_.size()));

    out.insert(out.end(), reservations_.begin(), reservations_.end());
    out.insert(out.end(), structure_.begin(), structure_.end());
    out.insert(out.end(), strings_.begin(), strings_.end());
    return out;
}

std::vector<u8> Fdt::build(u64 dram_bytes, const std::string& bootargs,
                           u64 initrd_start, u64 initrd_end) {
    Fdt t;

    t.begin_node("");
    // Two cells for an address and two for a size: 64-bit values, in a format
    // whose cells are 32 bits wide. Every `reg` in the tree is read according
    // to these, so getting them wrong misplaces every device at once.
    t.prop_u32("#address-cells", 2);
    t.prop_u32("#size-cells", 2);
    t.prop_string("compatible", "riscv-virtio");
    t.prop_string("model", "riscv-emu,virt");

    // --- /chosen: what the bootloader wants to tell the kernel ---------------
    t.begin_node("chosen");
    t.prop_string("bootargs", bootargs);
    // Where to print. Without this Linux boots silently - it has a UART driver
    // and a device tree node describing the UART, but no reason to believe that
    // particular UART is the console.
    t.prop_string("stdout-path", "/soc/serial@10000000");
    if (initrd_end > initrd_start) {
        t.prop_u32("linux,initrd-start", static_cast<u32>(initrd_start));
        t.prop_u32("linux,initrd-end", static_cast<u32>(initrd_end));
    }
    t.end_node();

    // --- /memory -------------------------------------------------------------
    t.begin_node("memory@80000000");
    t.prop_string("device_type", "memory");
    t.prop_reg("reg", DRAM_BASE, dram_bytes);
    t.end_node();

    // --- /cpus ---------------------------------------------------------------
    t.begin_node("cpus");
    t.prop_u32("#address-cells", 1);
    t.prop_u32("#size-cells", 0);
    // The timer frequency. Our mtime advances once per instruction, and this
    // number is what the kernel divides by to convert ticks to seconds - so it
    // is a claim about how fast the emulator runs, and the guest's idea of
    // wall-clock time is only as good as this estimate.
    t.prop_u32("timebase-frequency", 10000000);
    t.begin_node("cpu@0");
    t.prop_string("device_type", "cpu");
    t.prop_u32("reg", 0);
    t.prop_string("status", "okay");
    t.prop_string("compatible", "riscv");
    // The ISA string is how Linux discovers what it may use. It must match what
    // the emulator actually implements: claim more and the kernel executes an
    // instruction that traps, claim less and it takes a slower path for no
    // reason.
    t.prop_string("riscv,isa", "rv64imafdc_zicsr_zifencei");
    t.prop_string("mmu-type", "riscv,sv39");
    t.begin_node("interrupt-controller");
    t.prop_u32("#interrupt-cells", 1);
    t.prop_empty("interrupt-controller");
    t.prop_string("compatible", "riscv,cpu-intc");
    t.prop_u32("phandle", PHANDLE_CPU0_INTC);
    t.end_node();
    t.end_node();
    t.end_node();

    // --- /soc: the memory-mapped devices -------------------------------------
    t.begin_node("soc");
    t.prop_u32("#address-cells", 2);
    t.prop_u32("#size-cells", 2);
    t.prop_stringlist("compatible", {"simple-bus"});
    // "ranges" with no value means addresses inside this bus are the same as
    // outside it - no translation. A real bus with a window would list one.
    t.prop_empty("ranges");

    // The console.
    t.begin_node("serial@10000000");
    t.prop_stringlist("compatible", {"ns16550a"});
    t.prop_reg("reg", UART0_BASE, 0x100);
    t.prop_u32("interrupt-parent", PHANDLE_PLIC);
    t.prop_u32("interrupts", UART0_IRQ);
    t.prop_u32("clock-frequency", 3686400);
    t.end_node();

    // The disk.
    t.begin_node("virtio_mmio@10001000");
    t.prop_stringlist("compatible", {"virtio,mmio"});
    t.prop_reg("reg", VIRTIO_BASE, 0x1000);
    t.prop_u32("interrupt-parent", PHANDLE_PLIC);
    t.prop_u32("interrupts", VIRTIO0_IRQ);
    t.end_node();

    // The timer and software-interrupt block. Its `interrupts-extended` names
    // the per-hart controller twice, once for the software interrupt (3) and
    // once for the timer (7) - the two things a hart raises for itself.
    t.begin_node("clint@2000000");
    t.prop_stringlist("compatible", {"sifive,clint0", "riscv,clint0"});
    t.prop_reg("reg", CLINT_BASE, 0x10000);
    t.prop_cells("interrupts-extended", {PHANDLE_CPU0_INTC, 3, PHANDLE_CPU0_INTC, 7});
    t.end_node();

    // The external interrupt controller. Context 1 is this hart's supervisor
    // context - cause 9 - which is the one Linux uses.
    t.begin_node("plic@c000000");
    t.prop_u32("#address-cells", 0);
    t.prop_u32("#interrupt-cells", 1);
    t.prop_stringlist("compatible", {"sifive,plic-1.0.0", "riscv,plic0"});
    t.prop_empty("interrupt-controller");
    t.prop_reg("reg", PLIC_BASE, 0x400000);
    t.prop_cells("interrupts-extended", {PHANDLE_CPU0_INTC, 11, PHANDLE_CPU0_INTC, 9});
    t.prop_u32("riscv,ndev", 31);
    t.prop_u32("phandle", PHANDLE_PLIC);
    t.end_node();

    t.end_node();   // /soc

    // --- /poweroff and /reboot ----------------------------------------------
    // How the kernel turns the machine off. Without these, `poweroff` prints
    // "System halted" and spins forever, which looks like a hang.
    t.begin_node("poweroff");
    t.prop_u32("value", 0x5555);
    t.prop_u32("offset", 0);
    t.prop_u32("regmap", 3);
    t.prop_string("compatible", "syscon-poweroff");
    t.end_node();

    t.begin_node("reboot");
    t.prop_u32("value", 0x7777);
    t.prop_u32("offset", 0);
    t.prop_u32("regmap", 3);
    t.prop_string("compatible", "syscon-reboot");
    t.end_node();

    t.begin_node("syscon@100000");
    t.prop_stringlist("compatible", {"syscon"});
    t.prop_reg("reg", SYSCON_BASE, 0x1000);
    t.prop_u32("phandle", 3);
    t.end_node();

    t.end_node();   // /
    return t.finish();
}
