// Storage: what sits on the IDE drives (hardware/ide/): the hard disk's layout and partitions (disk_layout.cpp, spec/evox/03, spec/cerbios/03,
// spec/oxos_internal/05), formatting it (disk_format.cpp) and virtual discs (virtual_disc.cpp, spec/oxos_internal/16).
#pragma once
#include <io/io.h>
#include <hardware/ide/ide.h>

// OXOS's own exports (storage/disk_layout.cpp, spec/oxos_internal/30_disk_format.md): format the hard disk as exFAT (keeping one file), and delete all
// partitions.
XBAPI NTSTATUS NTAPI OXOSFormatDisk(PCSZ KeepPath);
XBAPI NTSTATUS NTAPI OXOSDeleteAllPartitions(ULONG CountdownSeconds);

// FATX disk partitions 1-14 (sectors): the retail five, the automatic F: and G: of a larger disk, or the disk's own PARTINFO table (oz_paulb's, spec/evox/03;
// spec/cerbios/03 §2-§3). Index = partition number.
struct IDE_PARTITION {
    ULONGLONG Start, Length;
};

namespace xk::storage {

// The layout (disk_layout.cpp): the partitions the disk devices are created over (spec/cerbios/03 §2-§3).
extern IDE_PARTITION DiskPartitions[15];
extern BOOLEAN DiskReformatted; // formatted or partitions deleted since boot: only the whole-disk device writes
void InitDiskLayout(); // disk start-up: the whole-disk device the layout is read through
void DetectDiskLayout();
// Reads whole-disk sectors; a failed read is tried twice more (10 ms apart).
NTSTATUS ReadDiskSectors(ULONGLONG Lba, PVOID Buffer, ULONG Length);



// exFAT layout (spec/oxos_internal/05_exfat_storage_spec.md): set before the first partition lookup.
extern BOOLEAN DiskHasEXFATLayout;
extern PDEVICE_OBJECT EXFATVolumeDevice; // the exFAT partition
extern BOOLEAN DiskHasForeignLayout; // a PC partition table without a usable exFAT volume: no partitions, no writes


// The virtual disc driver's hooks in the DVD driver (virtual_disc.cpp), and its restart: the disc images mounted before a quick reboot are mounted again (the
// title launcher calls it).
bool ServeVirtualDvd(PIRP Irp, NTSTATUS *Status);
bool IsDvdImageMounted();
void RestoreVirtualDiscMounts();
void InitVirtualDiscs(); // start-up: the virtual disc control device (the IDE start-up calls it)

} // namespace xk::storage
