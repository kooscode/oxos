// Event objects (spec/06 §6). Waking waiters arrives with the scheduler (milestone 2b).
#include <xk/ke.h>

void KiWaitTest(DISPATCHER_HEADER *Object, LONG Increment);

XBAPI void NTAPI KeInitializeEvent(PKEVENT Event, EVENT_TYPE Type, BOOLEAN State) {
    // Bytes 1 (Absolute) and 3 (Inserted) are deliberately left as they are, like the reference.
    Event->Header.Type = (UCHAR)Type;
    Event->Header.Size = sizeof(KEVENT) / sizeof(ULONG);
    Event->Header.SignalState = State;
    InitializeListHead(&Event->Header.WaitListHead);
}

XBAPI LONG NTAPI KeSetEvent(PKEVENT Event, LONG Increment, BOOLEAN Wait) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    LONG previous = Event->Header.SignalState;
    Event->Header.SignalState = 1;
    if (!IsListEmpty(&Event->Header.WaitListHead))
        KiWaitTest(&Event->Header, Increment);
    KfLowerIrql(old);
    return previous;
}

XBAPI LONG NTAPI KeResetEvent(PKEVENT Event) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    LONG previous = Event->Header.SignalState;
    Event->Header.SignalState = 0;
    KfLowerIrql(old);
    return previous;
}
