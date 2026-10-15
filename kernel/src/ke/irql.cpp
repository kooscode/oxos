// IRQL bookkeeping. Milestone 1 tracks the level only; PIC masking by IRQL and
// software interrupts (APC/DPC delivery) arrive with the scheduler.
#include <xk/ke.h>

XBAPI KIRQL NTAPI KeGetCurrentIrql() { return KiPcr.Irql; }

XBAPI KIRQL FASTCALL KfRaiseIrql(KIRQL NewIrql) {
    KIRQL old = KiPcr.Irql;
    KiPcr.Irql = NewIrql;
    return old;
}

XBAPI void FASTCALL KfLowerIrql(KIRQL NewIrql) { KiPcr.Irql = NewIrql; }

XBAPI KIRQL NTAPI KeRaiseIrqlToDpcLevel() { return KfRaiseIrql(DISPATCH_LEVEL); }

XBAPI KIRQL NTAPI KeRaiseIrqlToSynchLevel() { return KfRaiseIrql(SYNCH_LEVEL); }
