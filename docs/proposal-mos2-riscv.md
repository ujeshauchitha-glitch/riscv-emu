# Project proposal: a 64-bit RISC-V architecture study and device demonstrator on MoS2

Draft for discussion with a supervising professor. Figures marked *(estimate)* are my own
order-of-magnitude numbers; items marked *(verify)* are cited from memory and must be checked
against the paper before submission.

## 1. Goal

Design and evaluate how a 64-bit RISC-V (RV64) processor could be built from monolayer MoS2
(three atoms thick: S-Mo-S), using a 2.5D package (MoS2 logic die plus silicon memory and I/O
chiplets), and demonstrate the key device layer in hardware at small scale.

Concretely:
1. **System level (software, done and ongoing):** a cycle-accurate-enough RV64GC model that boots
   real operating systems, used to count the instructions, gates and transistors a 2D implementation
   needs and to predict performance from measured device data.
2. **Device level (needs lab access):** fabricate and characterise MoS2 field-effect transistors,
   then an inverter / NAND gate / 1-bit adder.
3. **Integration level (design study):** a 2.5D partition that keeps timing-critical logic on one die
   and uses the interposer only for memory and I/O.

## 2. Prior art (what exists, and where this differs)

| Work | What it showed | Limitation |
|---|---|---|
| Fudan University (Zhou Peng group), *A RISC-V 32-bit microprocessor based on two-dimensional semiconductors*, Nature 640, 2025 ([record](https://ideas.repec.org/a/nat/nature/v640y2025i8059d10.1038_s41586-025-08759-9.html), [press summary](https://semiconductor-today.com/news_items/2025/apr/fudan-170425.shtml)) | "WUJI": 5,900 MoS2 transistors, 25-cell standard-cell library, runs RV32E-class instructions | **n-type only (not CMOS)**, 3 um channels, 4 V supply, one bit at a time (a 32-bit operation takes 32 cycles), yield fell to about 7% for a 64-bit register of 1,152 transistors |
| Wachter et al., *A microprocessor based on a two-dimensional semiconductor*, Nature Communications 2017 *(verify)* | First MoS2 microprocessor, 115 transistors, 1-bit | Tiny; very slow |
| Radisavljevic et al., *Single-layer MoS2 transistors*, Nature Nanotechnology 2011 *(verify)* | The first monolayer MoS2 transistor | Single device |
| Wang et al., *Integrated circuits based on bilayer MoS2 transistors*, Nano Letters 2012 *(verify)* | First small MoS2 logic circuits | Few transistors |

**How this project differs**
1. **64-bit, not 32-bit**: an explicit RV64 design study (bit-serial datapath, 64 cycles per operation) with
   transistor and yield budgets, instead of RV32E.
2. **Complementary logic and 2.5D**: WUJI is n-type only. This proposal evaluates adding p-type
   (for example WSe2) and, separately, a 2.5D split by *function* (logic die versus memory/I-O chiplets)
   rather than by transistor type.
3. **Software-validated architecture**: the design is checked against a full RV64GC emulator that boots
   xv6 and passes the official riscv-tests suite (128/128), so the instruction set is known-correct
   before any gate count is claimed.
4. **Yield-aware design**: uses the published yield-versus-complexity data to size the circuit, rather than
   assuming ideal devices.

## 3. Application

- Ultra-thin, flexible or transparent computing (sensors, wearables, smart labels) where silicon is rigid.
- Low-power edge logic, because a monolayer channel has very good electrostatic control.
- Long-term: a path past silicon scaling limits, and monolithic 3D stacking on top of silicon.
- Research/education: an open, reproducible reference for 2D-material processor design.

## 4. What has been achieved so far

- A complete **RV64GC emulator written in Rust** (no third-party dependencies, about 15,000 lines
  including tests): integer, multiply/divide, atomics, single/double floating point (own IEEE-754
  soft-float unit), compressed instructions, bit-manipulation extensions, machine/supervisor/user
  modes, Sv39 virtual memory, multi-core scheduling.
- Devices: UART, timer and interrupt controllers, virtio disk/network/entropy, real-time clock,
  user-mode networking, GDB debugger support.
- **Boots xv6** to a shell on 1 and 3 cores; **passes the official riscv-tests suite 128/128**.
- About 300 automated tests, plus differential checks: 89,122 bit-manipulation vectors, 45,000
  floating-point vectors against an exact-arithmetic reference, and register-for-register agreement
  with the earlier C++ implementation over billions of simulated instructions.
- Throughput of about 85-100 million instructions per second, fast enough to run an operating system
  and to profile instruction mixes.
- Not yet done: booting Debian (planned), any hardware fabrication, any device measurements.

## 5. What remains (plan)

| Phase | Work | Needs |
|---|---|---|
| 1 (months 1-2) | Literature review; instruction-mix profiling of real workloads in the emulator; transistor/gate-count model for a bit-serial RV64 core | Reading list below; no lab |
| 2 (months 2-4) | Learn exfoliation or CVD; fabricate and measure MoS2 FETs (I-V, on/off ratio, mobility) | Supervised cleanroom, probe station, MoS2, substrates |
| 3 (months 4-6) | Build inverter / NAND / 1-bit adder; extract delay and yield | Same, plus Raman/AFM |
| 4 (months 5-7) | Timing and yield simulation of the full RV64 core from measured device data; 2.5D partition study | Simulation licences or open tools |
| 5 (month 8) | Report, comparison with WUJI, conclusions | None |

## 6. Expected performance (hypothetical, to be replaced by measurements)

| Case | Gate delay |
|---|---|
| Theoretical limit | about 0.1-1 ps |
| Optimistic engineered circuit | 1-10 ps |
| Best lab ring oscillators | 10 ps-1 ns |
| Typical lab demo (what to expect) | 1 ns-1 us |

Die-to-die latency over a silicon interposer is about 0.5-5 ns, so it only matters if MoS2 gates reach the
picosecond range.

## 7. Cost *(estimates, USD)*

**A. Student project scope (device work plus simulation, sharing the professor's lab)**

| Item | Cost |
|---|---|
| MoS2 crystals / a few CVD samples | $100-$1,000 |
| Substrates (Si/SiO2), resist, developer, metals (Ti/Au), consumables | $500-$2,000 |
| Cleanroom and tool time (often free or internal rate for a supervised student) | $0-$5,000 |
| Characterisation (Raman, AFM, probe station) via shared tools | $0-$2,000 |
| Simulation software (open source or academic licence) | $0-$1,000 |
| Computing (the existing laptop) | $0 |
| **Total** | **about $600-$11,000** |

**B. Full research-grant scope (3 years, for reference)**

| Item | Cost |
|---|---|
| Personnel (PhD student, postdoc, PI time) | $350k-$550k |
| Cleanroom and facility fees | $90k-$250k |
| Materials | $40k-$100k |
| Characterisation, design tools, packaging, travel | $80k-$210k |
| Institutional overhead (40-60%) | $220k-$600k |
| Equipment if not available (CVD furnace etc.) | $0-$300k |
| **Total** | **about $0.8M-$1.7M** |

**C. For comparison: a real RISC-V board (not MoS2)**: about $100-$250.

## 8. Reading list for the professor

Verified through search (open the links to confirm details):
1. Ao et al., *A RISC-V 32-bit microprocessor based on two-dimensional semiconductors*, Nature 640 (2025) - [bibliographic record](https://ideas.repec.org/a/nat/nature/v640y2025i8059d10.1038_s41586-025-08759-9.html), [Semiconductor Today summary](https://semiconductor-today.com/news_items/2025/apr/fudan-170425.shtml), [Fudan / Shanghai government news](https://english.shanghai.gov.cn/en-Latest-WhatsNew/20250403/974000163e7e4cad8bb0f5a08666953f.html).

From memory, to be checked before citing *(verify)*:
2. Wachter et al., *A microprocessor based on a two-dimensional semiconductor*, Nature Communications 8, 14948 (2017).
3. Radisavljevic et al., *Single-layer MoS2 transistors*, Nature Nanotechnology 6, 147 (2011).
4. Wang et al., *Integrated circuits based on bilayer MoS2 transistors*, Nano Letters 12, 4674 (2012).
5. Akinwande et al., *Two-dimensional flexible nanoelectronics*, Nature Communications 5, 5678 (2014).
6. A review on 2D-material CMOS and p-type 2D semiconductors (for example WSe2), and the wafer-scale
   CVD growth of monolayer MoS2 - search terms: "2D CMOS WSe2 MoS2 complementary inverter",
   "wafer-scale monolayer MoS2 CVD yield".
7. RISC-V instruction set manual, volumes I and II (riscv.org) - the architecture being implemented.

Search terms for further reading: "two-dimensional semiconductor integrated circuit yield", "MoS2 contact resistance
transistor scaling", "heterogeneous 2.5D integration chiplet interposer latency".

## 9. What to ask the professor for

Guidance: a 20-minute scope meeting; agreement to supervise; an introduction to a PhD student working
on 2D materials; two or three starting papers.
Access: supervised cleanroom time, probe station / parameter analyser, Raman and AFM, CVD furnace
(optional), simulation licences.
Materials: MoS2 crystals or CVD samples, Si/SiO2 substrates, resist and developer, contact metals,
sample holders, consumables.
