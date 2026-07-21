#include <iostream>
#include "cpu.hpp"
#include "memory.hpp"

int main() {
    Memory mem;
    CPU cpu(mem);

    mem.write32(0, 0x00500093);

    cpu.step();

    std::cout << cpu.read_reg(1) << "\n";
}