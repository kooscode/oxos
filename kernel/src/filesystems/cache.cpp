// File system cache (spec/stock/16 §Fsc): one pool of 4 KB buffers shared by FATX, XDVDFS and UDF, in the reference's
// structure. Each buffer is a page of system memory counted as file-cache pages (MmQueryStatistics), mapped
// read-only except while a caller writes it, described by a 20-byte element; the elements form one most-recently-
// used list. 16 pages at boot; FscSetCacheSize changes it (the dashboard asks for 256).
#include <filesystems/filesystems.h>
#include <io/io.h>
#include <core/core.h>
#include <memory/memory.h>
#include <runtime/runtime.h>

namespace xk::filesystems {

constexpr NTSTATUS STATUS_NO_MEMORY = (NTSTATUS)0xC0000017;

// CacheBuffer: page VA, the low byte = number of mappings, plus these flags.
constexpr ULONG FSC_READ_IN_PROGRESS = 0x100, FSC_READ_WAITERS = 0x200, FSC_DISCARDING = 0x400;
struct FSC_ELEMENT {
    ULONG BlockNumber;                 // byte offset >> 12 on the extension's device
    FSCACHE_EXTENSION *CacheExtension; // nullptr = empty
    ULONG CacheBuffer;                 // see the flags above
    LIST_ENTRY ListEntry;              // head = least recently used, empty elements first
};
static_assert(sizeof(FSC_ELEMENT) == 0x14);

static FSC_ELEMENT *CacheElementArray; // one element per cache page
static ULONG CachePages;
static LIST_ENTRY CacheLruList;  // every element, least recently used first
static KEVENT CacheLock;         // the list and elements (callers run at passive level)
static KEVENT CacheBufferFree;   // notification: set when a buffer may have become idle
static KEVENT CacheReadComplete; // notification: set when a read finishes
static KEVENT CacheSizeLock;     // serialises FscSetCacheSize

static void Lock() {
    KeWaitForSingleObject(&CacheLock, Executive, KernelMode, FALSE, nullptr);
}

static void Unlock() {
    KeSetEvent(&CacheLock, 0, FALSE);
}

static inline ULONG CacheUsage(FSC_ELEMENT *e) {
    return e->CacheBuffer & 0xFF;
}

static inline ULONG CacheVirtualAddress(FSC_ELEMENT *e) {
    return e->CacheBuffer & ~0xFFFu;
}

static void SetCacheWritable(ULONG va, bool writable) {
    ULONG *pte = memory::PteAddress(va);
    *pte = (*pte & ~PTE_WRITE) | (writable ? PTE_WRITE : 0);
    memory::InvalidatePage(va);
}

// The element of a cache page, from its page database entry (bits 17-27 = element index), as the reference.
static FSC_ELEMENT *CacheElementOf(ULONG va) {
    ULONG pfn = *memory::PteAddress(va) >> PAGE_SHIFT;
    return &CacheElementArray[(((ULONG *)0x83FF0000)[pfn] >> 17) & 0x7FF];
}

static void SetCacheIndex(ULONG va, ULONG index) {
    ULONG *e = &((ULONG *)0x83FF0000)[*memory::PteAddress(va) >> PAGE_SHIFT];
    *e = (*e & ~0x0FFE0000u) | (index << 17);
}

// Page relocation (memory/init.cpp): the PTE mapping a cache page.
ULONG *CachePte(ULONG elementIndex) {
    if (elementIndex >= CachePages)
        return nullptr;
    return memory::PteAddress(CacheVirtualAddress(&CacheElementArray[elementIndex]));
}

// Rebuild the list: empty elements at the head, the rest after them.
static void RebuildCacheList() {
    InitializeListHead(&CacheLruList);
    for (ULONG i = 0; i < CachePages; i++) {
        FSC_ELEMENT *e = &CacheElementArray[i];
        if (e->CacheExtension)
            InsertTailList(&CacheLruList, &e->ListEntry);
        else
            InsertHeadList(&CacheLruList, &e->ListEntry);
    }
}

static void MoveToCacheHead(FSC_ELEMENT *e) {
    RemoveEntryList(&e->ListEntry);
    InsertHeadList(&CacheLruList, &e->ListEntry);
}

static void MoveToCacheTail(FSC_ELEMENT *e) {
    RemoveEntryList(&e->ListEntry);
    InsertTailList(&CacheLruList, &e->ListEntry);
}

// Most recently used first; the walk stops at the first empty element.
static FSC_ELEMENT *LookUpCacheBlock(FSCACHE_EXTENSION *ext, ULONG block) {
    for (PLIST_ENTRY l = CacheLruList.Blink; l != &CacheLruList; l = l->Blink) {
        auto e = CONTAINING_RECORD(l, FSC_ELEMENT, ListEntry);
        if (!e->CacheExtension)
            return nullptr;
        if (e->CacheExtension == ext && e->BlockNumber == block) {
            MoveToCacheTail(e);
            return e;
        }
    }
    return nullptr;
}

// The least recently used unmapped element; waits (lock dropped) while every buffer is mapped.
static FSC_ELEMENT *TakeIdleCacheBlock() {
    for (;;) {
        if (!CachePages)
            return nullptr;
        for (PLIST_ENTRY l = CacheLruList.Flink; l != &CacheLruList; l = l->Flink) {
            auto e = CONTAINING_RECORD(l, FSC_ELEMENT, ListEntry);
            if (!CacheUsage(e) && !(e->CacheBuffer & (FSC_READ_IN_PROGRESS | FSC_DISCARDING)))
                return e;
        }
        KeResetEvent(&CacheBufferFree);
        Unlock();
        KeWaitForSingleObject(&CacheBufferFree, Executive, KernelMode, FALSE, nullptr);
        Lock();
    }
}

static NTSTATUS CacheDeviceIo(FSCACHE_EXTENSION *ext, UCHAR major, LONGLONG offset, PVOID buffer, ULONG length) {
    LARGE_INTEGER o;
    o.QuadPart = offset;
    return IoSynchronousFsdRequest(major, ext->TargetDeviceObject, buffer, length, &o);
}

NTSTATUS MapCacheBuffer(FSCACHE_EXTENSION *ext, LONGLONG offset, BOOLEAN writable, PVOID *buffer) {
    ULONG block = (ULONG)(offset >> PAGE_SHIFT);
    Lock();
    for (;;) {
        FSC_ELEMENT *e = LookUpCacheBlock(ext, block);
        if (e && (e->CacheBuffer & FSC_READ_IN_PROGRESS)) { // another thread is reading it
            e->CacheBuffer |= FSC_READ_WAITERS;
            KeResetEvent(&CacheReadComplete);
            Unlock();
            KeWaitForSingleObject(&CacheReadComplete, Executive, KernelMode, FALSE, nullptr);
            Lock();
            continue;
        }
        if (e) {
            e->CacheBuffer++;
        } else {
            e = TakeIdleCacheBlock();
            if (!e) {
                Unlock();
                return STATUS_NO_MEMORY;
            }
            e->BlockNumber = block;
            e->CacheExtension = ext;
            e->CacheBuffer = CacheVirtualAddress(e) | FSC_READ_IN_PROGRESS | 1;
            MoveToCacheTail(e);
            Unlock();
            // The last block of the device is read only up to its end.
            LONGLONG start = (LONGLONG)block << PAGE_SHIFT, length = ext->PartitionLength.QuadPart - start;
            ULONG n = length >= PAGE_SIZE ? PAGE_SIZE : (ULONG)length;
            SetCacheWritable(CacheVirtualAddress(e), true);
            NTSTATUS status = CacheDeviceIo(ext, IRP_MJ_READ, start, (PVOID)CacheVirtualAddress(e), n);
            SetCacheWritable(CacheVirtualAddress(e), false);
            Lock();
            bool waiters = e->CacheBuffer & FSC_READ_WAITERS;
            e->CacheBuffer &= ~(FSC_READ_IN_PROGRESS | FSC_READ_WAITERS);
            if (waiters)
                KeSetEvent(&CacheReadComplete, 0, FALSE);
            if (!NT_SUCCESS(status)) {
                e->CacheBuffer = CacheVirtualAddress(e);
                e->CacheExtension = nullptr;
                MoveToCacheHead(e);
                KeSetEvent(&CacheBufferFree, 0, FALSE);
                Unlock();
                return status;
            }
        }
        if (writable)
            SetCacheWritable(CacheVirtualAddress(e), true);
        *buffer = (PVOID)(CacheVirtualAddress(e) + (ULONG)(offset & (PAGE_SIZE - 1)));
        Unlock();
        return STATUS_SUCCESS;
    }
}

// A buffer for a block that will be written whole: no read, filled with 0xFF, writable.
NTSTATUS MapEmptyCacheBuffer(FSCACHE_EXTENSION *ext, LONGLONG offset, PVOID *buffer) {
    ULONG block = (ULONG)(offset >> PAGE_SHIFT);
    Lock();
    FSC_ELEMENT *e = LookUpCacheBlock(ext, block);
    if (e && (e->CacheBuffer & FSC_READ_IN_PROGRESS)) {
        Unlock();
        return MapCacheBuffer(ext, offset & ~(LONGLONG)(PAGE_SIZE - 1), TRUE, buffer); // rare: let the read finish
    }
    if (e) {
        e->CacheBuffer++;
    } else {
        e = TakeIdleCacheBlock();
        if (!e) {
            Unlock();
            return STATUS_NO_MEMORY;
        }
        e->BlockNumber = block;
        e->CacheExtension = ext;
        e->CacheBuffer = CacheVirtualAddress(e) | 1;
        MoveToCacheTail(e);
    }
    SetCacheWritable(CacheVirtualAddress(e), true);
    RtlFillMemory((PVOID)CacheVirtualAddress(e), PAGE_SIZE, 0xFF);
    *buffer = (PVOID)CacheVirtualAddress(e);
    Unlock();
    return STATUS_SUCCESS;
}

void UnmapCacheBuffer(PVOID buffer) {
    ULONG va = (ULONG)buffer & ~(PAGE_SIZE - 1);
    Lock();
    FSC_ELEMENT *e = CacheElementOf(va);
    if (CacheUsage(e))
        e->CacheBuffer--;
    if (!CacheUsage(e)) { // read-only again once nobody has it mapped (the reference: at every unmap)
        SetCacheWritable(va, false);
        KeSetEvent(&CacheBufferFree, 0, FALSE);
    }
    Unlock();
}

// Drop the block (after a failed write, or contents that are no longer valid).
void DiscardCacheBuffer(PVOID buffer) {
    ULONG va = (ULONG)buffer & ~(PAGE_SIZE - 1);
    Lock();
    SetCacheWritable(va, false);
    FSC_ELEMENT *e = CacheElementOf(va);
    e->CacheBuffer = va | (e->CacheBuffer & FSC_DISCARDING);
    e->CacheExtension = nullptr;
    MoveToCacheHead(e);
    KeSetEvent(&CacheBufferFree, 0, FALSE);
    Unlock();
}

// Write the sectors covering [offset, offset + length) of a mapped buffer to the device, then unmap it (discard it
// if the write fails).
NTSTATUS WriteCacheBuffer(FSCACHE_EXTENSION *ext, LONGLONG offset, ULONG length, PVOID buffer) {
    ULONG mask = ext->SectorSize - 1;
    LONGLONG first = offset & ~(LONGLONG)mask, end = (offset + length + mask) & ~(LONGLONG)mask;
    ULONG va = ((ULONG)buffer & ~(PAGE_SIZE - 1)) | (ULONG)(first & (PAGE_SIZE - 1));
    NTSTATUS status = CacheDeviceIo(ext, IRP_MJ_WRITE, first, (PVOID)va, (ULONG)(end - first));
    if (NT_SUCCESS(status))
        UnmapCacheBuffer(buffer);
    else
        DiscardCacheBuffer(buffer);
    return status;
}

// Unmapped blocks of the extension in [firstBlock, lastBlock] become empty; mapped ones stay.
static void InvalidateCacheBlocks(FSCACHE_EXTENSION *ext, ULONG firstBlock, ULONG lastBlock) {
    Lock();
    for (PLIST_ENTRY l = CacheLruList.Blink, prev; l != &CacheLruList; l = prev) {
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
            MoveToCacheHead(e);
        }
    }
    KeSetEvent(&CacheBufferFree, 0, FALSE);
    Unlock();
}

void InvalidateCacheDevice(FSCACHE_EXTENSION *ext) {
    InvalidateCacheBlocks(ext, 0, 0xFFFFFFFF);
}

void InvalidateCacheRange(FSCACHE_EXTENSION *ext, LONGLONG offset, ULONG length) {
    if (length)
        InvalidateCacheBlocks(ext, (ULONG)(offset >> PAGE_SHIFT), (ULONG)((offset + length - 1) >> PAGE_SHIFT));
}

// Cached read into a caller's buffer (XDVDFS, UDF). As the reference: a read whose offset and buffer meet the
// device's alignment goes straight to the device for its whole pages, with the unaligned head and the short tail
// through the cache; anything else (and every read of a random-access file) is copied out of cache buffers.
// `toEnd` = the read runs to the end of the file: then the direct part may be rounded up to whole sectors.
static bool UseCache(PDEVICE_OBJECT device, UCHAR fileFlags, LONGLONG offset, ULONG length, PVOID buffer,
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

NTSTATUS CachedRead(FSCACHE_EXTENSION *ext, UCHAR fileFlags, LONGLONG offset, ULONG length, PVOID buffer,
                    bool toEnd) {
    auto out = (PUCHAR)buffer;
    if (UseCache(ext->TargetDeviceObject, fileFlags, offset, length, buffer, toEnd)) {
        while (length) {
            PVOID data;
            NTSTATUS status = MapCacheBuffer(ext, offset, FALSE, &data);
            if (!NT_SUCCESS(status))
                return status;
            ULONG n = PAGE_SIZE - (ULONG)(offset & (PAGE_SIZE - 1));
            if (n > length)
                n = length;
            RtlCopyMemory(out, data, n);
            UnmapCacheBuffer(data);
            offset += n;
            out += n;
            length -= n;
        }
        return STATUS_SUCCESS;
    }
    if (offset & (PAGE_SIZE - 1)) { // head up to the next page through the cache
        PVOID data;
        NTSTATUS status = MapCacheBuffer(ext, offset, FALSE, &data);
        if (!NT_SUCCESS(status))
            return status;
        ULONG n = PAGE_SIZE - (ULONG)(offset & (PAGE_SIZE - 1));
        RtlCopyMemory(out, data, n);
        UnmapCacheBuffer(data);
        offset += n;
        out += n;
        length -= n;
    }
    ULONG mask = ext->SectorSize - 1, direct = toEnd ? (length + mask) & ~mask : length & ~(PAGE_SIZE - 1);
    NTSTATUS status = CacheDeviceIo(ext, IRP_MJ_READ, offset, out, direct);
    if (!NT_SUCCESS(status) || direct >= length)
        return status;
    offset += direct;
    out += direct;
    length -= direct; // the short tail through the cache
    PVOID data;
    status = MapCacheBuffer(ext, offset, FALSE, &data);
    if (!NT_SUCCESS(status))
        return status;
    RtlCopyMemory(out, data, length);
    UnmapCacheBuffer(data);
    return STATUS_SUCCESS;
}

// --- Size

static NTSTATUS GrowCache(ULONG pages) {
    auto elements = (FSC_ELEMENT *)ExAllocatePoolWithTag(pages * sizeof(FSC_ELEMENT), 'AcsF');
    if (!elements)
        return STATUS_INSUFFICIENT_RESOURCES;
    ULONG added = pages - CachePages;
    auto va = (ULONG)memory::AllocateMappedPages(added, PTE_PRESENT | PTE_ACCESSED | PTE_DIRTY, false, MmFsCacheUsage);
    if (!va) {
        ExFreePool(elements);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    if (CachePages)
        RtlCopyMemory(elements, CacheElementArray, CachePages * sizeof(FSC_ELEMENT));
    for (ULONG i = CachePages; i < pages; i++, va += PAGE_SIZE) {
        elements[i] = {0, nullptr, va, {}};
        SetCacheIndex(va, i);
    }
    if (CacheElementArray)
        ExFreePool(CacheElementArray);
    CacheElementArray = elements;
    CachePages = pages;
    RebuildCacheList();
    return STATUS_SUCCESS;
}

// Shrinking discards unmapped buffers (waiting for mapped ones to be released) and frees their pages.
static NTSTATUS ShrinkCache(ULONG pages) {
    auto elements = pages ? (FSC_ELEMENT *)ExAllocatePoolWithTag(pages * sizeof(FSC_ELEMENT), 'AcsF') : nullptr;
    if (pages && !elements)
        return STATUS_INSUFFICIENT_RESOURCES;
    ULONG excess = CachePages - pages;
    while (excess) {
        for (ULONG i = CachePages; i-- && excess;) {
            FSC_ELEMENT *e = &CacheElementArray[i];
            if (!CacheUsage(e) && !(e->CacheBuffer & (FSC_READ_IN_PROGRESS | FSC_DISCARDING))) {
                e->CacheBuffer |= FSC_DISCARDING;
                e->CacheExtension = nullptr;
                excess--;
            }
        }
        if (excess) {
            KeResetEvent(&CacheBufferFree);
            Unlock();
            KeWaitForSingleObject(&CacheBufferFree, Executive, KernelMode, FALSE, nullptr);
            Lock();
        }
    }
    ULONG kept = 0;
    for (ULONG i = 0; i < CachePages; i++) {
        FSC_ELEMENT *e = &CacheElementArray[i];
        if (e->CacheBuffer & FSC_DISCARDING) {
            MmFreeSystemMemory((PVOID)CacheVirtualAddress(e), PAGE_SIZE);
            continue;
        }
        elements[kept] = *e;
        SetCacheIndex(CacheVirtualAddress(e), kept);
        kept++;
    }
    ExFreePool(CacheElementArray);
    CacheElementArray = elements;
    CachePages = pages;
    RebuildCacheList();
    KeSetEvent(&CacheBufferFree, 0, FALSE);
    return STATUS_SUCCESS;
}

XBAPI ULONG NTAPI FscGetCacheSize() {
    return CachePages;
}

XBAPI NTSTATUS NTAPI FscSetCacheSize(ULONG NumberOfCachePages) {
    if (NumberOfCachePages > 0x800)
        return STATUS_INVALID_PARAMETER;
    KeWaitForSingleObject(&CacheSizeLock, Executive, KernelMode, FALSE, nullptr);
    Lock();
    NTSTATUS status = STATUS_SUCCESS;
    if (NumberOfCachePages > CachePages)
        status = GrowCache(NumberOfCachePages);
    else if (NumberOfCachePages < CachePages)
        status = ShrinkCache(NumberOfCachePages);
    Unlock();
    KeSetEvent(&CacheSizeLock, 0, FALSE);
    return status;
}

// Every unmapped buffer becomes empty.
XBAPI void NTAPI FscInvalidateIdleBlocks() {
    Lock();
    for (ULONG i = 0; i < CachePages; i++) {
        FSC_ELEMENT *e = &CacheElementArray[i];
        if (e->CacheExtension && !CacheUsage(e) && !(e->CacheBuffer & FSC_READ_IN_PROGRESS)) {
            e->CacheExtension = nullptr;
            MoveToCacheHead(e);
        }
    }
    KeSetEvent(&CacheBufferFree, 0, FALSE);
    Unlock();
}

void InitCache() {
    KeInitializeEvent(&CacheLock, SynchronizationEvent, TRUE);
    KeInitializeEvent(&CacheSizeLock, SynchronizationEvent, TRUE);
    KeInitializeEvent(&CacheBufferFree, NotificationEvent, FALSE);
    KeInitializeEvent(&CacheReadComplete, NotificationEvent, FALSE);
    CacheElementArray = nullptr;
    CachePages = 0;
    InitializeListHead(&CacheLruList);
    FscSetCacheSize(16);
}

} // namespace xk::filesystems
