#!/usr/bin/env bash
#
# Build the emulator, run every test, and run the demos.
#
# Usage:  ./run-all.sh [--quick]
#           --quick   skip the clean rebuild (reuse target/)
#
# See docs/RUNNING.md for what each step does and how to read its output.

set -uo pipefail
cd "$(dirname "$0")"

QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1

if [ -t 1 ]; then
  BOLD=$'\033[1m'; GREEN=$'\033[32m'; RED=$'\033[31m'; DIM=$'\033[2m'; OFF=$'\033[0m'
else
  BOLD=""; GREEN=""; RED=""; DIM=""; OFF=""
fi

FAILURES=0
step()  { printf '\n%s== %s ==%s\n' "$BOLD" "$1" "$OFF"; }
ok()    { printf '%s  ok%s  %s\n' "$GREEN" "$OFF" "$1"; }
fail()  { printf '%s FAIL%s %s\n' "$RED" "$OFF" "$1"; FAILURES=$((FAILURES + 1)); }
note()  { printf '%s       %s%s\n' "$DIM" "$1" "$OFF"; }

command -v cargo > /dev/null || { echo "cargo not found: install Rust (https://rustup.rs)"; exit 2; }

# ---------------------------------------------------------------------------
step "Build"
[ "$QUICK" -eq 0 ] && cargo clean --quiet 2>/dev/null
if RUSTFLAGS="-D warnings" cargo build --release > /tmp/riscv-emu-build.log 2>&1; then
  ok "built cleanly, no warnings"
else
  fail "build failed; see /tmp/riscv-emu-build.log"
  tail -20 /tmp/riscv-emu-build.log
  exit 1
fi
EMU=target/release/riscv_emu
[ -x "$EMU" ] || EMU="$EMU.exe"

# ---------------------------------------------------------------------------
step "Tests (unit, differential, CLI, bare-metal self-tests)"

HAVE_TOOLCHAIN=0
for p in riscv64-unknown-elf- riscv64-elf- riscv-none-elf- riscv64-linux-gnu-; do
  command -v "${p}as" > /dev/null && HAVE_TOOLCHAIN=1
done
[ "$HAVE_TOOLCHAIN" -eq 0 ] && note "no RISC-V toolchain: the bare-metal self-tests will be skipped"

if cargo test --release > /tmp/riscv-emu-test.log 2>&1; then
  ok "$(grep -E '^test result' /tmp/riscv-emu-test.log | awk '{p+=$4; f+=$6} END {print p " passed, " f " failed"}')"
else
  fail "some tests failed"
  grep -E "FAILED|panicked" /tmp/riscv-emu-test.log | head -20
fi

# ---------------------------------------------------------------------------
step "Built-in demo"

DEMO_OUT=$("$EMU" 2>&1)
if printf '%s' "$DEMO_OUT" | grep -q "hello, RISC-V"; then
  ok "printed over the UART and powered off"
  printf '%s\n' "$DEMO_OUT" | sed 's/^/       /'
else
  fail "the demo did not produce the expected output"
  printf '%s\n' "$DEMO_OUT" | sed 's/^/       /'
fi

# ---------------------------------------------------------------------------
step "Official riscv-tests"

if [ "$HAVE_TOOLCHAIN" -eq 0 ]; then
  note "skipped - no RISC-V toolchain installed"
elif EMU="$EMU" ./scripts/run-riscv-tests.sh > /tmp/riscv-emu-riscv-tests.log 2>&1; then
  ok "$(grep -E 'passed' /tmp/riscv-emu-riscv-tests.log | tail -1 | sed 's/^ *//')"
else
  fail "riscv-tests failed; see /tmp/riscv-emu-riscv-tests.log"
fi

# ---------------------------------------------------------------------------
step "Summary"

if [ "$FAILURES" -eq 0 ]; then
  printf '%s  Everything passed.%s\n' "$GREEN$BOLD" "$OFF"
  printf '  To boot a real OS:  %s./scripts/boot-xv6.sh%s\n\n' "$BOLD" "$OFF"
  exit 0
else
  printf '%s  %d step(s) failed.%s\n\n' "$RED$BOLD" "$FAILURES" "$OFF"
  exit 1
fi
