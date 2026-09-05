#!/usr/bin/env bash
#
# Fetch, build and boot Linux on the emulator.
#
# The counterpart to boot-xv6.sh, and a much bigger machine: a real Linux
# kernel, a device tree it discovers the hardware from, and an SBI firmware
# layer underneath it - all of which phase 8 added.
#
# Usage:  scripts/boot-linux.sh [-- extra emulator args]
#
#   --rebuild   rebuild the kernel and initramfs from scratch
#   --check     boot, wait for the shell, print the result and exit
#   --          everything after this is passed straight to the emulator
#
# Requires a Linux-target RISC-V toolchain and, on first run, network access
# and a good deal of patience: the kernel is a few thousand source files.
#
#   Debian/Ubuntu: apt install gcc-riscv64-linux-gnu libc6-dev-riscv64-cross \
#                              flex bison bc libssl-dev libelf-dev cpio
#   Fedora:        dnf install gcc-riscv64-linux-gnu binutils-riscv64-linux-gnu \
#                              flex bison bc openssl-devel elfutils-libelf-devel cpio
#
# To leave the guest, press Ctrl-A then X.

set -uo pipefail
cd "$(dirname "$0")/.."

LINUX_URL="https://github.com/torvalds/linux.git"
LINUX_TAG="v6.6"
SRC_DIR="third_party/linux"
INITRAMFS_DIR="build/initramfs"
INITRAMFS="build/initramfs.cpio"
EMU="build/riscv_emu"

# Enough memory for the kernel, the initramfs and a page cache. The device tree
# reports this same number, so the kernel's idea of memory always matches the
# emulator's.
DRAM_MB=512
MAX_STEPS=100000000000

REBUILD=0
CHECK=0
EMU_ARGS=()
while [ $# -gt 0 ]; do
  case "$1" in
    --rebuild) REBUILD=1; shift ;;
    --check)   CHECK=1;   shift ;;
    --)        shift; EMU_ARGS=("$@"); break ;;
    -h|--help) sed -n '3,25p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
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

# A *Linux*-target toolchain, not the bare-metal one the rest of the project
# uses. riscv64-unknown-elf targets newlib and has no Linux headers, so it can
# build the emulator's self-tests but not a kernel or a userspace binary.
CROSS=""
for p in riscv64-linux-gnu- riscv64-unknown-linux-gnu-; do
  command -v "${p}gcc" > /dev/null && { CROSS="$p"; break; }
done
[ -n "$CROSS" ] || die "no Linux-target RISC-V toolchain found (riscv64-linux-gnu-gcc).
See the header of this script for the package names."

for tool in make flex bison bc cpio; do
  command -v "$tool" > /dev/null || die "$tool not found - see the header of this script"
done

# --- the emulator -----------------------------------------------------------

if [ ! -x "$EMU" ]; then
  step "Building the emulator"
  cmake -S . -B build > /dev/null || die "cmake configure failed"
  cmake --build build -j"$(nproc 2>/dev/null || echo 4)" > /dev/null \
    || die "emulator build failed"
fi

# --- the initramfs ----------------------------------------------------------
#
# The kernel starts exactly one process, /init, and panics if it exits. This is
# a single static binary: no shell, no libraries, no dynamic loader - the
# smallest thing that proves the boot worked and then stays alive to be typed
# at.
if [ "$REBUILD" = 1 ] || [ ! -f "$INITRAMFS" ]; then
  step "Building the initramfs"
  rm -rf "$INITRAMFS_DIR"
  mkdir -p "$INITRAMFS_DIR"
  "${CROSS}gcc" -static -Os -o "$INITRAMFS_DIR/init" examples/initramfs/init.c \
    || die "could not build examples/initramfs/init.c"
  "${CROSS}strip" "$INITRAMFS_DIR/init" 2>/dev/null

  # A newc-format cpio archive, which is the one format the kernel's built-in
  # unpacker understands.
  ( cd "$INITRAMFS_DIR" && find . | cpio -o -H newc --quiet ) > "$INITRAMFS" \
    || die "cpio failed"
  printf '%s    %s (%s bytes)%s\n' "$DIM" "$INITRAMFS" \
         "$(stat -c %s "$INITRAMFS")" "$OFF"
fi

# --- the kernel -------------------------------------------------------------

if [ ! -d "$SRC_DIR/arch" ]; then
  step "Fetching Linux $LINUX_TAG into $SRC_DIR (this is a large clone)"
  mkdir -p "$(dirname "$SRC_DIR")"
  git clone --depth 1 --branch "$LINUX_TAG" "$LINUX_URL" "$SRC_DIR" \
    || die "could not clone $LINUX_URL"
fi

if [ "$REBUILD" = 1 ]; then
  step "Cleaning the kernel tree"
  make -C "$SRC_DIR" ARCH=riscv mrproper > /dev/null 2>&1
fi

if [ ! -f "$SRC_DIR/arch/riscv/boot/Image" ]; then
  step "Configuring the kernel"
  make -C "$SRC_DIR" ARCH=riscv CROSS_COMPILE="$CROSS" defconfig \
       > /tmp/linux-config.log 2>&1 \
    || { tail -20 /tmp/linux-config.log; die "kernel configuration failed"; }

  step "Building the kernel (several minutes)"
  make -C "$SRC_DIR" ARCH=riscv CROSS_COMPILE="$CROSS" \
       -j"$(nproc 2>/dev/null || echo 4)" Image > /tmp/linux-build.log 2>&1 \
    || { tail -30 /tmp/linux-build.log; die "kernel build failed (log: /tmp/linux-build.log)"; }
fi

KERNEL="$SRC_DIR/arch/riscv/boot/Image"
[ -f "$KERNEL" ] || die "no kernel at $KERNEL"

# --- boot -------------------------------------------------------------------

step "Booting Linux  ${DIM}($(stat -c %s "$KERNEL") bytes, ${DRAM_MB} MiB of RAM)${OFF}"

BOOT_ARGS=(
  --linux
  --dram-size-mb "$DRAM_MB"
  --max-steps "$MAX_STEPS"
  --initrd "$INITRAMFS"
  --bootargs "console=ttyS0 earlycon=sbi rdinit=/init"
)

if [ "$CHECK" = 1 ]; then
  echo
  out=$( (sleep 600) | "$EMU" "${BOOT_ARGS[@]}" "${EMU_ARGS[@]}" "$KERNEL" 2>&1 )
  echo "$out"
  echo
  if printf '%s' "$out" | grep -q "Linux is running on the riscv-emu emulator"; then
    printf '%sLinux booted to its init process.%s\n' "$GREEN" "$OFF"
    exit 0
  fi
  die "Linux did not reach its init process"
fi

printf '%s    Ctrl-C goes to the guest; press Ctrl-A then X to leave%s\n\n' "$DIM" "$OFF"
exec "$EMU" "${BOOT_ARGS[@]}" "${EMU_ARGS[@]}" "$KERNEL"
