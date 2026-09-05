#!/usr/bin/env bash
#
# Report what this machine has and what it is missing, with the exact command
# to install the rest.
#
# Nothing here is required to *use* the project - every script checks its own
# prerequisites and says what is missing. This exists so you can find out
# everything at once, before starting a kernel build that takes ten minutes and
# then fails on a missing `bc`.
#
# Usage:  scripts/check-setup.sh

set -uo pipefail
cd "$(dirname "$0")/.."

if [ -t 1 ]; then
  BOLD=$'\033[1m'; GREEN=$'\033[32m'; RED=$'\033[31m'; YELLOW=$'\033[33m'
  DIM=$'\033[2m'; OFF=$'\033[0m'
else
  BOLD=""; GREEN=""; RED=""; YELLOW=""; DIM=""; OFF=""
fi

# --- which distribution is this? --------------------------------------------
#
# /etc/os-release is the one file every modern Linux has, and ID_LIKE catches
# the derivatives - Linux Mint reports ID=linuxmint, ID_LIKE=ubuntu.
DISTRO="unknown"
if [ -r /etc/os-release ]; then
  # shellcheck disable=SC1091
  . /etc/os-release
  case "${ID:-} ${ID_LIKE:-}" in
    *fedora*|*rhel*|*centos*) DISTRO="fedora" ;;
    *debian*|*ubuntu*)        DISTRO="debian" ;;
    *arch*)                   DISTRO="arch" ;;
    *suse*)                   DISTRO="suse" ;;
  esac
fi

MISSING_CORE=0
MISSING_BARE=0
MISSING_LINUX=0

have() { command -v "$1" > /dev/null 2>&1; }

# Report on one requirement. `alternatives` is a space-separated list of
# commands, any one of which satisfies it.
check() {
  local label="$1" alternatives="$2" bucket="$3"
  local found=""
  for c in $alternatives; do
    if have "$c"; then found="$c"; break; fi
  done
  if [ -n "$found" ]; then
    printf '  %s✓%s %-34s %s%s%s\n' "$GREEN" "$OFF" "$label" "$DIM" "$found" "$OFF"
  else
    printf '  %s✗%s %-34s %smissing%s\n' "$RED" "$OFF" "$label" "$RED" "$OFF"
    case "$bucket" in
      core)  MISSING_CORE=1 ;;
      bare)  MISSING_BARE=1 ;;
      linux) MISSING_LINUX=1 ;;
    esac
  fi
}

printf '%sriscv-emu setup check%s  %s(%s)%s\n\n' "$BOLD" "$OFF" "$DIM" "$DISTRO" "$OFF"

printf '%sTo build and run the emulator%s\n' "$BOLD" "$OFF"
check "C++20 compiler"   "g++ clang++" core
check "CMake"            "cmake"       core
check "make"             "make"        core

printf '\n%sTo run the test suites and boot xv6%s\n' "$BOLD" "$OFF"
check "RISC-V bare-metal toolchain" \
      "riscv64-unknown-elf-gcc riscv64-elf-gcc riscv64-linux-gnu-gcc" bare
check "git"              "git"         bare

printf '\n%sTo boot Linux%s\n' "$BOLD" "$OFF"
check "RISC-V Linux toolchain" \
      "riscv64-linux-gnu-gcc riscv64-unknown-linux-gnu-gcc" linux
check "flex"             "flex"        linux
check "bison"            "bison"       linux
check "bc"               "bc"          linux
check "cpio"             "cpio"        linux

printf '\n%sOptional%s\n' "$BOLD" "$OFF"
check "dtc (validates the device tree)" "dtc" optional

# --- what to do about it ----------------------------------------------------

if [ "$MISSING_CORE" = 0 ] && [ "$MISSING_BARE" = 0 ] && [ "$MISSING_LINUX" = 0 ]; then
  printf '\n%sEverything is here.%s\n\n' "$GREEN$BOLD" "$OFF"
  printf '  ./run-all.sh              build and run every test\n'
  printf '  ./scripts/boot-xv6.sh     boot xv6 to a shell\n'
  printf '  ./scripts/boot-linux.sh   boot Linux\n\n'
  exit 0
fi

printf '\n%sTo install what is missing:%s\n\n' "$BOLD" "$OFF"

case "$DISTRO" in
  fedora)
    [ "$MISSING_CORE" = 1 ] &&
      echo "  sudo dnf install gcc-c++ cmake make"
    [ "$MISSING_BARE" = 1 ] &&
      echo "  sudo dnf install gcc-riscv64-elf binutils-riscv64-elf git"
    [ "$MISSING_LINUX" = 1 ] &&
      echo "  sudo dnf install gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu \\
                   flex bison bc openssl-devel elfutils-libelf-devel cpio"
    echo "  sudo dnf install dtc          # optional"
    ;;
  debian)
    [ "$MISSING_CORE" = 1 ] &&
      echo "  sudo apt install build-essential cmake"
    [ "$MISSING_BARE" = 1 ] &&
      echo "  sudo apt install gcc-riscv64-unknown-elf git"
    [ "$MISSING_LINUX" = 1 ] &&
      echo "  sudo apt install gcc-riscv64-linux-gnu libc6-dev-riscv64-cross \\
                   flex bison bc libssl-dev libelf-dev cpio"
    echo "  sudo apt install device-tree-compiler   # optional"
    ;;
  arch)
    [ "$MISSING_CORE" = 1 ]  && echo "  sudo pacman -S gcc cmake make"
    [ "$MISSING_BARE" = 1 ]  && echo "  sudo pacman -S riscv64-elf-gcc riscv64-elf-binutils git"
    [ "$MISSING_LINUX" = 1 ] && echo "  sudo pacman -S riscv64-linux-gnu-gcc flex bison bc cpio openssl"
    echo "  sudo pacman -S dtc          # optional"
    ;;
  suse)
    [ "$MISSING_CORE" = 1 ]  && echo "  sudo zypper install gcc-c++ cmake make"
    [ "$MISSING_BARE" = 1 ]  && echo "  sudo zypper install cross-riscv64-gcc13 git"
    [ "$MISSING_LINUX" = 1 ] && echo "  sudo zypper install flex bison bc cpio libopenssl-devel libelf-devel"
    ;;
  *)
    echo "  Could not identify the distribution. The packages needed are:"
    echo "    a C++20 compiler, CMake, make"
    echo "    a RISC-V bare-metal toolchain (riscv64-unknown-elf or riscv64-elf)"
    echo "    a RISC-V Linux toolchain (riscv64-linux-gnu), flex, bison, bc, cpio"
    ;;
esac

# Only the first group is genuinely required; the rest gate specific things, so
# say which rather than implying nothing works.
echo
if [ "$MISSING_CORE" = 1 ]; then
  printf '%sThe emulator itself cannot be built until the first group is installed.%s\n' \
         "$YELLOW" "$OFF"
else
  printf '%sThe emulator builds and runs. ' "$GREEN"
  [ "$MISSING_BARE" = 1 ]  && printf 'The bare-metal self-tests and xv6 are unavailable. '
  [ "$MISSING_LINUX" = 1 ] && printf 'Booting Linux is unavailable. '
  printf '%s\n' "$OFF"
fi
echo
