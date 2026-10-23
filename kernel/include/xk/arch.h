// x86 primitives: port I/O, MSRs, interrupt flag, POST codes.
#pragma once
#include <xk/types.h>

static inline void outb(USHORT port, UCHAR v) { __asm__ volatile("outb %0, %1" ::"a"(v), "Nd"(port)); }
static inline void outw(USHORT port, USHORT v) { __asm__ volatile("outw %0, %1" ::"a"(v), "Nd"(port)); }
static inline void outl(USHORT port, ULONG v) { __asm__ volatile("outl %0, %1" ::"a"(v), "Nd"(port)); }

static inline UCHAR inb(USHORT port) {
    UCHAR v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline USHORT inw(USHORT port) {
    USHORT v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}
static inline ULONG inl(USHORT port) {
    ULONG v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port));
    return v;
}

static inline ULONGLONG rdmsr(ULONG msr) {
    ULONG lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((ULONGLONG)hi << 32) | lo;
}
static inline void wrmsr(ULONG msr, ULONGLONG v) {
    __asm__ volatile("wrmsr" ::"c"(msr), "a"((ULONG)v), "d"((ULONG)(v >> 32)));
}

static inline ULONGLONG rdtsc() {
    ULONG lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return ((ULONGLONG)hi << 32) | lo;
}

static inline ULONG read_cr3() {
    ULONG v;
    __asm__ volatile("mov %%cr3, %0" : "=r"(v));
    return v;
}

static inline void cli() { __asm__ volatile("cli" ::: "memory"); }
static inline void sti() { __asm__ volatile("sti" ::: "memory"); }
static inline void halt() { __asm__ volatile("hlt" ::: "memory"); }

#ifdef XK_HOST_TEST
// Host unit tests: user mode cannot change the interrupt flag, so the test harness keeps it (tests/unit/host);
// cli/sti/hlt and the other privileged instructions fault and are emulated there.
extern "C" ULONG HostSaveFlagsCli();
extern "C" void HostRestoreFlags(ULONG Flags);
static inline ULONG save_flags_cli() { return HostSaveFlagsCli(); }
static inline void restore_flags(ULONG f) { HostRestoreFlags(f); }
#else
static inline ULONG save_flags_cli() {
    ULONG f;
    __asm__ volatile("pushfl; popl %0; cli" : "=r"(f)::"memory");
    return f;
}
static inline void restore_flags(ULONG f) { __asm__ volatile("pushl %0; popfl" ::"r"(f) : "memory", "cc"); }
#endif

// Progress codes on port 0x80: visible on an LPC POST display on real hardware.
static inline void post(UCHAR code) { outb(0x80, code); }

namespace post_code {
enum : UCHAR {
    Entry = 0x10,
    Gdt = 0x11,
    Idt = 0x12,
    Pcr = 0x13,
    HalPhase0 = 0x20,
    Pic = 0x21,
    Rtc = 0x22,
    Pit = 0x23,
    Pci = 0x24,
    AcpiPm = 0x25,
    Idle = 0x30,
    Exception = 0xE0, // + vector (low bits)
};
}
