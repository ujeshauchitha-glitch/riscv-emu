//! Optional execution backends.
//!
//! Both are absent by default, and with them absent the CPU behaves exactly as it
//! always did - the plain host arithmetic and the plain `regs` array. They exist so
//! that a different implementation of the integer register file or of the integer
//! ALU (a sliced, fault-injected one, say) can be slotted in underneath the
//! unchanged decoder, CSRs, privilege modes and MMU, and compared against this
//! core as the reference.

/// The integer register file, x1..x31. x0 is hardwired to zero by the CPU itself,
/// so a backend is never asked for it and never sees a write to it.
///
/// While a backend is installed `Cpu::regs` is not updated and must not be relied
/// on; use `Cpu::read_reg` / `Cpu::write_reg`.
pub trait RegFileBackend {
    fn read(&mut self, index: usize) -> u64;
    fn write(&mut self, index: usize, value: u64);
}

/// Integer ALU operations. Every one takes two 64-bit operands and returns a 64-bit
/// result, so one entry point covers the register-register forms, the immediate
/// forms (the immediate arrives as `b`, already sign-extended), the comparisons that
/// branches use, and data-address generation (`Add`).
///
/// `*W` operations use the low 32 bits of each operand and return the result
/// sign-extended from bit 31. Shift amounts are taken from the low bits of `b`: 6
/// bits for the 64-bit shifts, 5 for the W shifts.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum AluOp {
    Add,
    Sub,
    And,
    Or,
    Xor,
    Sll,
    Srl,
    Sra,
    Slt,
    Sltu,
    AddW,
    SubW,
    SllW,
    SrlW,
    SraW,
}

pub trait AluBackend {
    fn exec(&mut self, op: AluOp, a: u64, b: u64) -> u64;
}

/// The plain ALU. This is what the CPU runs when no backend is installed, and it is
/// the reference any other `AluBackend` is checked against.
#[inline(always)]
pub fn alu_reference(op: AluOp, a: u64, b: u64) -> u64 {
    let sx32 = |v: u32| v as i32 as i64 as u64;
    let a32 = a as u32;
    let b32 = b as u32;
    match op {
        AluOp::Add => a.wrapping_add(b),
        AluOp::Sub => a.wrapping_sub(b),
        AluOp::And => a & b,
        AluOp::Or => a | b,
        AluOp::Xor => a ^ b,
        AluOp::Sll => a << (b & 0x3f),
        AluOp::Srl => a >> (b & 0x3f),
        AluOp::Sra => ((a as i64) >> (b & 0x3f)) as u64,
        AluOp::Slt => ((a as i64) < (b as i64)) as u64,
        AluOp::Sltu => (a < b) as u64,
        AluOp::AddW => sx32(a32.wrapping_add(b32)),
        AluOp::SubW => sx32(a32.wrapping_sub(b32)),
        AluOp::SllW => sx32(a32 << (b & 0x1f)),
        AluOp::SrlW => sx32(a32 >> (b & 0x1f)),
        AluOp::SraW => sx32(((a32 as i32) >> (b & 0x1f)) as u32),
    }
}
