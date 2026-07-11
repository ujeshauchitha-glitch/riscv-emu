#include <iostream>
#include "cpu.hpp"

int main() {
    CPU cpu;

    //Verification
    cpu.write_reg(1,0xDEADBEEf);
    cpu.write_reg(2,0x80000000);
    cpu.write_reg(0,0x12345678); //should be ignored

    uint64_t zero_val = cpu.read_reg(0);
    std::cout << "x0 (should always be 0) 0x" << std::hex << zero_val << "\n";

    //verify x1 and x2
    std::cout << "x1 (ra): 0x" << cpu.read_reg(1) << "\n";
    std::cout << "x2 (sp): 0x" << cpu.read_reg(2) << "\n";

    cpu.pc = 0x80000000;
    std::cout << "PC: 0x" << cpu.pc << std::dec << "\n\n";

    cpu.dump_registers();
    return 0;
}