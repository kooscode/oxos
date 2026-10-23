// The fake CPU, MMU and devices of the host unit tests (tests/unit/README.md).
//
// The kernel's core sources run unchanged in a 32-bit Linux process. Two things make that work:
//  - Privileged instructions (cli, sti, hlt, in/out, invlpg, wbinvd, mov to/from CR and DR registers, rdmsr/wrmsr,
//    lgdt/lidt/ltr) fault with SIGSEGV in user mode. The handler decodes the instruction, applies it to the fake
//    CPU state below and resumes after it.
//  - The kernel's address windows (identity/title space, the 0x80000000 physical map, the page-table self-map, the
//    system PTE range, the GPU registers) are reserved PROT_NONE. An access faults; the handler walks the kernel's
//    own page tables from the fake CR3 and maps the physical page there (a TLB fill). invlpg and CR3 writes unmap
//    again (a TLB flush), so a missing invalidation behaves as on real hardware: the stale translation stays.
// Interrupts are delivered synchronously, when the fake interrupt flag turns on with an IRQ pending, at hlt, and
// when a test advances the fake time; the kernel's dispatcher then runs on the interrupted stack via a trampoline.
#include "host.h"

#include <cerrno>
#include <string>
#include <csetjmp>
#include <csignal>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

extern "C" {
volatile uint32_t HostIf; // EFLAGS.IF of the fake CPU (0x200 = interrupts enabled); also used by host_asm.S
void host_irq_trampoline();
void KiDispatchHardwareInterrupt(unsigned long irq);
}

namespace host {

volatile uint32_t Cr0 = 0x80010031, Cr2, Cr3 = 0xF000, Cr4 = 0x610;
volatile uint64_t PageFills, TlbFlushes, ClockInterrupts;
volatile uint32_t KernelFaultAddress;

static int PhysFd = -1, DevFd = -1;
static uint8_t *PhysView;
constexpr uint32_t DEV_BASE = 0xFD000000, DEV_SIZE = 0x01000000; // the GPU register window (zeros, writable)

uint8_t *Phys(uint32_t pa) { return PhysView + pa; }

// --- Kernel address windows reserved for the fake MMU.
struct Window {
    uint32_t start, end;
};
static const Window Windows[] = {
    {0x00010000, 0x08000000}, // title space, low part (the test binary is at 0x08048000)
    {0x70000000, 0x80000000}, // title space, top part (top-down allocations)
    {0x80000000, 0x90000000}, // physical map
    {0xC0000000, 0xC0400000}, // page-table self-map
    {0xD0000000, 0xD8000000}, // system PTE range, its first 128 MB (committed from the bottom up)
    {DEV_BASE, DEV_BASE + DEV_SIZE},
};
static bool InWindow(uint32_t va) {
    for (auto &w : Windows)
        if (va >= w.start && va < w.end)
            return true;
    return false;
}

// Pages currently mapped (a TLB entry exists): one bit per 4 KB page.
static uint32_t Mapped[1u << 15];
static void Unmap(uint32_t page) {
    mmap((void *)page, 4096, PROT_NONE, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    Mapped[page >> 17] &= ~(1u << ((page >> 12) & 31));
}

void FlushTlb() {
    TlbFlushes = TlbFlushes + 1;
    for (uint32_t w = 0; w < (1u << 15); w++)
        while (Mapped[w]) {
            uint32_t bit = __builtin_ctz(Mapped[w]);
            Unmap((w << 17) | (bit << 12));
        }
}

// Walk the kernel's page tables (physical, from CR3). Returns false for a not-present translation.
static bool Translate(uint32_t va, uint32_t *pa, bool *writable) {
    auto pd = (uint32_t *)Phys(Cr3 & ~0xFFFu);
    uint32_t pde = pd[va >> 22];
    if (!(pde & 1))
        return false;
    if (pde & 0x80) {
        *pa = (pde & 0xFFC00000) | (va & 0x3FF000);
        *writable = pde & 2;
        return true;
    }
    if ((pde & ~0xFFFu) >= PHYS_SIZE)
        return false;
    uint32_t pte = ((uint32_t *)Phys(pde & ~0xFFFu))[(va >> 12) & 0x3FF];
    if (!(pte & 1))
        return false;
    *pa = pte & ~0xFFFu;
    *writable = (pde & 2) && (pte & 2);
    return true;
}

// Map the page holding `va` as the kernel's page tables say (a TLB fill). False: a genuine page fault.
static bool Fill(uint32_t va, bool write) {
    uint32_t pa;
    bool writable;
    if (!Translate(va, &pa, &writable) || (write && !writable))
        return false;
    uint32_t page = va & ~0xFFFu;
    int prot = PROT_READ | PROT_EXEC | (writable ? PROT_WRITE : 0);
    void *r;
    if (pa >= PHYS_SIZE && pa < 2 * PHYS_SIZE) // a 64 MB console: the address wraps
        pa -= PHYS_SIZE;
    if (pa < PHYS_SIZE)
        r = mmap((void *)page, 4096, prot, MAP_FIXED | MAP_SHARED, PhysFd, pa);
    else if (pa >= DEV_BASE && pa < DEV_BASE + DEV_SIZE)
        r = mmap((void *)page, 4096, prot, MAP_FIXED | MAP_SHARED, DevFd, pa - DEV_BASE);
    else // no device: reads as zeros
        r = mmap((void *)page, 4096, prot, MAP_FIXED | MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (r == MAP_FAILED)
        return false;
    Mapped[page >> 17] |= 1u << ((page >> 12) & 31);
    PageFills = PageFills + 1;
    return true;
}

// --- Fake devices behind the I/O ports.
static uint8_t PicImr[2] = {0xFF, 0xFF}, PicInit[2], PicReadIsr[2];
static uint8_t Cmos[256], CmosIndex;
static uint32_t PciAddress;
static volatile uint32_t PendingIrqs;
static volatile uint64_t NowNs;
volatile uint64_t IdleTicksThisTest;
static uint8_t Elcr[2];
static uint32_t HostBridge84 = 0x07FFFFFF;

uint64_t NowMs() { return NowNs / 1000000; }

static uint32_t PortIn(uint16_t port, int size) {
    switch (port) {
    case 0x21: return PicImr[0];
    case 0xA1: return PicImr[1];
    case 0x20: case 0xA0: return 0; // ISR / IRR: nothing in service
    case 0x4D0: return Elcr[0];
    case 0x4D1: return Elcr[1];
    case 0x71: return Cmos[CmosIndex];
    case 0x8008: return (uint32_t)(NowNs * 27 / 8000); // ACPI PM timer, 3.375 MHz
    case 0xCFC:
        if (PciAddress == 0x80000084)
            return HostBridge84;
        return 0xFFFFFFFF;
    default: return size == 1 ? 0xFF : size == 2 ? 0xFFFF : 0xFFFFFFFF;
    }
}

static void PortOut(uint16_t port, uint32_t v, int size) {
    switch (port) {
    case 0x20: case 0xA0: {
        int p = port == 0xA0;
        if (v & 0x10)
            PicInit[p] = 3; // ICW1: ICW2..4 follow on the data port
        else if (v == 0x0B)
            PicReadIsr[p] = 1;
        break;
    }
    case 0x21: case 0xA1: {
        int p = port == 0xA1;
        if (PicInit[p])
            PicInit[p]--;
        else
            PicImr[p] = (uint8_t)v;
        break;
    }
    case 0x4D0: Elcr[0] = (uint8_t)v; break;
    case 0x4D1: Elcr[1] = (uint8_t)v; break;
    case 0x70: CmosIndex = v & 0x7F; break;
    case 0x72: CmosIndex = (uint8_t)v; break;
    case 0x71: Cmos[CmosIndex] = (uint8_t)v; break;
    case 0xCF8: PciAddress = v; break;
    case 0xCFC:
        if (PciAddress == 0x80000084)
            HostBridge84 = v;
        break;
    default: break;
    }
    (void)size;
}

// --- MSRs (a small table; unknown ones read as 0).
static struct {
    uint32_t msr;
    uint64_t value;
} Msrs[64];
static uint64_t ReadMsr(uint32_t msr) {
    for (auto &m : Msrs)
        if (m.msr == msr && msr)
            return m.value;
    return 0;
}
static void WriteMsr(uint32_t msr, uint64_t v) {
    for (auto &m : Msrs)
        if (m.msr == msr || !m.msr) {
            m.msr = msr;
            m.value = v;
            return;
        }
}

// --- Interrupts.
static bool Deliverable(unsigned *irq) {
    uint32_t mask = PicImr[0] | (uint32_t)PicImr[1] << 8;
    if ((mask & 0xFF00) != 0xFF00 && (PicImr[0] & 4))
        mask |= 0xFF00; // the cascade is masked: no slave IRQ gets through
    uint32_t ready = PendingIrqs & ~mask;
    if (!ready)
        return false;
    *irq = __builtin_ctz(ready);
    return true;
}

bool InterruptsEnabled() { return HostIf & 0x200; }

void RaiseIrq(unsigned irq) { __atomic_or_fetch(&PendingIrqs, 1u << irq, __ATOMIC_SEQ_CST); }

// Called with the fake IF on, in normal (non-signal) context on the interrupted stack.
extern "C" void HostDeliverInterrupts() {
    unsigned irq;
    while ((HostIf & 0x200) && Deliverable(&irq)) {
        __atomic_and_fetch(&PendingIrqs, ~(1u << irq), __ATOMIC_SEQ_CST);
        HostIf = 0; // an interrupt gate
        if (irq == 0)
            ClockInterrupts = ClockInterrupts + 1;
        KiDispatchHardwareInterrupt(irq);
        HostIf = 0x200; // iret
    }
}

static void Tick() {
    NowNs = NowNs + 1000000;
    RaiseIrq(0);
}

void SkipTime(uint64_t ms) { NowNs = NowNs + ms * 1000000; }

void AdvanceTime(uint32_t ms) {
    for (uint32_t i = 0; i < ms; i++) {
        Tick();
        HostDeliverInterrupts();
    }
}

// --- Kernel log (XK_SERIAL).
static char Log[1 << 16];
static size_t LogLength;
extern "C" void HostLogWrite(const char *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        if (LogLength < sizeof(Log) - 1)
            Log[LogLength++] = s[i];
    }
    if (getenv("OXOS_TEST_LOG"))
        fwrite(s, 1, n, stderr);
}
const char *TakeLog() {
    static char copy[sizeof(Log)];
    memcpy(copy, Log, LogLength);
    copy[LogLength] = 0;
    LogLength = 0;
    return copy;
}

// --- The fault handler.
enum { R_EAX, R_ECX, R_EDX, R_EBX, R_ESP, R_EBP, R_ESI, R_EDI };
static greg_t &Reg(ucontext_t *uc, int r) {
    static const int map[8] = {REG_EAX, REG_ECX, REG_EDX, REG_EBX, REG_ESP, REG_EBP, REG_ESI, REG_EDI};
    return uc->uc_mcontext.gregs[map[r]];
}

// Effective address of a memory operand (32-bit addressing); returns the bytes used after the ModRM byte.
static int Modrm(ucontext_t *uc, const uint8_t *p, uint32_t *ea) {
    uint8_t m = p[0], mod = m >> 6, rm = m & 7;
    int len = 1;
    uint32_t addr = 0;
    if (rm == 4) {
        uint8_t sib = p[1], scale = sib >> 6, idx = (sib >> 3) & 7, base = sib & 7;
        len++;
        if (idx != 4)
            addr += (uint32_t)Reg(uc, idx) << scale;
        if (base == 5 && mod == 0) {
            addr += *(const uint32_t *)(p + len);
            len += 4;
        } else {
            addr += (uint32_t)Reg(uc, base);
        }
    } else if (rm == 5 && mod == 0) {
        addr = *(const uint32_t *)(p + 1);
        len += 4;
    } else {
        addr = (uint32_t)Reg(uc, rm);
    }
    if (mod == 1) {
        addr += (int8_t)p[len];
        len += 1;
    } else if (mod == 2) {
        addr += *(const uint32_t *)(p + len);
        len += 4;
    }
    *ea = addr;
    return len;
}

extern "C" char __executable_start[], etext[];
static bool InText(uint32_t a) { return a >= (uint32_t)(uintptr_t)__executable_start && a < (uint32_t)(uintptr_t)etext; }

// A stack word for the report: through the fake MMU for kernel windows, directly elsewhere.
static bool ReadWord(uint32_t a, uint32_t *v) {
    if (InWindow(a)) {
        uint32_t pa;
        bool w;
        if (!Translate(a, &pa, &w) || pa >= PHYS_SIZE)
            return false;
        *v = *(uint32_t *)Phys(pa + (a & 0xFFF));
        return true;
    }
    if (InText(a) || (a >= (uint32_t)(uintptr_t)etext && a < 0x10000000) || a >= 0xF0000000) {
        *v = *(volatile uint32_t *)(uintptr_t)a;
        return true;
    }
    return false;
}

extern std::string CurrentTestName;
[[noreturn]] static void Die(ucontext_t *uc, const char *what, uint32_t addr) {
    char msg[512];
    if (!CurrentTestName.empty()) {
        int m = snprintf(msg, sizeof(msg), "\n*** in test %s", CurrentTestName.c_str());
        write(2, msg, m);
    }
    uint32_t esp = (uint32_t)uc->uc_mcontext.gregs[REG_ESP];
    int n = snprintf(msg, sizeof(msg), "\n*** host CPU: %s at eip %08x (address %08x, esp %08x, ebp %08x)\n    stack:",
                     what, (uint32_t)uc->uc_mcontext.gregs[REG_EIP], addr, esp,
                     (uint32_t)uc->uc_mcontext.gregs[REG_EBP]);
    for (int i = 0; i < 12; i++) {
        uint32_t v;
        if (ReadWord(esp + 4 * i, &v))
            n += snprintf(msg + n, sizeof(msg) - n, " %08x", v);
    }
    msg[n++] = '\n';
    write(2, msg, n);
    write(2, Log, LogLength);
    _exit(99);
}

// Push a return address on the interrupted stack and continue in the interrupt trampoline.
static void RedirectToInterrupts(ucontext_t *uc, uint32_t resume) {
    uint32_t esp = (uint32_t)uc->uc_mcontext.gregs[REG_ESP] - 4;
    if (InWindow(esp))
        Fill(esp, true);
    *(uint32_t *)esp = resume;
    uc->uc_mcontext.gregs[REG_ESP] = esp;
    uc->uc_mcontext.gregs[REG_EIP] = (greg_t)(uintptr_t)host_irq_trampoline;
}

static bool Emulate(ucontext_t *uc) {
    auto eip = (uint32_t)uc->uc_mcontext.gregs[REG_EIP];
    auto p = (const uint8_t *)eip;
    bool op16 = false;
    if (*p == 0x66) {
        op16 = true;
        p++;
    }
    uint32_t next;
    switch (p[0]) {
    case 0xFA: // cli
        HostIf = 0;
        uc->uc_mcontext.gregs[REG_EIP] = eip + 1;
        return true;
    case 0xFB: { // sti (its shadow covers a following hlt)
        HostIf = 0x200;
        next = eip + 1;
        unsigned irq;
        if (*(const uint8_t *)next != 0xF4 && Deliverable(&irq))
            RedirectToInterrupts(uc, next);
        else
            uc->uc_mcontext.gregs[REG_EIP] = next;
        return true;
    }
    case 0xF4: { // hlt: idle until the next interrupt (time jumps to the next clock tick)
        if (!(HostIf & 0x200))
            Die(uc, "hlt with interrupts disabled (the CPU would stop for good)", 0);
        unsigned irq;
        if (!Deliverable(&irq)) {
            Tick();
            // Every thread blocked for 600 s of fake time within one test: a deadlock, not a slow test.
            if (++IdleTicksThisTest > 600000)
                Die(uc, "deadlock: the CPU has been idle for 600 s of fake time in this test", 0);
        }
        RedirectToInterrupts(uc, eip + 1);
        return true;
    }
    case 0xE4: case 0xE5: case 0xEC: case 0xED: { // in
        bool imm = p[0] < 0xEC;
        uint16_t port = imm ? p[1] : (uint16_t)Reg(uc, R_EDX);
        int size = (p[0] & 1) ? (op16 ? 2 : 4) : 1;
        uint32_t v = PortIn(port, size);
        uint32_t &eax = (uint32_t &)Reg(uc, R_EAX);
        eax = size == 1 ? (eax & ~0xFFu) | (v & 0xFF) : size == 2 ? (eax & ~0xFFFFu) | (v & 0xFFFF) : v;
        uc->uc_mcontext.gregs[REG_EIP] = (uint32_t)(uintptr_t)(p + (imm ? 2 : 1));
        return true;
    }
    case 0xE6: case 0xE7: case 0xEE: case 0xEF: { // out
        bool imm = p[0] < 0xEE;
        uint16_t port = imm ? p[1] : (uint16_t)Reg(uc, R_EDX);
        int size = (p[0] & 1) ? (op16 ? 2 : 4) : 1;
        uint32_t v = (uint32_t)Reg(uc, R_EAX);
        PortOut(port, size == 1 ? v & 0xFF : size == 2 ? v & 0xFFFF : v, size);
        uc->uc_mcontext.gregs[REG_EIP] = (uint32_t)(uintptr_t)(p + (imm ? 2 : 1));
        return true;
    }
    case 0x0F:
        switch (p[1]) {
        case 0x08: case 0x09: // invd / wbinvd: the caches are coherent here
            uc->uc_mcontext.gregs[REG_EIP] = eip + 2;
            return true;
        case 0x01: { // group 7: lgdt /2, lidt /3, invlpg /7
            uint8_t reg = (p[2] >> 3) & 7;
            uint32_t ea;
            int len = Modrm(uc, p + 2, &ea);
            if (reg == 7) {
                TlbFlushes = TlbFlushes + 1;
                uint32_t page = ea & ~0xFFFu;
                if (Mapped[page >> 17] & (1u << ((page >> 12) & 31)))
                    Unmap(page);
            } else if (reg != 2 && reg != 3) {
                return false;
            }
            uc->uc_mcontext.gregs[REG_EIP] = eip + 2 + len;
            return true;
        }
        case 0x00: { // ltr / lldt
            uint32_t ea;
            int len = (p[2] >> 6) == 3 ? 1 : Modrm(uc, p + 2, &ea);
            uc->uc_mcontext.gregs[REG_EIP] = eip + 2 + len;
            return true;
        }
        case 0x20: case 0x22: { // mov r32, crN / mov crN, r32
            int cr = (p[2] >> 3) & 7, r = p[2] & 7;
            volatile uint32_t *crs[5] = {&Cr0, nullptr, &Cr2, &Cr3, &Cr4};
            if (cr > 4 || !crs[cr])
                return false;
            if (p[1] == 0x20) {
                Reg(uc, r) = *crs[cr];
            } else {
                *crs[cr] = (uint32_t)Reg(uc, r);
                if (cr == 3 || cr == 0 || cr == 4)
                    FlushTlb();
            }
            uc->uc_mcontext.gregs[REG_EIP] = eip + 3;
            return true;
        }
        case 0x21: case 0x23: { // mov to/from debug registers: kept, without effect
            static uint32_t dr[8];
            int n = (p[2] >> 3) & 7, r = p[2] & 7;
            if (p[1] == 0x21)
                Reg(uc, r) = dr[n];
            else
                dr[n] = (uint32_t)Reg(uc, r);
            uc->uc_mcontext.gregs[REG_EIP] = eip + 3;
            return true;
        }
        case 0x30: // wrmsr
            WriteMsr((uint32_t)Reg(uc, R_ECX), (uint64_t)(uint32_t)Reg(uc, R_EDX) << 32 | (uint32_t)Reg(uc, R_EAX));
            uc->uc_mcontext.gregs[REG_EIP] = eip + 2;
            return true;
        case 0x32: { // rdmsr
            uint64_t v = ReadMsr((uint32_t)Reg(uc, R_ECX));
            Reg(uc, R_EAX) = (uint32_t)v;
            Reg(uc, R_EDX) = (uint32_t)(v >> 32);
            uc->uc_mcontext.gregs[REG_EIP] = eip + 2;
            return true;
        }
        default: return false;
        }
    default: return false;
    }
}

static void OnFault(int sig, siginfo_t *si, void *ctx) {
    auto uc = (ucontext_t *)ctx;
    auto addr = (uint32_t)(uintptr_t)si->si_addr;
    if (sig == SIGSEGV && (si->si_code == SEGV_MAPERR || si->si_code == SEGV_ACCERR) && InWindow(addr)) {
        bool write = uc->uc_mcontext.gregs[REG_ERR] & 2;
        if (Fill(addr, write))
            return;
        KernelFaultAddress = addr;
        Cr2 = addr;
        Die(uc, write ? "kernel page fault (write to an unmapped or read-only page)" : "kernel page fault (read)", addr);
    }
    auto eip = (uint32_t)uc->uc_mcontext.gregs[REG_EIP];
    if (!InText(eip))
        Die(uc, "jump to an address outside the program", eip);
    if (sig == SIGSEGV && Emulate(uc))
        return;
    Die(uc, sig == SIGBUS ? "bus error" : "unhandled fault (not a kernel window, not an emulated instruction)", addr);
}

// Before anything else maps memory into the kernel's windows.
__attribute__((constructor(101))) static void HostInit() {
    for (auto &w : Windows) {
        void *r = mmap((void *)w.start, w.end - w.start, PROT_NONE,
                       MAP_FIXED_NOREPLACE | MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (r != (void *)w.start) {
            fprintf(stderr, "host: cannot reserve %08x-%08x (%s); /proc/self/maps:\n", w.start, w.end,
                    strerror(errno));
            FILE *f = fopen("/proc/self/maps", "r");
            char line[256];
            while (f && fgets(line, sizeof(line), f))
                fputs(line, stderr);
            _exit(98);
        }
    }
    PhysFd = memfd_create("oxos-phys", 0);
    DevFd = memfd_create("oxos-dev", 0);
    if (PhysFd < 0 || DevFd < 0 || ftruncate(PhysFd, PHYS_SIZE) || ftruncate(DevFd, DEV_SIZE))
        _exit(97);
    PhysView = (uint8_t *)mmap(nullptr, PHYS_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, PhysFd, 0);
    static uint8_t altstack[1 << 16];
    stack_t ss = {altstack, 0, sizeof(altstack)};
    sigaltstack(&ss, nullptr);
    struct sigaction sa = {};
    sa.sa_sigaction = OnFault;
    sa.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_NODEFER;
    sigaction(SIGSEGV, &sa, nullptr);
    sigaction(SIGBUS, &sa, nullptr);
    HostIf = 0; // the kernel starts with interrupts off
}

} // namespace host

// --- Bug checks (kernel/src/ke/bugcheck.cpp calls this first in host builds).
namespace host {
jmp_buf *BugCheckCatcher;
volatile uint32_t LastBugCheck[5];
}
extern "C" void HostBugCheck(unsigned long code, unsigned long p1, unsigned long p2, unsigned long p3,
                             unsigned long p4) {
    host::LastBugCheck[0] = code, host::LastBugCheck[1] = p1, host::LastBugCheck[2] = p2;
    host::LastBugCheck[3] = p3, host::LastBugCheck[4] = p4;
    if (jmp_buf *c = host::BugCheckCatcher) {
        host::BugCheckCatcher = nullptr;
        longjmp(*c, 1);
    }
    fprintf(stderr, "\n*** bug check %#lx (%#lx, %#lx, %#lx, %#lx)\n%s\n", code, p1, p2, p3, p4, host::TakeLog());
    _exit(95);
}

// --- arch.h helpers in host builds.
extern "C" unsigned long HostSaveFlagsCli() {
    uint32_t f = HostIf | 2;
    HostIf = 0;
    return f;
}
extern "C" void HostRestoreFlags(unsigned long f) {
    HostIf = f & 0x200;
    if (HostIf)
        host::HostDeliverInterrupts();
}
