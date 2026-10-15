// Ethernet PHY over the NIC's MII management interface (NIC registers at 0xFEF00000) (spec/16).
#include <xk/hal.h>
#include <xk/ke.h>

constexpr ULONG NIC_BASE = 0xFEF00000, MII_CONTROL = 0x190, MII_DATA = 0x194, MII_BUSY = 0x8000, MII_WRITE = 0x400,
                PHY_ADDRESS = 0x20; // PHY 1, shifted into the control register
constexpr ULONG PHY_LINK_UP = 1, PHY_100MBPS = 2, PHY_10MBPS = 4, PHY_FULL_DUPLEX = 8, PHY_HALF_DUPLEX = 0x10;
constexpr ULONG E_PHY_FAIL = 0x801F0001, E_PHY_BUSY = 0x800700AA;

static volatile LONG PhypBusy;
static ULONG PhypLinkState;
static BOOLEAN PhypInitialized;

static volatile ULONG &Nic(ULONG offset) { return *(volatile ULONG *)(NIC_BASE + offset); }

static bool PhypWait() {
    for (int us = 0x2000; us > 0 && (Nic(MII_CONTROL) & MII_BUSY); us -= 50)
        KeStallExecutionProcessor(50);
    return !(Nic(MII_CONTROL) & MII_BUSY);
}

static bool PhypRead(ULONG reg, PULONG value) {
    if (Nic(MII_CONTROL) & MII_BUSY)
        PhypWait();
    Nic(MII_CONTROL) = reg | PHY_ADDRESS;
    bool ok = PhypWait();
    *value = Nic(MII_DATA);
    return ok;
}

static bool PhypWrite(ULONG reg, ULONG value) {
    if (Nic(MII_CONTROL) & MII_BUSY)
        PhypWait();
    Nic(MII_DATA) = value;
    Nic(MII_CONTROL) = reg | PHY_ADDRESS | MII_WRITE;
    return PhypWait();
}

// Link state from the negotiated abilities (advertised & partner) and the status register.
static bool PhypUpdateLinkState() {
    ULONG advertised, partner, status;
    if (!PhypRead(4, &advertised) || !PhypRead(5, &partner) || !PhypRead(1, &status))
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
    PhypLinkState = state;
    return true;
}

static ULONG PhypWaitForLink() {
    ULONG status = 0;
    for (int i = 0; i < 1000 && !(status & 4); i++) {
        KeStallExecutionProcessor(500);
        if (!PhypRead(1, &status))
            break;
    }
    return status;
}

XBAPI NTSTATUS NTAPI PhyInitialize(BOOLEAN ForceReset, PVOID Parameter2) {
    if (__atomic_exchange_n(&PhypBusy, 1, __ATOMIC_SEQ_CST))
        return (NTSTATUS)E_PHY_BUSY;
    NTSTATUS result = (NTSTATUS)E_PHY_FAIL;
    ULONG control = 0, status = 0;
    if (!ForceReset && PhypInitialized) {
        PhypUpdateLinkState();
        result = STATUS_SUCCESS;
        goto done;
    }
    PhypInitialized = FALSE;
    PhypLinkState = 0;
    if (ForceReset) {
        if (!PhypWrite(0, 0x8000)) // reset
            goto done;
        control = 0x8000;
        for (int i = 0; i < 1000 && (control & 0x8000); i++) {
            KeStallExecutionProcessor(500);
            if (!PhypRead(0, &control))
                goto done;
        }
        if (control & 0x8000)
            goto done;
    }
    for (int i = 0; i < 6000 && !(status & 0x20); i++) { // auto-negotiation complete
        KeStallExecutionProcessor(500);
        if (!PhypRead(1, &status))
            goto done;
    }
    if (!PhypRead(0, &control))
        goto done;
    if (!(control & 0x200)) {
        PhypWaitForLink();
        if (!PhypUpdateLinkState())
            goto done;
    } else {
        // Without negotiation: pick the best ability the PHY reports.
        if (status & 0xA200) {
            control = (control & ~0x40u) | 0x2000;
            PhypLinkState |= PHY_100MBPS;
        } else if (status & 0x800) {
            control = (control & ~0x2000u) | 0x40;
            PhypLinkState |= PHY_10MBPS;
        } else {
            goto done;
        }
        PhypLinkState |= PHY_HALF_DUPLEX;
        PhypWrite(0, control);
        if (PhypWaitForLink() & 4)
            PhypLinkState |= PHY_LINK_UP;
    }
    PhypInitialized = TRUE;
    result = STATUS_SUCCESS;
done:
    PhypBusy = 0;
    return result;
}

XBAPI ULONG NTAPI PhyGetLinkState(BOOLEAN Verify) {
    if ((!PhypLinkState || Verify) && !__atomic_exchange_n(&PhypBusy, 1, __ATOMIC_SEQ_CST)) {
        PhypUpdateLinkState();
        PhypBusy = 0;
    }
    return PhypLinkState;
}
