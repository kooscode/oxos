// RTL critical sections and executive read/write locks (spec/stock/13). Layouts are the Xbox ABI.
#include <xk/ke.h>

typedef struct _RTL_CRITICAL_SECTION {
    DISPATCHER_HEADER Synchronization; // a synchronization event (Type 1, Size 4)
    LONG LockCount;                    // -1 = free
    LONG RecursionCount;
    PKTHREAD OwningThread;
} RTL_CRITICAL_SECTION, *PRTL_CRITICAL_SECTION;
XK_SIZE(RTL_CRITICAL_SECTION, 0x1C);

XBAPI void NTAPI RtlInitializeCriticalSection(PRTL_CRITICAL_SECTION CriticalSection) {
    KeInitializeEvent((PKEVENT)&CriticalSection->Synchronization, SynchronizationEvent, FALSE);
    CriticalSection->LockCount = -1;
    CriticalSection->RecursionCount = 0;
    CriticalSection->OwningThread = nullptr;
}

XBAPI void NTAPI RtlEnterCriticalSection(PRTL_CRITICAL_SECTION CriticalSection) {
    PKTHREAD self = KeGetCurrentThread();
    if (__atomic_add_fetch(&CriticalSection->LockCount, 1, __ATOMIC_SEQ_CST) != 0) {
        if (CriticalSection->OwningThread == self) {
            CriticalSection->RecursionCount++;
            return;
        }
        KeWaitForSingleObject(&CriticalSection->Synchronization, Executive, KernelMode, FALSE, nullptr);
    }
    CriticalSection->OwningThread = self;
    CriticalSection->RecursionCount = 1;
}

XBAPI void NTAPI RtlLeaveCriticalSection(PRTL_CRITICAL_SECTION CriticalSection) {
    if (--CriticalSection->RecursionCount != 0) {
        __atomic_sub_fetch(&CriticalSection->LockCount, 1, __ATOMIC_SEQ_CST);
        return;
    }
    CriticalSection->OwningThread = nullptr;
    if (__atomic_sub_fetch(&CriticalSection->LockCount, 1, __ATOMIC_SEQ_CST) >= 0)
        KeSetEvent((PKEVENT)&CriticalSection->Synchronization, 1, FALSE);
}

XBAPI BOOLEAN NTAPI RtlTryEnterCriticalSection(PRTL_CRITICAL_SECTION CriticalSection) {
    PKTHREAD self = KeGetCurrentThread();
    LONG expected = -1;
    if (__atomic_compare_exchange_n(&CriticalSection->LockCount, &expected, 0, false, __ATOMIC_SEQ_CST,
                                    __ATOMIC_SEQ_CST)) {
        CriticalSection->OwningThread = self;
        CriticalSection->RecursionCount = 1;
        return TRUE;
    }
    if (CriticalSection->OwningThread == self) {
        __atomic_add_fetch(&CriticalSection->LockCount, 1, __ATOMIC_SEQ_CST);
        CriticalSection->RecursionCount++;
        return TRUE;
    }
    return FALSE;
}

XBAPI void NTAPI RtlEnterCriticalSectionAndRegion(PRTL_CRITICAL_SECTION CriticalSection) {
    KeEnterCriticalRegion();
    RtlEnterCriticalSection(CriticalSection);
}

XBAPI void NTAPI RtlLeaveCriticalSectionAndRegion(PRTL_CRITICAL_SECTION CriticalSection) {
    RtlLeaveCriticalSection(CriticalSection);
    KeLeaveCriticalRegion();
}

// --- Read/write lock: writers wait on an event, readers on a semaphore.
typedef struct _ERWLOCK {
    LONG LockCount;
    ULONG WritersWaitingCount;
    ULONG ReadersWaitingCount;
    ULONG ReadersEntryCount;
    KEVENT WriterEvent;
    KSEMAPHORE ReaderSemaphore;
} ERWLOCK, *PERWLOCK;
XK_SIZE(ERWLOCK, 0x34);

XBAPI void NTAPI ExInitializeReadWriteLock(PERWLOCK ReadWriteLock) {
    ReadWriteLock->LockCount = -1;
    ReadWriteLock->WritersWaitingCount = ReadWriteLock->ReadersWaitingCount = ReadWriteLock->ReadersEntryCount = 0;
    KeInitializeEvent(&ReadWriteLock->WriterEvent, SynchronizationEvent, FALSE);
    KeInitializeSemaphore(&ReadWriteLock->ReaderSemaphore, 0, 0x7FFFFFFF);
}

// The lock words are only touched with interrupts off (single processor), as in the reference.
XBAPI void NTAPI ExAcquireReadWriteLockExclusive(PERWLOCK ReadWriteLock) {
    ULONG flags = __builtin_ia32_readeflags_u32();
    __asm__ volatile("cli");
    bool wait = ++ReadWriteLock->LockCount != 0;
    if (wait)
        ReadWriteLock->WritersWaitingCount++;
    __builtin_ia32_writeeflags_u32(flags);
    if (wait)
        KeWaitForSingleObject(&ReadWriteLock->WriterEvent, Executive, KernelMode, FALSE, nullptr);
}

XBAPI void NTAPI ExAcquireReadWriteLockShared(PERWLOCK ReadWriteLock) {
    ULONG flags = __builtin_ia32_readeflags_u32();
    __asm__ volatile("cli");
    bool wait = ++ReadWriteLock->LockCount != 0 &&
                (ReadWriteLock->ReadersEntryCount == 0 || ReadWriteLock->WritersWaitingCount != 0);
    if (wait)
        ReadWriteLock->ReadersWaitingCount++;
    else
        ReadWriteLock->ReadersEntryCount++;
    __builtin_ia32_writeeflags_u32(flags);
    if (wait)
        KeWaitForSingleObject(&ReadWriteLock->ReaderSemaphore, Executive, KernelMode, FALSE, nullptr);
}

XBAPI void NTAPI ExReleaseReadWriteLock(PERWLOCK ReadWriteLock) {
    ULONG flags = __builtin_ia32_readeflags_u32();
    __asm__ volatile("cli");
    if (--ReadWriteLock->LockCount < 0) {
        ReadWriteLock->ReadersEntryCount = 0;
        __builtin_ia32_writeeflags_u32(flags);
        return;
    }
    if (ReadWriteLock->ReadersEntryCount == 0) { // a writer leaves: let all waiting readers in at once
        ULONG readers = ReadWriteLock->ReadersWaitingCount;
        if (readers) {
            ReadWriteLock->ReadersEntryCount = readers;
            ReadWriteLock->ReadersWaitingCount = 0;
            __builtin_ia32_writeeflags_u32(flags);
            KeReleaseSemaphore(&ReadWriteLock->ReaderSemaphore, 1, readers, FALSE);
            return;
        }
    } else if (--ReadWriteLock->ReadersEntryCount != 0) {
        __builtin_ia32_writeeflags_u32(flags);
        return;
    }
    ReadWriteLock->WritersWaitingCount--;
    __builtin_ia32_writeeflags_u32(flags);
    KeSetEvent(&ReadWriteLock->WriterEvent, 1, FALSE);
}
