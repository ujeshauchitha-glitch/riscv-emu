#!/usr/bin/env bash
#
# Build the emulator, run every test, and run every demo.
#
# Usage:  ./run-all.sh [--quick]
#           --quick   reuse the existing build directory instead of rebuilding
#
# See docs/RUNNING.md for what each step does and how to read its output.

set -uo pipefail
cd "$(dirname "$0")"

QUICK=0
[ "${1:-}" = "--quick" ] && QUICK=1

# Colour only when writing to a terminal, so piping to a file stays readable.
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

# ---------------------------------------------------------------------------
step "Build"

if [ "$QUICK" -eq 0 ]; then
  rm -rf build
fi

if ! cmake -S . -B build > /tmp/riscv-emu-cmake.log 2>&1; then
  fail "cmake configure failed; see /tmp/riscv-emu-cmake.log"
  exit 1
fi

if grep -q "toolchain not found" /tmp/riscv-emu-cmake.log; then
  note "no RISC-V toolchain: the bare-metal self-tests will be skipped"
  note "install with: sudo apt-get install gcc-riscv64-unknown-elf"
  HAVE_TOOLCHAIN=0
else
  HAVE_TOOLCHAIN=1
fi

if cmake --build build -j > /tmp/riscv-emu-build.log 2>&1; then
  WARNINGS=$(grep -ci warning: /tmp/riscv-emu-build.log || true)
  if [ "$WARNINGS" -gt 0 ]; then
    fail "built with $WARNINGS warning(s); see /tmp/riscv-emu-build.log"
  else
    ok "built cleanly, no warnings"
  fi
else
  fail "build failed; see /tmp/riscv-emu-build.log"
  exit 1
fi

# ---------------------------------------------------------------------------
step "Tests"

if (cd build && ctest --output-on-failure > /tmp/riscv-emu-ctest.log 2>&1); then
  ok "$(grep -E 'tests passed' /tmp/riscv-emu-ctest.log | head -1)"
  grep -E 'Test +#' /tmp/riscv-emu-ctest.log | sed 's/^/       /'
else
  fail "some tests failed"
  cat /tmp/riscv-emu-ctest.log
fi

# ---------------------------------------------------------------------------
step "Built-in demo"

DEMO_OUT=$(./build/riscv_emu 2>&1)
if printf '%s' "$DEMO_OUT" | grep -q "hello, RISC-V"; then
  ok "printed over the UART and powered off"
  printf '%s\n' "$DEMO_OUT" | sed 's/^/       /'
else
  fail "the demo did not produce the expected output"
  printf '%s\n' "$DEMO_OUT" | sed 's/^/       /'
fi

# ---------------------------------------------------------------------------
step "Bare-metal self-tests"

if [ "$HAVE_TOOLCHAIN" -eq 0 ]; then
  note "skipped - no RISC-V toolchain installed"
else
  # name:expected a0
  for entry in \
      "rv64i_selftest:3fff" \
      "trap_selftest:fff" \
      "muldiv_atomic_selftest:7fff" \
      "device_selftest:1f"
  do
    name="${entry%%:*}"
    want="${entry##*:}"
    bin="build/${name}.bin"

    if [ ! -f "$bin" ]; then
      fail "$name: binary was not built"
      continue
    fi

    out=$(./build/riscv_emu --dump "$bin" 2>&1)
    got=$(printf '%s' "$out" | grep 'x10 ' | grep -oE '[0-9a-f]+$' | sed 's/^0*//')
    got=${got:-0}

    if [ "$got" = "$want" ]; then
      ok "$name: a0 = 0x$got (all checks passed)"
    else
      fail "$name: a0 = 0x$got, expected 0x$want"
      note "a clear bit names the failing check; see examples/${name}.S"
    fi
  done
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
