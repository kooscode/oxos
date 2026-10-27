// Machine-state probe (OXOS diagnostics): prints the CPU, chipset and GPU set-up a kernel leaves behind, one text line
// at a time, so two kernels can be compared on the same console (e.g. OXOS against a reference kernel at the end of
// start-up). Freestanding and header-only: no kernel headers, no imports; it touches the machine only through port
// I/O, MSR and descriptor-table reads and plain memory reads (NV2A registers at 0xFD000000, the page directory through
// its self-map at 0xC0300000), and prints through the caller's function. Reads only registers that are safe while
// the GPU engines are off (PMC, PFB, PRAMDAC clocks, PCRTC; never PFIFO/PGRAPH) and only MSRs the Pentium III has.
#pragma once

namespace statedump {

typedef unsigned long u32;
typedef unsigned long long u64;
typedef unsigned short u16;
typedef unsigned char u8;
typedef void (*Out)(const char *line);

static inline void outl_(u16 p, u32 v) { __asm__ volatile("outl %0, %1" ::"a"(v), "Nd"(p)); }
static inline u32 inl_(u16 p) { u32 v; __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline u8 inb_(u16 p) { u8 v; __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(p)); return v; }
static inline u64 rdmsr_(u32 m) { u32 lo, hi; __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(m)); return (u64)hi << 32 | lo; }
static inline void cpuid_(u32 leaf, u32 r[4]) { __asm__ volatile("cpuid" : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3]) : "a"(leaf), "c"(0)); }

// A tiny line builder: text and hex only.
struct Line {
    char b[240];
    int n = 0;
    Line &s(const char *t) { while (*t && n < 236) b[n++] = *t++; return *this; }
    Line &x(u32 v, int digits = 8) {
        for (int i = digits - 1; i >= 0 && n < 236; i--) b[n++] = "0123456789abcdef"[(v >> (4 * i)) & 15];
        return *this;
    }
    Line &sp() { if (n < 236) b[n++] = ' '; return *this; }
    void put(Out out) { b[n++] = '\n'; b[n] = 0; out(b); n = 0; }
};

static void Cpu(Out out) {
    Line l;
    u32 cr0, cr2, cr3, cr4, efl, dr7;
    __asm__ volatile("mov %%cr0, %0" : "=r"(cr0));
    __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
    __asm__ volatile("mov %%cr3, %0" : "=r"(cr3));
    __asm__ volatile("mov %%cr4, %0" : "=r"(cr4));
    __asm__ volatile("pushfl; popl %0" : "=r"(efl));
    __asm__ volatile("mov %%dr7, %0" : "=r"(dr7));
    l.s("sd cpu cr0 ").x(cr0).s(" cr3 ").x(cr3).s(" cr4 ").x(cr4).s(" eflags ").x(efl).s(" dr7 ").x(dr7).put(out);
    (void)cr2;
    for (u32 leaf = 0; leaf <= 2; leaf++) {
        u32 r[4];
        cpuid_(leaf, r);
        l.s("sd cpuid ").x(leaf, 2).sp().x(r[0]).sp().x(r[1]).sp().x(r[2]).sp().x(r[3]).put(out);
    }
    // Descriptor tables.
    struct __attribute__((packed)) { u16 limit; u32 base; } gdtr, idtr;
    u16 tr;
    __asm__ volatile("sgdt %0" : "=m"(gdtr));
    __asm__ volatile("sidt %0" : "=m"(idtr));
    __asm__ volatile("str %0" : "=r"(tr));
    l.s("sd gdtr ").x(gdtr.base).sp().x(gdtr.limit, 4).s(" idtr ").x(idtr.base).sp().x(idtr.limit, 4).s(" tr ").x(tr, 4)
        .put(out);
    auto gdt = (const volatile u32 *)gdtr.base;
    for (u32 i = 0; i < ((u32)gdtr.limit + 1) / 8 && i < 16; i++)
        l.s("sd gdt ").x(i * 8, 2).sp().x(gdt[2 * i + 1]).sp().x(gdt[2 * i]).put(out);
    // IDT: selector and type byte per vector (handler addresses differ between kernels by nature).
    auto idt = (const volatile u32 *)idtr.base;
    for (u32 v = 0; v < 0x50 && v < ((u32)idtr.limit + 1) / 8; v += 8) {
        l.s("sd idt ").x(v, 2);
        for (u32 k = v; k < v + 8; k++)
            l.sp().x(idt[2 * k] >> 16, 4).s(":").x((idt[2 * k + 1] >> 8) & 0xFF, 2);
        l.put(out);
    }
    // The TSS the task register selects: ESP0, SS0, CR3, I/O map base.
    u32 lo = gdt[(tr >> 3) * 2], hi = gdt[(tr >> 3) * 2 + 1];
    auto tss = (const volatile u32 *)((lo >> 16) | ((hi & 0xFF) << 16) | (hi & 0xFF000000));
    l.s("sd tss ").x((u32)tss).s(" esp0 ").x(tss[1]).s(" ss0 ").x(tss[2]).s(" cr3 ").x(tss[7]).s(" iomap ")
        .x(tss[25] >> 16, 4).put(out);
}

static void Msrs(Out out) {
    // Pentium III MSRs (Intel SDM vol. 3, P6 family): platform, APIC base, power-on configuration, microcode
    // revision, L2 control, SYSENTER, machine check, performance counters, debug control, MTRRs, PAT.
    static const u32 list[] = {0x17,  0x1B,  0x2A,  0x8B,  0xFE,  0x11E, 0x174, 0x175, 0x176, 0x179, 0x17A, 0x186,
                               0x187, 0x1D9, 0x200, 0x201, 0x202, 0x203, 0x204, 0x205, 0x206, 0x207, 0x208, 0x209,
                               0x20A, 0x20B, 0x20C, 0x20D, 0x20E, 0x20F, 0x250, 0x258, 0x259, 0x268, 0x269, 0x26A,
                               0x26B, 0x26C, 0x26D, 0x26E, 0x26F, 0x277, 0x2FF, 0x400, 0x401, 0x404, 0x405, 0x408,
                               0x409, 0x40C, 0x40D, 0x410, 0x411}; // banks' CTL and STATUS (ADDR may fault)
    Line l;
    u32 n = 0;
    for (u32 m : list) {
        if (m == 0x8B) { // microcode revision: write 0, CPUID 1, then read
            __asm__ volatile("wrmsr" ::"c"(0x8B), "a"(0), "d"(0));
            u32 r[4];
            cpuid_(1, r);
        }
        u64 v = rdmsr_(m);
        if (n == 0)
            l.s("sd msr");
        l.sp().x(m, 3).s("=").x((u32)(v >> 32)).x((u32)v);
        if (++n == 4) { l.put(out); n = 0; }
    }
    if (n)
        l.put(out);
}

static u32 PciRead(u32 bus, u32 dev, u32 reg) {
    outl_(0xCF8, 0x80000000 | bus << 16 | dev << 11 | reg);
    return inl_(0xCFC);
}

static void Pci(Out out) {
    // Function 0 of each device, plus 0:1.1 (SMBus). Probing functions that do not exist hangs the console.
    static const u8 devs[][3] = {{0, 0, 0}, {0, 1, 0}, {0, 1, 1}, {0, 2, 0}, {0, 3, 0}, {0, 4, 0}, {0, 5, 0},
                                 {0, 6, 0}, {0, 9, 0}, {0, 0x1E, 0}, {1, 0, 0}};
    Line l;
    for (auto &d : devs) {
        u32 df = (u32)d[1] << 3 | d[2];
        auto read = [&](u32 reg) { return PciRead(d[0], 0, df << 8 | reg); };
        if (read(0) == 0xFFFFFFFF)
            continue;
        for (u32 r = 0; r < 0x100; r += 0x20) {
            l.s("sd pci ").x(d[0], 1).s(":").x(d[1], 2).s(".").x(d[2], 1).s(" ").x(r, 2);
            for (u32 k = r; k < r + 0x20; k += 4)
                l.sp().x(read(k));
            l.put(out);
        }
    }
}

static void Gpu(Out out) {
    auto r = [](u32 o) { return *(const volatile u32 *)(0xFD000000 + o); };
    Line l;
    l.s("sd nv pmc ").x(r(0)).sp().x(r(0x100)).sp().x(r(0x140)).sp().x(r(0x200)).s(" pll ").x(r(0x680500)).sp()
        .x(r(0x680504)).sp().x(r(0x680508)).sp().x(r(0x68050C)).s(" gc ").x(r(0x680600)).s(" crtc ").x(r(0x600100))
        .sp().x(r(0x600140)).sp().x(r(0x600800)).put(out);
    // The blocks that set up memory and the display, read whole: PMC, PBUS (memory timing and pads), PFB, PEXTDEV
    // (straps), PCRTC and PRAMDAC. Not PFIFO, PGRAPH or PTIMER (running state, and PGRAPH reads can have effects).
    static const struct { const char *name; u32 from, to; } blocks[] = {
        {"pmc", 0x000000, 0x000400}, {"pbus", 0x001000, 0x002000}, {"pfb", 0x100000, 0x101000},
        {"pextdev", 0x101000, 0x101100}, {"pcrtc", 0x600000, 0x601000}, {"pramdac", 0x680000, 0x681000}};
    for (auto &b : blocks)
        for (u32 o = b.from; o < b.to; o += 0x20) {
            l.s("sd nv ").s(b.name).sp().x(o, 6);
            for (u32 k = o; k < o + 0x20; k += 4)
                l.sp().x(r(k));
            l.put(out);
        }
}

static void Io(Out out) {
    Line l;
    l.s("sd io pic ").x(inb_(0x21), 2).sp().x(inb_(0xA1), 2).s(" elcr ").x(inb_(0x4D0), 2).sp().x(inb_(0x4D1), 2)
        .s(" p61 ").x(inb_(0x61), 2).s(" bm ").x(inl_(0xFF60)).sp().x(inl_(0xFF64)).put(out);
    for (u32 p = 0x8000; p < 0x8100; p += 0x20) {
        l.s("sd io acpi ").x(p, 4);
        for (u32 k = p; k < p + 0x20; k += 4)
            l.sp().x(inl_((u16)k));
        l.put(out);
    }
}

static void Paging(Out out) {
    auto pd = (const volatile u32 *)0xC0300000;
    Line l;
    u32 n = 0;
    for (u32 i = 0; i < 1024; i++) {
        if (!pd[i])
            continue;
        if (n == 0)
            l.s("sd pde");
        l.sp().x(i, 3).s(":").x(pd[i]);
        if (++n == 8) { l.put(out); n = 0; }
    }
    if (n)
        l.put(out);
    // PTEs of the kernel image's first pages and of 0x80000000-0x8000F000, when mapped with 4 KB pages.
    if ((pd[0x200] & 1) && !(pd[0x200] & 0x80)) {
        auto pt = (const volatile u32 *)0xC0200000;
        for (u32 i = 0; i < 0x80; i += 8) {
            l.s("sd pte ").x(0x80000000 + i * 0x1000);
            for (u32 k = i; k < i + 8; k++)
                l.sp().x(pt[k]);
            l.put(out);
        }
    }
}

static void Dump(Out out, const char *who) {
    Line l;
    l.s("sd begin ").s(who).put(out);
    Cpu(out);
    Msrs(out);
    Pci(out);
    Gpu(out);
    Io(out);
    Paging(out);
    l.s("sd end").put(out);
}

} // namespace statedump
