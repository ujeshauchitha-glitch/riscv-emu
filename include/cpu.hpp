#pragma once

#include <array>
#include <cstdint>
#include <iostream>

constexpr int NUM_REGS = 32;

class CPU {
    public:
    //Registers
    std::array<uint64_t, NUM_REGS> regs{};

    //Program counter
    uint64_t pc = 0;

    //Constructor
    CPU(){
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