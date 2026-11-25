// Storage: what sits on the IDE drives (hardware/ide/): the hard disk's layout and partitions (disk_layout.cpp, spec/evox/03, spec/oxos_internal/45,
// spec/oxos_internal/05), formatting it (disk_format.cpp) and virtual discs (virtual_disc.cpp, spec/oxos_internal/16).
#pragma once
#include <io/io.h>
#include <hardware/ide/ide.h>

// OXOS's own exports (storage/disk_layout.cpp, spec/oxos_internal/30_disk_format.md): format the hard disk as exFAT (keeping one file), and delete all
// partitions.
XBAPI NTSTATUS NTAPI OXOSFormatDisk(PCSZ KeepPath);
XBAPI NTSTATUS NTAPI OXOSDeleteAllPartitions(ULONG CountdownSeconds);

// FATX disk partitions 1-14 (sectors): the retail five, the automatic F: and G: of a larger disk, or the disk's own PARTINFO table (oz_paulb's, spec/evox/03;
// spec/oxos_internal/45 §5). Index = partition number.
struct IDE_PARTITION {
    ULONGLONG Start, Length;
};

namespace xk::storage {

// The layout (disk_layout.cpp): the partitions the disk devices are created over (spec/oxos_internal/45 §5).
extern IDE_PARTITION DiskPartitions[15];
extern BOOLEAN DiskReformatted; // formatted or partitions deleted since boot: only the whole-disk device writes
void InitDiskLayout(); // disk start-up: the whole-disk device the layout is read through
void DetectDiskLayout();
// Reads whole-disk sectors; a failed read is tried twice more (10 ms apart).
NTSTATUS ReadDiskSectors(ULONGLONG Lba, PVOID Buffer, ULONG Length);



// exFAT layout (spec/oxos_internal/05_exfat_storage_spec.md): set before the first partition lookup.
extern BOOLEAN DiskHasEXFATLayout;
extern PDEVICE_OBJECT EXFATVolumeDevice; // the exFAT partition, from the 4 KB disk boundary at or before its start
extern ULONG EXFATVolumeBias;             // bytes from EXFATVolumeDevice's start to the exFAT volume's (0 when the partition starts on a 4 KB boundary)
extern BOOLEAN DiskHasForeignLayout; // a PC partition table without a usable exFAT volume: no partitions, no writes


// The virtual disc driver's hooks in the DVD driver (virtual_disc.cpp), and its restart: the disc images mounted before a quick reboot are mounted again (the
// title launcher calls it).
bool ServeVirtualDvd(PIRP Irp, NTSTATUS *Status);
bool IsDvdImageMounted();
void RestoreVirtualDiscMounts();
// Unmounts every disc image (spec/oxos_internal/16 §5): the title launcher calls it at a launch away from the disc (titles::IsLaunchAwayFromDisc) and when a
// title ends with a quick reboot that asks for nothing, so an image left mounted does not become the disc the next title falls back to.
void UnmountVirtualDiscs();
// Whether the title that ran last mounted an image (an attach XBE that then quick-reboots into it, spec/oxos_internal/16 §5), and the title launcher's
// reset of that mark as it enters the next title.
bool HasVirtualDiscFromLastTitle();
void ForgetVirtualDiscMounters();
void InitVirtualDiscs(); // start-up: the virtual disc control device (the IDE start-up calls it)
// The mounts a quick reboot keeps, copied out (into Out, Size bytes; the bytes copied, 0 when they do not fit) and back after a power cycle (then mounted
// again): the sound recovery's saved launch (kernel/modules/sound_recovery.cpp, spec/oxos_internal/46 §3).
ULONG CopyVirtualDiscMounts(void *Out, ULONG Size);
void RestoreVirtualDiscMountsFrom(const void *In, ULONG Size);

} // namespace xk::storage
