// HAL phase 0, cold path (spec/stock/02-executive-init.md §3). Order matches the reference kernel;
// the 0x80CF pulse runs between steps.
#include <xk/arch.h>
#include <xk/hal.h>
#include <xk/ke.h>
#include <xk/mm.h>

UCHAR HalBootFlags;

// Purpose unknown (spec 02 §3): writes 5 then 4 to MCPX I/O 0x80CF with 5 us stalls.
void HalpPulse() {
    if (HalBootFlags & 1)
        return;
    outb(0x80CF, 5);
    KeStallExecutionProcessor(5);
    outb(0x80CF, 4);
    KeStallExecutionProcessor(5);
}

static void HalpDisableLocalApic() {
    constexpr ULONG IA32_APIC_BASE = 0x1B;
    constexpr ULONGLONG APIC_GLOBAL_ENABLE = 1ull << 11;
    wrmsr(IA32_APIC_BASE, rdmsr(IA32_APIC_BASE) & ~APIC_GLOBAL_ENABLE);
}

void HalInitSystemPhase0(bool coldBoot) {
    if (!coldBoot) {
        // Quick reboot: only the interrupt controller is reprogrammed.
        HalpPulse();
        HalpInitPic();
        return;
    }

    outb(0x61, 0x08); // system control port B: disable IOCHK NMI
    HalpPulse();
    HalpDisableLocalApic();
    HalpPulse();
    HalpInitPic();
    post(post_code::Pic);
    HalpPulse();
    HalpInitRtc();
    post(post_code::Rtc);
    HalpPulse();
    HalpInitPit();
    post(post_code::Pit);
    HalpPulse();
    HalpInitRtcLate();
    HalpPulse();
    HalpInitPci();
    post(post_code::Pci);
    HalpPulse();
    HalpInitAcpiPm();
    post(post_code::AcpiPm);
}

// spec/stock/02 §5 (subset). TODO(spec): GpuRevision should come from NV2A PMC_BOOT_0 (0xFD000000)
// once the MMIO mapping exists; the strap-derived flags 0x10/0x20/0x40/0x80 are also pending.
// Filled at cold boot only and kept across quick reboots, as in the reference (0x8003B150 lies past the .data/BSS
// range the quick reboot re-initializes). Titles launched by another title read it too: Half-Life 2's USB library
// reserves its descriptors by Flags bit 0 and, with the flags zeroed, ran out of them and never found the pad.
XK_PERSIST XBOX_HARDWARE_INFO XboxHardwareInfo;

// spec/stock/02 §5 (confirmed from the assembly, 0x80055081).
void HalpDetectHardware() {
    auto flash = (volatile UCHAR *)MmMapIoSpace(0xFFF00078, 4, PAGE_READWRITE | PAGE_NOCACHE);
    if (flash) {
        if (*flash == 0x80) {
            XboxHardwareInfo.Flags |= 0x10;
            if (inb(0x80CE) & 0x20)
                XboxHardwareInfo.Flags |= 0x20;
        }
        MmUnmapIoSpace((PVOID)flash, 4);
    }
    // NV2A straps, bits 25-26: 0 → 0x40. (The reference also tests the masked value against 1 for 0x80, which can
    // never match; 0x80 is therefore never set.)
    if (!(*(volatile ULONG *)0xFD101000 & 0x06000000))
        XboxHardwareInfo.Flags |= 0x40;
    XboxHardwareInfo.McpRevision = HalpPciRead8(0, 1, 0x08);
    XboxHardwareInfo.GpuRevision = *(volatile UCHAR *)0xFD000000; // NV2A boot ID, low byte
    if (XboxHardwareInfo.McpRevision <= 0xB3)
        XboxHardwareInfo.Flags |= 0x01;
}
