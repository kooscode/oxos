// IRQL, software interrupts and hardware interrupt dispatch (spec/06 §1-3).
//
// Device interrupts are masked in the PIC as soon as IRQL reaches their level (eager masking), so
// a hardware interrupt never arrives at or below the current IRQL and nothing has to be replayed.
// APC/DISPATCH software interrupts are pending bits delivered when IRQL drops below them.
#include <xk/arch.h>
#include <xk/hal.h>
#include <xk/ke.h>

constexpr ULONG SWI_APC = 1u << APC_LEVEL, SWI_DISPATCH = 1u << DISPATCH_LEVEL;

static volatile ULONG KiPendingSoftwareInterrupts;
static PKINTERRUPT KiInterruptObjects[16];

// Interrupts are disabled on entry and exit; enabled while a handler runs.
void KiDeliverSoftwareInterrupts() {
    for (;;) {
        KIRQL current = KiPcr.Irql;
        if ((KiPendingSoftwareInterrupts & SWI_DISPATCH) && current < DISPATCH_LEVEL) {
            KiPendingSoftwareInterrupts &= ~SWI_DISPATCH;
            KiPcr.Irql = DISPATCH_LEVEL;
            sti();
            KiDispatchInterrupt();
            cli();
            KiPcr.Irql = current;
            continue;
        }
        if ((KiPendingSoftwareInterrupts & SWI_APC) && current < APC_LEVEL) {
            KiPendingSoftwareInterrupts &= ~SWI_APC;
            KiPcr.Irql = APC_LEVEL;
            sti();
            KiDeliverApc();
            cli();
            KiPcr.Irql = current;
            continue;
        }
        return;
    }
}

XBAPI KIRQL NTAPI KeGetCurrentIrql() { return KiPcr.Irql; }

XBAPI KIRQL FASTCALL KfRaiseIrql(KIRQL NewIrql) {
    KIRQL old = KiPcr.Irql;
    if (NewIrql > DISPATCH_LEVEL) {
        ULONG flags = save_flags_cli();
        KiPcr.Irql = NewIrql;
        HalpSetIrqlMask(NewIrql);
        restore_flags(flags);
    } else {
        KiPcr.Irql = NewIrql;
    }
    return old;
}

XBAPI void FASTCALL KfLowerIrql(KIRQL NewIrql) {
    ULONG flags = save_flags_cli();
    KIRQL old = KiPcr.Irql;
    KiPcr.Irql = NewIrql;
    if (old > DISPATCH_LEVEL)
        HalpSetIrqlMask(NewIrql);
    if (KiPendingSoftwareInterrupts)
        KiDeliverSoftwareInterrupts();
    restore_flags(flags);
}

XBAPI KIRQL NTAPI KeRaiseIrqlToDpcLevel() { return KfRaiseIrql(DISPATCH_LEVEL); }

XBAPI KIRQL NTAPI KeRaiseIrqlToSynchLevel() { return KfRaiseIrql(SYNCH_LEVEL); }

XBAPI void FASTCALL HalRequestSoftwareInterrupt(KIRQL RequestIrql) {
    ULONG flags = save_flags_cli();
    KiPendingSoftwareInterrupts |= 1u << RequestIrql;
    if (RequestIrql > KiPcr.Irql)
        KiDeliverSoftwareInterrupts();
    restore_flags(flags);
}

XBAPI void FASTCALL HalClearSoftwareInterrupt(KIRQL RequestIrql) {
    ULONG flags = save_flags_cli();
    KiPendingSoftwareInterrupts &= ~(1u << RequestIrql);
    restore_flags(flags);
}

// Called from the IRQ entry stubs with interrupts disabled.
extern "C" void KiDispatchHardwareInterrupt(ULONG irq) {
    if (HalpIsSpuriousIrq(irq))
        return;
    KIRQL old = KiPcr.Irql;
    KIRQL level = HalpGetIrqLevel(irq);
    KiPcr.Irql = level;
    HalpSetIrqlMask(level);
    HalpEoi(irq);
    KiPcr.PrcbData.InterruptCount++;

    sti();
    if (irq == 0) {
        KiClockInterrupt(old);
    } else if (PKINTERRUPT obj = KiInterruptObjects[irq]) {
        obj->ServiceCount++;
        obj->ServiceRoutine(obj, obj->ServiceContext);
    }
    cli();

    KiPcr.Irql = old;
    HalpSetIrqlMask(old);
    if (KiPendingSoftwareInterrupts)
        KiDeliverSoftwareInterrupts();
}

// --- Interrupt objects (spec/06 §3). The IDT keeps fixed per-IRQ stubs that look up the
// connected object, so DispatchCode is only filled with a recognisable marker.

XBAPI void NTAPI KeInitializeInterrupt(PKINTERRUPT Interrupt, PKSERVICE_ROUTINE ServiceRoutine, PVOID ServiceContext,
                                       ULONG Vector, KIRQL Irql, KINTERRUPT_MODE InterruptMode, BOOLEAN ShareVector) {
    Interrupt->ServiceRoutine = ServiceRoutine;
    Interrupt->ServiceContext = ServiceContext;
    Interrupt->BusInterruptLevel = Vector - HAL_IRQ_BASE_VECTOR;
    Interrupt->Irql = Irql;
    Interrupt->ShareVector = ShareVector;
    Interrupt->Mode = (UCHAR)InterruptMode;
    Interrupt->Connected = FALSE;
    for (ULONG &w : Interrupt->DispatchCode)
        w = 0xCCCCCCCC; // int3: never executed
}

XBAPI BOOLEAN NTAPI KeConnectInterrupt(PKINTERRUPT Interrupt) {
    ULONG level = Interrupt->BusInterruptLevel;
    BOOLEAN connected = FALSE;
    KIRQL old = KfRaiseIrql(DISPATCH_LEVEL);
    if (!Interrupt->Connected && level > 0 && level < 16 && !KiInterruptObjects[level]) {
        KiInterruptObjects[level] = Interrupt;
        Interrupt->Connected = TRUE;
        HalpSetIrqLevel(level, (KIRQL)Interrupt->Irql);
        HalEnableSystemInterrupt(level, (KINTERRUPT_MODE)Interrupt->Mode);
        connected = TRUE;
    }
    KfLowerIrql(old);
    return connected;
}

XBAPI BOOLEAN NTAPI KeDisconnectInterrupt(PKINTERRUPT Interrupt) {
    BOOLEAN wasConnected = Interrupt->Connected;
    KIRQL old = KfRaiseIrql(DISPATCH_LEVEL);
    if (wasConnected) {
        ULONG level = Interrupt->BusInterruptLevel;
        HalDisableSystemInterrupt(level);
        KiInterruptObjects[level] = nullptr;
        HalpResetIrqLevel(level);
        Interrupt->Connected = FALSE;
    }
    KfLowerIrql(old);
    return wasConnected;
}
