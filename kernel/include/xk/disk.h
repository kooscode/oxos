// The hard disk driver's internal interface between its two files: disk.cpp (devices, transfers, IOCTLs, start-up) and
// disk_layout.cpp (the partition layout).
#pragma once
#include <xk/storage.h>

// Kinds of disk device: a FATX partition (or Partition0), a folder of the exFAT volume, the exFAT volume itself, and
// the whole disk.
enum : UCHAR { IDE_KIND_PARTITION, IDE_KIND_FOLDER, IDE_KIND_VOLUME, IDE_KIND_PHYSICAL };

// FATX disk partitions 1-14 (sectors): the retail five, the automatic F: and G: of a larger disk, or the disk's own
// PARTINFO table (spec/cerbios/03 §2-§3). Index = partition number.
struct IDE_PARTITION {
    ULONGLONG Start, Length;
};

namespace xk::storage {

// Creates a disk device of Kind over the byte range Start, Length (disk.cpp).
NTSTATUS CreateDiskDevice(ULONG Number, LONGLONG Start, LONGLONG Length, PDEVICE_OBJECT *Out,
                          UCHAR Kind = IDE_KIND_PARTITION);

extern IDE_PARTITION DiskPartitions[15];
extern BOOLEAN DiskReformatted; // formatted or partitions deleted since boot: only the whole-disk device writes

// The layout (disk_layout.cpp).
void InitDiskLayout(); // disk start-up: the whole-disk device the layout is read through
void DetectDiskLayout();
// Reads whole-disk sectors; a failed read is tried twice more (10 ms apart).
NTSTATUS ReadDiskSectors(ULONGLONG Lba, PVOID Buffer, ULONG Length);

} // namespace xk::storage
