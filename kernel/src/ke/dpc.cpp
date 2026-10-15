// Deferred procedure calls (spec/06 §4).
#include <xk/arch.h>
#include <xk/hal.h>
#include <xk/ke.h>

#define Prcb (KiPcr.PrcbData)

XBAPI void NTAPI KeInitializeDpc(PKDPC Dpc, PKDEFERRED_ROUTINE DeferredRoutine, PVOID DeferredContext) {
    Dpc->Type = DpcObject;
    Dpc->Inserted = FALSE;
    Dpc->DeferredRoutine = DeferredRoutine;
    Dpc->DeferredContext = DeferredContext;
}

XBAPI BOOLEAN NTAPI KeInsertQueueDpc(PKDPC Dpc, PVOID SystemArgument1, PVOID SystemArgument2) {
    ULONG flags = save_flags_cli(); // HIGH_LEVEL for the list update
    BOOLEAN queued = !Dpc->Inserted;
    if (queued) {
        Dpc->SystemArgument1 = SystemArgument1;
        Dpc->SystemArgument2 = SystemArgument2;
        Dpc->Inserted = TRUE;
        InsertTailList(&Prcb.DpcListHead, &Dpc->DpcListEntry);
        if (!Prcb.DpcRoutineActive && !Prcb.DpcInterruptRequested) {
            Prcb.DpcInterruptRequested = TRUE;
            HalRequestSoftwareInterrupt(DISPATCH_LEVEL);
        }
    }
    restore_flags(flags);
    return queued;
}

XBAPI BOOLEAN NTAPI KeRemoveQueueDpc(PKDPC Dpc) {
    ULONG flags = save_flags_cli();
    BOOLEAN wasInserted = Dpc->Inserted;
    if (wasInserted) {
        RemoveEntryList(&Dpc->DpcListEntry);
        Dpc->Inserted = FALSE;
    }
    restore_flags(flags);
    return wasInserted;
}

XBAPI BOOLEAN NTAPI KeIsExecutingDpc() { return (BOOLEAN)Prcb.DpcRoutineActive; }

// Runs at DISPATCH_LEVEL with interrupts enabled.
void KiRetireDpcList() {
    Prcb.DpcRoutineActive = TRUE;
    for (;;) {
        cli();
        if (IsListEmpty(&Prcb.DpcListHead)) {
            Prcb.DpcInterruptRequested = FALSE;
            Prcb.DpcRoutineActive = FALSE;
            sti();
            return;
        }
        PKDPC dpc = CONTAINING_RECORD(RemoveHeadList(&Prcb.DpcListHead), KDPC, DpcListEntry);
        dpc->Inserted = FALSE;
        PKDEFERRED_ROUTINE routine = dpc->DeferredRoutine;
        PVOID ctx = dpc->DeferredContext, a1 = dpc->SystemArgument1, a2 = dpc->SystemArgument2;
        sti();
        routine(dpc, ctx, a1, a2);
    }
}

// DISPATCH_LEVEL software interrupt handler.
void KiDispatchInterrupt() {
    if (!IsListEmpty(&Prcb.DpcListHead))
        KiRetireDpcList();
    // Milestone 2b: quantum end and switching to Prcb.NextThread.
}

void KiInitializeDpcs() { InitializeListHead(&Prcb.DpcListHead); }
