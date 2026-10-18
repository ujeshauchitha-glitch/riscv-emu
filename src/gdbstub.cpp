#include "gdbstub.hpp"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <sstream>
#include <thread>

#include "csr.hpp"
#include "dram.hpp"
#include "mmu.hpp"

namespace {

// --- hex ---------------------------------------------------------------------------

constexpr char HEX_DIGITS[] = "0123456789abcdef";

std::string to_hex(const std::vector<u8>& bytes) {
    std::string s;
    s.reserve(bytes.size() * 2);
    for (u8 b : bytes) { s += HEX_DIGITS[b >> 4]; s += HEX_DIGITS[b & 15]; }
    return s;
}

std::string to_hex(const std::string& text) {
    return to_hex(std::vector<u8>(text.begin(), text.end()));
}

int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool from_hex(const std::string& s, std::vector<u8>& out) {
    if (s.size() % 2 != 0) return false;
    out.clear();
    for (std::size_t i = 0; i < s.size(); i += 2) {
        const int hi = hex_value(s[i]), lo = hex_value(s[i + 1]);
        if (hi < 0 || lo < 0) return false;
        out.push_back(static_cast<u8>((hi << 4) | lo));
    }
    return true;
}

// A 64-bit (or 32-bit) register as little-endian bytes, which is how gdb wants
// a RISC-V register on the wire.
std::string reg_hex(u64 value, unsigned bytes = 8) {
    std::vector<u8> v;
    for (unsigned i = 0; i < bytes; ++i) v.push_back(static_cast<u8>(value >> (8 * i)));
    return to_hex(v);
}

bool parse_u64(const std::string& s, u64& out) {
    if (s.empty()) return false;
    char* end = nullptr;
    out = std::strtoull(s.c_str(), &end, 16);
    return end != nullptr && *end == '\0';
}

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> parts;
    std::string cur;
    for (char c : s) {
        if (c == sep) { parts.push_back(cur); cur.clear(); }
        else cur += c;
    }
    parts.push_back(cur);
    return parts;
}

// --- register numbering ---------------------------------------------------------------
//
// gdb's RISC-V register numbers: 0-31 the integer registers, 32 the pc, 33-64 the
// floating-point registers, and 65 + n for control register number n. (Machine
// state like mstatus is a "register" to a debugger; satp is 65 + 0x180.)
constexpr u32 REG_PC      = 32;
constexpr u32 REG_FP_BASE = 33;
constexpr u32 REG_CSR_BASE = 65;

// The control registers worth naming in the target description. gdb shows any
// the description lists; the rest remain reachable through `monitor csr`.
struct CsrName { const char* name; u32 number; };
constexpr CsrName CSR_NAMES[] = {
    {"fflags", 0x001}, {"frm", 0x002}, {"fcsr", 0x003},
    {"sstatus", 0x100}, {"sie", 0x104}, {"stvec", 0x105}, {"scounteren", 0x106},
    {"sscratch", 0x140}, {"sepc", 0x141}, {"scause", 0x142}, {"stval", 0x143},
    {"sip", 0x144}, {"satp", 0x180},
    {"mstatus", 0x300}, {"misa", 0x301}, {"medeleg", 0x302}, {"mideleg", 0x303},
    {"mie", 0x304}, {"mtvec", 0x305}, {"mcounteren", 0x306},
    {"mscratch", 0x340}, {"mepc", 0x341}, {"mcause", 0x342}, {"mtval", 0x343}, {"mip", 0x344},
    {"cycle", 0xc00}, {"time", 0xc01}, {"instret", 0xc02},
    {"mhartid", 0xf14},
};

const char* const FP_NAMES[32] = {
    "ft0", "ft1", "ft2", "ft3", "ft4", "ft5", "ft6", "ft7", "fs0", "fs1", "fa0",
    "fa1", "fa2", "fa3", "fa4", "fa5", "fa6", "fa7", "fs2", "fs3", "fs4", "fs5",
    "fs6", "fs7", "fs8", "fs9", "fs10", "fs11", "ft8", "ft9", "ft10", "ft11"};

// Is this csr one of the three floating-point control registers that are only
// 32 bits wide in gdb's description?
bool csr_is_narrow(u32 csr) { return csr >= 0x001 && csr <= 0x003; }

int signal_for(Exception e) {
    switch (e) {
        case Exception::Breakpoint:
        case Exception::ECallFromUMode:
        case Exception::ECallFromSMode:
        case Exception::ECallFromMMode:
            return 5;   // SIGTRAP
        case Exception::IllegalInstruction:
            return 4;   // SIGILL
        case Exception::InstructionAddressMisaligned:
        case Exception::LoadAddressMisaligned:
        case Exception::StoreAMOAddressMisaligned:
            return 7;   // SIGBUS
        default:
            return 11;  // SIGSEGV
    }
}

}  // namespace

// ---------------------------------------------------------------------------------------

GdbStub::GdbStub(Platform& platform) : platform_(platform) { hostnet::init(); }

GdbStub::~GdbStub() {
    hostnet::close_socket(client_);
    hostnet::close_socket(listener_);
}

u16 GdbStub::listen(u16 port, u32 ip) {
    listener_ = hostnet::tcp_socket();
    if (listener_ == INVALID_SOCK) return 0;
    const u16 bound = hostnet::bind_ipv4(listener_, ip, port);
    if (bound == 0 || !hostnet::listen_on(listener_)) {
        hostnet::close_socket(listener_);
        listener_ = INVALID_SOCK;
        return 0;
    }
    return bound;
}

bool GdbStub::accept_client() {
    if (listener_ == INVALID_SOCK) return false;
    for (;;) {
        client_ = hostnet::accept_connection(listener_);
        if (client_ != INVALID_SOCK) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}

// --- packet framing -----------------------------------------------------------------------

std::string GdbStub::encode_packet(const std::string& payload) {
    unsigned sum = 0;
    for (unsigned char c : payload) sum += c;
    std::string out = "$" + payload + "#";
    out += HEX_DIGITS[(sum >> 4) & 15];
    out += HEX_DIGITS[sum & 15];
    return out;
}

bool GdbStub::send_packet(const std::string& payload) {
    last_sent_ = encode_packet(payload);
    std::size_t sent = 0;
    while (sent < last_sent_.size()) {
        const long n = hostnet::send_data(client_, reinterpret_cast<const u8*>(last_sent_.data()) + sent,
                                          last_sent_.size() - sent);
        if (n < 0) return false;
        if (n == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        sent += static_cast<std::size_t>(n);
    }
    return true;
}

bool GdbStub::read_packet(std::string& payload, bool block) {
    for (;;) {
        // Anything already buffered first.
        while (!inbuf_.empty()) {
            const char c = inbuf_.front();
            if (c == '+') { inbuf_.erase(0, 1); continue; }                // an acknowledgment
            if (c == '-') { inbuf_.erase(0, 1); send_packet(last_sent_.substr(1, last_sent_.size() - 4)); continue; }
            if (c != '$') { inbuf_.erase(0, 1); continue; }                // noise, or a stray Ctrl-C
            const std::size_t hash = inbuf_.find('#');
            if (hash == std::string::npos || inbuf_.size() < hash + 3) break;   // incomplete

            payload = inbuf_.substr(1, hash - 1);
            const int hi = hex_value(inbuf_[hash + 1]), lo = hex_value(inbuf_[hash + 2]);
            inbuf_.erase(0, hash + 3);
            unsigned sum = 0;
            for (unsigned char ch : payload) sum += ch;
            if (hi < 0 || lo < 0 || ((hi << 4) | lo) != static_cast<int>(sum & 0xff)) {
                const char nak = '-';
                hostnet::send_data(client_, reinterpret_cast<const u8*>(&nak), 1);
                continue;
            }
            const char ack = '+';
            hostnet::send_data(client_, reinterpret_cast<const u8*>(&ack), 1);
            return true;
        }

        u8 buf[4096];
        const long n = hostnet::recv_data(client_, buf, sizeof buf);
        if (n > 0) { inbuf_.append(reinterpret_cast<char*>(buf), static_cast<std::size_t>(n)); continue; }
        if (n < 0) return false;                                           // closed or failed
        if (!block) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

// While the target runs, the only thing gdb sends is a bare 0x03 to stop it.
bool GdbStub::poll_interrupt_byte() {
    if (client_ == INVALID_SOCK) return false;
    u8 buf[256];
    const long n = hostnet::recv_data(client_, buf, sizeof buf);
    if (n == -1 || n == -2) return true;                  // the debugger went away: stop
    bool interrupted = false;
    for (long i = 0; i < n; ++i) {
        if (buf[i] == 0x03) interrupted = true;
        else inbuf_ += static_cast<char>(buf[i]);
    }
    return interrupted;
}

bool GdbStub::serve() {
    for (;;) {
        std::string payload;
        if (!read_packet(payload, /*block=*/true)) { killed_ = true; return false; }

        std::vector<std::string> console;
        const std::string reply = handle_packet(payload, &console);
        for (const std::string& line : console) send_packet("O" + to_hex(line));

        if (killed_) return false;
        if (!send_packet(reply)) { killed_ = true; return false; }
        if (detached_) return true;                       // let the guest run on
        if (!reply.empty() && reply[0] == 'W') return false;   // the machine exited
    }
}

// --- translating the guest's addresses ------------------------------------------------------

Cpu* GdbStub::hart_for(u32 thread_id) {
    if (thread_id == 0 || thread_id > platform_.num_harts()) return nullptr;
    return &platform_.hart(thread_id - 1);
}

// gdb speaks virtual addresses. Resolve one the way the hart itself would, but
// report a failure instead of raising the page fault into the guest.
bool GdbStub::translate(Cpu& cpu, u64 vaddr, bool write, u64& paddr) {
    const AccessType primary = write ? AccessType::Store : AccessType::Load;
    Result<u64> pa = cpu.mmu.translate(vaddr, primary, cpu.priv, cpu.csrs);
    // A debugger wants to read code that is execute-only, which a load refuses.
    if (!pa && !write) pa = cpu.mmu.translate(vaddr, AccessType::Instruction, cpu.priv, cpu.csrs);
    if (!pa) return false;
    paddr = pa.value;
    return true;
}

bool GdbStub::read_bytes(Cpu& cpu, u64 vaddr, u64 len, std::vector<u8>& out) {
    Dram* ram = platform_.bus().ram();
    if (ram == nullptr) return false;
    out.assign(static_cast<std::size_t>(len), 0);
    for (u64 i = 0; i < len;) {
        u64 pa;
        if (!translate(cpu, vaddr + i, false, pa)) return false;
        // Stay inside one page per translation.
        const u64 chunk = std::min<u64>(len - i, PAGE_SIZE - ((vaddr + i) & (PAGE_SIZE - 1)));
        // Device registers are refused: reading some of them (a UART's receive
        // register, the PLIC's claim register) changes the machine, and a debugger
        // that merely looks must not.
        if (!ram->read_bytes(pa, out.data() + i, chunk)) return false;
        i += chunk;
    }
    return true;
}

bool GdbStub::write_bytes(Cpu& cpu, u64 vaddr, const std::vector<u8>& data) {
    Dram* ram = platform_.bus().ram();
    if (ram == nullptr) return false;
    for (u64 i = 0; i < data.size();) {
        u64 pa;
        if (!translate(cpu, vaddr + i, true, pa)) return false;
        const u64 chunk = std::min<u64>(data.size() - i, PAGE_SIZE - ((vaddr + i) & (PAGE_SIZE - 1)));
        if (!ram->write_bytes(pa, data.data() + i, chunk)) return false;
        i += chunk;
    }
    return true;
}

// --- registers ------------------------------------------------------------------------------

std::string GdbStub::read_registers() {
    Cpu* cpu = hart_for(general_hart_ + 1);
    if (cpu == nullptr) return "E01";
    // Only the 32 integer registers and the pc, exactly as QEMU's stub does. gdb
    // fetches the floating-point and control registers one at a time (`p`) when
    // it needs them, which keeps this reply small enough to be sent on every stop.
    std::string out;
    for (u32 i = 0; i < NUM_REGS; ++i) out += reg_hex(cpu->read_reg(i));
    out += reg_hex(cpu->pc);
    return out;
}

std::string GdbStub::read_register(u32 regnum) {
    Cpu* cpu = hart_for(general_hart_ + 1);
    if (cpu == nullptr) return "E01";
    if (regnum < NUM_REGS) return reg_hex(cpu->read_reg(regnum));
    if (regnum == REG_PC)  return reg_hex(cpu->pc);
    if (regnum >= REG_FP_BASE && regnum < REG_FP_BASE + 32) return reg_hex(cpu->fregs[regnum - REG_FP_BASE]);
    if (regnum >= REG_CSR_BASE && regnum < REG_CSR_BASE + 4096) {
        const u32 csr = regnum - REG_CSR_BASE;
        if (!cpu->csrs.exists(csr)) return "E02";
        return reg_hex(cpu->csrs.read(csr), csr_is_narrow(csr) ? 4 : 8);
    }
    return "E02";
}

std::string GdbStub::write_register(u32 regnum, const std::string& hexvalue) {
    Cpu* cpu = hart_for(general_hart_ + 1);
    if (cpu == nullptr) return "E01";
    std::vector<u8> bytes;
    if (!from_hex(hexvalue, bytes) || bytes.empty() || bytes.size() > 8) return "E03";
    u64 value = 0;
    for (std::size_t i = 0; i < bytes.size(); ++i) value |= static_cast<u64>(bytes[i]) << (8 * i);

    if (regnum < NUM_REGS)  { cpu->write_reg(regnum, value); return "OK"; }
    if (regnum == REG_PC)   { cpu->pc = value; return "OK"; }
    if (regnum >= REG_FP_BASE && regnum < REG_FP_BASE + 32) { cpu->fregs[regnum - REG_FP_BASE] = value; return "OK"; }
    if (regnum >= REG_CSR_BASE && regnum < REG_CSR_BASE + 4096) {
        const u32 csr = regnum - REG_CSR_BASE;
        if (!cpu->csrs.exists(csr)) return "E02";
        cpu->csrs.write(csr, value);
        return "OK";
    }
    return "E02";
}

// --- memory ----------------------------------------------------------------------------------

std::string GdbStub::read_memory(u64 addr, u64 len) {
    Cpu* cpu = hart_for(general_hart_ + 1);
    if (cpu == nullptr) return "E01";
    len = std::min<u64>(len, 0x10000);
    std::vector<u8> data;
    if (!read_bytes(*cpu, addr, len, data)) return "E14";
    return to_hex(data);
}

std::string GdbStub::write_memory(u64 addr, const std::string& hexdata) {
    Cpu* cpu = hart_for(general_hart_ + 1);
    if (cpu == nullptr) return "E01";
    std::vector<u8> data;
    if (!from_hex(hexdata, data)) return "E03";
    return write_bytes(*cpu, addr, data) ? "OK" : "E14";
}

// --- breakpoints and watchpoints ----------------------------------------------------------------

std::string GdbStub::breakpoint_command(bool insert, const std::string& args) {
    // "type,addr,kind" - kind is the instruction size for a breakpoint, the byte
    // length for a watchpoint.
    const std::vector<std::string> f = split(args, ',');
    if (f.size() < 3 || f[0].size() != 1) return "E01";
    u64 addr, kind;
    if (!parse_u64(f[1], addr) || !parse_u64(f[2].substr(0, f[2].find(';')), kind)) return "E01";

    switch (f[0][0]) {
        case '0':    // software breakpoint
        case '1':    // hardware breakpoint - to an emulator, the same thing
            if (insert) breakpoints_.insert(addr);
            else        breakpoints_.erase(addr);
            return "OK";
        case '2': {  // write watchpoint
            if (kind == 0 || kind > 64) return "E01";
            if (insert) {
                Watch w{addr, kind, {}};
                Cpu* cpu = hart_for(general_hart_ + 1);
                if (cpu == nullptr || !read_bytes(*cpu, addr, kind, w.last)) return "E14";
                watches_.push_back(w);
            } else {
                watches_.erase(std::remove_if(watches_.begin(), watches_.end(),
                                              [&](const Watch& w) { return w.addr == addr && w.len == kind; }),
                               watches_.end());
            }
            return "OK";
        }
        default:     // read and access watchpoints would need a hook on every load
            return "";
    }
}

void GdbStub::refresh_watches() {
    Cpu* cpu = hart_for(general_hart_ + 1);
    if (cpu == nullptr) return;
    for (Watch& w : watches_) read_bytes(*cpu, w.addr, w.len, w.last);
}

// --- running -------------------------------------------------------------------------------------

void GdbStub::classify_trap(const Trap& trap) {
    stop_ = Stop::Trap;
    signal_ = signal_for(trap.cause);
}

// An EBREAK the program put there itself (the end of a test, say) is a trap the
// emulator reports as a stop with nowhere to go. Resuming from it should move on
// rather than hit the same instruction forever.
void GdbStub::skip_ebreak_if_stopped_on_one(Cpu& cpu) {
    if (stop_ != Stop::Trap) return;
    std::vector<u8> insn;
    if (!read_bytes(cpu, cpu.pc, 4, insn)) return;
    const u32 word = insn[0] | (insn[1] << 8) | (insn[2] << 16) | (static_cast<u32>(insn[3]) << 24);
    if (word == 0x00100073) cpu.pc += 4;                  // ebreak
    else if ((word & 0xffff) == 0x9002) cpu.pc += 2;      // c.ebreak
}

bool GdbStub::run_until_stop(int only_hart, bool single_step) {
    if (platform_.halted()) { stop_ = Stop::Halted; return false; }

    if (single_step) {
        const u32 h = only_hart >= 0 ? static_cast<u32>(only_hart) : general_hart_;
        Cpu& cpu = platform_.hart(h);
        skip_ebreak_if_stopped_on_one(cpu);
        Status st = platform_.step_hart(h);
        stop_hart_ = h;
        if (!st)                      { classify_trap(st.trap); stop_hart_ = platform_.fault_hart(); }
        else if (platform_.halted())  { stop_ = Stop::Halted; }
        else                          { stop_ = Stop::Step; signal_ = 5; }
        return true;
    }

    for (u32 h = 0; h < platform_.num_harts(); ++h) skip_ebreak_if_stopped_on_one(platform_.hart(h));
    refresh_watches();

    // A hart sitting on a breakpoint must be allowed to leave it: the check runs
    // before the instruction, so without this the first step would stop at once.
    std::set<u32> leaving;
    for (u32 h = 0; h < platform_.num_harts(); ++h) {
        if (breakpoints_.count(platform_.hart(h).pc)) leaving.insert(h);
    }

    u64 calls = 0;
    platform_.before_step = [&](Cpu& c) -> bool {
        if (++calls % interrupt_poll_interval == 0) {
            const bool stop = interrupt_requested ? interrupt_requested() : poll_interrupt_byte();
            if (stop) { stop_ = Stop::Interrupt; stop_hart_ = c.hartid; signal_ = 2; return true; }
        }
        for (Watch& w : watches_) {
            std::vector<u8> now;
            if (read_bytes(c, w.addr, w.len, now) && now != w.last) {
                w.last = now;
                stop_ = Stop::Watch; stop_hart_ = c.hartid; stop_addr_ = w.addr; signal_ = 5;
                return true;
            }
        }
        if (leaving.erase(c.hartid) == 0 && breakpoints_.count(c.pc)) {
            stop_ = Stop::Breakpoint; stop_hart_ = c.hartid; signal_ = 5;
            return true;
        }
        return false;
    };

    Status st = platform_.run(platform_.retired() + (1ull << 60), nullptr);
    platform_.before_step = nullptr;

    if (platform_.hook_stopped()) return true;
    if (!st) { classify_trap(st.trap); stop_hart_ = platform_.fault_hart(); return true; }
    if (platform_.halted()) { stop_ = Stop::Halted; return false; }
    stop_ = Stop::Interrupt; signal_ = 2;                 // ran out of budget: treat as a stop
    return true;
}

std::string GdbStub::resume(char how, int hart) {
    run_until_stop(hart, how == 's');
    return stop_reply();
}

std::string GdbStub::vcont(const std::string& args) {
    // vCont;a1[:tid];a2[:tid]... Each action is c, s (optionally with a signal
    // number we ignore) and applies to the named thread, or to the rest.
    int step_hart = -1;
    bool step = false;
    for (const std::string& action : split(args, ';')) {
        if (action.empty()) continue;
        const char op = action[0];
        if (op != 's' && op != 'S') continue;
        step = true;
        const std::size_t colon = action.find(':');
        if (colon != std::string::npos) {
            u64 tid;
            std::string t = action.substr(colon + 1);
            if (!t.empty() && t[0] == 'p') t = t.substr(t.find('.') + 1);   // pPID.TID form
            if (parse_u64(t, tid) && tid >= 1) step_hart = static_cast<int>(tid - 1);
        }
    }
    return resume(step ? 's' : 'c', step_hart);
}

std::string GdbStub::stop_reply() const {
    char buf[96];
    switch (stop_) {
        case Stop::Halted:
            return "W00";
        case Stop::Watch:
            std::snprintf(buf, sizeof buf, "T05watch:%llx;thread:%x;",
                          static_cast<unsigned long long>(stop_addr_), stop_hart_ + 1);
            return buf;
        case Stop::None:
            std::snprintf(buf, sizeof buf, "T05thread:%x;", general_hart_ + 1);
            return buf;
        default:
            std::snprintf(buf, sizeof buf, "T%02xthread:%x;", signal_, stop_hart_ + 1);
            return buf;
    }
}

// --- monitor commands -----------------------------------------------------------------------------

std::string GdbStub::monitor(const std::string& command, std::vector<std::string>* console) {
    auto say = [&](const std::string& line) { if (console) console->push_back(line + "\n"); };
    std::istringstream words(command);
    std::string verb;
    words >> verb;

    if (verb == "help" || verb.empty()) {
        say("monitor harts            list the harts and what each is doing");
        say("monitor csr NAME|NUMBER  print a control register of the selected hart");
        say("monitor time             print the machine clock");
        return "OK";
    }
    if (verb == "harts") {
        for (u32 h = 0; h < platform_.num_harts(); ++h) {
            Cpu& c = platform_.hart(h);
            char line[160];
            std::snprintf(line, sizeof line, "hart %u: pc=0x%llx priv=%s%s%s%s", h,
                          static_cast<unsigned long long>(c.pc), privilege_name(c.priv),
                          c.parked ? " (parked)" : "", c.waiting_for_interrupt ? " (wfi)" : "",
                          h == general_hart_ ? "  <- selected" : "");
            say(line);
        }
        return "OK";
    }
    if (verb == "csr") {
        std::string which;
        words >> which;
        u32 number = 0;
        bool found = false;
        for (const CsrName& n : CSR_NAMES) {
            if (which == n.name) { number = n.number; found = true; }
        }
        if (!found) {
            char* end = nullptr;
            number = static_cast<u32>(std::strtoul(which.c_str(), &end, 0));
            found = !which.empty() && end != nullptr && *end == '\0' && number < 4096;
        }
        Cpu* cpu = hart_for(general_hart_ + 1);
        if (!found || cpu == nullptr || !cpu->csrs.exists(number)) {
            say("no such control register: " + which);
        } else {
            char line[96];
            std::snprintf(line, sizeof line, "%s (0x%x) = 0x%llx", which.c_str(), number,
                          static_cast<unsigned long long>(cpu->csrs.read(number)));
            say(line);
        }
        return "OK";
    }
    if (verb == "time") {
        char line[64];
        std::snprintf(line, sizeof line, "%llu", static_cast<unsigned long long>(
                          platform_.boot_hart().clint ? platform_.boot_hart().clint->mtime() : 0));
        say(line);
        return "OK";
    }
    say("unknown monitor command '" + verb + "' (try: monitor help)");
    return "OK";
}

// --- the target description ------------------------------------------------------------------------

std::string GdbStub::target_xml() const {
    static const char* const GPR_NAMES[32] = {
        "zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2", "fp", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
        "a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7", "s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"};

    std::string x =
        "<?xml version=\"1.0\"?>\n"
        "<!DOCTYPE target SYSTEM \"gdb-target.dtd\">\n"
        "<target version=\"1.0\">\n"
        "  <architecture>riscv:rv64</architecture>\n"
        "  <feature name=\"org.gnu.gdb.riscv.cpu\">\n";
    for (u32 i = 0; i < 32; ++i) {
        const char* type = (i == 1) ? "code_ptr" : (i == 2 || i == 3 || i == 4 || i == 8) ? "data_ptr" : "int";
        x += std::string("    <reg name=\"") + GPR_NAMES[i] + "\" bitsize=\"64\" type=\"" + type + "\" regnum=\"" +
             std::to_string(i) + "\"/>\n";
    }
    x += "    <reg name=\"pc\" bitsize=\"64\" type=\"code_ptr\" regnum=\"32\"/>\n  </feature>\n";

    x += "  <feature name=\"org.gnu.gdb.riscv.fpu\">\n";
    for (u32 i = 0; i < 32; ++i) {
        x += std::string("    <reg name=\"") + FP_NAMES[i] + "\" bitsize=\"64\" type=\"ieee_double\" regnum=\"" +
             std::to_string(REG_FP_BASE + i) + "\"/>\n";
    }
    x += "    <reg name=\"fflags\" bitsize=\"32\" type=\"int\" regnum=\"66\"/>\n"
         "    <reg name=\"frm\" bitsize=\"32\" type=\"int\" regnum=\"67\"/>\n"
         "    <reg name=\"fcsr\" bitsize=\"32\" type=\"int\" regnum=\"68\"/>\n"
         "  </feature>\n";

    x += "  <feature name=\"org.gnu.gdb.riscv.csr\">\n";
    for (const CsrName& n : CSR_NAMES) {
        if (csr_is_narrow(n.number)) continue;       // declared with the fpu above
        x += std::string("    <reg name=\"") + n.name + "\" bitsize=\"64\" type=\"int\" regnum=\"" +
             std::to_string(REG_CSR_BASE + n.number) + "\"/>\n";
    }
    x += "  </feature>\n</target>\n";
    return x;
}

// --- the dispatcher ---------------------------------------------------------------------------------

std::string GdbStub::handle_packet(const std::string& p, std::vector<std::string>* console) {
    if (p.empty()) return "";
    const char cmd = p[0];
    const std::string rest = p.substr(1);

    switch (cmd) {
        case '?':
            return stop_reply();

        case 'g': return read_registers();
        case 'G': {
            Cpu* cpu = hart_for(general_hart_ + 1);
            std::vector<u8> bytes;
            if (cpu == nullptr || !from_hex(rest, bytes) || bytes.size() < 33 * 8) return "E03";
            for (u32 i = 0; i < 33; ++i) {
                u64 v = 0;
                for (u32 b = 0; b < 8; ++b) v |= static_cast<u64>(bytes[i * 8 + b]) << (8 * b);
                if (i < 32) cpu->write_reg(i, v); else cpu->pc = v;
            }
            return "OK";
        }
        case 'p': {
            u64 n;
            return parse_u64(rest, n) ? read_register(static_cast<u32>(n)) : "E01";
        }
        case 'P': {
            const std::size_t eq = rest.find('=');
            u64 n;
            if (eq == std::string::npos || !parse_u64(rest.substr(0, eq), n)) return "E01";
            return write_register(static_cast<u32>(n), rest.substr(eq + 1));
        }

        case 'm': {
            const std::vector<std::string> f = split(rest, ',');
            u64 addr, len;
            if (f.size() != 2 || !parse_u64(f[0], addr) || !parse_u64(f[1], len)) return "E01";
            return read_memory(addr, len);
        }
        case 'M': {
            const std::size_t colon = rest.find(':');
            if (colon == std::string::npos) return "E01";
            const std::vector<std::string> f = split(rest.substr(0, colon), ',');
            u64 addr, len;
            if (f.size() != 2 || !parse_u64(f[0], addr) || !parse_u64(f[1], len)) return "E01";
            return write_memory(addr, rest.substr(colon + 1));
        }

        case 'Z': return breakpoint_command(true, rest);
        case 'z': return breakpoint_command(false, rest);

        case 'c': {
            u64 addr;
            if (!rest.empty() && parse_u64(rest, addr)) platform_.hart(general_hart_).pc = addr;
            return resume('c', -1);
        }
        case 's': {
            u64 addr;
            if (!rest.empty() && parse_u64(rest, addr)) platform_.hart(general_hart_).pc = addr;
            return resume('s', -1);
        }

        case 'D':
            detached_ = true;
            return "OK";
        case 'k':
            killed_ = true;
            return "";

        case 'H': {
            // Hg<tid> picks the thread for register and memory access; Hc<tid>
            // for execution, which this all-stop stub does not distinguish.
            if (rest.empty()) return "E01";
            if (rest[0] == 'g') {
                const std::string t = rest.substr(1);
                if (t == "-1" || t == "0") return "OK";
                u64 tid;
                if (!parse_u64(t, tid) || hart_for(static_cast<u32>(tid)) == nullptr) return "E01";
                general_hart_ = static_cast<u32>(tid - 1);
            }
            return "OK";
        }
        case 'T': {
            u64 tid;
            return (parse_u64(rest, tid) && hart_for(static_cast<u32>(tid)) != nullptr) ? "OK" : "E01";
        }

        case 'v':
            if (p == "vCont?") return "vCont;c;C;s;S";
            if (p.compare(0, 6, "vCont;") == 0) return vcont(p.substr(6));
            if (p == "vMustReplyEmpty") return "";
            return "";

        case 'q': {
            if (p.compare(0, 10, "qSupported") == 0) {
                return "PacketSize=8000;qXfer:features:read+;swbreak+;hwbreak+;vContSupported+";
            }
            if (p == "qAttached") return "1";
            if (p == "qC") {
                char buf[24];
                std::snprintf(buf, sizeof buf, "QC%x", general_hart_ + 1);
                return buf;
            }
            if (p == "qfThreadInfo") {
                std::string out = "m";
                for (u32 h = 0; h < platform_.num_harts(); ++h) {
                    char buf[16];
                    std::snprintf(buf, sizeof buf, "%s%x", h ? "," : "", h + 1);
                    out += buf;
                }
                return out;
            }
            if (p == "qsThreadInfo") return "l";
            if (p.compare(0, 16, "qThreadExtraInfo") == 0) {
                u64 tid = 0;
                const std::size_t comma = p.find(',');
                if (comma != std::string::npos) parse_u64(p.substr(comma + 1), tid);
                Cpu* c = hart_for(static_cast<u32>(tid));
                if (c == nullptr) return "E01";
                return to_hex(std::string("hart ") + std::to_string(c->hartid) + " (" + privilege_name(c->priv) + ")" +
                              (c->parked ? ", parked" : ""));
            }
            if (p.compare(0, 6, "qRcmd,") == 0) {
                std::vector<u8> bytes;
                if (!from_hex(p.substr(6), bytes)) return "E01";
                return monitor(std::string(bytes.begin(), bytes.end()), console);
            }
            if (p.compare(0, 30, "qXfer:features:read:target.xml") == 0) {
                // qXfer:features:read:target.xml:OFFSET,LENGTH
                const std::size_t colon = p.rfind(':');
                const std::vector<std::string> ol = split(p.substr(colon + 1), ',');
                u64 off, len;
                if (ol.size() != 2 || !parse_u64(ol[0], off) || !parse_u64(ol[1], len)) return "E01";
                const std::string xml = target_xml();
                if (off >= xml.size()) return "l";
                const std::string chunk = xml.substr(static_cast<std::size_t>(off), static_cast<std::size_t>(len));
                return std::string(off + len >= xml.size() ? "l" : "m") + chunk;
            }
            return "";                                    // qOffsets, qSymbol, qTStatus, ...
        }

        default:
            return "";                                    // unsupported: an empty reply says so
    }
}
