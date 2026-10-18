#!/usr/bin/env bash
#
# Boot a real Debian on the emulator.
#
# Not a toy initramfs: Debian's own riscv64 packages - systemd, apt, a shell, ssh,
# everything `apt install` gives you - on a disk image the emulator reads and
# writes, with working networking, a clock, and as many cores as you ask for.
#
# Usage:  scripts/boot-debian.sh [options] [-- extra emulator args]
#
#   --smp N            number of cores (default 2)
#   --mem MB           guest RAM in MiB (default 1024)
#   --size SIZE        grow the root filesystem to SIZE, e.g. 4G (default 4G)
#   --snapshot         discard the guest's writes on exit; the image on disk is
#                      never modified (good for experimenting)
#   --no-net           no network card
#   --rebuild-rootfs   build the Debian root filesystem again from scratch
#   --rebuild-kernel   build the kernel again from scratch
#   --prepare          build everything but do not boot
#   --check            boot, run a few commands, power off, and report whether it
#                      worked (for a smoke test)
#   --suite NAME       Debian release (default trixie, Debian 13: the first with
#                      riscv64 as an official architecture)
#   --mirror URL       package mirror (default http://deb.debian.org/debian)
#   --                 everything after this is passed straight to the emulator
#
# To leave the guest: press Ctrl-A then X.  Log in as root (the console logs in
# for you; the password is "root").
#
# WHAT YOU NEED (on Debian or Ubuntu; this is the only part that needs Linux):
#
#   sudo apt install mmdebstrap qemu-user-static binfmt-support e2fsprogs uidmap \
#                    gcc-riscv64-linux-gnu libc6-dev-riscv64-cross \
#                    flex bison bc libssl-dev libelf-dev cpio make git
#
# qemu-user-static is how the *host* runs the riscv64 programs inside the new
# root filesystem while it is being built (package maintainer scripts must run).
# It is only used to build the image; the finished system runs on this emulator.
#
# On Windows, run this under WSL2 or in a Linux container: `wsl --install`.
#
# FIRST RUN is slow: it downloads about 250 MB of packages and clones and builds a
# Linux kernel. Both are cached and reused afterwards.
#
# WHAT HAS AND HAS NOT BEEN VERIFIED: the emulator side of this - SMP, the block
# device, user-mode networking, the clock, the entropy seed - is covered by the
# test suite. This script itself needs a Linux host with network access to run, and
# a full Debian boot is a long, demanding workload; if a step fails the message
# says which, and docs/13-booting-debian.md lists what to look at.

set -uo pipefail
cd "$(dirname "$0")/.."

# --- defaults ---------------------------------------------------------------

SMP=2
MEM_MB=1024
SIZE=4G
SNAPSHOT=0
NET=1
REBUILD_ROOTFS=0
REBUILD_KERNEL=0
PREPARE=0
CHECK=0
SUITE="${DEB_SUITE:-trixie}"
MIRROR="${DEB_MIRROR:-http://deb.debian.org/debian}"
EMU_ARGS=()

LINUX_URL="https://github.com/torvalds/linux.git"
LINUX_TAG="v6.6"
SRC_DIR="third_party/linux"
BUILD_DIR="build/debian"
ROOTFS_IMG="$BUILD_DIR/rootfs.ext2"
EMU="build/riscv_emu"
MAX_STEPS=100000000000

while [ $# -gt 0 ]; do
  case "$1" in
    --smp)            SMP="$2"; shift 2 ;;
    --mem)            MEM_MB="$2"; shift 2 ;;
    --size)           SIZE="$2"; shift 2 ;;
    --snapshot)       SNAPSHOT=1; shift ;;
    --no-net)         NET=0; shift ;;
    --rebuild-rootfs) REBUILD_ROOTFS=1; shift ;;
    --rebuild-kernel) REBUILD_KERNEL=1; shift ;;
    --prepare)        PREPARE=1; shift ;;
    --check)          CHECK=1; shift ;;
    --suite)          SUITE="$2"; shift 2 ;;
    --mirror)         MIRROR="$2"; shift 2 ;;
    --)               shift; EMU_ARGS=("$@"); break ;;
    -h|--help)        sed -n '3,/^$/p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
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
JOBS="$(nproc 2>/dev/null || echo 4)"

# --- prerequisites ----------------------------------------------------------

[ "$(uname -s)" = "Linux" ] || die "this script needs a Linux host (building a Debian image uses
Linux-only tools). On Windows, run it inside WSL2: 'wsl --install', then re-run from there."

missing=()
for tool in mmdebstrap mke2fs e2fsck resize2fs make flex bison bc cpio git; do
  command -v "$tool" > /dev/null || missing+=("$tool")
done
if [ ${#missing[@]} -gt 0 ]; then
  die "missing tools: ${missing[*]}
Install them with:
  sudo apt install mmdebstrap qemu-user-static binfmt-support e2fsprogs uidmap \\
                   gcc-riscv64-linux-gnu libc6-dev-riscv64-cross \\
                   flex bison bc libssl-dev libelf-dev cpio make git"
fi

CROSS=""
for p in riscv64-linux-gnu- riscv64-unknown-linux-gnu-; do
  command -v "${p}gcc" > /dev/null && { CROSS="$p"; break; }
done
[ -n "$CROSS" ] || die "no Linux-target RISC-V toolchain found (riscv64-linux-gnu-gcc):
  sudo apt install gcc-riscv64-linux-gnu libc6-dev-riscv64-cross"

# qemu-user-static must be registered with the kernel, or the host cannot run the
# riscv64 binaries inside the root filesystem it is building.
if [ ! -e /proc/sys/fs/binfmt_misc/qemu-riscv64 ] && [ ! -e /proc/sys/fs/binfmt_misc/riscv64 ]; then
  die "the host cannot run riscv64 programs (no binfmt_misc entry for riscv64).
  sudo apt install qemu-user-static binfmt-support
and make sure binfmt_misc is mounted (it is by default on Debian and Ubuntu)."
fi

mkdir -p "$BUILD_DIR"

# --- the emulator -----------------------------------------------------------

if [ ! -x "$EMU" ]; then
  step "Building the emulator"
  cmake -S . -B build > /dev/null || die "cmake configure failed"
  cmake --build build -j"$JOBS" > /dev/null || die "emulator build failed"
fi

# --- the root filesystem ----------------------------------------------------
#
# mmdebstrap installs Debian into an image without needing root: it uses a user
# namespace, so files come out owned by uid 0 inside the image and by you outside.
# The customize hooks below turn the minimal install into something that boots
# unattended on this machine:
#
#   hostname, fstab        the basics
#   networkd + DHCP        the emulator's user-mode network gives the guest
#                          10.0.2.15 over DHCP, with DNS at 10.0.2.3
#   serial console login   the UART is the only console; log root in automatically
#                          so the first thing you see is a prompt
#   root password          "root", for ssh and for su

build_rootfs() {
  step "Building a Debian $SUITE riscv64 root filesystem (downloads packages)"
  rm -f "$ROOTFS_IMG"

  mmdebstrap --arch=riscv64 --variant=minbase --format=ext2 \
    --include=systemd-sysv,udev,dbus,iproute2,iputils-ping,ca-certificates,openssh-server,openssh-client,nano,less,procps,kmod,ifupdown,netbase,curl,wget,apt-utils,bash-completion,locales \
    --customize-hook='echo riscv-emu > "$1/etc/hostname"' \
    --customize-hook='printf "127.0.0.1\tlocalhost\n127.0.1.1\triscv-emu\n" > "$1/etc/hosts"' \
    --customize-hook='printf "/dev/vda\t/\text4\tdefaults,noatime\t0\t1\n" > "$1/etc/fstab"' \
    --customize-hook='mkdir -p "$1/etc/systemd/network" && printf "[Match]\nName=en* eth*\n\n[Network]\nDHCP=yes\n" > "$1/etc/systemd/network/20-wired.network"' \
    --customize-hook='chroot "$1" systemctl enable systemd-networkd' \
    --customize-hook='mkdir -p "$1/etc/systemd/system/serial-getty@ttyS0.service.d" && printf "[Service]\nExecStart=\nExecStart=-/sbin/agetty --autologin root --noclear --keep-baud 115200,38400,9600 ttyS0 vt220\n" > "$1/etc/systemd/system/serial-getty@ttyS0.service.d/autologin.conf"' \
    --customize-hook='mkdir -p "$1/etc/systemd/system/getty.target.wants" && ln -sf /lib/systemd/system/serial-getty@.service "$1/etc/systemd/system/getty.target.wants/serial-getty@ttyS0.service"' \
    --customize-hook='echo "root:root" | chroot "$1" chpasswd' \
    --customize-hook='printf "PermitRootLogin yes\n" > "$1/etc/ssh/sshd_config.d/10-root.conf"' \
    --customize-hook='rm -f "$1/etc/resolv.conf" && printf "nameserver 10.0.2.3\n" > "$1/etc/resolv.conf"' \
    "$SUITE" "$ROOTFS_IMG" "$MIRROR" \
    || die "mmdebstrap failed. Common causes: no network access to $MIRROR; user namespaces
disabled (try: sudo sysctl kernel.unprivileged_userns_clone=1); or qemu-user-static
not registered. Re-run with --rebuild-rootfs after fixing it."

  # mmdebstrap sizes the image to its contents. Give the guest room to work.
  step "Growing the root filesystem to $SIZE"
  truncate -s "$SIZE" "$ROOTFS_IMG" || die "could not grow $ROOTFS_IMG"
  e2fsck -fp "$ROOTFS_IMG" > /dev/null 2>&1
  resize2fs "$ROOTFS_IMG" > /dev/null 2>&1 || die "resize2fs failed"
}

if [ "$REBUILD_ROOTFS" = 1 ] || [ ! -f "$ROOTFS_IMG" ]; then
  build_rootfs
fi
printf '%s    root filesystem: %s (%s)%s\n' "$DIM" "$ROOTFS_IMG" "$(du -h "$ROOTFS_IMG" | cut -f1) on disk" "$OFF"

# --- the kernel ---------------------------------------------------------------
#
# The stock riscv defconfig plus what a systemd userspace expects that it does not
# already turn on. Everything the machine needs to reach its root filesystem is
# built in (=y), not a module: there is no initramfs to load modules from.

KERNEL="$SRC_DIR/arch/riscv/boot/Image"

build_kernel() {
  if [ ! -d "$SRC_DIR/arch" ]; then
    step "Fetching Linux $LINUX_TAG into $SRC_DIR (a large clone)"
    mkdir -p "$(dirname "$SRC_DIR")"
    git clone --depth 1 --branch "$LINUX_TAG" "$LINUX_URL" "$SRC_DIR" \
      || die "could not clone $LINUX_URL"
  fi

  if [ "$REBUILD_KERNEL" = 1 ]; then
    step "Cleaning the kernel tree"
    make -C "$SRC_DIR" ARCH=riscv mrproper > /dev/null 2>&1
  fi

  step "Configuring the kernel for Debian"
  make -C "$SRC_DIR" ARCH=riscv CROSS_COMPILE="$CROSS" defconfig > "$BUILD_DIR/config.log" 2>&1 \
    || { tail -20 "$BUILD_DIR/config.log"; die "kernel configuration failed"; }

  # What systemd and a root filesystem on virtio need, on top of defconfig.
  local cfg="$SRC_DIR/scripts/config"
  local file="$SRC_DIR/.config"
  for opt in \
      SMP BLK_DEV_INITRD DEVTMPFS DEVTMPFS_MOUNT TMPFS TMPFS_POSIX_ACL TMPFS_XATTR \
      EXT4_FS EXT4_FS_POSIX_ACL EXT4_FS_SECURITY \
      VIRTIO_MMIO VIRTIO_BLK VIRTIO_NET HW_RANDOM HW_RANDOM_VIRTIO \
      RTC_CLASS RTC_DRV_GOLDFISH RTC_HCTOSYS \
      NET INET UNIX PACKET IPV6 \
      CGROUPS CGROUP_PIDS CGROUP_CPUACCT CGROUP_FREEZER CGROUP_DEVICE CGROUP_SCHED MEMCG \
      AUTOFS_FS FHANDLE SECCOMP INOTIFY_USER EPOLL SIGNALFD TIMERFD EVENTFD \
      SERIAL_8250 SERIAL_8250_CONSOLE SERIAL_OF_PLATFORM \
      CRYPTO_USER_API_HASH SYSFS PROC_FS BINFMT_ELF; do
    "$cfg" --file "$file" --enable "$opt" 2> /dev/null
  done
  make -C "$SRC_DIR" ARCH=riscv CROSS_COMPILE="$CROSS" olddefconfig > "$BUILD_DIR/config.log" 2>&1 \
    || { tail -20 "$BUILD_DIR/config.log"; die "kernel olddefconfig failed"; }

  step "Building the kernel (several minutes)"
  make -C "$SRC_DIR" ARCH=riscv CROSS_COMPILE="$CROSS" -j"$JOBS" Image > "$BUILD_DIR/build.log" 2>&1 \
    || { tail -30 "$BUILD_DIR/build.log"; die "kernel build failed (log: $BUILD_DIR/build.log)"; }
}

if [ "$REBUILD_KERNEL" = 1 ] || [ ! -f "$KERNEL" ]; then
  build_kernel
fi
[ -f "$KERNEL" ] || die "no kernel at $KERNEL"
printf '%s    kernel: %s (%s bytes)%s\n' "$DIM" "$KERNEL" "$(stat -c %s "$KERNEL")" "$OFF"

if [ "$PREPARE" = 1 ]; then
  printf '%sEverything is built. Run scripts/boot-debian.sh to boot it.%s\n' "$GREEN" "$OFF"
  exit 0
fi

# --- boot ---------------------------------------------------------------------

BOOT_ARGS=(
  --linux
  --smp "$SMP"
  --dram-size-mb "$MEM_MB"
  --max-steps "$MAX_STEPS"
  --rng --rtc
  --bootargs "console=ttyS0 earlycon=sbi root=/dev/vda rootfstype=ext4 rw rootwait"
)
if [ "$SNAPSHOT" = 1 ]; then BOOT_ARGS+=(--drive-snapshot "$ROOTFS_IMG"); else BOOT_ARGS+=(--drive "$ROOTFS_IMG"); fi
if [ "$NET" = 1 ]; then BOOT_ARGS+=(--net user); fi

step "Booting Debian $SUITE  ${DIM}($SMP cores, ${MEM_MB} MiB of RAM)${OFF}"

if [ "$CHECK" = 1 ]; then
  # Give the system time to come up, then type a few commands at the console and
  # power off. The marker lines let us check the answers.
  #
  # Each command is printed with printf '%s\n' and single quotes so that the
  # $(...) inside it reaches the *guest* unexpanded; written any other way this
  # shell would run `uname -m` itself and report the host's architecture.
  type_at_console() {
    sleep 240
    printf '%s\n' 'echo CHECK-UNAME $(uname -m)';                                   sleep 5
    printf '%s\n' 'echo CHECK-OS $(. /etc/os-release; echo $PRETTY_NAME)';          sleep 5
    printf '%s\n' 'echo CHECK-CPUS $(nproc)';                                       sleep 5
    printf '%s\n' 'echo CHECK-NET $(ip -4 -o addr show scope global | awk "{print \$4}" | head -1)'
    sleep 5
    printf '%s\n' 'poweroff';                                                       sleep 60
  }
  out=$( type_at_console | "$EMU" "${BOOT_ARGS[@]}" "${EMU_ARGS[@]}" "$KERNEL" 2>&1 )
  echo "$out" | tail -40
  echo
  ok=1
  printf '%s' "$out" | grep -q "CHECK-UNAME riscv64"   || { echo "missing: architecture"; ok=0; }
  printf '%s' "$out" | grep -q "CHECK-OS Debian"       || { echo "missing: Debian";       ok=0; }
  printf '%s' "$out" | grep -q "CHECK-CPUS $SMP"       || { echo "missing: $SMP cores";   ok=0; }
  if [ "$NET" = 1 ]; then
    printf '%s' "$out" | grep -q "CHECK-NET 10.0.2.15" || { echo "missing: DHCP address"; ok=0; }
  fi
  if [ "$ok" = 1 ]; then printf '%sDebian booted and answered.%s\n' "$GREEN" "$OFF"; exit 0; fi
  die "the Debian smoke test did not pass (see the console output above)"
fi

printf '%s    Ctrl-C goes to the guest; press Ctrl-A then X to leave%s\n\n' "$DIM" "$OFF"
exec "$EMU" "${BOOT_ARGS[@]}" "${EMU_ARGS[@]}" "$KERNEL"
