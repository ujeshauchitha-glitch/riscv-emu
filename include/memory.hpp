#pragma once

#include <array>
#include <cstdint>
#include <cstddef>

constexpr std::size_t RAM_SIZE = 1024*1024;

class Memory {
    public:
        Memory(){
            memory.fill(0);
        }

        uint8_t read8(uint64_t address) const{
            return memory[address];
        }
        uint32_t read32(uint64_t address) const {
            return static_cast<uint32_t>(memory[address]) |
            (static_cast<uint32_t>(memory[address + 1]) << 8) |
            (static_cast<uint32_t>(memory[address + 2]) << 16) |
            (static_cast<uint32_t>(memory[address + 3]) << 24);
        }
        void write8(uint64_t address, uint8_t value){
            memory[address] = value;
        }
        void write32(uint64_t address, uint32_t value) {
            memory[address]     = value & 0xFF;
            memory[address + 1] = (value >> 8) & 0xFF;
            memory[address + 2] = (value >> 16) & 0xFF;
            memory[address + 3] = (value >> 24) & 0xFF;
        }
    private:
        std::array<uint8_t, RAM_SIZE> memory{};
};