// HAL phase 0, cold path (spec/02-executive-init.md §3). Order matches the reference kernel;
// the 0x80CF pulse runs between steps.
#include <xk/arch.h>
#include <xk/hal.h>
#include <xk/ke.h>

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

// spec/02 §5 (subset). TODO(spec): GpuRevision should come from NV2A PMC_BOOT_0 (0xFD000000)
// once the MMIO mapping exists; the strap-derived flags 0x10/0x20/0x40/0x80 are also pending.
XBOX_HARDWARE_INFO XboxHardwareInfo;

void HalpDetectHardware() {
    XboxHardwareInfo.McpRevision = HalpPciRead8(0, 1, 0x08);
    XboxHardwareInfo.GpuRevision = HalpPciRead8(1, 0, 0x08);
    if (XboxHardwareInfo.McpRevision < 0xB4)
        XboxHardwareInfo.Flags |= 0x01;
}
