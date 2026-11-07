// File systems: their mount routines and the cache they share (spec/stock/16 §Fsc).
#pragma once
#include <xk/io.h>

// What the cache needs to know about a volume's device (the reference's layout: device, length, sector size).
struct FSCACHE_EXTENSION {
    PDEVICE_OBJECT TargetDeviceObject;
    LARGE_INTEGER PartitionLength;
    ULONG SectorSize;
};

void FscInitialize();
// Map the block holding `offset` (read from the device if not cached); *buffer points at `offset` itself. Every
// map is matched by FscUnmapBuffer, FscWriteBuffer or FscDiscardBuffer.
NTSTATUS FscMapBuffer(FSCACHE_EXTENSION *ext, LONGLONG offset, BOOLEAN writable, PVOID *buffer);
NTSTATUS FscMapEmptyBuffer(FSCACHE_EXTENSION *ext, LONGLONG offset, PVOID *buffer);
void FscUnmapBuffer(PVOID buffer);
void FscDiscardBuffer(PVOID buffer);
NTSTATUS FscWriteBuffer(FSCACHE_EXTENSION *ext, LONGLONG offset, ULONG length, PVOID buffer);
void FscInvalidateDevice(FSCACHE_EXTENSION *ext);
void FscInvalidateByteRange(FSCACHE_EXTENSION *ext, LONGLONG offset, ULONG length);
NTSTATUS FscCachedRead(FSCACHE_EXTENSION *ext, UCHAR fileFlags, LONGLONG offset, ULONG length, PVOID buffer,
                       bool toEnd);

// Set-up and the cache size (titles' exports).
XBAPI ULONG NTAPI FscGetCacheSize();
XBAPI NTSTATUS NTAPI FscSetCacheSize(ULONG NumberOfCachePages);

// Mounting (filesystems/mount.cpp): the routines the I/O manager tries for a disk and for a CD-ROM, registered at
// start-up.
void FsInitSystem();

// The file systems' set-up and mount routines (filesystems/hddfs/volume.cpp, filesystems/raw.cpp, udf.cpp,
// xdvdfs.cpp).
void HDDFSInitialize();
NTSTATUS RawMountVolume(PDEVICE_OBJECT TargetDevice);
NTSTATUS UdfMountVolume(PDEVICE_OBJECT TargetDevice);
NTSTATUS XdvdfsMountVolume(PDEVICE_OBJECT TargetDevice);

// The title's D: folder on the hard disk (filesystems/hddfs/dispatch.cpp, spec/oxos_internal/25 §2): remembered
// before the title starts, so that D: reports a DVD's 2048-byte sectors.
void HDDFSRememberTitleFolder();
