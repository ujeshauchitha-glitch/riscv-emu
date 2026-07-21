#pragma once

#include <array>
#include <cstdint>
#include <iostream>
#include "memory.hpp"

constexpr int NUM_REGS = 32;

class CPU {
    public:
    Memory& memory;
    //Registers
    std::array<uint64_t, NUM_REGS> regs{};

    //Program counter
    uint64_t pc = 0;

    //Constructor
    CPU(Memory& mem) : memory(mem) {
        regs.fill(0);
        pc = 0;
    }
    //Read register
    inline uint64_t read_reg(uint32_t index) const{
        if(index == 0){
            return 0;
        }
        return regs[index];
    }

    //Write register
    inline void write_reg(uint32_t index, uint64_t value){
        if(index == 0){
            return;
        }
        regs[index] = value;
    }

    uint32_t fetch() const{
        return memory.read32(pc);
    }

    void decode(uint32_t instruction) {
        uint32_t opcode = instruction & 0x7F;
        uint32_t rd     = (instruction >> 7) & 0x1F;
        uint32_t funct3 = (instruction >> 12) & 0x07;
        uint32_t rs1    = (instruction >> 15) & 0x1F;
        int32_t imm = static_cast<int32_t>(instruction) >> 20;
        uint32_t funct7 = (instruction >> 25) & 0x7F;

        std::cout << "imm: " << imm << "\n";

        if (opcode == 0x13) {
            std::cout << "Instruction: ADDI (I-Type)\n";
        }
        else {
            std::cout << "Unknown opcode\n";
        }

        std::cout << "Instruction: 0x"
                  << std::hex << instruction << std::dec << "\n";

        std::cout << "Opcode: " << opcode << "\n";
        std::cout << "rd: " << rd << "\n";
        std::cout << "funct3: " << funct3 << "\n";
        std::cout << "rs1: " << rs1 << "\n";
        std::cout << "funct7: " << funct7 << "\n";

        execute(opcode, rd, rs1, imm);
    }

    void execute(uint32_t opcode,
             uint32_t rd,
             uint32_t rs1,
             int32_t imm)
    {
        if (opcode == 0x13) {
            write_reg(rd, read_reg(rs1) + imm);
        }

        pc += 4;
    }

    void step() {
        uint32_t instruction = fetch();
        decode(instruction);
    }

    //debug dump
    void dump_registers() const{
        const char* abi_names[NUM_REGS] = {
            "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
            "s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5", 
            "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
            "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"
        };
        std::cout << "===Register Dump===\n";
        std::cout << "PC: 0x" << std::hex << pc << std::dec << "\n";
        for(int i = 0; i < NUM_REGS; i++){
            std::cout << i 
            << "(" << abi_names[i] << ")" 
            << "\t: 0x" << std::hex << regs[i] << std::dec << "\n";
        }
        std::cout << "=====================\n";
    }
};