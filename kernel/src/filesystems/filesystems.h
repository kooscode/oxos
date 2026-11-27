// File systems: their mount routines and the cache they share (spec/stock/16 §Fsc).
#pragma once
#include <io/io.h>
#include <storage/disk_format.h>

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

// The cache partitions' formats from C:\OXOS\cache\oxos_cache_partitions.cache, on an exFAT disk (filesystems/hddfs/volume_exfat.cpp): at a cold boot,
// boot-only.
void ReadCachePartitions();

void InitCache();
// Map the block holding `offset` (read from the device if not cached); *buffer points at `offset` itself. Every map is matched by UnmapCacheBuffer,
// WriteCacheBuffer or DiscardCacheBuffer.
NTSTATUS MapCacheBuffer(FSCACHE_EXTENSION *ext, LONGLONG offset, BOOLEAN writable, PVOID *buffer);
NTSTATUS MapEmptyCacheBuffer(FSCACHE_EXTENSION *ext, LONGLONG offset, PVOID *buffer);
void UnmapCacheBuffer(PVOID buffer);
void DiscardCacheBuffer(PVOID buffer);
NTSTATUS WriteCacheBuffer(FSCACHE_EXTENSION *ext, LONGLONG offset, ULONG length, PVOID buffer);
void InvalidateCacheDevice(FSCACHE_EXTENSION *ext);
void InvalidateCacheRange(FSCACHE_EXTENSION *ext, LONGLONG offset, ULONG length);
// The PTE that maps cache element ElementIndex (the page accounting in memory/init.cpp).
ULONG *CachePte(ULONG ElementIndex);
NTSTATUS CachedRead(FSCACHE_EXTENSION *ext, UCHAR fileFlags, LONGLONG offset, ULONG length, PVOID buffer, bool toEnd);

// Mounting (filesystems/mount.cpp): the routines the I/O manager tries for a disk and for a CD-ROM, registered at start-up.
void InitFileSystems();

// The file systems' set-up and mount routines (filesystems/hddfs/volume.cpp, filesystems/raw.cpp, udf.cpp, xdvdfs.cpp).
void HDDFSInitialize();
NTSTATUS RawMountVolume(PDEVICE_OBJECT TargetDevice);
NTSTATUS UdfMountVolume(PDEVICE_OBJECT TargetDevice);
NTSTATUS XdvdfsMountVolume(PDEVICE_OBJECT TargetDevice);

// The title's D: folder on the hard disk (filesystems/hddfs/dispatch.cpp, spec/oxos_internal/25 §2): remembered before the title starts, so that D: reports a
// DVD's 2048-byte sectors.
void HDDFSRememberTitleFolder();
// [storage] show_exfat_as_fatx = yes (read at a cold boot): an exFAT volume names its file system "FATX", not "exFAT" (spec/oxos_internal/05 §3.1).
extern XK_PERSIST bool ExfatShownAsFatx;

} // namespace xk::filesystems

// --- Formatting an exFAT volume (filesystems/hddfs/format_exfat.cpp; spec/oxos_internal/30): the disk format
// (storage/disk_format.cpp) calls it with the types of storage/disk_format.h.

namespace xk::filesystems {

// An exFAT volume of Sectors sectors at disk sector Start: 32 KB clusters below 32 GB, else 128 KB; its root holds the folders C E F X Y Z OXOS and, when File
// is given, that file. Timestamp is an exFAT timestamp (EXFATTimestamp) for the folders and the file.
NTSTATUS EXFATFormatVolume(ULONGLONG Start, ULONGLONG Sectors, ULONG Serial, ULONG Timestamp, const DISK_FORMAT_FILE *File, const DISK_FORMAT_IO *Io,
                           PUCHAR Scratch, DISK_FORMAT_RESULT *Result);

// exFAT timestamp: year (1980-2107), month, day, hour, minute, second.
ULONG EXFATTimestamp(ULONG Year, ULONG Month, ULONG Day, ULONG Hour, ULONG Minute, ULONG Second);

} // namespace xk::filesystems
