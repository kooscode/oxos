// File systems: their mount routines and the cache they share (spec/stock/16 §Fsc).
#pragma once
#include <xk/io.h>

// What the cache needs to know about a volume's device (the reference's layout: device, length, sector size).
struct FSCACHE_EXTENSION {
    PDEVICE_OBJECT TargetDeviceObject;
    LARGE_INTEGER PartitionLength;
    ULONG SectorSize;
};

// The cache size (titles' exports).
XBAPI ULONG NTAPI FscGetCacheSize();
XBAPI NTSTATUS NTAPI FscSetCacheSize(ULONG NumberOfCachePages);

namespace xk::filesystems {

void InitCache();
// Map the block holding `offset` (read from the device if not cached); *buffer points at `offset` itself. Every
// map is matched by UnmapCacheBuffer, WriteCacheBuffer or DiscardCacheBuffer.
NTSTATUS MapCacheBuffer(FSCACHE_EXTENSION *ext, LONGLONG offset, BOOLEAN writable, PVOID *buffer);
NTSTATUS MapEmptyCacheBuffer(FSCACHE_EXTENSION *ext, LONGLONG offset, PVOID *buffer);
void UnmapCacheBuffer(PVOID buffer);
void DiscardCacheBuffer(PVOID buffer);
NTSTATUS WriteCacheBuffer(FSCACHE_EXTENSION *ext, LONGLONG offset, ULONG length, PVOID buffer);
void InvalidateCacheDevice(FSCACHE_EXTENSION *ext);
void InvalidateCacheRange(FSCACHE_EXTENSION *ext, LONGLONG offset, ULONG length);
// The PTE that maps cache element ElementIndex (the page accounting in memory/init.cpp).
ULONG *CachePte(ULONG ElementIndex);
NTSTATUS CachedRead(FSCACHE_EXTENSION *ext, UCHAR fileFlags, LONGLONG offset, ULONG length, PVOID buffer,
                    bool toEnd);

// Mounting (filesystems/mount.cpp): the routines the I/O manager tries for a disk and for a CD-ROM, registered at
// start-up.
void InitFileSystems();

// The file systems' set-up and mount routines (filesystems/hddfs/volume.cpp, filesystems/raw.cpp, udf.cpp,
// xdvdfs.cpp).
void HDDFSInitialize();
NTSTATUS RawMountVolume(PDEVICE_OBJECT TargetDevice);
NTSTATUS UdfMountVolume(PDEVICE_OBJECT TargetDevice);
NTSTATUS XdvdfsMountVolume(PDEVICE_OBJECT TargetDevice);

// The title's D: folder on the hard disk (filesystems/hddfs/dispatch.cpp, spec/oxos_internal/25 §2): remembered
// before the title starts, so that D: reports a DVD's 2048-byte sectors.
void HDDFSRememberTitleFolder();

} // namespace xk::filesystems
