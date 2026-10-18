# Phase 13: Debian

Linux with a toy initramfs proves the kernel boots. Debian proves the *machine* is
a machine: systemd, `udev`, `apt`, `ssh`, a real filesystem on a real disk image,
a network, four cores.

```bash
scripts/boot-debian.sh                     # builds everything on first run, then boots
scripts/boot-debian.sh --smp 4 --mem 2048  # more of it
scripts/boot-debian.sh --snapshot          # experiment; the image on disk is never modified
scripts/boot-debian.sh --check             # boot, run a few commands, power off, report
```

Press Ctrl-A then X to leave. The console logs in as `root` for you.

## What you need

A Linux host (Debian or Ubuntu) — **on Windows, use WSL2** (`wsl --install`):

```bash
sudo apt install mmdebstrap qemu-user-static binfmt-support e2fsprogs uidmap \
                 gcc-riscv64-linux-gnu libc6-dev-riscv64-cross \
                 flex bison bc libssl-dev libelf-dev cpio make git
```

`qemu-user-static` is **only** for building the image: the host has to run
riscv64 programs (package maintainer scripts) inside the new root filesystem while
it is being assembled. The finished system runs on this emulator.

The script itself says exactly what is missing, and the command to install it.

The first run downloads about 250 MB of packages and builds a kernel; both are cached.

## What the script does

1. **Root filesystem.** `mmdebstrap` installs Debian 13 ("trixie" — the first release
   with riscv64 as an official architecture) into an ext2 image without needing
   root, then the script grows it to 4 GiB. Customisation hooks make it boot
   unattended: hostname, `fstab`, `systemd-networkd` with DHCP, `nameserver 10.0.2.3`,
   auto-login on the serial console, root password `root`, ssh allowing root.
2. **Kernel.** Linux 6.6, the stock riscv `defconfig` plus what a systemd userspace
   expects (cgroups, `autofs`, `fhandle`, `seccomp`, `devtmpfs`, ...). Everything
   needed to *reach* the root filesystem — virtio block, ext4, the serial port, the
   RTC — is built in, not a module, because there is no initramfs to load modules from.
3. **Boot.**

```
riscv_emu --linux --smp 2 --dram-size-mb 1024 --rng --rtc \
          --drive build/debian/rootfs.ext2 --net user \
          --bootargs "console=ttyS0 earlycon=sbi root=/dev/vda rootfstype=ext4 rw rootwait" \
          Image
```

## What each piece of the machine is for

| Debian needs | the emulator provides |
|---|---|
| a root filesystem it can write | `--drive` (file-backed, persistent) or `--drive-snapshot` |
| to find its disk without a driver module | virtio-blk, built into the kernel |
| DHCP, DNS, outbound TCP | `--net user` ([phase 11](11-virtio-and-networking.md)) |
| today's date | `--rtc` (goldfish RTC) |
| entropy, so systemd and sshd don't wait | `--rng` and `/chosen/rng-seed` |
| more than one core | `--smp N` ([phase 10](10-multiple-harts.md)) |
| the instructions a modern libc probes for | Zba/Zbb/Zbc/Zbs/Zicond, advertised in the device tree ([phase 14](14-speed-and-extensions.md)) |
| to be debugged when it won't boot | `--gdb PORT` ([phase 12](12-debugging-with-gdb.md)) |

## What has and has not been verified

**Verified by the test suite:** each piece above, individually and under load
(multi-hart atomics and IPIs, the block device's multi-segment chains and persistence,
a complete TCP connection through the user-mode network, the clock, the instruction
extensions against an independent model, the debugger).

**Not verified: a complete Debian boot.** The machine this phase was developed on is a
Windows laptop with no RISC-V cross-compiler and no WSL, so `boot-debian.sh` has not
been run end to end here — only syntax-checked. A full Debian boot is a long, demanding
workload (hundreds of millions to billions of instructions, thousands of system calls,
every subsystem a real distribution touches), and *"every piece tested"* is not the
same as *"the whole thing boots"*. The earlier phases are a fair warning: Linux took
five fixes beyond a working xv6, three of them firmware the emulator didn't know it was
missing.

So expect to meet something. When you do, in rough order of likelihood:

- **The kernel doesn't reach userspace.** Boot with `--gdb` and break in
  `start_kernel`/`kernel_init`; `monitor harts` shows what every core is doing. Try
  `--smp 1` first — if one core works and two don't, the bug is in the multi-hart
  paths (SBI HSM, IPI, remote fences); `--quantum 1` makes the interleaving as fine as
  it can be.
- **`Run /sbin/init` then silence.** Usually a kernel option systemd needs, or the
  console. `rootwait` and `console=ttyS0` are already set; add
  `systemd.log_level=debug systemd.log_target=console` to `--bootargs`.
- **Hangs waiting for something.** `--rng-seed 1` removes entropy as a variable;
  `--rtc-fixed 1760000000` removes the clock as one.
- **Slow.** A boot is about a billion instructions. Bigger `--quantum` is faster;
  `--smp 1` is faster still. `build/bench_cpu` measures the interpreter.
- **No network.** In the guest: `ip addr` should show `10.0.2.15`; `ping 10.0.2.2` tests
  the gateway; `curl` tests TCP. (`ping` to anything beyond the gateway is not answered —
  see [phase 11](11-virtio-and-networking.md).)

Please send what you find. Every one of these is a bug report the next phase is made of.
