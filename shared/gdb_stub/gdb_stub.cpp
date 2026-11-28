// The GDB remote stub (gdb_stub.h, spec/oxos_internal/51): the GDB remote serial protocol (from the public GDB documentation, "Remote Protocol").
//
// One target, one session: the kernel and the title share the address space and the processor. A stop (a breakpoint, a single step, a break-in, an unhandled
// exception, a bug check) runs in the trap with interrupts off, sends the stop reply and serves gdb's packets until gdb resumes or detaches; nothing else runs
// meanwhile. Break-in: the clock interrupt polls the transport once a millisecond; a byte from gdb (Ctrl-C, or the first packet of a gdb that attaches) sets
// the trap flag of the interrupted code, whose single-step trap then stops it with its own registers.
//
// The Modxo's COM1 shares its USB stream with the modchip's own markers (0xFC, 0xFE to the PC; 0xFD from it), so everything here is plain ASCII: memory goes
// as hex (`m`/`M`; binary `X` is refused), and acks stay on (a packet hit by a status line or emptied by a boot is sent again).
#include <gdb_stub/gdb_stub.h>

// One byte copied with a recovery point: a fault on its read or its write resumes at the fault label, which returns 0 (HandleTrap moves eip there).
extern "C" uint32_t GdbStubCopyByte(volatile uint8_t *Destination, const volatile uint8_t *Source);
extern "C" char GdbStubCopyByteRead[], GdbStubCopyByteWrite[], GdbStubCopyByteFault[];
__asm__(".text\n"
        ".globl _GdbStubCopyByte, _GdbStubCopyByteRead, _GdbStubCopyByteWrite, _GdbStubCopyByteFault\n"
        "_GdbStubCopyByte:\n"
        "    movl 8(%esp), %ecx\n"
        "    movl 4(%esp), %edx\n"
        "_GdbStubCopyByteRead:\n"
        "    movb (%ecx), %al\n"
        "_GdbStubCopyByteWrite:\n"
        "    movb %al, (%edx)\n"
        "    movl $1, %eax\n"
        "    ret\n"
        "_GdbStubCopyByteFault:\n"
        "    xorl %eax, %eax\n"
        "    ret\n");

// Function on the stack whose top is StackTop, and back.
extern "C" void GdbStubCallOnStack(void (*Function)(), void *StackTop);
__asm__(".text\n"
        ".globl _GdbStubCallOnStack\n"
        "_GdbStubCallOnStack:\n"
        "    pushl %ebp\n"
        "    movl %esp, %ebp\n"
        "    movl 8(%ebp), %eax\n"
        "    movl 12(%ebp), %esp\n"
        "    pushl %ebp\n"
        "    call *%eax\n"
        "    popl %ebp\n"
        "    movl %ebp, %esp\n"
        "    popl %ebp\n"
        "    ret\n");

// The registers a resume loads (GdbStubResume): the stopped code's, its stack pointer included.
struct RESUME_CONTEXT {
    uint32_t Eax, Ebx, Ecx, Edx, Esi, Edi, Ebp, Eip, SegCs, EFlags, Esp;
};
static_assert(sizeof(RESUME_CONTEXT) == 44);

// Resumes with Context: an iret frame (eip, cs, eflags) built below Context->Esp with eax and ebx under it, the other registers loaded, then iret.
extern "C" [[noreturn]] void GdbStubResume(const RESUME_CONTEXT *Context);
__asm__(".text\n"
        ".globl _GdbStubResume\n"
        "_GdbStubResume:\n"
        "    movl 4(%esp), %ebx\n"
        "    movl 40(%ebx), %eax\n"
        "    subl $20, %eax\n"
        "    movl 0(%ebx), %ecx\n"
        "    movl %ecx, 0(%eax)\n"
        "    movl 4(%ebx), %ecx\n"
        "    movl %ecx, 4(%eax)\n"
        "    movl 28(%ebx), %ecx\n"
        "    movl %ecx, 8(%eax)\n"
        "    movl 32(%ebx), %ecx\n"
        "    movl %ecx, 12(%eax)\n"
        "    movl 36(%ebx), %ecx\n"
        "    movl %ecx, 16(%eax)\n"
        "    movl 8(%ebx), %ecx\n"
        "    movl 12(%ebx), %edx\n"
        "    movl 16(%ebx), %esi\n"
        "    movl 20(%ebx), %edi\n"
        "    movl 24(%ebx), %ebp\n"
        "    movl %eax, %esp\n"
        "    popl %eax\n"
        "    popl %ebx\n"
        "    iretl\n");

namespace oxos::gdb_stub {

namespace {

constexpr uint32_t PACKET_SIZE = 4096;         // the largest packet either side sends (qSupported PacketSize)
constexpr uint32_t EFLAGS_TF = 0x100;          // trap flag: a single-step trap after the next instruction
constexpr uint32_t EFLAGS_IF = 0x200;          // interrupts on
constexpr uint32_t EFLAGS_RF = 0x10000;        // resume flag: an instruction breakpoint does not trap again on the instruction it stopped at
constexpr uint32_t DR6_BS = 0x4000;            // DR6: the trap was a single step
constexpr uint32_t FIRST_IO_ADDRESS = 0xFD000000; // NV2A, MCPX and flash registers: reads can have side effects, so gdb gets an error there
constexpr int SIGINT_ = 2, SIGILL_ = 4, SIGTRAP_ = 5, SIGABRT_ = 6, SIGBUS_ = 7, SIGFPE_ = 8, SIGSEGV_ = 11;
constexpr uint32_t KERNEL_DATA_SEGMENT = 0x10; // ss: the one data segment

// Timing while stopped (interrupts are off): the ACPI PM timer, 3.375 MHz; 24 bits are enough for the waits here (under 4 s).
constexpr uint16_t ACPI_PM_TIMER = 0x8008;
constexpr uint32_t PM_TICKS_PER_MS = 3375;
constexpr uint32_t WAIT_FOREVER = 0xFFFFFFFF;

// The stub's large buffers. In a GDB=1 kernel they go to the end of .bss (a grouped section, sorted after the others), so they do not move the kernel's own
// data from where the other builds have it: the bus master's tables must not cross 64 KB (tools/check_kernel_layout.py).
#if defined(OXOS_KERNEL)
#define STUB_BUFFER __attribute__((section(".bss$zz_gdb_stub")))
#else
#define STUB_BUFFER
#endif

// While stopped the stub runs on a stack of its own, so gdb may write below the stopped code's stack pointer (gdb `call`).
constexpr uint32_t STUB_STACK_LENGTH = 0x2000;
alignas(16) STUB_BUFFER uint8_t StubStack[STUB_STACK_LENGTH];
// Below this: the title's memory. Before a title is loaded the low addresses map other pages (at boot, the kernel's own), so a title breakpoint waits.
constexpr uint32_t TITLE_SPACE_END = 0x80000000;

SESSION *Session;                        // the session (Start): breakpoints, watchpoints, whether gdb is attached
const OXOS_DEBUG_PORT *Port;             // the kernel's debug port
const OXOS_DEBUG_TRANSPORT *OwnTransport; // the bytes' way when no transport is registered with the port
bool InTitle;                            // the title gdb debugs is loaded (TitleLoaded): breakpoints below TITLE_SPACE_END go in only now
bool WaitingForGdb;                      // the next int3 that is not a breakpoint is WaitForDebugger's stop
bool LibraryStop;                        // the next int3 that is not a breakpoint is TitleLoaded's stop for gdb's library list
bool AfterBugCheck;                      // the console bug-checked: every int3 of WaitAfterBugCheck is a stop
bool ReportedException;                  // a stop for an unhandled exception was reported: its bug check (0x1E) does not stop again
volatile uint8_t BreakInByte;            // the byte the clock interrupt took from gdb (0x03 or '$'), until its single-step trap stops the target
bool PacketStarted;                      // the '$' of the next packet was already read (by the break-in)
bool InMemoryAccess;                     // GdbStubCopyByte is running for the stub: a fault there resumes at its recovery label
uint32_t StoppedStackTop;                // while stopped: the stopped code's stack pointer at the stop; the trap frame and the stub's caller lie just below it
uint32_t StoppedFramesBottom;            // while stopped: the lowest address of those frames (a normal resume returns through them)
bool StackWritten;                       // gdb wrote between StoppedFramesBottom and StoppedStackTop (gdb `call` puts its arguments there)
bool StopResumed;                        // ServeDebugger's answer for this stop: gdb resumed (true) or detached
OXOS_TRAP_FRAME StoppedFrame;            // the stopped code's registers, as gdb reads and changes them
uint32_t StoppedEsp;                     // its stack pointer (gdb may move it: `call`)
STUB_BUFFER char Packet[PACKET_SIZE + 1]; // the packet being read, NUL-terminated
STUB_BUFFER char Reply[PACKET_SIZE + 1];  // the reply being built
char StopReply[96];                      // the last stop reply, for '?'
uint32_t SelectedThread;                 // 'Hg': the thread whose registers 'g' reads (0: the stopped one)

const char HEX_DIGITS[] = "0123456789abcdef";

// --- Processor

void DisableInterrupts() {
    __asm__ volatile("cli" ::: "memory");
}

uint32_t SaveFlagsAndDisableInterrupts() {
    uint32_t flags;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(flags)::"memory");
    return flags;
}

void RestoreFlags(uint32_t Flags) {
    __asm__ volatile("pushl %0; popfl" ::"r"(Flags) : "memory", "cc");
}

// --- Bytes and timing

uint32_t ReadPmTimer() {
    uint32_t value;
    __asm__ volatile("inl %1, %0" : "=a"(value) : "Nd"(ACPI_PM_TIMER));
    return value;
}

uint32_t ElapsedMs(uint32_t Start) {
    return ((ReadPmTimer() - Start) & 0xFFFFFF) / PM_TICKS_PER_MS;
}

// The transport in use: the one registered with the port (an extension's, the network's), else the stub's own.
const OXOS_DEBUG_TRANSPORT &Transport() {
    const OXOS_DEBUG_TRANSPORT *registered = Port->Transport();
    return registered ? *registered : *OwnTransport;
}

bool TakeByte(uint8_t &Byte) {
    UCHAR byte;
    if (!Transport().ReadByte(&byte))
        return false;
    Byte = byte;
    return true;
}

// The next byte from gdb, or -1 when none came within TimeoutMs.
int ReadByte(uint32_t TimeoutMs) {
    uint32_t start = ReadPmTimer();
    for (;;) {
        uint8_t byte;
        if (TakeByte(byte))
            return byte;
        if (TimeoutMs != WAIT_FOREVER && ElapsedMs(start) >= TimeoutMs)
            return -1;
        // A pause between empty polls: back-to-back port reads keep an emulator's I/O thread (the one that delivers gdb's bytes) from its lock, and load
        // the Modxo's firmware, which answers every one.
        for (int i = 0; i < 200; i++)
            __asm__ volatile("pause");
    }
}

void WriteByte(char Byte) {
    Transport().WriteByte((UCHAR)Byte);
}

int ParseHexDigit(char Digit) {
    if (Digit >= '0' && Digit <= '9')
        return Digit - '0';
    if (Digit >= 'a' && Digit <= 'f')
        return Digit - 'a' + 10;
    if (Digit >= 'A' && Digit <= 'F')
        return Digit - 'A' + 10;
    return -1;
}

// --- Packets

// "$data#cs"; with WaitForAck, sent again until gdb acks it (eight tries, 330 ms each: not a multiple of the Modxo's 0.5 s status lines, so a packet they
// cut is not cut again on every try).
void SendPacket(const char *Data, bool WaitForAck = true) {
    for (int attempt = 0; attempt < 8; attempt++) {
        uint8_t sum = 0;
        WriteByte('$');
        for (const char *p = Data; *p; p++) {
            WriteByte(*p);
            sum += (uint8_t)*p;
        }
        WriteByte('#');
        WriteByte(HEX_DIGITS[sum >> 4]);
        WriteByte(HEX_DIGITS[sum & 15]);
        if (!WaitForAck)
            return;
        for (;;) {
            int c = ReadByte(330);
            if (c == '+')
                return;
            if (c == '$') { // gdb went on to its next packet: it had the reply
                PacketStarted = true;
                return;
            }
            if (c == '-' || c < 0)
                break; // send it again
        }
    }
}

// The next packet's data in Packet (acked), its length; -1 for a Ctrl-C between packets.
int ReadPacket() {
    for (;;) {
        int c = PacketStarted ? '$' : ReadByte(WAIT_FOREVER);
        PacketStarted = false;
        if (c == 0x03)
            return -1;
        if (c != '$')
            continue; // acks and noise between packets
        uint32_t length = 0;
        uint8_t sum = 0;
        bool complete = false;
        for (;;) {
            c = ReadByte(2000);
            if (c < 0)
                break;
            if (c == '$') { // a new start: the rest was lost
                length = 0;
                sum = 0;
                continue;
            }
            if (c == '#') {
                complete = true;
                break;
            }
            if (length < PACKET_SIZE)
                Packet[length++] = (char)c;
            sum += (uint8_t)c;
        }
        if (!complete)
            continue;
        int high = ParseHexDigit((char)ReadByte(2000)), low = ParseHexDigit((char)ReadByte(2000));
        if (high < 0 || low < 0 || (uint8_t)(high << 4 | low) != sum || length >= PACKET_SIZE) {
            WriteByte('-');
            continue;
        }
        WriteByte('+');
        Packet[length] = 0;
        return (int)length;
    }
}

void SetReply(const char *Text) {
    strlcpy(Reply, Text, sizeof(Reply));
}

// Text to the host as it is, outside any packet, when gdb is not attached (a bridge shows it as a log line).
void Announce(const char *Text) {
    if (Session->Attached)
        return;
    for (const char *p = Text; *p; p++)
        WriteByte(*p);
}

// --- Hex text

char *AppendHexByte(char *Out, uint8_t Value) {
    *Out++ = HEX_DIGITS[Value >> 4];
    *Out++ = HEX_DIGITS[Value & 15];
    return Out;
}

// A 32-bit register in target byte order (little-endian), as the 'g' packet and the stop reply want it.
char *AppendRegister(char *Out, uint32_t Value) {
    for (int i = 0; i < 4; i++)
        Out = AppendHexByte(Out, (uint8_t)(Value >> (8 * i)));
    return Out;
}

// A hex number from Text up to the first non-hex character; Text is left there.
uint32_t ParseHexNumber(const char *&Text) {
    uint32_t value = 0;
    for (int digit; (digit = ParseHexDigit(*Text)) >= 0; Text++)
        value = value << 4 | (uint32_t)digit;
    return value;
}

uint32_t ParseRegister(const char *Text) {
    uint32_t value = 0;
    for (int i = 0; i < 4; i++)
        value |= (uint32_t)(ParseHexDigit(Text[2 * i]) << 4 | ParseHexDigit(Text[2 * i + 1])) << (8 * i);
    return value;
}

// --- Memory: page-table checked, I/O space refused, and every byte copied with a recovery point

bool IsAccessible(uint32_t Address) {
    return Address >= 0x1000 && Address < FIRST_IO_ADDRESS && Port->IsAddressValid((PVOID)Address);
}

bool CopyByte(volatile uint8_t *Destination, const volatile uint8_t *Source) {
    InMemoryAccess = true;
    bool copied = GdbStubCopyByte(Destination, Source) != 0;
    InMemoryAccess = false;
    return copied;
}

bool ReadMemoryByte(uint32_t Address, uint8_t &Value) {
    return IsAccessible(Address) && CopyByte(&Value, (const volatile uint8_t *)Address);
}

// Writes go through read-only pages too (code): CR0.WP off for the write. Never into the stub's own stack (StubStack); one into the frames below the
// stopped code's stack pointer (gdb `call`) makes the target resume without returning through them (StopTarget).
bool WriteMemoryByte(uint32_t Address, uint8_t Value) {
    if (!IsAccessible(Address) || (Address >= (uint32_t)StubStack && Address < (uint32_t)StubStack + STUB_STACK_LENGTH))
        return false;
    if (StoppedStackTop && Address >= StoppedFramesBottom && Address < StoppedStackTop)
        StackWritten = true;
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" ::"r"(cr0 & ~0x10000u) : "memory");
    bool written = CopyByte((volatile uint8_t *)Address, &Value);
    __asm__ volatile("mov %0, %%cr0" ::"r"(cr0) : "memory");
    return written;
}

// --- Breakpoints

BREAKPOINT *FindBreakpoint(uint32_t Address) {
    for (auto &b : Session->Breakpoints) {
        if (b.Address == Address)
            return &b;
    }
    return nullptr;
}

void InsertBreakpoint(BREAKPOINT &Breakpoint) {
    if (Breakpoint.Inserted || (Breakpoint.Address < TITLE_SPACE_END && !InTitle) || !ReadMemoryByte(Breakpoint.Address, Breakpoint.Saved))
        return; // already in, or pending until its title or its page is there
    Breakpoint.Inserted = WriteMemoryByte(Breakpoint.Address, 0xCC);
}

// The int3 out of memory; the breakpoint stays set (pending).
void WithdrawBreakpoint(BREAKPOINT &Breakpoint) {
    if (Breakpoint.Inserted)
        WriteMemoryByte(Breakpoint.Address, Breakpoint.Saved);
    Breakpoint.Inserted = false;
}

void RemoveBreakpoint(BREAKPOINT &Breakpoint) {
    WithdrawBreakpoint(Breakpoint);
    Breakpoint = {};
}

// 'Z0'/'z0': true when done (a breakpoint on a page that is not there yet is kept pending and inserted when it is).
bool SetBreakpoint(uint32_t Address, bool Insert) {
    BREAKPOINT *b = FindBreakpoint(Address);
    if (!Insert) {
        if (b)
            RemoveBreakpoint(*b);
        return true;
    }
    if (!b)
        b = FindBreakpoint(0);
    if (!b)
        return false;
    b->Address = Address;
    InsertBreakpoint(*b);
    return true;
}

// DR0-DR3 and DR7 from the session's watchpoints. DR7: L0-L3 (bits 0, 2, 4, 6), LE (bit 8, exact data breakpoints), R/W and LEN per slot from bit 16.
void LoadDebugRegisters() {
    uint32_t dr7 = 0;
    for (int i = 0; i < 4; i++) {
        const WATCHPOINT &w = Session->Watchpoints[i];
        if (!w.Address)
            continue;
        uint32_t rw = w.Kind == 1 ? 0 : w.Kind == 2 ? 1 : 3; // execute, write, read or write (the processor has no read-only)
        uint32_t len = w.Kind == 1 ? 0 : w.Length == 4 ? 3 : w.Length - 1;
        dr7 |= 1u << (2 * i) | (rw | len << 2) << (16 + 4 * i);
        switch (i) {
        case 0: __asm__ volatile("mov %0, %%dr0" ::"r"(w.Address)); break;
        case 1: __asm__ volatile("mov %0, %%dr1" ::"r"(w.Address)); break;
        case 2: __asm__ volatile("mov %0, %%dr2" ::"r"(w.Address)); break;
        default: __asm__ volatile("mov %0, %%dr3" ::"r"(w.Address)); break;
        }
    }
    if (dr7)
        dr7 |= 0x100;
    __asm__ volatile("mov %0, %%dr7" ::"r"(dr7));
}

// The largest piece a debug register can watch at Address with Remaining bytes left: 4, 2 or 1, aligned to its size.
uint32_t WatchPieceLength(uint32_t Address, uint32_t Remaining) {
    if (Remaining >= 4 && !(Address & 3))
        return 4;
    if (Remaining >= 2 && !(Address & 1))
        return 2;
    return 1;
}

// 'Z1'-'Z4' / 'z1'-'z4': true when done (the debug registers are loaded when the target resumes). A watchpoint takes as many registers as the aligned
// pieces it splits into; false when the free ones are too few.
bool SetWatchpoint(uint8_t Kind, uint32_t Address, uint32_t Length, bool Insert) {
    if (Kind == 1)
        Length = 1;
    bool found = false;
    for (auto &w : Session->Watchpoints) {
        if (w.Address && w.Kind == Kind && w.WatchAddress == Address && w.WatchLength == Length) {
            found = true;
            if (!Insert)
                w = {};
        }
    }
    if (!Insert || found)
        return true;
    if (Address == 0 || Length == 0)
        return false;
    uint32_t pieces = 0, free = 0;
    for (uint32_t at = Address; at < Address + Length; at += WatchPieceLength(at, Address + Length - at))
        pieces++;
    for (const auto &w : Session->Watchpoints)
        free += w.Address == 0;
    if (pieces > free)
        return false;
    uint32_t at = Address;
    for (auto &w : Session->Watchpoints) {
        if (w.Address || at >= Address + Length)
            continue;
        uint32_t length = WatchPieceLength(at, Address + Length - at);
        w = {at, Kind, (uint8_t)length, Address, Length};
        at += length;
    }
    return true;
}

void InsertPendingBreakpoints() {
    for (auto &b : Session->Breakpoints) {
        if (b.Address)
            InsertBreakpoint(b);
    }
}

// --- Registers: gdb's i386 order eax ecx edx ebx esp ebp esi edi eip eflags cs ss ds es fs gs. The x87 and SSE registers are not in 'g' (gdb reads them
// with 'p': see the float registers below).

uint32_t ReadSegment(int Index) {
    uint16_t value = 0;
    switch (Index) {
    case 0: __asm__ volatile("mov %%ds, %0" : "=r"(value)); break;
    case 1: __asm__ volatile("mov %%es, %0" : "=r"(value)); break;
    case 2: __asm__ volatile("mov %%fs, %0" : "=r"(value)); break;
    default: __asm__ volatile("mov %%gs, %0" : "=r"(value)); break;
    }
    return value;
}

uint32_t ReadRegister(const OXOS_TRAP_FRAME *Frame, int Number) {
    switch (Number) {
    case 0: return Frame->Eax;
    case 1: return Frame->Ecx;
    case 2: return Frame->Edx;
    case 3: return Frame->Ebx;
    case 4: return Frame == &StoppedFrame ? StoppedEsp : (uint32_t)(&Frame->EFlags + 1);
    case 5: return Frame->Ebp;
    case 6: return Frame->Esi;
    case 7: return Frame->Edi;
    case 8: return Frame->Eip;
    case 9: return Frame->EFlags & ~EFLAGS_TF;
    case 10: return Frame->SegCs;
    case 11: return KERNEL_DATA_SEGMENT;
    default: return ReadSegment(Number - 12);
    }
}

// The segments stay as they are; a new stack pointer makes the target resume there (StopTarget).
void WriteRegister(OXOS_TRAP_FRAME *Frame, int Number, uint32_t Value) {
    switch (Number) {
    case 0: Frame->Eax = Value; break;
    case 1: Frame->Ecx = Value; break;
    case 2: Frame->Edx = Value; break;
    case 3: Frame->Ebx = Value; break;
    case 4: StoppedEsp = Value; break;
    case 5: Frame->Ebp = Value; break;
    case 6: Frame->Esi = Value; break;
    case 7: Frame->Edi = Value; break;
    case 8: Frame->Eip = Value; break;
    case 9: Frame->EFlags = (Frame->EFlags & EFLAGS_IF) | (Value & ~EFLAGS_IF); break; // gdb does not switch interrupts on or off
    default: break;
    }
}

// --- Threads: gdb's thread ids are the KTHREAD addresses / 4 (gdb reads ids as signed 32-bit numbers; 1 before the scheduler runs). The stopped thread's
// registers are the trap frame; another's come from where the kernel switched it out (the port's ReadThreadRegisters).

constexpr uint32_t MAX_THREADS = 64; // the most listed

uint32_t ThreadId(PVOID Thread) {
    return Thread ? (uint32_t)Thread >> 2 : 1;
}

PVOID ThreadFromId(uint32_t Id) {
    return Id > 1 && Id < 0x40000000 ? (PVOID)(Id << 2) : nullptr;
}

uint32_t StoppedThreadId() {
    return ThreadId(Port->CurrentThread());
}

// A thread id in a packet: hex, or -1 (all threads) and 0 (any) as 0.
uint32_t ParseThreadId(const char *Text) {
    return *Text == '-' ? 0 : ParseHexNumber(Text);
}

bool IsListedThread(uint32_t Id) {
    PVOID threads[MAX_THREADS];
    uint32_t count = Port->ListThreads(threads, MAX_THREADS);
    for (uint32_t i = 0; i < count; i++) {
        if (ThreadId(threads[i]) == Id)
            return true;
    }
    return Id == StoppedThreadId();
}

// The thread 'Hg' selected when it is another one than the stopped one and the kernel knows it, else null.
PVOID SelectedOtherThread() {
    PVOID other = ThreadFromId(SelectedThread);
    return other && IsListedThread(SelectedThread) ? other : nullptr;
}

// The registers of the thread 'Hg' selected: the stopped one's from Frame, another's from where it was switched out.
void ReadThreadRegisters(const OXOS_TRAP_FRAME *Frame, ULONG Registers[16]) {
    PVOID other = SelectedOtherThread();
    if (other && Port->ReadThreadRegisters(other, Registers))
        return;
    for (int number = 0; number < 16; number++)
        Registers[number] = ReadRegister(Frame, number);
}

void DescribeThread(uint32_t Id, char *Text, uint32_t Size) {
    static const char *const STATES[] = {"new", "ready", "running", "standby", "ended", "waiting"};
    PVOID threads[MAX_THREADS];
    uint32_t count = Port->ListThreads(threads, MAX_THREADS);
    PVOID thread = ThreadFromId(Id);
    ULONG state;
    LONG priority;
    if (!thread || !Port->DescribeThread(thread, &state, &priority)) {
        strlcpy(Text, "boot", Size);
        return;
    }
    snprintf(Text, Size, "%s%s, priority %d%s", count && thread == threads[0] ? "idle, " : "", state < 6 ? STATES[state] : "?", (int)priority,
             Id == StoppedThreadId() ? ", stopped here" : "");
}

// --- x87 and SSE registers: gdb's i386 numbers 16-40 (st0-st7, fctrl fstat ftag fiseg fioff foseg fooff fop, xmm0-xmm7, mxcsr). 'g' leaves them out, so gdb
// asks for each with 'p' and writes it with 'P'. A thread's state is in the processor while the thread owns the FPU (the kernel switches it lazily), else in
// its FXSAVE area (the port's FloatArea).

constexpr uint32_t FIRST_FLOAT_REGISTER = 16;
constexpr uint32_t FIRST_FLOAT_CONTROL_REGISTER = 24;
constexpr uint32_t FIRST_XMM_REGISTER = 32;
constexpr uint32_t MXCSR_REGISTER = 40;
constexpr uint32_t ORIG_EAX_REGISTER = 41; // gdb's i386 Linux layout (its default without an OS note in the ELF): the system call to restart; none here (-1)
constexpr uint32_t FXSAVE_LENGTH = 512;
constexpr uint32_t CR0_TS = 1u << 3;

alignas(16) STUB_BUFFER uint8_t FloatState[FXSAVE_LENGTH]; // the FXSAVE image of the thread gdb reads or writes

// The size in bytes of float register Number.
uint32_t FloatRegisterSize(uint32_t Number) {
    if (Number < FIRST_FLOAT_CONTROL_REGISTER)
        return 10;
    if (Number >= FIRST_XMM_REGISTER && Number < MXCSR_REGISTER)
        return 16;
    return 4;
}

// The thread whose float registers gdb reads: the one 'Hg' selected, or the stopped one (none before the scheduler starts).
PVOID SelectFloatThread() {
    PVOID other = SelectedOtherThread();
    return other ? other : Port->CurrentThread();
}

// The processor's float state into FloatState (FXSAVE), or from it (FXRSTOR); CR0.TS is cleared for the moment, as either would trap.
void TransferLiveFloatState(bool FromFloatState) {
    uint32_t cr0;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %0, %%cr0" ::"r"(cr0 & ~CR0_TS));
    if (FromFloatState)
        __asm__ volatile("fxrstor %0" ::"m"(FloatState));
    else
        __asm__ volatile("fxsave %0" : "=m"(FloatState));
    __asm__ volatile("mov %0, %%cr0" ::"r"(cr0));
}

// Thread's float state into FloatState; false when it has none to read.
bool LoadFloatState(PVOID Thread) {
    if (!Thread)
        return false;
    if (Thread == Port->FloatOwnerThread()) {
        TransferLiveFloatState(false);
        return true;
    }
    auto area = (uint32_t)Port->FloatArea(Thread);
    if (!area || !IsAccessible(area) || !IsAccessible(area + FXSAVE_LENGTH - 1))
        return false;
    memcpy(FloatState, (const void *)area, FXSAVE_LENGTH);
    return true;
}

// FloatState back to where LoadFloatState took it from.
void StoreFloatState(PVOID Thread) {
    if (Thread == Port->FloatOwnerThread())
        TransferLiveFloatState(true);
    else
        memcpy(Port->FloatArea(Thread), FloatState, FXSAVE_LENGTH);
}

// The full x87 tag word gdb shows (2 bits a physical register: 0 valid, 1 zero, 2 special, 3 empty), from FXSAVE's abridged one (1 bit: in use) and each
// register's value.
uint32_t ReadFullTagWord() {
    uint8_t inUse = FloatState[4];
    uint32_t top = (*(const uint16_t *)(FloatState + 2) >> 11) & 7;
    uint32_t tags = 0;
    for (uint32_t physical = 0; physical < 8; physical++) {
        uint32_t tag = 3;
        if (inUse & (1u << physical)) {
            const uint8_t *value = FloatState + 32 + 16 * ((physical - top) & 7); // FXSAVE keeps them in stack order, st0 first
            uint32_t exponent = *(const uint16_t *)(value + 8) & 0x7FFF;
            uint32_t low = *(const uint32_t *)value, high = *(const uint32_t *)(value + 4);
            if (exponent == 0x7FFF)
                tag = 2;
            else if (exponent == 0)
                tag = (low | high) ? 2 : 1;
            else
                tag = (high & 0x80000000) ? 0 : 2;
        }
        tags |= tag << (2 * physical);
    }
    return tags;
}

// Where float register Number is in FloatState, with Width bytes there (0 for the tag word, which FXSAVE keeps abridged).
uint8_t *LocateFloatRegister(uint32_t Number, uint32_t &Width) {
    static const uint8_t CONTROL_OFFSETS[8] = {0, 2, 4, 12, 8, 20, 16, 6}; // fctrl fstat ftag fiseg fioff foseg fooff fop
    static const uint8_t CONTROL_WIDTHS[8] = {2, 2, 0, 2, 4, 2, 4, 2};
    if (Number < FIRST_FLOAT_CONTROL_REGISTER) {
        Width = 10;
        return FloatState + 32 + 16 * (Number - FIRST_FLOAT_REGISTER);
    }
    if (Number < FIRST_XMM_REGISTER) {
        Width = CONTROL_WIDTHS[Number - FIRST_FLOAT_CONTROL_REGISTER];
        return FloatState + CONTROL_OFFSETS[Number - FIRST_FLOAT_CONTROL_REGISTER];
    }
    if (Number < MXCSR_REGISTER) {
        Width = 16;
        return FloatState + 160 + 16 * (Number - FIRST_XMM_REGISTER);
    }
    Width = 4;
    return FloatState + 24;
}

// Float register Number (16-40) from FloatState as gdb's hex.
char *AppendFloatRegister(char *Out, uint32_t Number) {
    uint32_t width;
    const uint8_t *at = LocateFloatRegister(Number, width);
    if (!width)
        return AppendRegister(Out, ReadFullTagWord());
    if (width == 2) {
        uint32_t value = *(const uint16_t *)at;
        return AppendRegister(Out, Number == FIRST_XMM_REGISTER - 1 ? value & 0x7FF : value); // fop: 11 bits
    }
    for (uint32_t i = 0; i < width; i++)
        Out = AppendHexByte(Out, at[i]);
    return Out;
}

// Float register Number (16-40) into FloatState from gdb's hex; false when the hex is short.
bool WriteFloatRegister(uint32_t Number, const char *Hex) {
    uint32_t width;
    uint8_t *at = LocateFloatRegister(Number, width);
    if (!width || width == 2) {
        for (int i = 0; i < 8; i++) {
            if (ParseHexDigit(Hex[i]) < 0)
                return false;
        }
        uint32_t value = ParseRegister(Hex);
        if (width) {
            *(uint16_t *)at = (uint16_t)value;
        } else {
            uint8_t inUse = 0;
            for (uint32_t physical = 0; physical < 8; physical++) {
                if (((value >> (2 * physical)) & 3) != 3)
                    inUse |= 1u << physical;
            }
            *at = inUse;
        }
        return true;
    }
    for (uint32_t i = 0; i < width; i++) {
        int high = ParseHexDigit(Hex[2 * i]), low = high < 0 ? -1 : ParseHexDigit(Hex[2 * i + 1]);
        if (low < 0)
            return false;
        at[i] = (uint8_t)(high << 4 | low);
    }
    return true;
}

// --- Libraries (qXfer:libraries:read): what is loaded where, so gdb loads each one's symbols itself (from its solib-search-path) and drops them when they
// go. gdb relocates a PE file by its lowest section and an ELF file by its first segment, so each entry gives that address: the files are linked where they
// run (the kernel, the kernel modules, the title) or moved as a whole (an extension).

constexpr uint32_t KERNEL_IMAGE_BASE = 0x80010000; // the kernel's image, headers included: its symbol file is oxoskrnl.elf, whose first segment starts here
constexpr uint32_t TITLE_IMAGE_BASE = 0x10000;     // the title's XBE header
constexpr uint32_t XBE_MAGIC = 0x48454258;         // "XBEH"
constexpr uint32_t MAX_LISTED_IMAGES = 20;         // the kernel modules and the extensions listed at most

STUB_BUFFER char LibraryList[PACKET_SIZE]; // the library list's XML, built when gdb reads it from its start

bool ReadMemoryWord(uint32_t Address, uint32_t &Value) {
    Value = 0;
    for (uint32_t i = 0; i < 4; i++) {
        uint8_t byte;
        if (!ReadMemoryByte(Address + i, byte))
            return false;
        Value |= (uint32_t)byte << (8 * i);
    }
    return true;
}

// Where the PE image at Base has its lowest section; Base when its headers cannot be read.
uint32_t FindFirstPeSection(uint32_t Base) {
    uint32_t mz, offset, signature, counts, optionalSize, lowest = 0xFFFFFFFF;
    if (!ReadMemoryWord(Base, mz) || (mz & 0xFFFF) != 0x5A4D || !ReadMemoryWord(Base + 0x3C, offset) || !ReadMemoryWord(Base + offset, signature) ||
        signature != 0x4550 || !ReadMemoryWord(Base + offset + 4, counts) || !ReadMemoryWord(Base + offset + 20, optionalSize))
        return Base;
    uint32_t sections = Base + offset + 24 + (optionalSize & 0xFFFF);
    for (uint32_t i = 0; i < (counts >> 16); i++) {
        uint32_t address;
        if (ReadMemoryWord(sections + 40 * i + 12, address) && address < lowest)
            lowest = address;
    }
    return lowest == 0xFFFFFFFF ? Base : Base + lowest;
}

// Where the title's XBE has its lowest section (the address its PE file was linked at); 0 when there is no XBE.
uint32_t FindFirstTitleSection() {
    uint32_t magic, count, headers, lowest = 0xFFFFFFFF;
    if (!ReadMemoryWord(TITLE_IMAGE_BASE, magic) || magic != XBE_MAGIC || !ReadMemoryWord(TITLE_IMAGE_BASE + 0x11C, count) ||
        !ReadMemoryWord(TITLE_IMAGE_BASE + 0x120, headers))
        return 0;
    for (uint32_t i = 0; i < count && i < 64; i++) {
        uint32_t address;
        if (ReadMemoryWord(headers + 0x38 * i + 4, address) && address < lowest)
            lowest = address;
    }
    return lowest == 0xFFFFFFFF ? 0 : lowest;
}

// One <library>: Name and Suffix (characters XML or the protocol would need escaped become '_'), at Address.
char *AppendLibrary(char *Out, const char *End, const char *Name, const char *Suffix, uint32_t Address) {
    char name[80];
    snprintf(name, sizeof(name), "%s%s", Name, Suffix);
    for (char *c = name; *c; c++) {
        if (*c == '<' || *c == '>' || *c == '&' || *c == '"' || *c == '$' || *c == '#' || *c == '}' || *c == '*')
            *c = '_';
    }
    int length = snprintf(Out, End - Out, "<library name=\"%s\"><segment address=\"0x%lx\"/></library>", name, (unsigned long)Address);
    return length > 0 && Out + length < End ? Out + length : Out;
}

// The library list: the kernel, the kernel modules while mapped, the extensions, and the title when gdb was told its symbol file (`monitor symbols`).
void BuildLibraryList() {
    char *out = LibraryList, *end = LibraryList + sizeof(LibraryList) - 16;
    out += snprintf(out, end - out, "<library-list>");
    out = AppendLibrary(out, end, "oxoskrnl", ".elf", KERNEL_IMAGE_BASE);
    OXOS_DEBUG_IMAGE images[MAX_LISTED_IMAGES];
    uint32_t count = Port->ListImages(images, MAX_LISTED_IMAGES);
    for (uint32_t i = 0; i < count; i++) {
        bool modules = images[i].Kind == OXOS_DEBUG_IMAGE_KERNEL_MODULES;
        out = AppendLibrary(out, end, images[i].Name ? images[i].Name : "extension", modules ? ".exe" : ".xos", FindFirstPeSection((uint32_t)images[i].Base));
    }
    uint32_t title = FindFirstTitleSection();
    if (Session->TitleSymbols[0] && title)
        out = AppendLibrary(out, end, Session->TitleSymbols, "", title);
    strlcpy(out, "</library-list>", LibraryList + sizeof(LibraryList) - out);
}

// A piece of Text for 'qXfer:...:read:annex:offset,length' (Arguments: "offset,length"): 'm' when more follows, 'l' for the last.
void ReadTransferPiece(const char *Text, const char *Arguments) {
    uint32_t offset = ParseHexNumber(Arguments);
    Arguments++;
    uint32_t length = ParseHexNumber(Arguments);
    uint32_t total = strlen(Text);
    if (offset >= total) {
        SetReply("l");
        return;
    }
    if (length > PACKET_SIZE - 2)
        length = PACKET_SIZE - 2;
    uint32_t piece = total - offset < length ? total - offset : length;
    Reply[0] = offset + piece < total ? 'm' : 'l';
    memcpy(Reply + 1, Text + offset, piece);
    Reply[1 + piece] = 0;
}

// 'qXfer:libraries:read::offset,length': built afresh when gdb reads from the start.
void ReadLibraryList(const char *Arguments) {
    const char *offset = Arguments;
    if (ParseHexNumber(offset) == 0)
        BuildLibraryList();
    ReadTransferPiece(LibraryList, Arguments);
}

// 'qXfer:features:read:target.xml:offset,length': an i386 without an operating system, so gdb takes its libraries from the list above whatever its
// default OS ABI (a GNU/Linux one reads them the Linux way, and finds none); the registers are gdb's own i386 ones.
void ReadTargetDescription(const char *Arguments) {
    static const char TARGET_XML[] = "<?xml version=\"1.0\"?><!DOCTYPE target SYSTEM \"gdb-target.dtd\"><target><architecture>i386</architecture>"
                                     "<osabi>none</osabi></target>";
    ReadTransferPiece(TARGET_XML, Arguments);
}

// --- Launches: gdb starts an XBE (vRun, `monitor launch`); the caller of TakeLaunchRequest starts it once the target runs

// Path as the title loader takes it: a \Device\ path as it is; a drive letter (C: E: F: G: X: Y: Z:, D: the disc) as its device; '/' as '\'. False when
// the drive is not one of those.
bool ConvertLaunchPath(const char *Path, char *Out, uint32_t Size) {
    static const char LETTERS[] = "CDEFGXYZ";
    static const char *const DEVICES[] = {"\\Device\\Harddisk0\\Partition2", "\\Device\\CdRom0", "\\Device\\Harddisk0\\Partition1",
                                          "\\Device\\Harddisk0\\Partition6", "\\Device\\Harddisk0\\Partition7", "\\Device\\Harddisk0\\Partition3",
                                          "\\Device\\Harddisk0\\Partition4", "\\Device\\Harddisk0\\Partition5"};
    if (Path[0] && Path[1] == ':') {
        uint32_t drive = 0;
        while (LETTERS[drive] && LETTERS[drive] != (Path[0] & ~0x20))
            drive++;
        if (!LETTERS[drive])
            return false;
        snprintf(Out, Size, "%s%s", DEVICES[drive], Path + 2);
    } else if (!strncasecmp(Path, "\\Device\\", 8) || !strncasecmp(Path, "/Device/", 8)) {
        strlcpy(Out, Path, Size);
    } else {
        return false;
    }
    for (char *c = Out; *c; c++) {
        if (*c == '/')
            *c = '\\';
    }
    return true;
}

// Asks for Path (empty: the title gdb debugs, again) to start when the target next runs; false when there is no such path to start.
bool RequestLaunch(const char *Path) {
    const char *path = Path[0] ? Path : Session->TitlePath;
    if (!path[0] || !ConvertLaunchPath(path, Session->LaunchPath, sizeof(Session->LaunchPath)))
        return false;
    Session->LaunchRequested = true;
    // The title's breakpoints wait for the new one: the old title runs on until the launch stops it, and must not stop in them meanwhile.
    InTitle = false;
    for (auto &b : Session->Breakpoints) {
        if (b.Address && b.Address < TITLE_SPACE_END)
            WithdrawBreakpoint(b);
    }
    return true;
}

// 'vRun;hex-path[;arguments]': gdb's `run` in extended-remote mode. The reply is a stop where the target is now; the launch follows when gdb resumes it.
void HandleRun(const char *Arguments) {
    char path[PATH_LENGTH];
    uint32_t length = 0;
    for (; Arguments[0] && Arguments[0] != ';' && Arguments[1] && length < sizeof(path) - 1; Arguments += 2)
        path[length++] = (char)(ParseHexDigit(Arguments[0]) << 4 | ParseHexDigit(Arguments[1]));
    path[length] = 0;
    if (RequestLaunch(path))
        SetReply(StopReply);
    else
        SetReply("E01");
}

// --- The stop: report, then serve gdb until it resumes or detaches

void BuildStopReply(const OXOS_TRAP_FRAME *Frame, int Signal, const char *Reason) {
    char *out = StopReply;
    *out++ = 'T';
    out = AppendHexByte(out, (uint8_t)Signal);
    for (const char *p = "thread:"; *p; p++)
        *out++ = *p;
    for (int shift = 28; shift >= 0; shift -= 4)
        *out++ = HEX_DIGITS[(StoppedThreadId() >> shift) & 15];
    *out++ = ';';
    for (const char *p = Reason; *p; p++)
        *out++ = *p;
    // Expedited registers: esp, ebp, eip, so gdb needs no 'g' for a backtrace's first frame.
    static constexpr int EXPEDITED[] = {4, 5, 8};
    for (int number : EXPEDITED) {
        out = AppendHexByte(out, (uint8_t)number);
        *out++ = ':';
        out = AppendRegister(out, ReadRegister(Frame, number));
        *out++ = ';';
    }
    *out = 0;
}

void DetachDebugger() {
    for (auto &b : Session->Breakpoints) {
        if (b.Address)
            RemoveBreakpoint(b);
    }
    memset(Session->Watchpoints, 0, sizeof(Session->Watchpoints));
    LoadDebugRegisters();
    Session->Attached = false;
    Session->WaitingForStop = false;
    Session->Stepping = false;
}

// Whether Path contains Part, in any case.
bool ContainsText(const char *Path, const char *Part) {
    uint32_t length = strlen(Path), partLength = strlen(Part);
    for (uint32_t i = 0; i + partLength <= length; i++) {
        if (!strncasecmp(Path + i, Part, partLength))
            return true;
    }
    return false;
}

// 'qRcmd,hex': gdb's `monitor` commands. Output goes as O packets before the final "OK".
void HandleMonitorCommand(const char *Hex) {
    char command[128];
    uint32_t length = 0;
    for (; Hex[0] && Hex[1] && length < sizeof(command) - 1; Hex += 2)
        command[length++] = (char)(ParseHexDigit(Hex[0]) << 4 | ParseHexDigit(Hex[1]));
    command[length] = 0;
    const char *argument = command;
    while (*argument && *argument != ' ')
        argument++;
    while (*argument == ' ')
        argument++;
    const char *message;
    if (!strncmp(command, "title", 5)) {
        strlcpy(Session->TitleFilter, argument, sizeof(Session->TitleFilter));
        message = Session->TitleFilter[0] ? "title breakpoints: only in the title whose path has that name\n" : "title breakpoints: in any title\n";
    } else if (!strncmp(command, "symbols", 7)) {
        strlcpy(Session->TitleSymbols, argument, sizeof(Session->TitleSymbols));
        message = Session->TitleSymbols[0] ? "the library list names the title by that file\n" : "the title is not in the library list\n";
    } else if (!strncmp(command, "launch", 6)) {
        message = RequestLaunch(argument) ? "starts when the target continues\n" : "no such path: give C:\\..., E:\\... or \\Device\\...\n";
    } else {
        message = "monitor commands: title [NAME], symbols [FILE], launch [PATH]\n";
    }
    char *out = Reply;
    *out++ = 'O';
    for (const char *p = message; *p; p++)
        out = AppendHexByte(out, (uint8_t)*p);
    *out = 0;
    SendPacket(Reply);
    SetReply("OK");
}

void HandleQuery(const char *Query) {
    if (!strncmp(Query, "qSupported", 10)) {
        SetReply("PacketSize=1000;swbreak+;hwbreak+;qXfer:libraries:read+;qXfer:features:read+");
    } else if (!strcmp(Query, "qAttached")) {
        SetReply("1");
    } else if (!strcmp(Query, "qC")) {
        snprintf(Reply, sizeof(Reply), "QC%lx", (unsigned long)StoppedThreadId());
    } else if (!strcmp(Query, "qfThreadInfo")) {
        PVOID threads[MAX_THREADS];
        uint32_t count = Port->ListThreads(threads, MAX_THREADS);
        char *out = Reply;
        *out++ = 'm';
        if (!count)
            out += snprintf(out, 16, "%lx", (unsigned long)StoppedThreadId());
        for (uint32_t i = 0; i < count; i++)
            out += snprintf(out, 16, i ? ",%lx" : "%lx", (unsigned long)ThreadId(threads[i]));
        *out = 0;
    } else if (!strcmp(Query, "qsThreadInfo")) {
        SetReply("l");
    } else if (!strncmp(Query, "qThreadExtraInfo,", 17)) {
        char text[64];
        DescribeThread(ParseThreadId(Query + 17), text, sizeof(text));
        char *out = Reply;
        for (const char *c = text; *c; c++)
            out = AppendHexByte(out, (uint8_t)*c);
        *out = 0;
    } else if (!strncmp(Query, "qSymbol", 7)) {
        SetReply("OK");
    } else if (!strncmp(Query, "qRcmd,", 6)) {
        HandleMonitorCommand(Query + 6);
    } else if (!strncmp(Query, "qXfer:libraries:read::", 22)) {
        ReadLibraryList(Query + 22);
    } else if (!strncmp(Query, "qXfer:features:read:target.xml:", 31)) {
        ReadTargetDescription(Query + 31);
    } else {
        Reply[0] = 0; // not supported
    }
}

// 'p': register Number into Reply.
void ReadOneRegister(const OXOS_TRAP_FRAME *Frame, uint32_t Number) {
    char *out = Reply;
    if (Number < FIRST_FLOAT_REGISTER) {
        ULONG registers[16];
        ReadThreadRegisters(Frame, registers);
        out = AppendRegister(out, registers[Number]);
    } else if (Number <= MXCSR_REGISTER) {
        if (LoadFloatState(SelectFloatThread())) {
            out = AppendFloatRegister(out, Number);
        } else {
            for (uint32_t i = 0; i < 2 * FloatRegisterSize(Number); i++)
                *out++ = 'x'; // unavailable
        }
    } else if (Number == ORIG_EAX_REGISTER) {
        out = AppendRegister(out, 0xFFFFFFFF);
    } else {
        out += snprintf(out, 4, "E01");
    }
    *out = 0;
}

// 'P': register Number from Hex; false when it cannot be written.
bool WriteOneRegister(OXOS_TRAP_FRAME *Frame, uint32_t Number, const char *Hex) {
    if (Number < FIRST_FLOAT_REGISTER) {
        WriteRegister(Frame, (int)Number, ParseRegister(Hex));
        return true;
    }
    if (Number <= MXCSR_REGISTER) {
        PVOID thread = SelectFloatThread();
        if (!LoadFloatState(thread) || !WriteFloatRegister(Number, Hex))
            return false;
        StoreFloatState(thread);
        return true;
    }
    return Number == ORIG_EAX_REGISTER; // written by gdb `call`: nothing to keep
}

// 'm': memory into Reply as hex.
void ReadMemory(const char *Arguments) {
    uint32_t address = ParseHexNumber(Arguments);
    Arguments++;
    uint32_t count = ParseHexNumber(Arguments);
    if (count > PACKET_SIZE / 2)
        count = PACKET_SIZE / 2;
    char *out = Reply;
    for (uint32_t i = 0; i < count; i++) {
        uint8_t value;
        if (!ReadMemoryByte(address + i, value))
            break;
        out = AppendHexByte(out, value);
    }
    *out = 0;
    if (out == Reply)
        SetReply("E14");
}

// 'M': memory from hex; false when a byte could not be written.
bool WriteMemory(const char *Arguments) {
    uint32_t address = ParseHexNumber(Arguments);
    Arguments++;
    uint32_t count = ParseHexNumber(Arguments);
    Arguments++;
    for (uint32_t i = 0; i < count; i++) {
        if (!WriteMemoryByte(address + i, (uint8_t)(ParseHexDigit(Arguments[2 * i]) << 4 | ParseHexDigit(Arguments[2 * i + 1]))))
            return false;
    }
    return true;
}

// 'qSearch:memory:ADDRESS;LENGTH;PATTERN' (gdb's `find`): the first place in the range that holds PATTERN (binary, '}' escapes), searched here so only the
// answer crosses the serial line: "1,ADDRESS", or "0". Pages that are not mapped (or are I/O space) are skipped whole; a mapped page is read directly (the
// target is stopped with interrupts off, so its mapping holds).
void SearchMemory(const char *Arguments, uint32_t Length) {
    const char *end = Arguments + Length;
    uint32_t address = ParseHexNumber(Arguments);
    if (*Arguments++ != ';') {
        SetReply("E01");
        return;
    }
    uint32_t length = ParseHexNumber(Arguments);
    if (*Arguments++ != ';') {
        SetReply("E01");
        return;
    }
    uint8_t pattern[64];
    uint32_t size = 0;
    while (Arguments < end && size < sizeof(pattern)) {
        uint8_t c = (uint8_t)*Arguments++;
        if (c == '}' && Arguments < end)
            c = (uint8_t)*Arguments++ ^ 0x20;
        pattern[size++] = c;
    }
    if (!size || Arguments < end) {
        SetReply("E01");
        return;
    }
    if (length < size) {
        SetReply("0");
        return;
    }
    uint32_t last = address + (length - size); // the last place a match may start
    for (uint32_t page = address & ~0xFFFu;; page += 0x1000) {
        // Each page's mapping is checked once; a match that runs into the next page needs that one mapped too.
        if (IsAccessible(page)) {
            bool nextMapped = IsAccessible(page + 0x1000);
            uint32_t first = page < address ? address : page, stop = page + 0xFFF < last ? page + 0xFFF : last;
            for (uint32_t at = first; at <= stop; at++) {
                if (at + size > page + 0x1000 && !nextMapped)
                    break;
                const volatile uint8_t *bytes = (const volatile uint8_t *)at;
                uint32_t i = 0;
                while (i < size && bytes[i] == pattern[i])
                    i++;
                if (i == size) {
                    snprintf(Reply, sizeof(Reply), "1,%lx", (unsigned long)at);
                    return;
                }
            }
        }
        if (page + 0xFFF >= last || page + 0x1000 == 0)
            break;
    }
    SetReply("0");
}

// Serve packets until gdb resumes (true) or detaches (false: the caller resumes without the debugger).
bool ServeDebugger(OXOS_TRAP_FRAME *Frame) {
    for (;;) {
        int length = ReadPacket();
        if (length < 0)
            continue; // Ctrl-C while stopped
        Session->Attached = true;
        const char *p = Packet + 1;
        Reply[0] = 0;
        switch (Packet[0]) {
        case '?':
            SetReply(StopReply);
            break;
        case 'q':
            if (!strncmp(Packet, "qSearch:memory:", 15))
                SearchMemory(Packet + 15, (uint32_t)length - 15);
            else
                HandleQuery(Packet);
            break;
        case 'H':
            if (Packet[1] == 'g') {
                uint32_t id = ParseThreadId(++p);
                SelectedThread = id == StoppedThreadId() ? 0 : id;
            }
            SetReply("OK");
            break;
        case 'T':
            SetReply(IsListedThread(ParseThreadId(p)) ? "OK" : "E01");
            break;
        case 'g': {
            ULONG registers[16];
            ReadThreadRegisters(Frame, registers);
            char *out = Reply;
            for (int number = 0; number < 16; number++)
                out = AppendRegister(out, registers[number]);
            *out = 0;
            break;
        }
        case 'G':
            for (int number = 0; number < 16 && p[8 * number]; number++)
                WriteRegister(Frame, number, ParseRegister(p + 8 * number));
            SetReply("OK");
            break;
        case 'p':
            ReadOneRegister(Frame, ParseHexNumber(p));
            break;
        case 'P': {
            uint32_t number = ParseHexNumber(p);
            SetReply(*p == '=' && WriteOneRegister(Frame, number, p + 1) ? "OK" : "E01");
            break;
        }
        case 'm':
            ReadMemory(p);
            break;
        case 'M':
            SetReply(WriteMemory(p) ? "OK" : "E14");
            break;
        case 'Z':
        case 'z':
            p = Packet + 3;
            if (Packet[1] == '0') {
                SetReply(SetBreakpoint(ParseHexNumber(p), Packet[0] == 'Z') ? "OK" : "E28");
            } else if (Packet[1] >= '1' && Packet[1] <= '4') {
                uint32_t address = ParseHexNumber(p);
                p++;
                SetReply(SetWatchpoint((uint8_t)(Packet[1] - '0'), address, ParseHexNumber(p), Packet[0] == 'Z') ? "OK" : "E28");
            }
            break;
        case 'C': // with a signal: there is nothing to deliver it to, so as 'c' / 's'
        case 'S':
            ParseHexNumber(p);
            if (*p == ';')
                p++;
            [[fallthrough]];
        case 'c':
        case 's':
            if (*p)
                Frame->Eip = ParseHexNumber(p);
            Session->Stepping = Packet[0] == 's' || Packet[0] == 'S';
            InsertPendingBreakpoints(); // those whose pages came since they were set
            Frame->EFlags = Session->Stepping ? Frame->EFlags | EFLAGS_TF : Frame->EFlags & ~EFLAGS_TF;
            Session->WaitingForStop = true;
            return true;
        case 'D':
            SendPacket("OK");
            DetachDebugger();
            Frame->EFlags &= ~EFLAGS_TF;
            return false;
        case 'k':
            DetachDebugger();
            Frame->EFlags &= ~EFLAGS_TF;
            return false;
        case '!': // extended-remote mode: `run` (vRun) starts titles
            SetReply("OK");
            break;
        case 'v':
            if (!strncmp(Packet, "vRun;", 5)) {
                HandleRun(Packet + 5);
            } else if (!strncmp(Packet, "vKill;", 6)) {
                SendPacket("OK");
                DetachDebugger();
                Frame->EFlags &= ~EFLAGS_TF;
                return false;
            }
            break; // others (vCont, vMustReplyEmpty): the empty reply
        default:
            break; // not supported: the empty reply
        }
        SendPacket(Reply);
    }
}

// The target goes on with StoppedFrame at StoppedEsp: from the stub's stack, not through the frames below the stopped code's stack pointer, which gdb may
// have written over.
[[noreturn]] void ResumeStoppedFrame() {
    STUB_BUFFER static RESUME_CONTEXT context; // GdbStubResume reads it after leaving this stack
    context.Eax = StoppedFrame.Eax;
    context.Ecx = StoppedFrame.Ecx;
    context.Edx = StoppedFrame.Edx;
    context.Ebx = StoppedFrame.Ebx;
    context.Ebp = StoppedFrame.Ebp;
    context.Esi = StoppedFrame.Esi;
    context.Edi = StoppedFrame.Edi;
    context.Eip = StoppedFrame.Eip;
    context.SegCs = StoppedFrame.SegCs;
    context.EFlags = StoppedFrame.EFlags;
    context.Esp = StoppedEsp;
    StoppedStackTop = 0;
    LoadDebugRegisters();
    GdbStubResume(&context);
}

// On the stub's stack: serve gdb. A resume that moved the stack pointer, or follows writes into the frames below it, does not come back here.
void ServeOnStubStack() {
    StopResumed = ServeDebugger(&StoppedFrame);
    if (StoppedEsp != StoppedStackTop || StackWritten)
        ResumeStoppedFrame(); // after a detach too: an exception's instruction runs again and, without gdb, goes on to the bug check
}

// A stop: interrupts off throughout (the frame's own flag comes back with iret). gdb works on a copy of the frame, served on the stub's own stack; StackPop
// bytes come off the stopped code's stack first (a ret the stub finished). True when gdb resumed, false when it detached.
bool StopTarget(OXOS_TRAP_FRAME *Frame, int Signal, const char *Reason, bool SendReply, uint32_t StackPop = 0) {
    DisableInterrupts();
    SelectedThread = 0;
    __asm__ volatile("mov %0, %%dr7" ::"r"(0u)); // the stub's own reads of a watched variable must not trap: on again when the target resumes
    StoppedFrame = *Frame;
    StoppedStackTop = (uint32_t)(&Frame->EFlags + 1);
    StoppedEsp = StoppedStackTop + StackPop;
    uint32_t esp;
    __asm__ volatile("mov %%esp, %0" : "=r"(esp));
    StoppedFramesBottom = esp - 32; // and GdbStubCallOnStack's frame, below this function's
    StackWritten = false;
    BuildStopReply(&StoppedFrame, Signal, Reason);
    if (SendReply)
        SendPacket(StopReply);
    Session->WaitingForStop = false;
    GdbStubCallOnStack(ServeOnStubStack, StubStack + STUB_STACK_LENGTH);
    *Frame = StoppedFrame;
    StoppedStackTop = 0;
    LoadDebugRegisters();
    return StopResumed;
}

// gdb `call` returns to a breakpoint gdb puts on the stack, above the code segment's limit (0xBFFFFFFF): the function's ret raises a #GP (on a console;
// xemu does not check the limit). When the ret's target is an inserted breakpoint, the stub finishes the ret and stops there, as gdb expects. False for any
// other #GP.
bool FinishReturnToBreakpoint(OXOS_TRAP_FRAME *Frame) {
    uint32_t esp = (uint32_t)(&Frame->EFlags + 1);
    uint8_t code[3], target[4];
    if (!ReadMemoryByte(Frame->Eip, code[0]) || (code[0] != 0xC3 && code[0] != 0xC2))
        return false; // ret, ret imm16
    for (uint32_t i = 0; i < 4; i++) {
        if (!ReadMemoryByte(esp + i, target[i]))
            return false;
    }
    uint32_t address = target[0] | target[1] << 8 | target[2] << 16 | (uint32_t)target[3] << 24;
    BREAKPOINT *b = FindBreakpoint(address);
    if (!b || !b->Inserted)
        return false;
    uint32_t pop = 4;
    if (code[0] == 0xC2) {
        if (!ReadMemoryByte(Frame->Eip + 1, code[1]) || !ReadMemoryByte(Frame->Eip + 2, code[2]))
            return false;
        pop += code[1] | code[2] << 8;
    }
    Frame->Eip = address;
    StopTarget(Frame, SIGTRAP_, "swbreak:;", Session->WaitingForStop, pop);
    return true;
}

// A hardware breakpoint or watchpoint (DR0-DR3) stopped the target: the stop names it. False when it is not one gdb set.
bool StopOnWatchpoint(OXOS_TRAP_FRAME *Frame, uint32_t Dr6) {
    int slot = Dr6 & 1 ? 0 : Dr6 & 2 ? 1 : Dr6 & 4 ? 2 : 3;
    const WATCHPOINT &w = Session->Watchpoints[slot];
    if (!Session->Attached || !w.Address)
        return false;
    static const char *const NAMES[] = {"hwbreak", "hwbreak", "watch", "rwatch", "awatch"};
    char reason[32];
    char *out = reason;
    for (const char *n = NAMES[w.Kind]; *n; n++)
        *out++ = *n;
    *out++ = ':';
    if (w.Kind != 1) {
        for (int shift = 28; shift >= 0; shift -= 4)
            *out++ = HEX_DIGITS[(w.Address >> shift) & 15];
    }
    *out++ = ';';
    *out = 0;
    Frame->EFlags &= ~EFLAGS_TF;
    if (w.Kind == 1)
        Frame->EFlags |= EFLAGS_RF; // the instruction runs once when resumed, instead of stopping again
    Session->Stepping = false;
    StopTarget(Frame, SIGTRAP_, reason, Session->WaitingForStop);
    return true;
}

// --- The debugger's functions (OXOS_DEBUGGER)

BOOLEAN HandleTrap(OXOS_TRAP_FRAME *Frame) {
    uint32_t vector = Frame->Vector;
    if (InMemoryAccess && (vector == 13 || vector == 14) &&
        (Frame->Eip == (uint32_t)GdbStubCopyByteRead || Frame->Eip == (uint32_t)GdbStubCopyByteWrite)) {
        Frame->Eip = (uint32_t)GdbStubCopyByteFault;
        return TRUE;
    }
    if (vector == 13 && Session->Attached && FinishReturnToBreakpoint(Frame))
        return TRUE;
    if (vector == 3) {
        uint32_t address = Frame->Eip - 1; // the int3 itself
        BREAKPOINT *b = FindBreakpoint(address);
        if (b && b->Inserted) {
            Frame->Eip = address;
            StopTarget(Frame, SIGTRAP_, "swbreak:;", Session->WaitingForStop);
            return TRUE;
        }
        if (LibraryStop) {
            LibraryStop = false;
            StopTarget(Frame, SIGTRAP_, "library:;", true); // gdb reads the library list and goes on by itself
            return TRUE;
        }
        if (WaitingForGdb || AfterBugCheck) {
            WaitingForGdb = false;
            StopTarget(Frame, AfterBugCheck ? SIGABRT_ : SIGTRAP_, "", Session->WaitingForStop); // a gdb that connects asks with '?'
            return TRUE;
        }
        if (Session->Attached) {
            StopTarget(Frame, SIGTRAP_, "", Session->WaitingForStop); // an int3 in the code (DbgBreakPoint, __debugbreak): resumes after it
            return TRUE;
        }
        return FALSE;
    }
    if (vector != 1)
        return FALSE;
    uint32_t dr6;
    __asm__ volatile("mov %%dr6, %0" : "=r"(dr6));
    __asm__ volatile("mov %0, %%dr6" ::"r"(0u));
    uint8_t breakIn = BreakInByte;
    if (dr6 & 0xF)
        return StopOnWatchpoint(Frame, dr6);
    if (!(dr6 & DR6_BS) || (!Session->Stepping && !breakIn))
        return FALSE;
    Frame->EFlags &= ~EFLAGS_TF;
    BreakInByte = 0;
    if (Session->Stepping) {
        Session->Stepping = false;
        StopTarget(Frame, SIGTRAP_, "", Session->WaitingForStop);
    } else {
        PacketStarted = breakIn == '$'; // gdb attaching: its first packet has begun
        StopTarget(Frame, SIGINT_, "", breakIn == 0x03);
    }
    return TRUE;
}

BOOLEAN StopOnUnhandledException(OXOS_TRAP_FRAME *Frame) {
    if (!Session->Attached)
        return FALSE;
    int signal;
    switch (Frame->Vector) {
    case 0:
    case 16:
    case 19: signal = SIGFPE_; break;
    case 6: signal = SIGILL_; break;
    case 17: signal = SIGBUS_; break;
    case 1:
    case 3: signal = SIGTRAP_; break;
    default: signal = SIGSEGV_; break;
    }
    ReportedException = true;
    return StopTarget(Frame, signal, "", Session->WaitingForStop);
}

void StopOnBugCheck(ULONG Code) {
    if (!Session->Attached || (Code == 0x1E && ReportedException))
        return;
    __asm__ volatile("int3"); // stops as a breakpoint in KeBugCheckEx: gdb shows the caller's stack
}

void WaitAfterBugCheck() {
    AfterBugCheck = true;
    __asm__ volatile("int3");
}

void PollBreakIn(PULONG InterruptedFrame) {
    if (BreakInByte)
        return; // already on its way
    uint8_t byte;
    if (!TakeByte(byte))
        return;
    if (byte == 0x03 || byte == '$') {
        BreakInByte = byte;
        InterruptedFrame[2] |= EFLAGS_TF;
    }
}

void TitleLoaded(const char *Path) {
    // Only the title gdb debugs (`monitor title`; any title without one): another one (the boot video player, a dashboard) has other code there.
    InTitle = !Session->TitleFilter[0] || (Path && ContainsText(Path, Session->TitleFilter));
    if (!InTitle)
        return;
    if (Path)
        strlcpy(Session->TitlePath, Path, sizeof(Session->TitlePath));
    uint32_t flags = SaveFlagsAndDisableInterrupts();
    InsertPendingBreakpoints();
    RestoreFlags(flags);
    if (Session->Attached && Session->WaitingForStop) {
        // gdb follows the console into this title: a library stop, after which gdb reads the list (symbols in and out) and resumes by itself.
        LibraryStop = true;
        __asm__ volatile("int3");
    } else if (Session->WaitAtTitle && !Session->Attached) {
        WaitForDebugger("gdb: waiting for gdb at the title's start\n");
    }
}

BOOLEAN PrintDebugText(const char *Text, ULONG Length) {
    if (!Session->Attached || !Session->WaitingForStop)
        return FALSE;
    // "O" + hex: half a packet of text each; not acked (gdb acks, the next poll drops it), so a running title is never held up by the round trip.
    while (Length) {
        uint32_t chunk = Length < (PACKET_SIZE - 2) / 2 ? Length : (PACKET_SIZE - 2) / 2;
        char *out = Reply;
        *out++ = 'O';
        for (uint32_t i = 0; i < chunk; i++)
            out = AppendHexByte(out, (uint8_t)Text[i]);
        *out = 0;
        SendPacket(Reply, false);
        Text += chunk;
        Length -= chunk;
    }
    return TRUE;
}

const OXOS_DEBUGGER GDB_STUB = {sizeof(OXOS_DEBUGGER), HandleTrap, StopOnUnhandledException, StopOnBugCheck, WaitAfterBugCheck, PollBreakIn, TitleLoaded,
                                PrintDebugText};

} // namespace

// --- Starting and ending

void Start(SESSION &SessionStorage, const OXOS_DEBUG_PORT &KernelPort, const OXOS_DEBUG_TRANSPORT &Own) {
    Session = &SessionStorage;
    Port = &KernelPort;
    OwnTransport = &Own;
    InTitle = false;
    if (Session->Magic != SESSION_MAGIC) {
        memset(Session, 0, sizeof(*Session));
        Session->Magic = SESSION_MAGIC;
    }
    // The title's memory is gone (a quick reboot) or not there yet: its breakpoints wait for the next title. The kernel's go in (again) now.
    for (auto &b : Session->Breakpoints) {
        if (b.Address && b.Address < TITLE_SPACE_END)
            b.Inserted = false;
    }
    uint32_t flags = SaveFlagsAndDisableInterrupts();
    InsertPendingBreakpoints();
    LoadDebugRegisters(); // the processor set-up may have cleared them
    RestoreFlags(flags);
}

const OXOS_DEBUGGER &Debugger() {
    return GDB_STUB;
}

void AnnounceReady() {
    Announce("gdb: stub ready\n");
}

void WaitForDebugger(const char *Announcement) {
    if (Announcement)
        Announce(Announcement);
    WaitingForGdb = true;
    __asm__ volatile("int3");
}

const char *TakeLaunchRequest() {
    if (!Session || !Session->LaunchRequested)
        return nullptr;
    Session->LaunchRequested = false;
    return Session->LaunchPath;
}

void PrepareForUnload() {
    uint32_t flags = SaveFlagsAndDisableInterrupts();
    for (auto &b : Session->Breakpoints)
        WithdrawBreakpoint(b);
    __asm__ volatile("mov %0, %%dr7" ::"r"(0u));
    RestoreFlags(flags);
}

} // namespace oxos::gdb_stub
