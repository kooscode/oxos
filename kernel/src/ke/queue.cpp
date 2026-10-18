// Queue objects (I/O completion back-end; NT semantics, spec/stock/08) and device queues.
#include <xk/arch.h>
#include <xk/ke.h>

#define Prcb (KiPcr.PrcbData)

XBAPI void NTAPI KeInitializeQueue(PKQUEUE Queue, ULONG Count) {
    Queue->Header.Type = QueueObject;
    Queue->Header.Size = sizeof(KQUEUE) / sizeof(ULONG);
    Queue->Header.SignalState = 0;
    InitializeListHead(&Queue->Header.WaitListHead);
    InitializeListHead(&Queue->EntryListHead);
    InitializeListHead(&Queue->ThreadListHead);
    Queue->CurrentCount = 0;
    Queue->MaximumCount = Count ? Count : 1;
}

// Hand Entry to the most recent waiter if the concurrency limit allows, else queue it.
LONG KiInsertQueue(PKQUEUE Queue, PLIST_ENTRY Entry, bool Head) {
    LONG previous = Queue->Header.SignalState;
    PKTHREAD current = Prcb.CurrentThread;
    PLIST_ENTRY waitEntry = Queue->Header.WaitListHead.Blink; // waiters are served LIFO
    if (waitEntry != &Queue->Header.WaitListHead && Queue->CurrentCount < Queue->MaximumCount &&
        (current->Queue != Queue || current->WaitReason != WrQueue)) {
        PKTHREAD t = CONTAINING_RECORD(waitEntry, KWAIT_BLOCK, WaitListEntry)->Thread;
        KiUnwaitThread(t, (LONG)Entry, 0);
    } else {
        Queue->Header.SignalState++;
        if (Head)
            InsertHeadList(&Queue->EntryListHead, Entry);
        else
            InsertTailList(&Queue->EntryListHead, Entry);
    }
    return previous;
}

XBAPI LONG NTAPI KeInsertQueue(PKQUEUE Queue, PLIST_ENTRY Entry) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    LONG previous = KiInsertQueue(Queue, Entry, false);
    KiUnlockDispatcherDatabase(old);
    return previous;
}

XBAPI LONG NTAPI KeInsertHeadQueue(PKQUEUE Queue, PLIST_ENTRY Entry) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    LONG previous = KiInsertQueue(Queue, Entry, true);
    KiUnlockDispatcherDatabase(old);
    return previous;
}

// A thread associated with Queue is about to block on something else: release its concurrency
// slot and give a queued entry to another waiter.
void KiActivateWaiterQueue(PKQUEUE Queue) {
    Queue->CurrentCount--;
    if (Queue->CurrentCount < Queue->MaximumCount && !IsListEmpty(&Queue->EntryListHead) &&
        !IsListEmpty(&Queue->Header.WaitListHead)) {
        PLIST_ENTRY entry = RemoveHeadList(&Queue->EntryListHead);
        Queue->Header.SignalState--;
        PKTHREAD t = CONTAINING_RECORD(Queue->Header.WaitListHead.Blink, KWAIT_BLOCK, WaitListEntry)->Thread;
        KiUnwaitThread(t, (LONG)entry, 0);
    }
}

XBAPI PLIST_ENTRY NTAPI KeRundownQueue(PKQUEUE Queue) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    PLIST_ENTRY first = nullptr;
    if (!IsListEmpty(&Queue->EntryListHead)) {
        first = Queue->EntryListHead.Flink;
        RemoveEntryList(&Queue->EntryListHead); // hand the entries back as a headless ring
        InitializeListHead(&Queue->EntryListHead);
        Queue->Header.SignalState = 0;
    }
    while (!IsListEmpty(&Queue->ThreadListHead)) {
        PKTHREAD t = CONTAINING_RECORD(RemoveHeadList(&Queue->ThreadListHead), KTHREAD, QueueListEntry);
        t->Queue = nullptr;
    }
    KiUnlockDispatcherDatabase(old);
    return first;
}

// --- Device queues (used by the I/O manager's StartIo path)

XBAPI void NTAPI KeInitializeDeviceQueue(PKDEVICE_QUEUE DeviceQueue) {
    DeviceQueue->Type = DeviceQueueObject;
    DeviceQueue->Size = sizeof(KDEVICE_QUEUE);
    DeviceQueue->Busy = FALSE;
    InitializeListHead(&DeviceQueue->DeviceListHead);
}

// FALSE = the queue was idle (now busy) and the caller should start the request itself.
XBAPI BOOLEAN NTAPI KeInsertDeviceQueue(PKDEVICE_QUEUE DeviceQueue, PKDEVICE_QUEUE_ENTRY Entry) {
    ULONG flags = save_flags_cli();
    BOOLEAN inserted = DeviceQueue->Busy;
    if (inserted)
        InsertTailList(&DeviceQueue->DeviceListHead, &Entry->DeviceListEntry);
    else
        DeviceQueue->Busy = TRUE;
    Entry->Inserted = inserted;
    restore_flags(flags);
    return inserted;
}

XBAPI BOOLEAN NTAPI KeInsertByKeyDeviceQueue(PKDEVICE_QUEUE DeviceQueue, PKDEVICE_QUEUE_ENTRY Entry, ULONG SortKey) {
    ULONG flags = save_flags_cli();
    Entry->SortKey = SortKey;
    BOOLEAN inserted = DeviceQueue->Busy;
    if (inserted) {
        PLIST_ENTRY e = DeviceQueue->DeviceListHead.Flink;
        while (e != &DeviceQueue->DeviceListHead &&
               CONTAINING_RECORD(e, KDEVICE_QUEUE_ENTRY, DeviceListEntry)->SortKey <= SortKey)
            e = e->Flink;
        InsertTailList(e, &Entry->DeviceListEntry);
    } else {
        DeviceQueue->Busy = TRUE;
    }
    Entry->Inserted = inserted;
    restore_flags(flags);
    return inserted;
}

XBAPI PKDEVICE_QUEUE_ENTRY NTAPI KeRemoveDeviceQueue(PKDEVICE_QUEUE DeviceQueue) {
    ULONG flags = save_flags_cli();
    PKDEVICE_QUEUE_ENTRY entry = nullptr;
    if (IsListEmpty(&DeviceQueue->DeviceListHead)) {
        DeviceQueue->Busy = FALSE;
    } else {
        entry = CONTAINING_RECORD(RemoveHeadList(&DeviceQueue->DeviceListHead), KDEVICE_QUEUE_ENTRY, DeviceListEntry);
        entry->Inserted = FALSE;
    }
    restore_flags(flags);
    return entry;
}

// First entry with SortKey >= key (wrapping to the head), as NT does for elevator scheduling.
XBAPI PKDEVICE_QUEUE_ENTRY NTAPI KeRemoveByKeyDeviceQueue(PKDEVICE_QUEUE DeviceQueue, ULONG SortKey) {
    ULONG flags = save_flags_cli();
    PKDEVICE_QUEUE_ENTRY entry = nullptr;
    if (IsListEmpty(&DeviceQueue->DeviceListHead)) {
        DeviceQueue->Busy = FALSE;
    } else {
        PLIST_ENTRY e = DeviceQueue->DeviceListHead.Flink;
        while (e != &DeviceQueue->DeviceListHead &&
               CONTAINING_RECORD(e, KDEVICE_QUEUE_ENTRY, DeviceListEntry)->SortKey < SortKey)
            e = e->Flink;
        if (e == &DeviceQueue->DeviceListHead)
            e = DeviceQueue->DeviceListHead.Flink;
        RemoveEntryList(e);
        entry = CONTAINING_RECORD(e, KDEVICE_QUEUE_ENTRY, DeviceListEntry);
        entry->Inserted = FALSE;
    }
    restore_flags(flags);
    return entry;
}

XBAPI BOOLEAN NTAPI KeRemoveEntryDeviceQueue(PKDEVICE_QUEUE DeviceQueue, PKDEVICE_QUEUE_ENTRY Entry) {
    ULONG flags = save_flags_cli();
    BOOLEAN wasInserted = Entry->Inserted;
    if (wasInserted) {
        Entry->Inserted = FALSE;
        RemoveEntryList(&Entry->DeviceListEntry);
    }
    restore_flags(flags);
    return wasInserted;
}

XBAPI BOOLEAN NTAPI KeSynchronizeExecution(PKINTERRUPT Interrupt, PKSYNCHRONIZE_ROUTINE SynchronizeRoutine,
                                           PVOID SynchronizeContext) {
    KIRQL old = KfRaiseIrql((KIRQL)Interrupt->Irql);
    BOOLEAN result = SynchronizeRoutine(SynchronizeContext);
    KfLowerIrql(old);
    return result;
}
