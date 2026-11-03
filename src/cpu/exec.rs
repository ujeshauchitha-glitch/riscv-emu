//! Instruction execution: integer, M, A, Zicsr, privileged, and the bit-manipulation
//! extensions. Floating point lives in `fp.rs`.

use super::{Cpu, NO_RES};
use crate::backends::AluOp;
use crate::bitmanip as bm;
use crate::csr::csr;
use crate::decoder::{opcodes, DecodedInst};
use crate::device::AccessType;
use crate::trap::{Exception, Res, Trap};
use crate::types::*;

#[inline(always)]
fn illegal(inst: &DecodedInst) -> Trap {
    Trap::new(Exception::IllegalInstruction, inst.encoded as u64)
}

impl Cpu {
    pub(crate) fn execute(&mut self, inst: &DecodedInst) -> Res<()> {
        match inst.opcode {
            // LUI - load upper immediate. The decoder already shifted the immediate
            // into the upper 20 bits, so this is just a move.
            opcodes::LUI => {
                self.write_reg(inst.rd, inst.imm as u64);
                Ok(())
            }
            // AUIPC - the basis of every PC-relative address in RISC-V: paired with
            // ADDI it reaches anywhere within +/-2GiB of the current instruction.
            opcodes::AUIPC => {
                self.write_reg(inst.rd, self.pc.wrapping_add(inst.imm as u64));
                Ok(())
            }
            opcodes::JAL => self.execute_jal(inst),
            opcodes::JALR => self.execute_jalr(inst),
            opcodes::BRANCH => self.execute_branch(inst),
            opcodes::LOAD => self.execute_load(inst),
            opcodes::STORE => self.execute_store(inst),
            opcodes::OP_IMM => self.execute_op_imm(inst),
            opcodes::OP_IMM_32 => self.execute_op_imm_32(inst),
            opcodes::OP => self.execute_op(inst),
            opcodes::OP_32 => self.execute_op_32(inst),
            opcodes::AMO => self.execute_amo(inst),
            opcodes::SYSTEM => self.execute_system(inst),

            // F and D.
            opcodes::LOAD_FP => self.execute_load_fp(inst),
            opcodes::STORE_FP => self.execute_store_fp(inst),
            opcodes::OP_FP => self.execute_op_fp(inst),
            opcodes::MADD | opcodes::MSUB | opcodes::NMSUB | opcodes::NMADD => {
                self.execute_fused_madd(inst)
            }

            // FENCE orders memory operations for other harts and devices. Harts run
            // one at a time and every access completes before the next instruction,
            // so the ordering FENCE asks for already holds and it is a no-op.
            // FENCE.I is the instruction-cache variant: the guest is saying "I have
            // modified code, make the next fetch see it", so cached decodes go.
            opcodes::MISC_MEM => {
                if inst.funct3 == 1 {
                    self.flush_decode_cache();
                }
                Ok(())
            }

            // Anything not implemented traps as an illegal instruction with the
            // offending bits in tval, rather than being skipped.
            _ => Err(illegal(inst)),
        }
    }

    // --- control transfer -----------------------------------------------------------

    fn execute_jal(&mut self, inst: &DecodedInst) -> Res<()> {
        let target = self.pc.wrapping_add(inst.imm as u64);
        // The link address is the instruction *after* this one, which with the C
        // extension is not always four bytes on: a compiler expects C.JALR to link
        // pc + 2, and linking pc + 4 makes the return skip whatever followed the call.
        let link = self.pc + inst.length;
        // Compute and check the target *before* writing the link register, so a
        // misaligned jump leaves the machine untouched.
        self.set_branch_target(target)?;
        self.write_reg(inst.rd, link);
        Ok(())
    }

    fn execute_jalr(&mut self, inst: &DecodedInst) -> Res<()> {
        if inst.funct3 != 0 {
            return Err(illegal(inst));
        }
        // The low bit of the computed target is cleared unconditionally - not an
        // alignment check but defined behaviour, so `jalr rd, rs1, 1` is a legal way
        // to reach an even address. Add, *then* clear.
        let rs1 = self.read_reg(inst.rs1);
        let target = self.alu(AluOp::Add, rs1, inst.imm as u64) & !1u64;
        let link = self.pc + inst.length;
        self.set_branch_target(target)?;
        // rd is written after the target is computed: rd and rs1 may be the same
        // register (`jalr ra, ra, 0` is a real idiom).
        self.write_reg(inst.rd, link);
        Ok(())
    }

    fn execute_branch(&mut self, inst: &DecodedInst) -> Res<()> {
        let a = self.read_reg(inst.rs1);
        let b = self.read_reg(inst.rs2);
        let taken = match inst.funct3 {
            0x0 => self.alu(AluOp::Xor, a, b) == 0,  // BEQ
            0x1 => self.alu(AluOp::Xor, a, b) != 0,  // BNE
            0x4 => self.alu(AluOp::Slt, a, b) != 0,  // BLT
            0x5 => self.alu(AluOp::Slt, a, b) == 0,  // BGE
            0x6 => self.alu(AluOp::Sltu, a, b) != 0, // BLTU
            0x7 => self.alu(AluOp::Sltu, a, b) == 0, // BGEU
            _ => return Err(illegal(inst)),
        };
        if !taken {
            return Ok(()); // next_pc already points past this one
        }
        self.set_branch_target(self.pc.wrapping_add(inst.imm as u64))
    }

    // --- loads and stores --------------------------------------------------------------
    //
    // The spec permits an implementation either to support misaligned accesses in
    // hardware or to raise a misaligned-address exception. We support them, matching
    // QEMU's virt machine - which is what the kernels we boot are developed against.

    fn execute_load(&mut self, inst: &DecodedInst) -> Res<()> {
        let rs1 = self.read_reg(inst.rs1);
        let addr = self.alu(AluOp::Add, rs1, inst.imm as u64);
        let (size, signed) = match inst.funct3 {
            0x0 => (1, true),  // LB
            0x1 => (2, true),  // LH
            0x2 => (4, true),  // LW
            0x3 => (8, false), // LD
            0x4 => (1, false), // LBU
            0x5 => (2, false), // LHU
            0x6 => (4, false), // LWU
            _ => return Err(illegal(inst)),
        };
        let v = self.mem_load(addr, size, AccessType::Load)?;
        // The signed/unsigned distinction is the whole reason LW and LWU are separate
        // instructions on RV64: both read four bytes, but LW sign-extends to 64 bits
        // and LWU zero-extends.
        let value = if signed { sign_extend(v, size as u32 * 8) as u64 } else { v };
        // Only commit to rd once the access has succeeded: a faulting load must leave
        // the destination register untouched.
        self.write_reg(inst.rd, value);
        Ok(())
    }

    fn execute_store(&mut self, inst: &DecodedInst) -> Res<()> {
        let rs1 = self.read_reg(inst.rs1);
        let addr = self.alu(AluOp::Add, rs1, inst.imm as u64);
        let value = self.read_reg(inst.rs2);
        let size = match inst.funct3 {
            0x0 => 1,
            0x1 => 2,
            0x2 => 4,
            0x3 => 8,
            _ => return Err(illegal(inst)),
        };
        // Stores have no sign/unsigned variants: they write the low `size` bytes of
        // the register and the rest is simply discarded.
        self.mem_store(addr, size, value)
    }

    // --- register-immediate ALU ------------------------------------------------------------

    fn execute_op_imm(&mut self, inst: &DecodedInst) -> Res<()> {
        // Zb only claims shift-slot encodings; ADDI, ANDI and friends skip the probe.
        if matches!(inst.funct3, 1 | 5) {
            if let Some(r) = self.execute_zb_op_imm(inst) {
                return r;
            }
        }
        let rs1 = self.read_reg(inst.rs1);
        let imm = inst.imm as u64; // already sign-extended to 64
        let result = match inst.funct3 {
            0x0 => self.alu(AluOp::Add, rs1, imm), // ADDI (wrapping; never traps)
            0x1 => {
                // SLLI: on RV64 the shift amount is 6 bits; the upper 6 bits must be
                // zero, any other encoding is reserved.
                if inst.funct6() != 0x00 {
                    return Err(illegal(inst));
                }
                self.alu(AluOp::Sll, rs1, inst.shamt6() as u64)
            }
            0x2 => self.alu(AluOp::Slt, rs1, imm), // SLTI
            // SLTIU: the immediate is still *sign-extended* to 64 bits first and only
            // then compared as unsigned, so `sltiu rd, rs1, -1` compares against
            // 0xFFFF_FFFF_FFFF_FFFF (the idiom for "is rs1 != max").
            0x3 => self.alu(AluOp::Sltu, rs1, imm),
            0x4 => self.alu(AluOp::Xor, rs1, imm),
            0x5 => {
                let shamt = inst.shamt6() as u64;
                match inst.funct6() {
                    0x00 => self.alu(AluOp::Srl, rs1, shamt), // SRLI
                    0x10 => self.alu(AluOp::Sra, rs1, shamt), // SRAI
                    _ => return Err(illegal(inst)),
                }
            }
            0x6 => self.alu(AluOp::Or, rs1, imm),
            0x7 => self.alu(AluOp::And, rs1, imm),
            _ => return Err(illegal(inst)),
        };
        self.write_reg(inst.rd, result);
        Ok(())
    }

    // The "*W" immediate forms: compute on 32 bits, sign-extend to 64. The shift
    // amount is 5 bits, so these select on funct7 rather than funct6.
    fn execute_op_imm_32(&mut self, inst: &DecodedInst) -> Res<()> {
        if matches!(inst.funct3, 1 | 5) {
            if let Some(r) = self.execute_zb_op_imm_32(inst) {
                return r;
            }
        }
        let rs1 = self.read_reg(inst.rs1);
        let result = match inst.funct3 {
            0x0 => self.alu(AluOp::AddW, rs1, inst.imm as u64), // ADDIW
            0x1 => {
                if inst.funct7 != 0x00 {
                    return Err(illegal(inst));
                }
                self.alu(AluOp::SllW, rs1, inst.shamt5() as u64) // SLLIW
            }
            0x5 => {
                let shamt = inst.shamt5() as u64;
                match inst.funct7 {
                    0x00 => self.alu(AluOp::SrlW, rs1, shamt), // SRLIW
                    0x20 => self.alu(AluOp::SraW, rs1, shamt), // SRAIW
                    _ => return Err(illegal(inst)),
                }
            }
            _ => return Err(illegal(inst)),
        };
        self.write_reg(inst.rd, result);
        Ok(())
    }

    // --- register-register ALU --------------------------------------------------------------

    fn execute_op(&mut self, inst: &DecodedInst) -> Res<()> {
        // funct7 == 0x01 selects the M extension, which shares this opcode with the
        // base ALU operations and differs only in funct7.
        if inst.funct7 == 0x01 {
            return self.execute_mul_div(inst);
        }
        // Every Zb/Zicond encoding has a non-zero funct7; plain ADD/SLL/... do not.
        if inst.funct7 != 0x00 {
            if let Some(r) = self.execute_zb_op(inst) {
                return r;
            }
        }
        if inst.funct7 != 0x00 && inst.funct7 != 0x20 {
            return Err(illegal(inst));
        }
        let a = self.read_reg(inst.rs1);
        let b = self.read_reg(inst.rs2);
        let sub = inst.funct7 == 0x20;

        let op = match inst.funct3 {
            0x0 => {
                if sub {
                    AluOp::Sub
                } else {
                    AluOp::Add
                }
            }
            0x1 | 0x2 | 0x3 | 0x4 | 0x6 | 0x7 if inst.funct7 != 0x00 => return Err(illegal(inst)),
            0x1 => AluOp::Sll,
            0x2 => AluOp::Slt,
            0x3 => AluOp::Sltu,
            0x4 => AluOp::Xor,
            0x5 => {
                if sub {
                    AluOp::Sra
                } else {
                    AluOp::Srl
                }
            }
            0x6 => AluOp::Or,
            0x7 => AluOp::And,
            _ => return Err(illegal(inst)),
        };
        let r = self.alu(op, a, b);
        self.write_reg(inst.rd, r);
        Ok(())
    }

    fn execute_op_32(&mut self, inst: &DecodedInst) -> Res<()> {
        if inst.funct7 == 0x01 {
            return self.execute_mul_div_32(inst);
        }
        if inst.funct7 != 0x00 {
            if let Some(r) = self.execute_zb_op_32(inst) {
                return r;
            }
        }
        if inst.funct7 != 0x00 && inst.funct7 != 0x20 {
            return Err(illegal(inst));
        }
        // The ALU takes the full operands: it uses the low 32 bits, and for the shifts
        // only the low 5 bits of rs2 as the shift amount.
        let a = self.read_reg(inst.rs1);
        let b = self.read_reg(inst.rs2);
        let sub = inst.funct7 == 0x20;
        let op = match inst.funct3 {
            0x0 => {
                if sub {
                    AluOp::SubW
                } else {
                    AluOp::AddW
                }
            }
            0x1 => {
                if inst.funct7 != 0x00 {
                    return Err(illegal(inst));
                }
                AluOp::SllW
            }
            0x5 => {
                if sub {
                    AluOp::SraW
                } else {
                    AluOp::SrlW
                }
            }
            _ => return Err(illegal(inst)),
        };
        let r = self.alu(op, a, b);
        self.write_reg(inst.rd, r);
        Ok(())
    }

    // --- M extension -------------------------------------------------------------------------
    //
    // The striking thing about RISC-V division is that **it never traps**. Divide by
    // zero and signed overflow both produce specific, defined values:
    //
    //   DIV  by zero -> -1        DIVU by zero -> all ones (2^64 - 1)
    //   REM  by zero -> dividend  REMU by zero -> dividend
    //   DIV(INT64_MIN, -1) -> INT64_MIN     REM(INT64_MIN, -1) -> 0

    fn execute_mul_div(&mut self, inst: &DecodedInst) -> Res<()> {
        let a = self.read_reg(inst.rs1);
        let b = self.read_reg(inst.rs2);
        let sa = a as i64;
        let sb = b as i64;
        let r = match inst.funct3 {
            0x0 => a.wrapping_mul(b), // MUL: low 64 bits; signedness does not affect them
            // The MULH family returns the *upper* 64 bits of the 128-bit product.
            0x1 => (((sa as i128) * (sb as i128)) >> 64) as u64, // MULH: signed x signed
            // MULHSU: signed x unsigned. Exists to make multi-word arithmetic work:
            // the limbs of a bignum are unsigned, but the top one carries the sign.
            0x2 => (((sa as i128) * (b as u128 as i128)) >> 64) as u64,
            0x3 => (((a as u128) * (b as u128)) >> 64) as u64, // MULHU
            0x4 => {
                // DIV
                if sb == 0 {
                    !0u64
                } else if sa == i64::MIN && sb == -1 {
                    sa as u64 // overflow: wraps
                } else {
                    (sa / sb) as u64
                }
            }
            0x5 => {
                // DIVU
                if b == 0 {
                    !0u64
                } else {
                    a / b
                }
            }
            0x6 => {
                // REM
                if sb == 0 {
                    a
                } else if sa == i64::MIN && sb == -1 {
                    0
                } else {
                    (sa % sb) as u64
                }
            }
            _ => {
                // REMU
                if b == 0 {
                    a
                } else {
                    a % b
                }
            }
        };
        self.write_reg(inst.rd, r);
        Ok(())
    }

    fn execute_mul_div_32(&mut self, inst: &DecodedInst) -> Res<()> {
        let a = self.read_reg(inst.rs1) as u32;
        let b = self.read_reg(inst.rs2) as u32;
        let sa = a as i32;
        let sb = b as i32;
        let r: u32 = match inst.funct3 {
            0x0 => a.wrapping_mul(b), // MULW
            0x4 => {
                // DIVW
                if sb == 0 {
                    !0u32
                } else if sa == i32::MIN && sb == -1 {
                    sa as u32
                } else {
                    (sa / sb) as u32
                }
            }
            0x5 => {
                // DIVUW
                if b == 0 {
                    !0u32
                } else {
                    a / b
                }
            }
            0x6 => {
                // REMW
                if sb == 0 {
                    a
                } else if sa == i32::MIN && sb == -1 {
                    0
                } else {
                    (sa % sb) as u32
                }
            }
            0x7 => {
                // REMUW
                if b == 0 {
                    a
                } else {
                    a % b
                }
            }
            _ => return Err(illegal(inst)),
        };
        self.write_reg(inst.rd, sext32(r));
        Ok(())
    }

    // --- A extension ----------------------------------------------------------------------------

    fn execute_amo(&mut self, inst: &DecodedInst) -> Res<()> {
        let size = match inst.funct3 {
            0x2 => 4,
            0x3 => 8,
            _ => return Err(illegal(inst)),
        };
        // Bits 31:27 select the operation; bits 26 and 25 are the aq/rl ordering
        // hints, which an in-order machine can ignore.
        let funct5 = (inst.funct7 >> 2) & 0x1f;
        let addr = self.read_reg(inst.rs1);

        if addr % size as u64 != 0 {
            // The cause depends on what the instruction does. LR is a load and must
            // report LoadAddressMisaligned; SC and the read-modify-write AMOs both
            // store, so they report StoreAMOAddressMisaligned.
            let cause = if funct5 == 0x02 {
                Exception::LoadAddressMisaligned
            } else {
                Exception::StoreAMOAddressMisaligned
            };
            return Err(Trap::new(cause, addr));
        }

        if funct5 == 0x02 {
            // LR
            if inst.rs2 != 0 {
                return Err(illegal(inst));
            }
            let v = self.mem_load(addr, size, AccessType::Load)?;
            self.reservation_valid = true;
            self.reservation_addr = addr;
            // The load just succeeded, so this translation is a TLB hit and cannot
            // fault. It is only needed to compare against other harts' stores.
            let p = self.data_privilege();
            let pa = self
                .mmu
                .translate(&self.bus, &self.csrs, addr, AccessType::Load, p)
                .unwrap_or(addr);
            self.mb.reservation.set(pa >> 3);
            let r = if size == 4 { sext32(v as u32) } else { v };
            self.write_reg(inst.rd, r);
            return Ok(());
        }

        if funct5 == 0x03 {
            // SC
            let holds = self.reservation_valid
                && self.reservation_addr == addr
                && self.mb.reservation.get() != NO_RES;
            // The reservation is consumed either way. Leaving it set after a failed SC
            // would let a later SC succeed against a stale reservation.
            self.reservation_valid = false;
            let src = self.read_reg(inst.rs2);
            if holds {
                self.mem_store(addr, size, src)?;
                self.mb.reservation.set(NO_RES);
                self.write_reg(inst.rd, 0); // 0 means the store happened
            } else {
                self.mb.reservation.set(NO_RES);
                self.write_reg(inst.rd, 1); // any non-zero means it did not
            }
            return Ok(());
        }

        // --- the read-modify-write AMOs ---
        let orig = self.mem_load(addr, size, AccessType::Load)?;
        // Read rs2 before writing rd: they may be the same register.
        let src = self.read_reg(inst.rs2);

        if size == 4 {
            let (ua, ub) = (orig as u32, src as u32);
            let (sa, sb) = (ua as i32, ub as i32);
            let result: u32 = match funct5 {
                0x00 => ua.wrapping_add(ub), // AMOADD
                0x01 => ub,                  // AMOSWAP
                0x04 => ua ^ ub,             // AMOXOR
                0x08 => ua | ub,             // AMOOR
                0x0c => ua & ub,             // AMOAND
                0x10 => sa.min(sb) as u32,   // AMOMIN
                0x14 => sa.max(sb) as u32,   // AMOMAX
                0x18 => ua.min(ub),          // AMOMINU
                0x1c => ua.max(ub),          // AMOMAXU
                _ => return Err(illegal(inst)),
            };
            self.mem_store(addr, 4, result as u64)?;
            self.write_reg(inst.rd, sext32(ua));
        } else {
            let (sa, sb) = (orig as i64, src as i64);
            let result: u64 = match funct5 {
                0x00 => orig.wrapping_add(src),
                0x01 => src,
                0x04 => orig ^ src,
                0x08 => orig | src,
                0x0c => orig & src,
                0x10 => sa.min(sb) as u64,
                0x14 => sa.max(sb) as u64,
                0x18 => orig.min(src),
                0x1c => orig.max(src),
                _ => return Err(illegal(inst)),
            };
            self.mem_store(addr, 8, result)?;
            self.write_reg(inst.rd, orig);
        }
        Ok(())
    }

    // --- SYSTEM -----------------------------------------------------------------------------------
    //
    // ECALL and EBREAK are the base RV64I members; the CSR instructions share this
    // opcode and are told apart by funct3 != 0. Both ECALL and EBREAK "fail" by
    // design: they raise a trap, which is how they do their job.

    fn execute_system(&mut self, inst: &DecodedInst) -> Res<()> {
        if inst.funct3 != 0 {
            return self.execute_csr(inst);
        }
        match inst.imm & 0xfff {
            0x000 => {
                // ECALL
                if inst.rd != 0 || inst.rs1 != 0 {
                    return Err(illegal(inst));
                }
                // The cause encodes which privilege level made the call, so that a
                // machine-mode handler can tell a kernel's SBI call from a user
                // program's system call.
                match self.priv_ {
                    PRIV_USER => Err(Trap::new(Exception::ECallFromUMode, 0)),
                    PRIV_SUPERVISOR => {
                        // With SBI enabled, an ecall from supervisor mode is a call
                        // into firmware, not an exception: the emulator answers it and
                        // execution continues after the ecall.
                        if self.sbi_enabled && crate::sbi::handle_ecall(self) {
                            return Ok(());
                        }
                        Err(Trap::new(Exception::ECallFromSMode, 0))
                    }
                    _ => Err(Trap::new(Exception::ECallFromMMode, 0)),
                }
            }
            0x001 => {
                // EBREAK
                if inst.rd != 0 || inst.rs1 != 0 {
                    return Err(illegal(inst));
                }
                Err(Trap::new(Exception::Breakpoint, self.pc))
            }
            0x102 => self.execute_sret(inst),
            0x302 => self.execute_mret(inst),
            0x105 => {
                // WFI. The spec permits implementing it as a no-op: it is a hint, and
                // software must re-check what it was waiting on. A multi-hart platform
                // turns on idle_on_wfi, because there it pays: a sleeping core is
                // skipped, and when every core sleeps the clock jumps to the next
                // deadline instead of retiring millions of empty loop iterations.
                if inst.rd != 0 || inst.rs1 != 0 {
                    return Err(illegal(inst));
                }
                if self.idle_on_wfi {
                    self.waiting_for_interrupt = true;
                }
                Ok(())
            }
            _ => {
                // SFENCE.VMA shares funct3 == 0 but is an R-type: funct7 == 0x09.
                if inst.funct7 == 0x09 {
                    return self.execute_sfence_vma(inst);
                }
                Err(illegal(inst))
            }
        }
    }

    // MRET exactly undoes what enter_trap() did:
    //   MIE <- MPIE; MPIE <- 1; privilege <- MPP; MPP <- least privileged; pc <- mepc
    fn execute_mret(&mut self, inst: &DecodedInst) -> Res<()> {
        if inst.rd != 0 || inst.rs1 != 0 || self.priv_ < PRIV_MACHINE {
            return Err(illegal(inst));
        }
        let mut status = self.csrs.mstatus();
        let mpie = status & csr::MSTATUS_MPIE != 0;
        status = if mpie { status | csr::MSTATUS_MIE } else { status & !csr::MSTATUS_MIE };
        // MPIE is set, not cleared. The spec is specific about this.
        status |= csr::MSTATUS_MPIE;

        let return_priv = ((status & csr::MSTATUS_MPP) >> csr::MSTATUS_MPP_SHIFT) as u32;
        status = (status & !csr::MSTATUS_MPP) | ((PRIV_LEAST_SUPPORTED as u64) << csr::MSTATUS_MPP_SHIFT);
        // Returning below machine mode clears MPRV; leaving it set would let the
        // supervisor we return into keep performing accesses at MPP's privilege.
        if return_priv != PRIV_MACHINE {
            status &= !csr::MSTATUS_MPRV;
        }
        self.csrs.set_mstatus(status);
        self.priv_ = return_priv;
        self.next_pc = self.csrs.read(csr::MEPC);
        Ok(())
    }

    // SRET: the supervisor counterpart of MRET, using SPP/SPIE.
    fn execute_sret(&mut self, inst: &DecodedInst) -> Res<()> {
        if inst.rd != 0 || inst.rs1 != 0 || self.priv_ < PRIV_SUPERVISOR {
            return Err(illegal(inst));
        }
        // mstatus.TSR ("trap SRET") lets machine-mode firmware intercept a supervisor's
        // returns - used by hypervisors.
        if self.priv_ == PRIV_SUPERVISOR && self.csrs.mstatus() & csr::MSTATUS_TSR != 0 {
            return Err(illegal(inst));
        }
        let mut status = self.csrs.mstatus();
        let spie = status & csr::MSTATUS_SPIE != 0;
        status = if spie { status | csr::MSTATUS_SIE } else { status & !csr::MSTATUS_SIE };
        status |= csr::MSTATUS_SPIE;

        // SPP is one bit: supervisor or user.
        let return_priv = if status & csr::MSTATUS_SPP != 0 { PRIV_SUPERVISOR } else { PRIV_USER };
        status &= !csr::MSTATUS_SPP; // reset to user, the least-privileged mode
        if return_priv != PRIV_MACHINE {
            status &= !csr::MSTATUS_MPRV;
        }
        self.csrs.set_mstatus(status);
        self.priv_ = return_priv;
        self.next_pc = self.csrs.read(csr::SEPC);
        Ok(())
    }

    // SFENCE.VMA: tell the hardware that page tables changed. Needed because the TLB
    // caches translations, and nothing about writing a PTE tells the hardware to
    // forget what it cached. rs1 names a virtual address and rs2 an ASID, allowing a
    // narrower flush; we flush everything, which is always correct.
    fn execute_sfence_vma(&mut self, inst: &DecodedInst) -> Res<()> {
        if inst.rd != 0 || self.priv_ < PRIV_SUPERVISOR {
            return Err(illegal(inst));
        }
        // mstatus.TVM ("trap virtual memory") intercepts a supervisor's page-table
        // management, again for virtualisation.
        if self.priv_ == PRIV_SUPERVISOR && self.csrs.mstatus() & csr::MSTATUS_TVM != 0 {
            return Err(illegal(inst));
        }
        self.mmu.flush();
        self.flush_decode_cache();
        Ok(())
    }

    // --- CSR access -------------------------------------------------------------------------------
    //
    // Three ways a CSR access can be illegal, all reported as IllegalInstruction: the
    // CSR is not implemented; the instruction writes a read-only CSR; or the current
    // privilege is below the CSR's minimum. The first is not a limitation but a
    // feature: probing a CSR and catching the trap is how software detects optional
    // extensions.

    /// mstatus.TVM traps a supervisor's access to satp as well as SFENCE.VMA.
    fn tvm_blocks(&self, addr: u32) -> bool {
        addr == csr::SATP && self.priv_ == PRIV_SUPERVISOR && self.csrs.mstatus() & csr::MSTATUS_TVM != 0
    }

    pub(crate) fn csr_read(&mut self, addr: u32) -> Res<u64> {
        let bad = || Trap::new(Exception::IllegalInstruction, 0);
        if !self.csrs.exists(addr) {
            return Err(bad());
        }
        // The floating-point CSRs are part of the state mstatus.FS protects, so they
        // are unreachable while the unit is off: fcsr is as much floating-point state
        // as the registers are.
        if (addr == csr::FCSR || addr == csr::FFLAGS || addr == csr::FRM) && !self.csrs.fpu_enabled() {
            return Err(bad());
        }
        if self.priv_ < csr::min_privilege(addr) || self.tvm_blocks(addr) {
            return Err(bad());
        }
        // The unprivileged counters are readable from a lower privilege level only
        // when the level above has enabled them - how a kernel denies user code a
        // high-resolution clock.
        if addr == csr::CYCLE && !self.csrs.counter_enabled(0, self.priv_) {
            return Err(bad());
        }
        if addr == csr::TIME && !self.csrs.counter_enabled(1, self.priv_) {
            return Err(bad());
        }
        if addr == csr::INSTRET && !self.csrs.counter_enabled(2, self.priv_) {
            return Err(bad());
        }
        // `time` is not a counter the hart keeps: it is a window onto the CLINT's
        // mtime, which is the machine-wide clock every hart shares.
        if addr == csr::TIME {
            if let Some(c) = &self.clock {
                return Ok(c.now());
            }
        }
        Ok(self.csrs.read(addr))
    }

    pub(crate) fn csr_write(&mut self, addr: u32, value: u64) -> Res<()> {
        let bad = || Trap::new(Exception::IllegalInstruction, 0);
        if !self.csrs.exists(addr) {
            return Err(bad());
        }
        // A write to an fp CSR also makes the unit Dirty - fcsr is state a context
        // switch has to save, exactly like the registers.
        if addr == csr::FCSR || addr == csr::FFLAGS || addr == csr::FRM {
            if !self.csrs.fpu_enabled() {
                return Err(bad());
            }
            self.csrs.mark_fpu_dirty();
        }
        if csr::is_read_only(addr) || self.priv_ < csr::min_privilege(addr) || self.tvm_blocks(addr) {
            return Err(bad());
        }
        if addr == csr::MINSTRET || addr == csr::MCYCLE {
            self.counter_written = true;
        }
        self.csrs.write(addr, value);
        // Changing satp changes the entire address space, so every cached translation
        // is now potentially wrong.
        if addr == csr::SATP {
            self.mmu.flush();
            self.flush_decode_cache();
        }
        Ok(())
    }

    // The six CSR instructions:
    //   CSRRW  rd, csr, rs1     rd = csr; csr = rs1
    //   CSRRS  rd, csr, rs1     rd = csr; csr |= rs1      (set bits)
    //   CSRRC  rd, csr, rs1     rd = csr; csr &= ~rs1     (clear bits)
    //   CSRRWI/CSRRSI/CSRRCI    same, with a 5-bit zero-extended immediate
    //
    // The subtlety is when the access is *suppressed*: CSRRW/CSRRWI with rd == x0 must
    // not READ the CSR, and CSRRS/CSRRC with a zero source must not WRITE. Some CSRs
    // have read or write side effects, and performing a suppressed access would
    // silently consume events. It also means `csrr rd, csr` (CSRRS with rs1 = x0) is a
    // pure read that can legally target a read-only CSR.
    fn execute_csr(&mut self, inst: &DecodedInst) -> Res<()> {
        let addr = (inst.imm & 0xfff) as u32;
        // For the immediate forms the rs1 field is a 5-bit unsigned value.
        let immediate_form = inst.funct3 & 0x4 != 0;
        let src = if immediate_form { inst.rs1 as u64 } else { self.read_reg(inst.rs1) };

        match inst.funct3 & 0x3 {
            0x1 => {
                // CSRRW / CSRRWI - swap
                let mut old = 0;
                if inst.rd != 0 {
                    old = self.csr_read(addr)?;
                } else if !self.csrs.exists(addr) || self.priv_ < csr::min_privilege(addr) {
                    // rd == x0: the read is suppressed, but the write still has to pass
                    // its access checks.
                    return Err(Trap::new(Exception::IllegalInstruction, 0));
                }
                self.csr_write(addr, src)?;
                self.write_reg(inst.rd, old);
                Ok(())
            }
            0x2 | 0x3 => {
                // CSRRS / CSRRC
                let old = self.csr_read(addr)?;
                // A zero source means "read only": no write is attempted at all, so
                // this is legal even on a read-only CSR.
                if src != 0 {
                    let next = if inst.funct3 & 0x3 == 0x2 { old | src } else { old & !src };
                    self.csr_write(addr, next)?;
                }
                self.write_reg(inst.rd, old);
                Ok(())
            }
            _ => Err(illegal(inst)),
        }
    }

    // --- Zba, Zbb, Zbc, Zbs, Zicond ----------------------------------------------------------------
    //
    // None of these adds an opcode. Each is a set of new (funct7, funct3) or
    // (funct12, funct3) combinations inside ALU opcodes the base ISA already uses, so
    // they are tried as a pre-check in the four dispatchers: an encoding that belongs
    // to them is executed here (Some), and anything else falls through exactly as
    // before (None) - which keeps the "unrecognised encodings must trap" rule intact.

    fn execute_zb_op(&mut self, inst: &DecodedInst) -> Option<Res<()>> {
        let a = self.read_reg(inst.rs1);
        let b = self.read_reg(inst.rs2);
        let key = (inst.funct7 << 3) | inst.funct3;
        let bit = |i: u64| 1u64 << (i & 63);
        let r = match key {
            // Zba: shift-and-add, the address calculation for an array of 2-, 4- or
            // 8-byte elements in one instruction.
            k if k == (0x10 << 3) | 2 => (a << 1).wrapping_add(b), // SH1ADD
            k if k == (0x10 << 3) | 4 => (a << 2).wrapping_add(b), // SH2ADD
            k if k == (0x10 << 3) | 6 => (a << 3).wrapping_add(b), // SH3ADD
            // Zbb: logic with a negated operand.
            k if k == (0x20 << 3) | 7 => a & !b, // ANDN
            k if k == (0x20 << 3) | 6 => a | !b, // ORN
            k if k == (0x20 << 3) | 4 => !(a ^ b), // XNOR
            // Zbb: minimum and maximum, signed and unsigned.
            k if k == (0x05 << 3) | 4 => if (a as i64) < (b as i64) { a } else { b }, // MIN
            k if k == (0x05 << 3) | 5 => a.min(b),                                      // MINU
            k if k == (0x05 << 3) | 6 => if (a as i64) > (b as i64) { a } else { b }, // MAX
            k if k == (0x05 << 3) | 7 => a.max(b),                                      // MAXU
            // Zbb: rotates. The amount is rs2 modulo 64.
            k if k == (0x30 << 3) | 1 => bm::rol64(a, b as u32), // ROL
            k if k == (0x30 << 3) | 5 => bm::ror64(a, b as u32), // ROR
            // Zbs: single-bit operations. The bit index is rs2 modulo 64.
            k if k == (0x24 << 3) | 1 => a & !bit(b),       // BCLR
            k if k == (0x24 << 3) | 5 => (a >> (b & 63)) & 1, // BEXT
            k if k == (0x34 << 3) | 1 => a ^ bit(b),         // BINV
            k if k == (0x14 << 3) | 1 => a | bit(b),         // BSET
            // Zbc: carry-less multiply.
            k if k == (0x05 << 3) | 1 => bm::clmul(a, b),  // CLMUL
            k if k == (0x05 << 3) | 2 => bm::clmulr(a, b), // CLMULR
            k if k == (0x05 << 3) | 3 => bm::clmulh(a, b), // CLMULH
            // Zicond: conditional zero. These build a branch-free select:
            //   rd = cond ? x : y   is   czero.nez(y, cond) | czero.eqz(x, cond)
            k if k == (0x07 << 3) | 5 => if b == 0 { 0 } else { a }, // CZERO.EQZ
            k if k == (0x07 << 3) | 7 => if b != 0 { 0 } else { a }, // CZERO.NEZ
            _ => return None,
        };
        self.write_reg(inst.rd, r);
        Some(Ok(()))
    }

    fn execute_zb_op_32(&mut self, inst: &DecodedInst) -> Option<Res<()>> {
        let a = self.read_reg(inst.rs1);
        let b = self.read_reg(inst.rs2);
        let key = (inst.funct7 << 3) | inst.funct3;
        let r = match key {
            // Zba: the .uw forms zero-extend the low 32 bits of rs1 first, so an index
            // held in a 32-bit unsigned variable can be scaled and added in one go.
            k if k == (0x04 << 3) => bm::zext32(a).wrapping_add(b), // ADD.UW
            k if k == (0x10 << 3) | 2 => (bm::zext32(a) << 1).wrapping_add(b),
            k if k == (0x10 << 3) | 4 => (bm::zext32(a) << 2).wrapping_add(b),
            k if k == (0x10 << 3) | 6 => (bm::zext32(a) << 3).wrapping_add(b),
            // Zbb: ZEXT.H lives in OP-32 on RV64 and is a special case (rs2 = 0) of an
            // encoding that, with other rs2 values, is Zbkb's PACK - not implemented.
            k if k == (0x04 << 3) | 4 => {
                if inst.rs2 != 0 {
                    return Some(Err(illegal(inst)));
                }
                a & 0xffff
            }
            // Zbb: 32-bit rotates, result sign-extended like every *W instruction.
            k if k == (0x30 << 3) | 1 => bm::sext(bm::rol32(a as u32, b as u32) as u64, 32),
            k if k == (0x30 << 3) | 5 => bm::sext(bm::ror32(a as u32, b as u32) as u64, 32),
            _ => return None,
        };
        self.write_reg(inst.rd, r);
        Some(Ok(()))
    }

    fn execute_zb_op_imm(&mut self, inst: &DecodedInst) -> Option<Res<()>> {
        let a = self.read_reg(inst.rs1);
        let imm12 = (inst.raw >> 20) & 0xfff; // the whole field, for the unary operations
        let f6 = inst.funct6(); // bits 31:26, for the shift-like forms
        let shamt = inst.shamt6() as u64;
        let bit = 1u64 << shamt;
        let r = if inst.funct3 == 1 {
            match imm12 {
                0x600 => bm::clz64(a),
                0x601 => bm::ctz64(a),
                0x602 => bm::cpop64(a),
                0x604 => bm::sext(a, 8),
                0x605 => bm::sext(a, 16),
                _ => match f6 {
                    0x0a => a | bit,  // BSETI
                    0x12 => a & !bit, // BCLRI
                    0x1a => a ^ bit,  // BINVI
                    _ => return None,
                },
            }
        } else if inst.funct3 == 5 {
            match imm12 {
                0x6b8 => bm::rev8(a),
                0x287 => bm::orc_b(a),
                _ => match f6 {
                    0x18 => bm::ror64(a, shamt as u32), // RORI
                    0x12 => (a >> shamt) & 1,           // BEXTI
                    _ => return None,
                },
            }
        } else {
            return None;
        };
        self.write_reg(inst.rd, r);
        Some(Ok(()))
    }

    fn execute_zb_op_imm_32(&mut self, inst: &DecodedInst) -> Option<Res<()>> {
        let a = self.read_reg(inst.rs1);
        let imm12 = (inst.raw >> 20) & 0xfff;
        let r = if inst.funct3 == 1 {
            match imm12 {
                0x600 => bm::clz32(a),
                0x601 => bm::ctz32(a),
                0x602 => bm::cpop32(a),
                // SLLI.UW: zero-extend the low 32 bits, then shift by a *6-bit* amount
                // - unlike SLLIW, whose amount is 5 bits, because the result is a full
                // 64-bit value that can usefully be shifted that far.
                _ if inst.funct6() == 0x02 => bm::zext32(a) << inst.shamt6(),
                _ => return None,
            }
        } else if inst.funct3 == 5 && inst.funct7 == 0x30 {
            bm::sext(bm::ror32(a as u32, inst.shamt5()) as u64, 32) // RORIW
        } else {
            return None;
        };
        self.write_reg(inst.rd, r);
        Some(Ok(()))
    }
}
