// Kernel timers (spec/06 §5). Inserted timers sit in one list sorted by due interrupt time; the clock
// interrupt queues an expiry DPC when the head is due.
#include <xk/arch.h>
#include <xk/ke.h>

static LIST_ENTRY KiTimerListHead;
static KDPC KiTimerExpireDpc;

static ULONGLONG KiReadSystemTime(const volatile KSYSTEM_TIME &t) {
    for (;;) {
        LONG high = t.High1Time;
        ULONG low = t.LowPart;
        if (high == t.High2Time)
            return (ULONGLONG)(ULONG)high << 32 | low;
    }
}

// Satisfy threads waiting on a signalled object. Milestone 2b.
void KiWaitTest(DISPATCHER_HEADER *Object, LONG Increment) {}

static void KiQueueTimerDpc(PKDPC dpc) {
    ULONGLONG now = KiReadSystemTime(KeSystemTime);
    KeInsertQueueDpc(dpc, (PVOID)(ULONG)now, (PVOID)(ULONG)(now >> 32));
}

// Insert relative to now (DueTime < 0) or at an absolute system time. Returns FALSE when already due.
static bool KiInsertTimer(PKTIMER Timer, LONGLONG DueTime) {
    ULONGLONG now = KiReadSystemTime(KeInterruptTime);
    ULONGLONG due;
    if (DueTime < 0) {
        Timer->Header.Absolute = FALSE;
        due = now + (ULONGLONG)(-DueTime);
    } else {
        Timer->Header.Absolute = TRUE;
        LONGLONG delta = DueTime - (LONGLONG)KiReadSystemTime(KeSystemTime);
        if (delta <= 0)
            return false;
        due = now + (ULONGLONG)delta;
    }
    if (due <= now)
        return false;

    Timer->DueTime = due;
    ULONG flags = save_flags_cli(); // the clock interrupt reads the list head
    PLIST_ENTRY pos = KiTimerListHead.Flink;
    while (pos != &KiTimerListHead && CONTAINING_RECORD(pos, KTIMER, TimerListEntry)->DueTime <= due)
        pos = pos->Flink;
    InsertTailList(pos, &Timer->TimerListEntry); // inserts before `pos`
    Timer->Header.Inserted = TRUE;
    restore_flags(flags);
    return true;
}

static void KiRemoveTimer(PKTIMER Timer) {
    ULONG flags = save_flags_cli();
    RemoveEntryList(&Timer->TimerListEntry);
    Timer->Header.Inserted = FALSE;
    restore_flags(flags);
}

XBAPI void NTAPI KeInitializeTimerEx(PKTIMER Timer, TIMER_TYPE Type) {
    Timer->Header.Type = (UCHAR)(TimerNotificationObject + (UCHAR)Type);
    Timer->Header.Inserted = FALSE;
    Timer->Header.Size = sizeof(KTIMER) / sizeof(ULONG);
    Timer->Header.SignalState = 0;
    InitializeListHead(&Timer->Header.WaitListHead);
    Timer->DueTime = 0;
    Timer->Period = 0;
}

XBAPI BOOLEAN NTAPI KeSetTimerEx(PKTIMER Timer, LARGE_INTEGER DueTime, LONG Period, PKDPC Dpc) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    BOOLEAN wasInserted = Timer->Header.Inserted;
    if (wasInserted)
        KiRemoveTimer(Timer);
    Timer->Header.SignalState = 0;
    Timer->Dpc = Dpc;
    Timer->Period = Period;

    if (!KiInsertTimer(Timer, DueTime.QuadPart)) {
        // Already due: signal now, and re-arm periodic timers relative to now.
        Timer->Header.SignalState = 1;
        if (!IsListEmpty(&Timer->Header.WaitListHead))
            KiWaitTest(&Timer->Header, 0);
        if (Dpc)
            KiQueueTimerDpc(Dpc);
        if (Period)
            while (!KiInsertTimer(Timer, -(LONGLONG)Period * 10000)) {
            }
    }
    KfLowerIrql(old);
    return wasInserted;
}

XBAPI BOOLEAN NTAPI KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc) {
    return KeSetTimerEx(Timer, DueTime, 0, Dpc);
}

XBAPI BOOLEAN NTAPI KeCancelTimer(PKTIMER Timer) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    BOOLEAN wasInserted = Timer->Header.Inserted;
    if (wasInserted)
        KiRemoveTimer(Timer);
    KfLowerIrql(old);
    return wasInserted;
}

// Expiry DPC: signal every due timer, queue its DPC, re-arm periodic ones.
static void NTAPI KiTimerExpiration(PKDPC, PVOID, PVOID, PVOID) {
    for (;;) {
        cli();
        if (IsListEmpty(&KiTimerListHead)) {
            sti();
            return;
        }
        PKTIMER timer = CONTAINING_RECORD(KiTimerListHead.Flink, KTIMER, TimerListEntry);
        if (timer->DueTime > KiReadSystemTime(KeInterruptTime)) {
            sti();
            return;
        }
        RemoveEntryList(&timer->TimerListEntry);
        timer->Header.Inserted = FALSE;
        sti();

        timer->Header.SignalState = 1;
        if (!IsListEmpty(&timer->Header.WaitListHead))
            KiWaitTest(&timer->Header, 0);
        if (timer->Dpc)
            KiQueueTimerDpc(timer->Dpc);
        if (timer->Period)
            while (!KiInsertTimer(timer, -(LONGLONG)timer->Period * 10000)) {
            }
    }
}

// Clock interrupt (CLOCK_LEVEL, interrupts enabled but every device IRQ masked).
void KiCheckTimers() {
    if (IsListEmpty(&KiTimerListHead) || KiTimerExpireDpc.Inserted)
        return;
    if (CONTAINING_RECORD(KiTimerListHead.Flink, KTIMER, TimerListEntry)->DueTime <= KiReadSystemTime(KeInterruptTime))
        KeInsertQueueDpc(&KiTimerExpireDpc, nullptr, nullptr);
}

void KiInitializeTimers() {
    InitializeListHead(&KiTimerListHead);
    KeInitializeDpc(&KiTimerExpireDpc, KiTimerExpiration, nullptr);
}
