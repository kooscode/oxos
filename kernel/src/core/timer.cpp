// Kernel timers (spec/stock/06 §5). Inserted timers sit in one list sorted by due interrupt time; the clock
// interrupt queues an expiry DPC when the head is due.
#include <xk/core.h>
#include <xk/arch.h>

namespace xk::core {

static LIST_ENTRY TimerListHead; // inserted timers, earliest due first
static KDPC TimerExpireDpc;      // queued by the clock interrupt when the first timer is due

static ULONGLONG ReadKernelTime(const volatile KSYSTEM_TIME &t) {
    for (;;) {
        LONG high = t.High1Time;
        ULONG low = t.LowPart;
        if (high == t.High2Time)
            return (ULONGLONG)(ULONG)high << 32 | low;
    }
}

static void QueueTimerDpc(PKDPC dpc) {
    ULONGLONG now = ReadKernelTime(KeSystemTime);
    KeInsertQueueDpc(dpc, (PVOID)(ULONG)now, (PVOID)(ULONG)(now >> 32));
}

// Insert relative to now (DueTime < 0) or at an absolute system time. Returns FALSE when already due.
static bool InsertTimer(PKTIMER Timer, LONGLONG DueTime) {
    ULONGLONG now = ReadKernelTime(KeInterruptTime);
    ULONGLONG due;
    if (DueTime < 0) {
        Timer->Header.Absolute = FALSE;
        due = now + (ULONGLONG)(-DueTime);
    } else {
        Timer->Header.Absolute = TRUE;
        LONGLONG delta = DueTime - (LONGLONG)ReadKernelTime(KeSystemTime);
        if (delta <= 0)
            return false;
        due = now + (ULONGLONG)delta;
    }
    if (due <= now)
        return false;

    Timer->DueTime = due;
    ULONG flags = save_flags_cli(); // the clock interrupt reads the list head
    PLIST_ENTRY pos = TimerListHead.Flink;
    while (pos != &TimerListHead && CONTAINING_RECORD(pos, KTIMER, TimerListEntry)->DueTime <= due)
        pos = pos->Flink;
    InsertTailList(pos, &Timer->TimerListEntry); // inserts before `pos`
    Timer->Header.Inserted = TRUE;
    restore_flags(flags);
    return true;
}

static void RemoveTimer(PKTIMER Timer) {
    ULONG flags = save_flags_cli();
    RemoveEntryList(&Timer->TimerListEntry);
    Timer->Header.Inserted = FALSE;
    restore_flags(flags);
}

// Wait timeouts use the thread's own timer (spec/stock/07).
bool InsertTimerRelative(PKTIMER Timer, LONGLONG DueTime) {
    return InsertTimer(Timer, DueTime);
}

void RemoveTimerIfInserted(PKTIMER Timer) {
    if (Timer->Header.Inserted)
        RemoveTimer(Timer);
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
        RemoveTimer(Timer);
    Timer->Header.SignalState = 0;
    Timer->Dpc = Dpc;
    Timer->Period = Period;

    if (!InsertTimer(Timer, DueTime.QuadPart)) {
        // Already due: signal now, and re-arm periodic timers relative to now.
        Timer->Header.SignalState = 1;
        if (!IsListEmpty(&Timer->Header.WaitListHead))
            WaitTest(&Timer->Header, 0);
        if (Dpc)
            QueueTimerDpc(Dpc);
        if (Period) {
            while (!InsertTimer(Timer, -(LONGLONG)Period * 10000)) {
                // Re-arm for the next period (a relative time, so this succeeds the first time).
            }
        }
    }
    KiUnlockDispatcherDatabase(old);
    return wasInserted;
}

XBAPI BOOLEAN NTAPI KeSetTimer(PKTIMER Timer, LARGE_INTEGER DueTime, PKDPC Dpc) {
    return KeSetTimerEx(Timer, DueTime, 0, Dpc);
}

XBAPI BOOLEAN NTAPI KeCancelTimer(PKTIMER Timer) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    BOOLEAN wasInserted = Timer->Header.Inserted;
    if (wasInserted)
        RemoveTimer(Timer);
    KfLowerIrql(old);
    return wasInserted;
}

// Expiry DPC: signal every due timer, queue its DPC, re-arm periodic ones.
static void NTAPI TimerExpiration(PKDPC, PVOID, PVOID, PVOID) {
    for (;;) {
        cli();
        if (IsListEmpty(&TimerListHead)) {
            sti();
            return;
        }
        PKTIMER timer = CONTAINING_RECORD(TimerListHead.Flink, KTIMER, TimerListEntry);
        if (timer->DueTime > ReadKernelTime(KeInterruptTime)) {
            sti();
            return;
        }
        RemoveEntryList(&timer->TimerListEntry);
        timer->Header.Inserted = FALSE;
        sti();

        timer->Header.SignalState = 1;
        if (!IsListEmpty(&timer->Header.WaitListHead))
            WaitTest(&timer->Header, 0);
        if (timer->Dpc)
            QueueTimerDpc(timer->Dpc);
        if (timer->Period) {
            while (!InsertTimer(timer, -(LONGLONG)timer->Period * 10000)) {
                // Re-arm for the next period (a relative time, so this succeeds the first time).
            }
        }
    }
}

// Clock interrupt (CLOCK_LEVEL, interrupts enabled but every device IRQ masked).
void CheckTimers() {
    if (IsListEmpty(&TimerListHead) || TimerExpireDpc.Inserted)
        return;
    if (CONTAINING_RECORD(TimerListHead.Flink, KTIMER, TimerListEntry)->DueTime <= ReadKernelTime(KeInterruptTime))
        KeInsertQueueDpc(&TimerExpireDpc, nullptr, nullptr);
}

void InitializeTimers() {
    InitializeListHead(&TimerListHead);
    KeInitializeDpc(&TimerExpireDpc, TimerExpiration, nullptr);
}

} // namespace xk::core
