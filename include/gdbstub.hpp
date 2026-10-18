#pragma once

#include <functional>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "hostnet.hpp"
#include "platform.hpp"
#include "types.hpp"

// ---------------------------------------------------------------------------
// A GDB remote-serial-protocol server.
//
// Lets a stock `gdb` (or `gdb-multiarch`, or an IDE's debugger front end) attach
// to the emulated machine over TCP, the way it would attach to a QEMU or to
// real hardware with a debug probe:
//
//     $ riscv_emu --gdb 1234 vmlinux      # waits for the debugger
//     $ gdb vmlinux
//     (gdb) target remote :1234
//     (gdb) break start_kernel
//     (gdb) continue
//
// Registers, memory (through the guest's own page tables), breakpoints,
// watchpoints, single-stepping, Ctrl-C, and every hart as a thread.
//
// **The protocol** is plain text over a byte stream. A packet is `$` payload `#`
// and two hex digits of checksum (the sum of the payload bytes mod 256); the
// receiver answers `+` to accept or `-` to ask for it again. The payload is a
// one-letter command followed by arguments - `m1000,4` reads four bytes at
// 0x1000 - and the reply is another packet. Stopping a running target is the
// one thing that is not a packet: gdb just sends a bare 0x03 byte.
//
// **Design.** The command handler, handle_packet(), takes a payload and returns
// a payload and touches no socket. That keeps nearly the whole server testable
// without a debugger, a network or a second process: the tests feed it the same
// strings gdb would. The socket code is a thin shell around it.
//
// **Breakpoints are the emulator's, not the guest's.** A real debug stub
// inserts an EBREAK instruction into the program. Here the machine checks each
// hart's PC before it executes an instruction, so nothing in guest memory is
// patched - which means breakpoints work in ROM, in code about to be copied
// elsewhere, and in pages the guest is rewriting, and they cannot corrupt a
// checksum the guest computes over its own text. The price is that a debugged
// run is slower, because the scheduler gives up its fast path to look at every
// instruction.
// ---------------------------------------------------------------------------
class GdbStub {
public:
    explicit GdbStub(Platform& platform);
    ~GdbStub();

    // Start listening on `port` (0 picks one). Returns the port, or 0 on failure.
    u16 listen(u16 port, u32 ip = 0x7f000001);

    // Block until a debugger connects. Returns false if `abort` becomes true
    // first or accepting fails.
    bool accept_client();

    // Serve the connected debugger until it detaches, kills the session, or the
    // machine halts. Returns true if the guest should now run on to completion
    // (the debugger detached), false if the emulator should stop.
    bool serve();

    // --- the protocol engine, exposed for tests ---------------------------------

    // Handle one command payload and return the reply payload. A command that
    // resumes the machine runs it until something stops it and returns the
    // stop reply. `console` collects output the server wants to show the user
    // (`monitor` commands), to be sent as O packets before the reply.
    std::string handle_packet(const std::string& payload, std::vector<std::string>* console = nullptr);

    // Frame a payload as `$...#cs`.
    static std::string encode_packet(const std::string& payload);

    // Why the machine last stopped, as a gdb stop-reply payload.
    std::string stop_reply() const;

    // How often (in instructions) a running target checks the socket for a
    // Ctrl-C. Public so a test can make it frequent.
    u64 interrupt_poll_interval = 4096;

    // Set by a test to inject an interrupt request.
    std::function<bool()> interrupt_requested;

    bool detached() const { return detached_; }
    bool killed() const { return killed_; }
    std::size_t breakpoint_count() const { return breakpoints_.size(); }

private:
    Platform& platform_;

    sock_t listener_ = INVALID_SOCK;
    sock_t client_   = INVALID_SOCK;
    std::string inbuf_;
    std::string last_sent_;

    // Debugger state.
    std::set<u64> breakpoints_;                        // by virtual address
    struct Watch { u64 addr; u64 len; std::vector<u8> last; };
    std::vector<Watch> watches_;
    u32  general_hart_ = 0;                            // `Hg`: whose registers and memory
    bool detached_ = false;
    bool killed_ = false;

    enum class Stop { None, Step, Breakpoint, Watch, Interrupt, Halted, Trap };
    Stop        stop_ = Stop::None;
    u32         stop_hart_ = 0;
    u64         stop_addr_ = 0;                        // watchpoint address
    int         signal_ = 5;                           // SIGTRAP

    // --- packet I/O ----------------------------------------------------------------
    bool send_packet(const std::string& payload);
    bool read_packet(std::string& payload, bool block);
    bool poll_interrupt_byte();

    // --- commands --------------------------------------------------------------------
    std::string read_registers();
    std::string read_register(u32 regnum);
    std::string write_register(u32 regnum, const std::string& hexvalue);
    std::string read_memory(u64 addr, u64 len);
    std::string write_memory(u64 addr, const std::string& hexdata);
    std::string breakpoint_command(bool insert, const std::string& args);
    std::string resume(char how, int hart);            // 'c' continue, 's' step
    std::string vcont(const std::string& args);
    std::string monitor(const std::string& command, std::vector<std::string>* console);
    std::string target_xml() const;

    Cpu*  hart_for(u32 thread_id);                     // gdb thread ids are hartid + 1
    bool  translate(Cpu& cpu, u64 vaddr, bool write, u64& paddr);
    bool  read_bytes(Cpu& cpu, u64 vaddr, u64 len, std::vector<u8>& out);
    bool  write_bytes(Cpu& cpu, u64 vaddr, const std::vector<u8>& data);
    void  skip_ebreak_if_stopped_on_one(Cpu& cpu);
    void  refresh_watches();
    bool  run_until_stop(int only_hart, bool single_step);
    void  classify_trap(const Trap& trap);
};
