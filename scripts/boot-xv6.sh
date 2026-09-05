#!/usr/bin/env bash
#
# Fetch, build and boot xv6-riscv on the emulator.
#
# This is the phase 7 milestone in one command: it clones xv6 into third_party/
# on first run, builds it for the extensions this emulator implements, checks
# that the result really is free of compressed instructions, builds the
# emulator if needed, and boots it with the filesystem image attached.
#
# Usage:  scripts/boot-xv6.sh [-- extra emulator args]
#
#   --rebuild   rebuild xv6 from scratch even if it is already built
#   --check     boot, run `ls`, print the result and exit - no interactive
#               shell. Useful in a script, or to confirm it works at all.
#   --          everything after this is passed straight to the emulator,
#               e.g.  scripts/boot-xv6.sh -- --trace 2> trace.log
#
# Requires a RISC-V toolchain (gcc-riscv64-unknown-elf) and, on first run,
# network access to fetch xv6.
#
# To leave the guest, press Ctrl-A then X. Ctrl-C goes to the *guest* shell,
# because the console is in raw mode - that is the point of raw mode, and it
# means the usual way out of a terminal program does not apply here.

set -uo pipefail
cd "$(dirname "$0")/.."

REPO_URL="https://github.com/mit-pdos/xv6-riscv.git"
SRC_DIR="third_party/xv6-riscv"
EMU="build/riscv_emu"

# xv6's stock build targets -march=rv64gc. The `c` is compressed 16-bit
# instructions and `g` pulls in floating point; this emulator implements
# neither yet (phase 8). These are the extensions it does implement.
XV6_MARCH="rv64ima_zicsr_zifencei"
XV6_MABI="lp64"

# xv6 reaches the shell at roughly 500 million instructions, so the emulator's
# 100-million default would stop it partway through boot. This budget is large
# enough to run `usertests` to completion.
MAX_STEPS=1000000000000

REBUILD=0
CHECK=0
EMU_ARGS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --rebuild) REBUILD=1; shift ;;
    --check)   CHECK=1;   shift ;;
    --)        shift; EMU_ARGS=("$@"); break ;;
    -h|--help) sed -n '3,23p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
    *) echo "unknown option: $1 (use -- to pass arguments to the emulator)"; exit 2 ;;
  esac
done

if [ -t 1 ]; then
  BOLD=$'\033[1m'; GREEN=$'\033[32m'; RED=$'\033[31m'; DIM=$'\033[2m'; OFF=$'\033[0m'
else
  BOLD=""; GREEN=""; RED=""; DIM=""; OFF=""
fi

step() { printf '%s==>%s %s\n' "$BOLD" "$OFF" "$1"; }
die()  { printf '%serror:%s %s\n' "$RED" "$OFF" "$1" >&2; exit 2; }

# --- prerequisites ----------------------------------------------------------

# xv6's Makefile finds its own toolchain prefix, but it only looks for the two
# common ones - so fail here with a useful message rather than inside make.
TOOLPREFIX=""
for p in riscv64-unknown-elf- riscv64-linux-gnu- riscv64-unknown-linux-gnu-; do
  command -v "${p}gcc" > /dev/null && { TOOLPREFIX="$p"; break; }
done
[ -n "$TOOLPREFIX" ] || die "no RISC-V toolchain found. Install with:
  sudo apt-get install gcc-riscv64-unknown-elf
(or gcc-riscv64-linux-gnu)"

command -v make > /dev/null || die "make not found"

# --- the emulator -----------------------------------------------------------

if [ ! -x "$EMU" ]; then
  step "Building the emulator"
  cmake -S . -B build > /dev/null || die "cmake configure failed"
  cmake --build build -j"$(nproc 2>/dev/null || echo 4)" > /dev/null \
    || die "emulator build failed"
fi

# --- xv6 --------------------------------------------------------------------

if [ ! -d "$SRC_DIR/kernel" ]; then
  step "Fetching xv6 into $SRC_DIR"
  mkdir -p "$(dirname "$SRC_DIR")"
  git clone --depth 1 "$REPO_URL" "$SRC_DIR" || die "could not clone $REPO_URL"
fi

# Point xv6's build at the extensions this emulator implements.
#
# There is no make variable for this. xv6's Makefile hardcodes `-march=rv64gc`
# into CFLAGS, and its rule for assembly files spells out `$(CC) -march=rv64gc`
# without including $(CFLAGS) at all - so even a Makefile that did honour an
# override for C would still assemble entry.S, swtch.S, kernelvec.S and
# trampoline.S as rv64gc. That is exactly what happens if you try the obvious
# `make CFLAGS_EXTRA=...`: the build succeeds, the kernel looks fine, and it
# dies on the third instruction of _entry with an illegal instruction.
#
# So patch the two occurrences directly. Idempotent - after the first run there
# is no `-march=rv64gc` left to replace.
if grep -q -- "-march=rv64gc" "$SRC_DIR/Makefile"; then
  step "Retargeting xv6's Makefile at -march=$XV6_MARCH"
  sed -i "s/-march=rv64gc/-march=$XV6_MARCH -mabi=$XV6_MABI/g" "$SRC_DIR/Makefile" \
    || die "could not patch $SRC_DIR/Makefile"
  # Any objects built before the patch are the wrong architecture.
  make -C "$SRC_DIR" clean > /dev/null 2>&1
fi

if [ "$REBUILD" = 1 ]; then
  step "Cleaning xv6"
  make -C "$SRC_DIR" clean > /dev/null 2>&1
fi

if [ ! -f "$SRC_DIR/kernel/kernel" ] || [ ! -f "$SRC_DIR/fs.img" ]; then
  step "Building xv6 for -march=$XV6_MARCH"
  make -C "$SRC_DIR" kernel/kernel fs.img > /tmp/xv6-build.log 2>&1 \
    || { tail -20 /tmp/xv6-build.log; die "xv6 build failed (full log: /tmp/xv6-build.log)"; }
fi

# Confirm the build really is free of compressed instructions, rather than
# trusting that the patch reached every file. A kernel with even one would stop
# the emulator with an illegal-instruction trap at a baffling address.
#
# Note the `-M no-aliases`. Without it objdump prints a compressed instruction
# under its expanded name - `c.lui` shows up as plain `lui` - so a search for
# `c.` finds nothing and the check passes on a kernel that is 5,000
# compressed instructions deep. That is not a hypothetical: it is what this
# check did before, and it is why the failure above reached the emulator.
if command -v "${TOOLPREFIX}objdump" > /dev/null; then
  compressed=$("${TOOLPREFIX}objdump" -d -M no-aliases "$SRC_DIR/kernel/kernel" \
                 | grep -c $'\tc\\.' || true)
  if [ "$compressed" != 0 ]; then
    die "kernel contains $compressed compressed instructions, which this
emulator cannot decode yet (phase 8). Try: scripts/boot-xv6.sh --rebuild"
  fi
  printf '%s    kernel is free of compressed instructions%s\n' "$DIM" "$OFF"
fi

# --- boot -------------------------------------------------------------------

sectors=$(( $(stat -c %s "$SRC_DIR/fs.img" 2>/dev/null || echo 0) / 512 ))
step "Booting xv6  ${DIM}(kernel $SRC_DIR/kernel/kernel, disk $sectors sectors)${OFF}"

if [ "$CHECK" = 1 ]; then
  # Non-interactive: boot, run one command, and report. The sleep gives the
  # kernel time to reach the shell before the input matters; without it `ls`
  # would be consumed by the console driver before init had started.
  echo
  out=$( (printf 'ls\n'; sleep 25) \
         | "$EMU" --disk "$SRC_DIR/fs.img" --max-steps 900000000 \
                  "${EMU_ARGS[@]}" "$SRC_DIR/kernel/kernel" 2>&1 )
  echo "$out"
  echo
  if printf '%s' "$out" | grep -q "init: starting sh" &&
     printf '%s' "$out" | grep -q "usertests"; then
    printf '%sxv6 booted to a shell and listed its filesystem.%s\n' "$GREEN" "$OFF"
    exit 0
  fi
  die "xv6 did not reach a shell prompt"
fi

printf '%s    the prompt is interactive - try ls, cat README, usertests%s\n' "$DIM" "$OFF"
printf '%s    Ctrl-C goes to the guest; press Ctrl-A then X to leave%s\n\n' "$DIM" "$OFF"

exec "$EMU" --disk "$SRC_DIR/fs.img" --max-steps "$MAX_STEPS" \
     "${EMU_ARGS[@]}" "$SRC_DIR/kernel/kernel"
