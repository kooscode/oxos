// The hard disk driver's internal interface between its two files: disk.cpp (devices, transfers, IOCTLs, start-up) and
// disk_layout.cpp (the partition layout).
#pragma once
#include <xk/storage.h>

// Kinds of disk device: a FATX partition (or Partition0), a folder of the exFAT volume, the exFAT volume itself, and
// the whole disk.
enum : UCHAR { IDE_KIND_PARTITION, IDE_KIND_FOLDER, IDE_KIND_VOLUME, IDE_KIND_PHYSICAL };

// Creates a disk device of Kind over the byte range Start, Length (disk.cpp).
NTSTATUS IdexDiskCreateDevice(ULONG Number, LONGLONG Start, LONGLONG Length, PDEVICE_OBJECT *Out,
                              UCHAR Kind = IDE_KIND_PARTITION);

// FATX disk partitions 1-14 (sectors): the retail five, the automatic F: and G: of a larger disk, or the disk's own
// PARTINFO table (spec/cerbios/03 §2-§3). Index = partition number.
struct IDE_PARTITION {
    ULONGLONG Start, Length;
};
extern IDE_PARTITION IdexDiskPartitions[15];
extern BOOLEAN IdexDiskReformatted;   // formatted or partitions deleted since boot: only the whole-disk device writes

// The layout (disk_layout.cpp).
void IdexDiskLayoutInit(); // disk start-up: the whole-disk device the layout is read through
void IdexDiskDetectLayout();
// Reads whole-disk sectors; a failed read is tried twice more (10 ms apart).
NTSTATUS IdexDiskReadPhysical(ULONGLONG Lba, PVOID Buffer, ULONG Length);
