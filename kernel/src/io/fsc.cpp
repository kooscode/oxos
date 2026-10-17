// File system cache (spec/16 §Fsc): one pool of 4 KB buffers shared by FATX, XDVDFS and UDF, in the reference's
// structure. Each buffer is a page of system memory counted as file-cache pages (MmQueryStatistics), mapped
// read-only except while a caller writes it, described by a 20-byte element; the elements form one most-recently-
// used list. 16 pages at boot; FscSetCacheSize changes it (the dashboard asks for 256).
#include <xk/io.h>
#include <xk/ke.h>
#include <xk/mm.h>
#include <xk/rtl.h>
#include <xk/fsc.h>

PVOID MiAllocateMappedPages(ULONG pages, ULONG pteBits, bool guardPage, ULONG usage);
constexpr NTSTATUS STATUS_NO_MEMORY = (NTSTATUS)0xC0000017;

// CacheBuffer: page VA, the low byte = number of mappings, plus these flags.
constexpr ULONG FSC_READ_IN_PROGRESS = 0x100, FSC_READ_WAITERS = 0x200, FSC_DISCARDING = 0x400;
struct FSC_ELEMENT {
    ULONG BlockNumber;                  // byte offset >> 12 on the extension's device
    FSCACHE_EXTENSION *CacheExtension;  // nullptr = empty
    ULONG CacheBuffer;
    LIST_ENTRY ListEntry;               // head = least recently used, empty elements first
};
static_assert(sizeof(FSC_ELEMENT) == 0x14);

static FSC_ELEMENT *FscElementArray;
static ULONG FscNumberOfCachePages;
static LIST_ENTRY FscLruList;
static KEVENT FscLock;          // the list and elements (callers run at passive level)
static KEVENT FscBufferFree;    // notification: set when a buffer may have become idle
static KEVENT FscReadComplete;  // notification: set when a read finishes
static KEVENT FscSizeLock;      // serialises FscSetCacheSize

static void Lock() { KeWaitForSingleObject(&FscLock, Executive, KernelMode, FALSE, nullptr); }
static void Unlock() { KeSetEvent(&FscLock, 0, FALSE); }

static inline ULONG FscUsage(FSC_ELEMENT *e) { return e->CacheBuffer & 0xFF; }
static inline ULONG FscVa(FSC_ELEMENT *e) { return e->CacheBuffer & ~0xFFFu; }

static void FscSetWritable(ULONG va, bool writable) {
    ULONG *pte = MiGetPteAddress(va);
    *pte = (*pte & ~PTE_WRITE) | (writable ? PTE_WRITE : 0);
    MiInvalidatePage(va);
}

// The element of a cache page, from its page database entry (bits 17-27 = element index), as the reference.
static FSC_ELEMENT *FscElementOf(ULONG va) {
    ULONG pfn = *MiGetPteAddress(va) >> PAGE_SHIFT;
    return &FscElementArray[(((ULONG *)0x83FF0000)[pfn] >> 17) & 0x7FF];
}
static void FscSetIndex(ULONG va, ULONG index) {
    ULONG *e = &((ULONG *)0x83FF0000)[*MiGetPteAddress(va) >> PAGE_SHIFT];
    *e = (*e & ~0x0FFE0000u) | (index << 17);
}

// Page relocation (mm/init.cpp): the PTE mapping a cache page.
ULONG *FscCachePte(ULONG elementIndex) {
    return elementIndex < FscNumberOfCachePages ? MiGetPteAddress(FscVa(&FscElementArray[elementIndex])) : nullptr;
}

// Rebuild the list: empty elements at the head, the rest after them.
static void FscRebuildList() {
    InitializeListHead(&FscLruList);
    for (ULONG i = 0; i < FscNumberOfCachePages; i++) {
        FSC_ELEMENT *e = &FscElementArray[i];
        if (e->CacheExtension)
            InsertTailList(&FscLruList, &e->ListEntry);
        else
            InsertHeadList(&FscLruList, &e->ListEntry);
    }
}

static void FscMoveToHead(FSC_ELEMENT *e) {
    RemoveEntryList(&e->ListEntry);
    InsertHeadList(&FscLruList, &e->ListEntry);
}
static void FscMoveToTail(FSC_ELEMENT *e) {
    RemoveEntryList(&e->ListEntry);
    InsertTailList(&FscLruList, &e->ListEntry);
}

// Most recently used first; the walk stops at the first empty element.
static FSC_ELEMENT *FscLookup(FSCACHE_EXTENSION *ext, ULONG block) {
    for (PLIST_ENTRY l = FscLruList.Blink; l != &FscLruList; l = l->Blink) {
        auto e = CONTAINING_RECORD(l, FSC_ELEMENT, ListEntry);
        if (!e->CacheExtension)
            return nullptr;
        if (e->CacheExtension == ext && e->BlockNumber == block) {
            FscMoveToTail(e);
            return e;
        }
    }
    return nullptr;
}

// The least recently used unmapped element; waits (lock dropped) while every buffer is mapped.
static FSC_ELEMENT *FscTakeIdle() {
    for (;;) {
        if (!FscNumberOfCachePages)
            return nullptr;
        for (PLIST_ENTRY l = FscLruList.Flink; l != &FscLruList; l = l->Flink) {
            auto e = CONTAINING_RECORD(l, FSC_ELEMENT, ListEntry);
            if (!FscUsage(e) && !(e->CacheBuffer & (FSC_READ_IN_PROGRESS | FSC_DISCARDING)))
                return e;
        }
        KeResetEvent(&FscBufferFree);
        Unlock();
        KeWaitForSingleObject(&FscBufferFree, Executive, KernelMode, FALSE, nullptr);
        Lock();
    }
}

static NTSTATUS FscDeviceIo(FSCACHE_EXTENSION *ext, UCHAR major, LONGLONG offset, PVOID buffer, ULONG length) {
    LARGE_INTEGER o;
    o.QuadPart = offset;
    return IoSynchronousFsdRequest(major, ext->TargetDeviceObject, buffer, length, &o);
}

NTSTATUS FscMapBuffer(FSCACHE_EXTENSION *ext, LONGLONG offset, BOOLEAN writable, PVOID *buffer) {
    ULONG block = (ULONG)(offset >> PAGE_SHIFT);
    Lock();
    for (;;) {
        FSC_ELEMENT *e = FscLookup(ext, block);
        if (e && (e->CacheBuffer & FSC_READ_IN_PROGRESS)) { // another thread is reading it
            e->CacheBuffer |= FSC_READ_WAITERS;
            KeResetEvent(&FscReadComplete);
            Unlock();
            KeWaitForSingleObject(&FscReadComplete, Executive, KernelMode, FALSE, nullptr);
            Lock();
            continue;
        }
        if (e) {
            e->CacheBuffer++;
        } else {
            e = FscTakeIdle();
            if (!e) {
                Unlock();
                return STATUS_NO_MEMORY;
            }
            e->BlockNumber = block;
            e->CacheExtension = ext;
            e->CacheBuffer = FscVa(e) | FSC_READ_IN_PROGRESS | 1;
            FscMoveToTail(e);
            Unlock();
            // The last block of the device is read only up to its end.
            LONGLONG start = (LONGLONG)block << PAGE_SHIFT, length = ext->PartitionLength.QuadPart - start;
            ULONG n = length >= PAGE_SIZE ? PAGE_SIZE : (ULONG)length;
            FscSetWritable(FscVa(e), true);
            NTSTATUS status = FscDeviceIo(ext, IRP_MJ_READ, start, (PVOID)FscVa(e), n);
            FscSetWritable(FscVa(e), false);
            Lock();
            bool waiters = e->CacheBuffer & FSC_READ_WAITERS;
            e->CacheBuffer &= ~(FSC_READ_IN_PROGRESS | FSC_READ_WAITERS);
            if (waiters)
                KeSetEvent(&FscReadComplete, 0, FALSE);
            if (!NT_SUCCESS(status)) {
                e->CacheBuffer = FscVa(e);
                e->CacheExtension = nullptr;
                FscMoveToHead(e);
                KeSetEvent(&FscBufferFree, 0, FALSE);
                Unlock();
                return status;
            }
        }
        if (writable)
            FscSetWritable(FscVa(e), true);
        *buffer = (PVOID)(FscVa(e) + (ULONG)(offset & (PAGE_SIZE - 1)));
        Unlock();
        return STATUS_SUCCESS;
    }
}

// A buffer for a block that will be written whole: no read, filled with 0xFF, writable.
NTSTATUS FscMapEmptyBuffer(FSCACHE_EXTENSION *ext, LONGLONG offset, PVOID *buffer) {
    ULONG block = (ULONG)(offset >> PAGE_SHIFT);
    Lock();
    FSC_ELEMENT *e = FscLookup(ext, block);
    if (e && (e->CacheBuffer & FSC_READ_IN_PROGRESS)) {
        Unlock();
        return FscMapBuffer(ext, offset & ~(LONGLONG)(PAGE_SIZE - 1), TRUE, buffer); // rare: let the read finish
    }
    if (e) {
        e->CacheBuffer++;
    } else {
        e = FscTakeIdle();
        if (!e) {
            Unlock();
            return STATUS_NO_MEMORY;
        }
        e->BlockNumber = block;
        e->CacheExtension = ext;
        e->CacheBuffer = FscVa(e) | 1;
        FscMoveToTail(e);
    }
    FscSetWritable(FscVa(e), true);
    RtlFillMemory((PVOID)FscVa(e), PAGE_SIZE, 0xFF);
    *buffer = (PVOID)FscVa(e);
    Unlock();
    return STATUS_SUCCESS;
}

void FscUnmapBuffer(PVOID buffer) {
    ULONG va = (ULONG)buffer & ~(PAGE_SIZE - 1);
    Lock();
    FSC_ELEMENT *e = FscElementOf(va);
    if (FscUsage(e))
        e->CacheBuffer--;
    if (!FscUsage(e)) { // read-only again once nobody has it mapped (the reference: at every unmap)
        FscSetWritable(va, false);
        KeSetEvent(&FscBufferFree, 0, FALSE);
    }
    Unlock();
}

// Drop the block (after a failed write, or contents that are no longer valid).
void FscDiscardBuffer(PVOID buffer) {
    ULONG va = (ULONG)buffer & ~(PAGE_SIZE - 1);
    Lock();
    FscSetWritable(va, false);
    FSC_ELEMENT *e = FscElementOf(va);
    e->CacheBuffer = va | (e->CacheBuffer & FSC_DISCARDING);
    e->CacheExtension = nullptr;
    FscMoveToHead(e);
    KeSetEvent(&FscBufferFree, 0, FALSE);
    Unlock();
}

// Write the sectors covering [offset, offset + length) of a mapped buffer to the device, then unmap it (discard it
// if the write fails).
NTSTATUS FscWriteBuffer(FSCACHE_EXTENSION *ext, LONGLONG offset, ULONG length, PVOID buffer) {
    ULONG mask = ext->SectorSize - 1;
    LONGLONG first = offset & ~(LONGLONG)mask, end = (offset + length + mask) & ~(LONGLONG)mask;
    ULONG va = ((ULONG)buffer & ~(PAGE_SIZE - 1)) | (ULONG)(first & (PAGE_SIZE - 1));
    NTSTATUS status = FscDeviceIo(ext, IRP_MJ_WRITE, first, (PVOID)va, (ULONG)(end - first));
    if (NT_SUCCESS(status))
        FscUnmapBuffer(buffer);
    else
        FscDiscardBuffer(buffer);
    return status;
}

// Unmapped blocks of the extension in [firstBlock, lastBlock] become empty; mapped ones stay.
static void FscInvalidateBlocks(FSCACHE_EXTENSION *ext, ULONG firstBlock, ULONG lastBlock) {
    Lock();
    for (PLIST_ENTRY l = FscLruList.Blink, prev; l != &FscLruList; l = prev) {
        prev = l->Blink;
        auto e = CONTAINING_RECORD(l, FSC_ELEMENT, ListEntry);
        if (!e->CacheExtension)
            break;
        // A mapped block is taken out of lookups too (its holders keep the memory; it is free once unmapped).
        // Deviation: the reference skips mapped blocks, but its file systems unmap at once; our FATX holds blocks
        // while its lock is held.
        if (e->CacheExtension == ext && e->BlockNumber >= firstBlock && e->BlockNumber <= lastBlock &&
            !(e->CacheBuffer & FSC_READ_IN_PROGRESS)) {
            e->CacheExtension = nullptr;
            FscMoveToHead(e);
        }
    }
    KeSetEvent(&FscBufferFree, 0, FALSE);
    Unlock();
}

void FscInvalidateDevice(FSCACHE_EXTENSION *ext) { FscInvalidateBlocks(ext, 0, 0xFFFFFFFF); }

void FscInvalidateByteRange(FSCACHE_EXTENSION *ext, LONGLONG offset, ULONG length) {
    if (length)
        FscInvalidateBlocks(ext, (ULONG)(offset >> PAGE_SHIFT), (ULONG)((offset + length - 1) >> PAGE_SHIFT));
}

// Cached read into a caller's buffer (XDVDFS, UDF). As the reference: a read whose offset and buffer meet the
// device's alignment goes straight to the device for its whole pages, with the unaligned head and the short tail
// through the cache; anything else (and every read of a random-access file) is copied out of cache buffers.
// `toEnd` = the read runs to the end of the file: then the direct part may be rounded up to whole sectors.
static bool FscUseCache(PDEVICE_OBJECT device, UCHAR fileFlags, LONGLONG offset, ULONG length, PVOID buffer,
                        bool toEnd) {
    ULONG align = device->AlignmentRequirement, off = (ULONG)offset;
    if ((fileFlags & FO_RANDOM_ACCESS) || (off & align) || ((ULONG)buffer & align & 0xFFF))
        return true;
    if (!toEnd) {
        if (length < PAGE_SIZE)
            return true;
        return !(((off + length) ^ (off + 0xFFF)) & ~0xFFFu); // no whole page after rounding the start up
    }
    if (!(off & 0xFFF))
        return false;
    if (!((off + length) & 0xFFF))
        return true;
    return !(((off + length) ^ off) & ~0xFFFu); // within one page
}

NTSTATUS FscCachedRead(FSCACHE_EXTENSION *ext, UCHAR fileFlags, LONGLONG offset, ULONG length, PVOID buffer,
                       bool toEnd) {
    auto out = (PUCHAR)buffer;
    if (FscUseCache(ext->TargetDeviceObject, fileFlags, offset, length, buffer, toEnd)) {
        while (length) {
            PVOID data;
            NTSTATUS status = FscMapBuffer(ext, offset, FALSE, &data);
            if (!NT_SUCCESS(status))
                return status;
            ULONG n = PAGE_SIZE - (ULONG)(offset & (PAGE_SIZE - 1));
            if (n > length)
                n = length;
            RtlCopyMemory(out, data, n);
            FscUnmapBuffer(data);
            offset += n, out += n, length -= n;
        }
        return STATUS_SUCCESS;
    }
    if (offset & (PAGE_SIZE - 1)) { // head up to the next page through the cache
        PVOID data;
        NTSTATUS status = FscMapBuffer(ext, offset, FALSE, &data);
        if (!NT_SUCCESS(status))
            return status;
        ULONG n = PAGE_SIZE - (ULONG)(offset & (PAGE_SIZE - 1));
        RtlCopyMemory(out, data, n);
        FscUnmapBuffer(data);
        offset += n, out += n, length -= n;
    }
    ULONG mask = ext->SectorSize - 1, direct = toEnd ? (length + mask) & ~mask : length & ~(PAGE_SIZE - 1);
    NTSTATUS status = FscDeviceIo(ext, IRP_MJ_READ, offset, out, direct);
    if (!NT_SUCCESS(status) || direct >= length)
        return status;
    offset += direct, out += direct, length -= direct; // the short tail through the cache
    PVOID data;
    status = FscMapBuffer(ext, offset, FALSE, &data);
    if (!NT_SUCCESS(status))
        return status;
    RtlCopyMemory(out, data, length);
    FscUnmapBuffer(data);
    return STATUS_SUCCESS;
}

// --- Size

static NTSTATUS FscGrow(ULONG pages) {
    auto elements = (FSC_ELEMENT *)ExAllocatePoolWithTag(pages * sizeof(FSC_ELEMENT), 'AcsF');
    if (!elements)
        return STATUS_INSUFFICIENT_RESOURCES;
    ULONG added = pages - FscNumberOfCachePages;
    auto va = (ULONG)MiAllocateMappedPages(added, PTE_PRESENT | PTE_ACCESSED | PTE_DIRTY, false, MmFsCacheUsage);
    if (!va) {
        ExFreePool(elements);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (FscNumberOfCachePages)
        RtlCopyMemory(elements, FscElementArray, FscNumberOfCachePages * sizeof(FSC_ELEMENT));
    for (ULONG i = FscNumberOfCachePages; i < pages; i++, va += PAGE_SIZE) {
        elements[i] = {0, nullptr, va, {}};
        FscSetIndex(va, i);
    }
    if (FscElementArray)
        ExFreePool(FscElementArray);
    FscElementArray = elements;
    FscNumberOfCachePages = pages;
    FscRebuildList();
    return STATUS_SUCCESS;
}

// Shrinking discards unmapped buffers (waiting for mapped ones to be released) and frees their pages.
static NTSTATUS FscShrink(ULONG pages) {
    auto elements = pages ? (FSC_ELEMENT *)ExAllocatePoolWithTag(pages * sizeof(FSC_ELEMENT), 'AcsF') : nullptr;
    if (pages && !elements)
        return STATUS_INSUFFICIENT_RESOURCES;
    ULONG excess = FscNumberOfCachePages - pages;
    while (excess) {
        for (ULONG i = FscNumberOfCachePages; i-- && excess;) {
            FSC_ELEMENT *e = &FscElementArray[i];
            if (!FscUsage(e) && !(e->CacheBuffer & (FSC_READ_IN_PROGRESS | FSC_DISCARDING))) {
                e->CacheBuffer |= FSC_DISCARDING;
                e->CacheExtension = nullptr;
                excess--;
            }
        }
        if (excess) {
            KeResetEvent(&FscBufferFree);
            Unlock();
            KeWaitForSingleObject(&FscBufferFree, Executive, KernelMode, FALSE, nullptr);
            Lock();
        }
    }
    ULONG kept = 0;
    for (ULONG i = 0; i < FscNumberOfCachePages; i++) {
        FSC_ELEMENT *e = &FscElementArray[i];
        if (e->CacheBuffer & FSC_DISCARDING) {
            MmFreeSystemMemory((PVOID)FscVa(e), PAGE_SIZE);
            continue;
        }
        elements[kept] = *e;
        FscSetIndex(FscVa(e), kept);
        kept++;
    }
    ExFreePool(FscElementArray);
    FscElementArray = elements;
    FscNumberOfCachePages = pages;
    FscRebuildList();
    KeSetEvent(&FscBufferFree, 0, FALSE);
    return STATUS_SUCCESS;
}

XBAPI ULONG NTAPI FscGetCacheSize() { return FscNumberOfCachePages; }

XBAPI NTSTATUS NTAPI FscSetCacheSize(ULONG NumberOfCachePages) {
    if (NumberOfCachePages > 0x800)
        return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&FscSizeLock, Executive, KernelMode, FALSE, nullptr);
    Lock();
    NTSTATUS status = STATUS_SUCCESS;
    if (NumberOfCachePages > FscNumberOfCachePages)
        status = FscGrow(NumberOfCachePages);
    else if (NumberOfCachePages < FscNumberOfCachePages)
        status = FscShrink(NumberOfCachePages);
    Unlock();
    KeSetEvent(&FscSizeLock, 0, FALSE);
    return status;
}

// Every unmapped buffer becomes empty.
XBAPI void NTAPI FscInvalidateIdleBlocks() {
    Lock();
    for (ULONG i = 0; i < FscNumberOfCachePages; i++) {
        FSC_ELEMENT *e = &FscElementArray[i];
        if (e->CacheExtension && !FscUsage(e) && !(e->CacheBuffer & FSC_READ_IN_PROGRESS)) {
            e->CacheExtension = nullptr;
            FscMoveToHead(e);
        }
    }
    KeSetEvent(&FscBufferFree, 0, FALSE);
    Unlock();
}

void FscInitialize() {
    KeInitializeEvent(&FscLock, SynchronizationEvent, TRUE);
    KeInitializeEvent(&FscSizeLock, SynchronizationEvent, TRUE);
    KeInitializeEvent(&FscBufferFree, NotificationEvent, FALSE);
    KeInitializeEvent(&FscReadComplete, NotificationEvent, FALSE);
    FscElementArray = nullptr;
    FscNumberOfCachePages = 0;
    InitializeListHead(&FscLruList);
    FscSetCacheSize(16);
}
