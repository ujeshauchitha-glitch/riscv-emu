// A minimal PID 1 for the emulator's Linux boot.
//
// The kernel starts exactly one process, /init, and if that process exits the
// kernel panics - there is nothing else to run. So this has to stay alive.
//
// Busybox would give a real shell, but it is a large external build. This is
// deliberately the smallest thing that proves the boot worked end to end: it
// prints, shows what the kernel discovered about the machine, and then reads
// commands from the console so the boot is interactive rather than a log.
//
// Built static, because there is no dynamic loader and no libraries in the
// initramfs - just this one file.

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/reboot.h>
#include <unistd.h>

static void cat(const char* path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("(cannot open %s)\n", path);
        return;
    }
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        if (write(1, buf, (size_t)n) < 0) break;
    }
    close(fd);
}

int main(void) {
    // /proc is where the kernel publishes what it knows about itself. Mounting
    // it is also the first real filesystem operation of the boot, so a failure
    // here says something quite different from a failure to print.
    mkdir("/proc", 0755);
    mount("proc", "/proc", "proc", 0, NULL);

    printf("\n");
    printf("=====================================================\n");
    printf("  Linux is running on the riscv-emu emulator.\n");
    printf("=====================================================\n\n");

    printf("--- /proc/cpuinfo ---\n");
    cat("/proc/cpuinfo");

    printf("\n--- uname ---\n");
    cat("/proc/version");

    printf("\n--- memory ---\n");
    int fd = open("/proc/meminfo", O_RDONLY);
    if (fd >= 0) {
        char buf[512];
        ssize_t n = read(fd, buf, sizeof buf - 1);
        if (n > 0) {
            buf[n] = 0;
            // Just the first two lines: total and free.
            char* nl = strchr(buf, '\n');
            if (nl) { nl = strchr(nl + 1, '\n'); if (nl) *nl = 0; }
            printf("%s\n", buf);
        }
        close(fd);
    }

    printf("\nType 'help' for commands, 'poweroff' to stop the machine.\n");

    for (;;) {
        printf("\n# ");
        fflush(stdout);

        char line[256];
        ssize_t n = read(0, line, sizeof line - 1);
        if (n <= 0) {
            // End of input. Sleeping forever rather than exiting, because PID 1
            // exiting is a kernel panic.
            printf("\n(no more input; the machine is idle - Ctrl-A x to leave)\n");
            for (;;) pause();
        }
        line[n] = 0;
        while (n > 0 && (line[n - 1] == '\n' || line[n - 1] == '\r')) line[--n] = 0;

        if (!strcmp(line, "help")) {
            printf("cpuinfo  meminfo  version  devices  interrupts  uptime  poweroff\n");
        } else if (!strcmp(line, "cpuinfo"))    cat("/proc/cpuinfo");
        else if (!strcmp(line, "meminfo"))      cat("/proc/meminfo");
        else if (!strcmp(line, "version"))      cat("/proc/version");
        else if (!strcmp(line, "devices"))      cat("/proc/devices");
        else if (!strcmp(line, "interrupts"))   cat("/proc/interrupts");
        else if (!strcmp(line, "uptime"))       cat("/proc/uptime");
        else if (!strcmp(line, "poweroff") || !strcmp(line, "halt")) {
            printf("powering off\n");
            fflush(stdout);
            sync();
            reboot(RB_POWER_OFF);
        } else if (n > 0) {
            printf("unknown command: %s\n", line);
        }
    }
}
