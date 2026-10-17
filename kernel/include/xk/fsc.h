// File system cache (spec/16 §Fsc): shared 4 KB buffers for the file systems.
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
