// Ethernet PHY over the NIC's MII management interface (NIC registers at 0xFEF00000) (spec/stock/16).
#include <hardware/chipset/chipset.h>
#include <core/core.h>

namespace xk::hardware::ethernet {

constexpr ULONG NIC_BASE = 0xFEF00000, MII_CONTROL = 0x190, MII_DATA = 0x194, MII_BUSY = 0x8000, MII_WRITE = 0x400,
                PHY_ADDRESS = 0x20; // PHY 1, shifted into the control register
constexpr ULONG PHY_LINK_UP = 1, PHY_100MBPS = 2, PHY_10MBPS = 4, PHY_FULL_DUPLEX = 8, PHY_HALF_DUPLEX = 0x10;
constexpr ULONG E_PHY_FAIL = 0x801F0001, E_PHY_BUSY = 0x800700AA;

static volatile LONG PhyBusy; // one caller at a time drives the MII interface (the others return E_PHY_BUSY)
static ULONG PhyLinkState;    // PHY_* flags of the last check
// Kept across quick reboots (reference 0x8003B340): no new negotiation per title.
XK_PERSIST static BOOLEAN PhyInitialized;

static volatile ULONG &Nic(ULONG offset) {
    return *(volatile ULONG *)(NIC_BASE + offset);
}

static bool WaitForPhy() {
    for (int us = 0x2000; us > 0 && (Nic(MII_CONTROL) & MII_BUSY); us -= 50)
        KeStallExecutionProcessor(50);
    return !(Nic(MII_CONTROL) & MII_BUSY);
}

static bool ReadPhy(ULONG reg, PULONG value) {
    if (Nic(MII_CONTROL) & MII_BUSY)
        WaitForPhy();
    Nic(MII_CONTROL) = reg | PHY_ADDRESS;
    bool ok = WaitForPhy();
    *value = Nic(MII_DATA);
    return ok;
}

static bool WritePhy(ULONG reg, ULONG value) {
    if (Nic(MII_CONTROL) & MII_BUSY)
        WaitForPhy();
    Nic(MII_DATA) = value;
    Nic(MII_CONTROL) = reg | PHY_ADDRESS | MII_WRITE;
    return WaitForPhy();
}

// Link state from the negotiated abilities (advertised & partner) and the status register.
static bool UpdatePhyLinkState() {
    ULONG advertised, partner, status;
    if (!ReadPhy(4, &advertised) || !ReadPhy(5, &partner) || !ReadPhy(1, &status))
        return false;
    ULONG common = advertised & partner, state = 0;
    if (common & 0x180)
        state = PHY_100MBPS;
    else if (common & 0x60)
        state = PHY_10MBPS;
    if (common & 0x140)
        state |= PHY_FULL_DUPLEX;
    else if (common & 0xA0)
        state |= PHY_HALF_DUPLEX;
    if (status & 4)
        state |= PHY_LINK_UP;
    PhyLinkState = state;
    return true;
}

// The status register once the link is up, or after 500 ms.
static ULONG WaitForPhyLink() {
    ULONG status = 0;
    for (int i = 0; i < 1000 && !(status & 4); i++) {
        KeStallExecutionProcessor(500);
        if (!ReadPhy(1, &status))
            break;
    }
    return status;
}

XBAPI NTSTATUS NTAPI PhyInitialize(BOOLEAN ForceReset, PVOID Parameter2) {
    if (__atomic_exchange_n(&PhyBusy, 1, __ATOMIC_SEQ_CST))
        return (NTSTATUS)E_PHY_BUSY;
    NTSTATUS result = (NTSTATUS)E_PHY_FAIL;
    ULONG control = 0, status = 0;
    if (!ForceReset && PhyInitialized) {
        UpdatePhyLinkState();
        result = STATUS_SUCCESS;
        goto done;
    }
    PhyInitialized = FALSE;
    PhyLinkState = 0;
    if (ForceReset) {
        if (!WritePhy(0, 0x8000)) // reset
            goto done;
        control = 0x8000;
        for (int i = 0; i < 1000 && (control & 0x8000); i++) {
            KeStallExecutionProcessor(500);
            if (!ReadPhy(0, &control))
                goto done;
        }
        if (control & 0x8000)
            goto done;
    }
    for (int i = 0; i < 6000 && !(status & 0x20); i++) { // auto-negotiation complete
        KeStallExecutionProcessor(500);
        if (!ReadPhy(1, &status))
            goto done;
    }
    if (!ReadPhy(0, &control))
        goto done;
    if (!(control & 0x200)) {
        WaitForPhyLink();
        if (!UpdatePhyLinkState())
            goto done;
    } else {
        // Without negotiation: pick the best ability the PHY reports.
        if (status & 0xA200) {
            control = (control & ~0x40u) | 0x2000;
            PhyLinkState |= PHY_100MBPS;
        } else if (status & 0x800) {
            control = (control & ~0x2000u) | 0x40;
            PhyLinkState |= PHY_10MBPS;
        } else {
            goto done;
        }
        PhyLinkState |= PHY_HALF_DUPLEX;
        WritePhy(0, control);
        if (WaitForPhyLink() & 4)
            PhyLinkState |= PHY_LINK_UP;
    }
    PhyInitialized = TRUE;
    result = STATUS_SUCCESS;
done:
    PhyBusy = 0;
    return result;
}

XBAPI ULONG NTAPI PhyGetLinkState(BOOLEAN Verify) {
    if ((!PhyLinkState || Verify) && !__atomic_exchange_n(&PhyBusy, 1, __ATOMIC_SEQ_CST)) {
        UpdatePhyLinkState();
        PhyBusy = 0;
    }
    return PhyLinkState;
}

} // namespace xk::hardware::ethernet
