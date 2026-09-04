#include "trap.hpp"

const char* exception_name(Exception e) {
    switch (e) {
        case Exception::InstructionAddressMisaligned: return "instruction address misaligned";
        case Exception::InstructionAccessFault:       return "instruction access fault";
        case Exception::IllegalInstruction:           return "illegal instruction";
        case Exception::Breakpoint:                   return "breakpoint";
        case Exception::LoadAddressMisaligned:        return "load address misaligned";
        case Exception::LoadAccessFault:              return "load access fault";
        case Exception::StoreAMOAddressMisaligned:    return "store/AMO address misaligned";
        case Exception::StoreAMOAccessFault:          return "store/AMO access fault";
        case Exception::ECallFromUMode:               return "ecall from U-mode";
        case Exception::ECallFromSMode:               return "ecall from S-mode";
        case Exception::ECallFromMMode:               return "ecall from M-mode";
        case Exception::InstructionPageFault:         return "instruction page fault";
        case Exception::LoadPageFault:                return "load page fault";
        case Exception::StoreAMOPageFault:            return "store/AMO page fault";
    }
    return "unknown exception";
}
