// Formatting the hard disk (spec/oxos_internal/30_disk_format.md), shared by its two halves: the partition table and
// the disk-level jobs (storage/disk_format.cpp: GPT, deleting all partitions, recognising a blank disk) and the exFAT
// volume (filesystems/hddfs/format_exfat.cpp). Neither touches the hardware: every sector goes through the caller's
// DISK_FORMAT_IO (the kernel: the whole-disk device, storage/disk_layout.cpp; the unit tests: an image file).
#pragma once
#include <xk/types.h>

// The disk, in 512-byte sectors.
struct DISK_FORMAT_IO {
    PVOID Context;
    NTSTATUS (*Write)(PVOID Context, ULONGLONG Lba, const void *Data, ULONG Count);
    NTSTATUS (*Read)(PVOID Context, ULONGLONG Lba, void *Data, ULONG Count);
};

// The scratch buffer every function below needs (bytes).
constexpr ULONG DISK_FORMAT_SCRATCH = 0x10000;

// One file placed on the new volume: Path is relative to the volume's root, its parts separated by '\\', each at most
// DISK_FORMAT_NAME characters of printable ASCII ("E\\ftp\\ftp.xbe"); the folders on the way are created.
constexpr ULONG DISK_FORMAT_NAME = 60;
struct DISK_FORMAT_FILE {
    const char *Path;
    const UCHAR *Data;
    ULONG Length;
};

struct DISK_FORMAT_RESULT {
    ULONGLONG PartitionStart, PartitionSectors; // the exFAT partition (disk sectors)
    ULONG ClusterShift, ClusterCount;
};

// The partition's first sector: 1 MiB in, after OXOS's reserved-area copy (sectors 1024-2047).
constexpr ULONGLONG DISK_FORMAT_PARTITION_START = 2048;

// Little-endian stores into on-disk structures.
inline void DiskFormatPut16(PUCHAR p, USHORT v) {
    p[0] = (UCHAR)v;
    p[1] = (UCHAR)(v >> 8);
}

inline void DiskFormatPut32(PUCHAR p, ULONG v) {
    DiskFormatPut16(p, (USHORT)v);
    DiskFormatPut16(p + 2, (USHORT)(v >> 16));
}

inline void DiskFormatPut64(PUCHAR p, ULONGLONG v) {
    DiskFormatPut32(p, (ULONG)v);
    DiskFormatPut32(p + 4, (ULONG)(v >> 32));
}

// --- storage/disk_format.cpp

// A GPT disk with one basic-data partition (from DISK_FORMAT_PARTITION_START to the backup GPT, in whole MiB) holding
// an exFAT volume (EXFATFormatVolume). Serial is the volume serial number and seeds the GPT's GUIDs.
NTSTATUS IdexDiskFormatEXFAT(ULONGLONG DiskSectors, ULONG Serial, ULONG Timestamp, const DISK_FORMAT_FILE *File,
                             const DISK_FORMAT_IO *Io, PUCHAR Scratch, DISK_FORMAT_RESULT *Result);

// Deletes all partitions: zeroes everything a layout is recognised by: the first 2 MiB (MBR or GPT, a whole-disk
// exFAT's boot regions, the reserved area, a partition's start at 2048), the last 2048 sectors (the backup GPT,
// PARTINFO tables), and the first 64 KB of every retail FATX partition and of the Extra sectors (other partitions'
// starts).
// Then writes the blank-disk marker (sector DISK_FORMAT_MARKER): the seconds the next boot counts down before it
// formats the disk (0: at once).
NTSTATUS IdexDiskDeleteAllPartitions(ULONGLONG DiskSectors, const ULONGLONG *Extra, ULONG ExtraCount,
                                     ULONG CountdownSeconds, const DISK_FORMAT_IO *Io, PUCHAR Scratch);

// The countdown before a blank disk is formatted: the marker's seconds (at most DISK_FORMAT_MAX_COUNTDOWN) when
// OXOSDeleteAllPartitions left one, else Default (a disk blanked by something else, or a new one).
// The marker's sector is one the blank check does not read and the format zeroes.
constexpr ULONGLONG DISK_FORMAT_MARKER = 1023;
// The countdown for a blank disk without a marker and for a foreign disk (not FATX, not exFAT), in seconds: release
// builds, development builds (made without VERSION=).
constexpr ULONG DISK_FORMAT_COUNTDOWN = 30, DISK_FORMAT_DEV_COUNTDOWN = 5, DISK_FORMAT_MAX_COUNTDOWN = 600;
ULONG IdexDiskBlankCountdown(const DISK_FORMAT_IO *Io, PUCHAR Scratch, ULONG Default);

// What a disk holds, for the countdown screen, into Out: the partition table and the file systems of its first
// partitions ("GPT: EFI, NTFS", "MBR: Linux ext2/3/4"), or the file system on the whole disk ("NTFS, no partition
// table"). Returns DISK_FORMAT_PC_TABLE and DISK_FORMAT_WHOLE_DISK_FS flags for what was found.
constexpr ULONG DISK_FORMAT_PC_TABLE = 1, DISK_FORMAT_WHOLE_DISK_FS = 2;
ULONG IdexDiskDescribe(ULONGLONG DiskSectors, const DISK_FORMAT_IO *Io, PUCHAR Scratch, char *Out, ULONG Size);

// True when every sector a layout is recognised by reads back as zeros: sectors 0, 1 and 2048, the last sector and
// the one 8 before it, and the first sectors of the retail C: and E:. A read that fails means not blank.
bool IdexDiskIsBlank(ULONGLONG DiskSectors, const DISK_FORMAT_IO *Io, PUCHAR Scratch);

// Count zero sectors from Lba (Scratch is zeroed and used as the source).
NTSTATUS IdexDiskWriteZeros(const DISK_FORMAT_IO *Io, PUCHAR Scratch, ULONGLONG Lba, ULONGLONG Count);

// CRC-32 (IEEE 802.3, as GPT uses it).
ULONG IdexCrc32(const UCHAR *Data, ULONG Length);

// --- filesystems/hddfs/format_exfat.cpp

// An exFAT volume of Sectors sectors at disk sector Start: 32 KB clusters below 32 GB, else 128 KB; its root holds the
// folders C E F X Y Z OXOS and, when File is given, that file. Timestamp is an exFAT timestamp (EXFATTimestamp) for
// the folders and the file.
NTSTATUS EXFATFormatVolume(ULONGLONG Start, ULONGLONG Sectors, ULONG Serial, ULONG Timestamp,
                           const DISK_FORMAT_FILE *File, const DISK_FORMAT_IO *Io, PUCHAR Scratch,
                           DISK_FORMAT_RESULT *Result);

// exFAT timestamp: year (1980-2107), month, day, hour, minute, second.
ULONG EXFATTimestamp(ULONG Year, ULONG Month, ULONG Day, ULONG Hour, ULONG Minute, ULONG Second);
