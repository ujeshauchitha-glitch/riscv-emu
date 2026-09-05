#include "fpu.hpp"

#include <cfenv>
#include <cmath>
#include <cstring>

float bits_to_f32(u32 bits) {
    float f;
    std::memcpy(&f, &bits, sizeof f);
    return f;
}

u32 f32_to_bits(float value) {
    u32 bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

double bits_to_f64(u64 bits) {
    double d;
    std::memcpy(&d, &bits, sizeof d);
    return d;
}

u64 f64_to_bits(double value) {
    u64 bits;
    std::memcpy(&bits, &value, sizeof bits);
    return bits;
}

namespace {

// Classification works on the bit pattern rather than on host predicates,
// because the distinctions that matter here - negative zero from positive zero,
// signalling NaN from quiet - are precisely the ones host comparisons erase.
template <typename Bits, unsigned kMantissaBits, unsigned kExponentBits>
u64 classify(Bits bits) {
    constexpr unsigned kTotalBits = sizeof(Bits) * 8;
    const bool  negative = (bits >> (kTotalBits - 1)) != 0;
    const Bits  exponent = (bits >> kMantissaBits) & ((Bits{1} << kExponentBits) - 1);
    const Bits  mantissa = bits & ((Bits{1} << kMantissaBits) - 1);
    const Bits  max_exp  = (Bits{1} << kExponentBits) - 1;

    if (exponent == max_exp) {
        if (mantissa == 0) return negative ? FCLASS_NEG_INF : FCLASS_POS_INF;
        // The top mantissa bit distinguishes quiet from signalling: set means
        // quiet. This is the one place the two kinds of NaN differ.
        const Bits quiet_bit = Bits{1} << (kMantissaBits - 1);
        return (mantissa & quiet_bit) ? FCLASS_QUIET_NAN : FCLASS_SIGNALLING;
    }
    if (exponent == 0) {
        if (mantissa == 0) return negative ? FCLASS_NEG_ZERO : FCLASS_POS_ZERO;
        return negative ? FCLASS_NEG_SUBNORM : FCLASS_POS_SUBNORM;
    }
    return negative ? FCLASS_NEG_NORMAL : FCLASS_POS_NORMAL;
}

}  // namespace

u64 fclass_f32(u32 bits) { return classify<u32, 23, 8>(bits); }
u64 fclass_f64(u64 bits) { return classify<u64, 52, 11>(bits); }

bool is_snan_f32(u32 bits) { return fclass_f32(bits) == FCLASS_SIGNALLING; }
bool is_snan_f64(u64 bits) { return fclass_f64(bits) == FCLASS_SIGNALLING; }

// ---------------------------------------------------------------------------
// Rounding.
//
// Four of RISC-V's five modes map straight onto C's fesetround. The fifth,
// RMM - round to nearest with ties away from zero - has no C equivalent and no
// hardware support on x86 or ARM. It is also essentially unused: compilers do
// not emit it and no libm depends on it. Rather than implement a whole
// soft-float path for it, this maps it to round-to-nearest-even, which differs
// only on an exact tie. That is a real deviation and is documented as one.
// ---------------------------------------------------------------------------
RoundingScope::RoundingScope(u32 inst_rm, u32 frm) {
    const u32 mode = (inst_rm == csr::FRM_DYN) ? frm : inst_rm;

    int host_mode;
    switch (mode) {
        case csr::FRM_RNE: host_mode = FE_TONEAREST;  break;
        case csr::FRM_RTZ: host_mode = FE_TOWARDZERO; break;
        case csr::FRM_RDN: host_mode = FE_DOWNWARD;   break;
        case csr::FRM_RUP: host_mode = FE_UPWARD;     break;
        case csr::FRM_RMM: host_mode = FE_TONEAREST;  break;   // see above
        default:
            // 5 and 6 are reserved, and so is DYN when frm itself holds a
            // reserved value. Either makes the instruction illegal.
            valid_ = false;
            return;
    }

    saved_ = std::fegetround();
    if (saved_ != host_mode) {
        std::fesetround(host_mode);
        changed_ = true;
    }
}

RoundingScope::~RoundingScope() {
    if (changed_) std::fesetround(saved_);
}

void clear_host_exceptions() {
    std::feclearexcept(FE_ALL_EXCEPT);
}

u64 take_host_exceptions() {
    const int raised = std::fetestexcept(FE_ALL_EXCEPT);
    std::feclearexcept(FE_ALL_EXCEPT);

    u64 flags = 0;
    if (raised & FE_INEXACT)   flags |= csr::FFLAG_NX;
    if (raised & FE_UNDERFLOW) flags |= csr::FFLAG_UF;
    if (raised & FE_OVERFLOW)  flags |= csr::FFLAG_OF;
    if (raised & FE_DIVBYZERO) flags |= csr::FFLAG_DZ;
    if (raised & FE_INVALID)   flags |= csr::FFLAG_NV;
    return flags;
}
