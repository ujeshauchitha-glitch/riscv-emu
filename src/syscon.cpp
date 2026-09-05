#include "syscon.hpp"

Result<u64> Syscon::load(u64 offset, unsigned) {
    (void)offset;
    return Result<u64>::good(0);
}

Status Syscon::store(u64 offset, unsigned, u64 value) {
    if (offset != 0) return Status::good();

    const u64 command = value & 0xffff;
    if (command == POWEROFF) {
        poweroff_ = true;
        // riscv-tests packs its result into the upper bits, so a non-zero exit
        // code identifies which test failed.
        exit_code_ = value >> 16;
    } else if (command == REBOOT) {
        reboot_ = true;
    }
    return Status::good();
}
