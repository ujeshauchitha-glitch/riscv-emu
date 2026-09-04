#pragma once

#include <memory>
#include <vector>

#include "bus.hpp"
#include "cpu.hpp"
#include "decoder.hpp"
#include "dram.hpp"
#include "types.hpp"

// ---------------------------------------------------------------------------
// Shared test scaffolding: a small machine, plus instruction encoders.
//
// The encoders are written independently of src/decoder.cpp on purpose. A test
// that used the emulator's own decoder to build its inputs would agree with
// itself no matter how wrong both were; two independent implementations that
// agree is real evidence.
// ---------------------------------------------------------------------------

namespace rvt {

constexpr u64 kTestDramSize = 1024 * 1024;

struct Machine {
    Bus                  bus;
    Dram*                dram = nullptr;
    std::unique_ptr<Cpu> cpu;

    explicit Machine(const std::vector<u32>& program) {
        auto d = std::make_unique<Dram>(kTestDramSize);
        dram   = d.get();
        bus.attach(std::move(d));

        std::vector<u8> bytes;
        bytes.reserve(program.size() * 4);
        for (u32 w : program) {
            bytes.push_back(static_cast<u8>(w & 0xff));
            bytes.push_back(static_cast<u8>((w >> 8) & 0xff));
            bytes.push_back(static_cast<u8>((w >> 16) & 0xff));
            bytes.push_back(static_cast<u8>((w >> 24) & 0xff));
        }
        dram->load_image(DRAM_BASE, bytes);

        cpu = std::make_unique<Cpu>(bus);
    }

    Status run_all(u64 steps) { return cpu->run(steps, nullptr); }

    u64 reg(u32 n) const { return cpu->read_reg(n); }
};

// --- instruction encoders ---------------------------------------------------

inline u32 r_type(u32 opcode, u32 rd, u32 funct3, u32 rs1, u32 rs2, u32 funct7) {
    return ((funct7 & 0x7f) << 25) | ((rs2 & 0x1f) << 20) | ((rs1 & 0x1f) << 15) |
           ((funct3 & 0x7) << 12) | ((rd & 0x1f) << 7) | (opcode & 0x7f);
}

inline u32 i_type(u32 opcode, u32 rd, u32 funct3, u32 rs1, i32 imm) {
    return ((static_cast<u32>(imm) & 0xfff) << 20) | ((rs1 & 0x1f) << 15) |
           ((funct3 & 0x7) << 12) | ((rd & 0x1f) << 7) | (opcode & 0x7f);
}

inline u32 s_type(u32 opcode, u32 funct3, u32 rs1, u32 rs2, i32 imm) {
    const u32 i = static_cast<u32>(imm);
    return (((i >> 5) & 0x7f) << 25) | ((rs2 & 0x1f) << 20) | ((rs1 & 0x1f) << 15) |
           ((funct3 & 0x7) << 12) | ((i & 0x1f) << 7) | (opcode & 0x7f);
}

inline u32 b_type(u32 opcode, u32 funct3, u32 rs1, u32 rs2, i32 imm) {
    const u32 i = static_cast<u32>(imm);
    return (((i >> 12) & 0x1) << 31) | (((i >> 5) & 0x3f) << 25) | ((rs2 & 0x1f) << 20) |
           ((rs1 & 0x1f) << 15) | ((funct3 & 0x7) << 12) | (((i >> 1) & 0xf) << 8) |
           (((i >> 11) & 0x1) << 7) | (opcode & 0x7f);
}

inline u32 u_type(u32 opcode, u32 rd, u32 imm_hi20) {
    return ((imm_hi20 & 0xfffff) << 12) | ((rd & 0x1f) << 7) | (opcode & 0x7f);
}

inline u32 j_type(u32 opcode, u32 rd, i32 imm) {
    const u32 i = static_cast<u32>(imm);
    return (((i >> 20) & 0x1) << 31) | (((i >> 1) & 0x3ff) << 21) | (((i >> 11) & 0x1) << 20) |
           (((i >> 12) & 0xff) << 12) | ((rd & 0x1f) << 7) | (opcode & 0x7f);
}

// --- convenience mnemonics --------------------------------------------------

inline u32 ADDI(u32 rd, u32 rs1, i32 imm)  { return i_type(opcodes::OP_IMM, rd, 0x0, rs1, imm); }
inline u32 LUI(u32 rd, u32 imm20)          { return u_type(opcodes::LUI, rd, imm20); }
inline u32 AUIPC(u32 rd, u32 imm20)        { return u_type(opcodes::AUIPC, rd, imm20); }
inline u32 JAL(u32 rd, i32 imm)            { return j_type(opcodes::JAL, rd, imm); }
inline u32 JALR(u32 rd, u32 rs1, i32 imm)  { return i_type(opcodes::JALR, rd, 0x0, rs1, imm); }
inline u32 NOP()                           { return ADDI(0, 0, 0); }

// End a test program without trapping: jump to self.
//
// EBREAK is the obvious way to stop, but it only halts the machine while no
// trap handler is installed. Once mtvec points somewhere, EBREAK vectors into
// the handler like any other trap - and if the handler itself ends in EBREAK it
// re-enters forever, quietly overwriting mcause and mepc along the way. A
// self-loop stops making progress without disturbing any architectural state,
// so the test runs a fixed budget and then inspects the machine.
inline u32 HALT()                          { return j_type(opcodes::JAL, 0, 0); }

// Load a full 64-bit constant into rd, for setting up test inputs. Built from a
// shift-and-or chain of OP-IMM instructions, which phase 0 already verified, so
// setup never depends on the instruction a given test is trying to check.
inline void load_imm64(std::vector<u32>& out, u32 rd, u64 value) {
    out.push_back(ADDI(rd, 0, 0));  // rd = 0
    for (int shift = 56; shift >= 0; shift -= 8) {
        const u32 byte = static_cast<u32>((value >> shift) & 0xff);
        out.push_back(i_type(opcodes::OP_IMM, rd, 0x1, rd, 8));  // rd <<= 8
        // rd |= byte. 0..255 fits the positive half of a 12-bit immediate, so
        // no sign-extension surprises.
        out.push_back(i_type(opcodes::OP_IMM, rd, 0x6, rd, static_cast<i32>(byte)));
    }
}

constexpr u64 kLoadImm64Steps = 1 + 8 * 2;

}  // namespace rvt
