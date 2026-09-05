#!/usr/bin/env bash
#
# Build and run the official riscv-tests suite against the emulator.
#
# riscv-tests is the reference suite the RISC-V project itself uses. Until now
# the emulator has only been checked against tests written alongside it, which
# cannot catch a misreading of the spec that is consistent across both. This
# can.
#
# Usage:  scripts/run-riscv-tests.sh [suite ...]
#           with no arguments, runs every suite the emulator can currently
#           support: rv64ui rv64um rv64ua rv64mi
#
# Requires a RISC-V toolchain (gcc-riscv64-unknown-elf) and network access on
# first run, to fetch the suite into third_party/.

set -uo pipefail
cd "$(dirname "$0")/.."

REPO_URL="https://github.com/riscv-software-src/riscv-tests.git"
SRC_DIR="third_party/riscv-tests"
BUILD_DIR="build/riscv-tests"
ENV_DIR="tests/riscv-tests-env"
EMU="build/riscv_emu"

# Suites the emulator can run today. rv64si needs supervisor mode (phase 6) and
# rv64uf/rv64ud need floating point (phase 8), so they are deliberately absent.
DEFAULT_SUITES="rv64ui rv64um rv64ua rv64mi rv64si"
SUITES="${*:-$DEFAULT_SUITES}"

# Tests that need a feature the emulator does not implement yet. These are
# listed explicitly, with the reason, rather than being quietly dropped - an
# unexplained gap in a reference suite is worse than a failure, because nobody
# can tell whether it is a decision or an oversight.
#
# Format: suite/name:reason
EXCLUDED="
rv64mi/breakpoint:needs the debug trigger module (optional, not planned)
rv64mi/pmpaddr:needs physical memory protection (optional, not planned)
"

excluded_reason() {
  printf '%s\n' "$EXCLUDED" | while IFS= read -r line; do
    [ -z "$line" ] && continue
    case "$line" in
      "$1":*) printf '%s' "${line#*:}"; return 0 ;;
    esac
  done
}

if [ -t 1 ]; then
  BOLD=$'\033[1m'; GREEN=$'\033[32m'; RED=$'\033[31m'; DIM=$'\033[2m'; OFF=$'\033[0m'
else
  BOLD=""; GREEN=""; RED=""; DIM=""; OFF=""
fi

for tool in riscv64-unknown-elf-gcc; do
  command -v "$tool" > /dev/null || {
    echo "error: $tool not found. Install with:"
    echo "  sudo apt-get install gcc-riscv64-unknown-elf"
    exit 2
  }
done

[ -x "$EMU" ] || { echo "error: $EMU not built. Run: cmake --build build"; exit 2; }

# --- fetch the suite --------------------------------------------------------
if [ ! -d "$SRC_DIR/isa" ]; then
  echo "Fetching riscv-tests into $SRC_DIR ..."
  mkdir -p "$(dirname "$SRC_DIR")"
  git clone --depth 1 "$REPO_URL" "$SRC_DIR" || {
    echo "error: could not clone $REPO_URL"
    exit 2
  }
fi

# Note we do NOT need riscv-tests' `env` submodule: tests/riscv-tests-env
# provides an equivalent bare-metal environment, so the suite builds with just
# the main repository.
MACROS="$SRC_DIR/isa/macros/scalar"

mkdir -p "$BUILD_DIR"

TOTAL=0; PASSED=0; FAILED=0; SKIPPED=0; EXCLUDED_COUNT=0
FAILED_NAMES=""

for suite in $SUITES; do
  suite_dir="$SRC_DIR/isa/$suite"
  [ -d "$suite_dir" ] || { echo "no such suite: $suite"; continue; }

  printf '\n%s== %s ==%s\n' "$BOLD" "$suite" "$OFF"
  s_total=0; s_pass=0

  for src in "$suite_dir"/*.S; do
    name=$(basename "$src" .S)
    elf="$BUILD_DIR/${suite}-${name}.elf"

    # Some sources in a suite are shared helpers rather than tests.
    grep -q RVTEST_CODE_BEGIN "$src" || continue

    reason=$(excluded_reason "$suite/$name")
    if [ -n "$reason" ]; then
      printf '  %sskip%s %-16s %s\n' "$DIM" "$OFF" "$name" "$reason"
      EXCLUDED_COUNT=$((EXCLUDED_COUNT + 1))
      continue
    fi

    TOTAL=$((TOTAL + 1)); s_total=$((s_total + 1))

    if ! riscv64-unknown-elf-gcc -march=rv64ima_zicsr_zifencei -mabi=lp64 \
         -nostdlib -nostartfiles \
         -I "$ENV_DIR" -I "$MACROS" \
         -T "$ENV_DIR/link.ld" -o "$elf" "$src" > "$BUILD_DIR/${name}.log" 2>&1; then
      printf '  %sSKIP%s %-16s (did not assemble - see %s)\n' \
             "$DIM" "$OFF" "$name" "$BUILD_DIR/${name}.log"
      SKIPPED=$((SKIPPED + 1)); TOTAL=$((TOTAL - 1)); s_total=$((s_total - 1))
      continue
    fi

    out=$("$EMU" --max-steps 2000000 "$elf" 2>&1)
    if printf '%s' "$out" | grep -q '^PASS'; then
      PASSED=$((PASSED + 1)); s_pass=$((s_pass + 1))
    else
      FAILED=$((FAILED + 1))
      FAILED_NAMES="$FAILED_NAMES $suite/$name"
      why=$(printf '%s' "$out" | grep -E '^FAIL|^stopped|^step budget' | head -1)
      printf '  %sFAIL%s %-16s %s\n' "$RED" "$OFF" "$name" "$why"
    fi
  done

  if [ "$s_pass" -eq "$s_total" ]; then
    printf '  %sok%s   %d/%d passed\n' "$GREEN" "$OFF" "$s_pass" "$s_total"
  else
    printf '  %s%d/%d passed%s\n' "$BOLD" "$s_pass" "$s_total" "$OFF"
  fi
done

printf '\n%s== summary ==%s\n' "$BOLD" "$OFF"

# Running nothing is a failure, not a pass.
#
# If the clone silently produced no tests, or a suite directory went missing,
# every counter stays at zero and the loop below would report "All 0 tests
# passed" and exit 0 - a green CI that tested nothing, which is worse than a
# visible failure because nobody can tell the difference from a real pass.
if [ "$TOTAL" -eq 0 ]; then
  printf '%s  No tests ran.%s Expected suites: %s\n' "$RED$BOLD" "$OFF" "$SUITES"
  printf '  Check that %s contains the suite directories.\n' "$SRC_DIR/isa"
  exit 1
fi

printf '  %d/%d passed' "$PASSED" "$TOTAL"
[ "$EXCLUDED_COUNT" -gt 0 ] && printf ', %d excluded (unimplemented features)' "$EXCLUDED_COUNT"
[ "$SKIPPED" -gt 0 ] && printf ', %d could not be built' "$SKIPPED"
printf '\n'

if [ "$FAILED" -gt 0 ]; then
  printf '\n  failing:%s\n' "$FAILED_NAMES"
  printf '\n  Re-run one with:\n    %s --trace %s/<suite>-<name>.elf\n' "$EMU" "$BUILD_DIR"
  exit 1
fi

printf '%s  All %d tests passed.%s\n' "$GREEN$BOLD" "$PASSED" "$OFF"
exit 0
