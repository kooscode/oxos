// Event objects (spec/06 §6, waits in spec/07).
#include <xk/ke.h>

XBAPI void NTAPI KeInitializeEvent(PKEVENT Event, EVENT_TYPE Type, BOOLEAN State) {
    // Bytes 1 (Absolute) and 3 (Inserted) are deliberately left as they are, like the reference.
    Event->Header.Type = (UCHAR)Type;
    Event->Header.Size = sizeof(KEVENT) / sizeof(ULONG);
    Event->Header.SignalState = State;
    InitializeListHead(&Event->Header.WaitListHead);
}

// Wait = TRUE: return still at DISPATCH_LEVEL; the caller's next Ke wait call completes the unlock.
static void KiFinishSignal(KIRQL old, BOOLEAN Wait) {
    if (Wait) {
        PKTHREAD t = KiPcr.PrcbData.CurrentThread;
        t->WaitNext = TRUE;
        t->WaitIrql = old;
    } else {
        KiUnlockDispatcherDatabase(old);
    }
}

XBAPI LONG NTAPI KeSetEvent(PKEVENT Event, LONG Increment, BOOLEAN Wait) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    LONG previous = Event->Header.SignalState;
    Event->Header.SignalState = 1;
    if (!IsListEmpty(&Event->Header.WaitListHead))
        KiWaitTest(&Event->Header, Increment);
    KiFinishSignal(old, Wait);
    return previous;
}

XBAPI LONG NTAPI KePulseEvent(PKEVENT Event, LONG Increment, BOOLEAN Wait) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    LONG previous = Event->Header.SignalState;
    if (previous == 0 && !IsListEmpty(&Event->Header.WaitListHead)) {
        Event->Header.SignalState = 1;
        KiWaitTest(&Event->Header, Increment);
    }
    Event->Header.SignalState = 0;
    KiFinishSignal(old, Wait);
    return previous;
}

XBAPI LONG NTAPI KeResetEvent(PKEVENT Event) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    LONG previous = Event->Header.SignalState;
    Event->Header.SignalState = 0;
    KfLowerIrql(old);
    return previous;
}
