#include "bus.hpp"

Exception access_fault_for(AccessType type) {
    switch (type) {
        case AccessType::Instruction: return Exception::InstructionAccessFault;
        case AccessType::Load:        return Exception::LoadAccessFault;
        case AccessType::Store:       return Exception::StoreAMOAccessFault;
    }
    return Exception::LoadAccessFault;
}

bool Bus::attach(std::unique_ptr<Device> dev) {
    if (!dev) return false;

    // Reject overlapping ranges. A silently shadowed device would be a
    // miserable bug to track down later, when a UART write lands in RAM and
    // nothing prints.
    for (const auto& existing : devices_) {
        const u64 a_lo = dev->base();
        const u64 a_hi = a_lo + dev->size();
        const u64 b_lo = existing->base();
        const u64 b_hi = b_lo + existing->size();
        if (a_lo < b_hi && b_lo < a_hi) return false;
    }

    devices_.push_back(std::move(dev));
    return true;
}

Device* Bus::find(u64 addr) {
    for (auto& dev : devices_) {
        if (dev->contains(addr)) return dev.get();
    }
    return nullptr;
}

const Device* Bus::find(u64 addr) const {
    for (const auto& dev : devices_) {
        if (dev->contains(addr)) return dev.get();
    }
    return nullptr;
}

Result<u64> Bus::load(u64 addr, unsigned size_bytes, AccessType type) const {
    const Device* dev = find(addr);
    if (dev == nullptr) {
        return Result<u64>::bad(access_fault_for(type), addr);
    }

    // A device claims the *starting* address, but a wide access could still run
    // past its end; the device's own bounds check catches that.
    return const_cast<Device*>(dev)->load(addr - dev->base(), size_bytes);
}

Status Bus::store(u64 addr, unsigned size_bytes, u64 value) {
    Device* dev = find(addr);
    if (dev == nullptr) {
        return Status::bad(Exception::StoreAMOAccessFault, addr);
    }
    return dev->store(addr - dev->base(), size_bytes, value);
}
