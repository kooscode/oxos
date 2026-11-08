// Formatting the hard disk, the disk-level half (spec/oxos_internal/30_disk_format.md): the GPT around the exFAT
// volume (filesystems/hddfs/format_exfat.cpp writes the volume), deleting all partitions, recognising a blank disk.
// Every sector goes out through the caller's DISK_FORMAT_IO (storage/disk_layout.cpp: the whole-disk device).
#include <xk/disk_format.h>
#include <xk/runtime.h>

ULONG IdexCrc32(const UCHAR *Data, ULONG Length) {
    ULONG crc = 0xFFFFFFFF;
    while (Length--) {
        crc ^= *Data++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
    }
    return ~crc;
}

NTSTATUS IdexDiskWriteZeros(const DISK_FORMAT_IO *Io, PUCHAR Scratch, ULONGLONG Lba, ULONGLONG Count) {
    constexpr ULONG chunk = DISK_FORMAT_SCRATCH / 512;
    RtlZeroMemory(Scratch, DISK_FORMAT_SCRATCH);
    while (Count) {
        ULONG n = Count < chunk ? (ULONG)Count : chunk;
        NTSTATUS status = Io->Write(Io->Context, Lba, Scratch, n);
        if (!NT_SUCCESS(status))
            return status;
        Lba += n;
        Count -= n;
    }
    return STATUS_SUCCESS;
}

// --- Deleting all partitions, recognising a blank disk

// The retail FATX partitions' first sectors (X:, Y:, Z:, C:, E:) and the automatic F: of a larger disk
// (disk_layout.cpp).
static const ULONGLONG IdexFatxStarts[] = {0x400, 0x177400, 0x2EE400, 0x465400, 0x55F400, 0xEE8AB0};

// The blank-disk marker: this text, then the countdown in seconds (little-endian, 4 bytes).
static const char IdexBlankMarker[16] = "OXOS BLANK DISK";

NTSTATUS IdexDiskDeleteAllPartitions(ULONGLONG DiskSectors, const ULONGLONG *Extra, ULONG ExtraCount,
                                     ULONG CountdownSeconds, const DISK_FORMAT_IO *Io, PUCHAR Scratch) {
    NTSTATUS status = IdexDiskWriteZeros(Io, Scratch, 0, DiskSectors < 4096 ? DiskSectors : 4096);
    if (NT_SUCCESS(status) && DiskSectors > 4096)
        status = IdexDiskWriteZeros(Io, Scratch, DiskSectors - 2048, 2048);
    for (ULONG i = 0; i < sizeof(IdexFatxStarts) / sizeof(IdexFatxStarts[0]) + ExtraCount && NT_SUCCESS(status); i++) {
        ULONGLONG lba = i < 6 ? IdexFatxStarts[i] : Extra[i - 6];
        if (lba + 128 <= DiskSectors)
            status = IdexDiskWriteZeros(Io, Scratch, lba, 128);
    }
    if (NT_SUCCESS(status)) {
        RtlZeroMemory(Scratch, 512);
        RtlCopyMemory(Scratch, IdexBlankMarker, sizeof(IdexBlankMarker));
        DiskFormatPut32(Scratch + 16, CountdownSeconds);
        status = Io->Write(Io->Context, DISK_FORMAT_MARKER, Scratch, 1);
    }
    return status;
}

ULONG IdexDiskBlankCountdown(const DISK_FORMAT_IO *Io, PUCHAR Scratch, ULONG Default) {
    if (!NT_SUCCESS(Io->Read(Io->Context, DISK_FORMAT_MARKER, Scratch, 1)))
        return Default;
    for (ULONG i = 0; i < sizeof(IdexBlankMarker); i++) {
        if (Scratch[i] != (UCHAR)IdexBlankMarker[i])
            return Default;
    }
    ULONG seconds = Scratch[16] | (Scratch[17] << 8) | (Scratch[18] << 16) | ((ULONG)Scratch[19] << 24);
    return seconds < DISK_FORMAT_MAX_COUNTDOWN ? seconds : DISK_FORMAT_MAX_COUNTDOWN;
}

bool IdexDiskIsBlank(ULONGLONG DiskSectors, const DISK_FORMAT_IO *Io, PUCHAR Scratch) {
    if (DiskSectors < 0x55F400 + 1)
        return false;
    const ULONGLONG checked[] = {0, 1, DISK_FORMAT_PARTITION_START, DiskSectors - 1, DiskSectors - 8,
                                 0x465400, 0x55F400};
    for (ULONGLONG lba : checked) {
        if (!NT_SUCCESS(Io->Read(Io->Context, lba, Scratch, 1)))
            return false;
        for (ULONG i = 0; i < 512; i++) {
            if (Scratch[i])
                return false;
        }
    }
    return true;
}

// --- Naming what is on a disk

// The file system whose first sectors are at Scratch (4 sectors from the partition's or the disk's start), or null.
static const char *IdexFileSystemName(const UCHAR *b) {
    auto at = [&](ULONG offset, const char *text) {
        for (ULONG i = 0; text[i]; i++) {
            if (b[offset + i] != (UCHAR)text[i])
                return false;
        }
        return true;
    };
    if (at(3, "NTFS    "))
        return "NTFS";
    if (at(3, "EXFAT   "))
        return "exFAT";
    if (at(82, "FAT32   "))
        return "FAT32";
    if (at(54, "FAT16   ") || at(54, "FAT12   "))
        return "FAT16";
    if (at(0, "FATX"))
        return "FATX";
    if (b[1080] == 0x53 && b[1081] == 0xEF)
        return "Linux ext2/3/4";
    if (at(0, "XFSB"))
        return "Linux XFS";
    if (at(1024, "H+") || at(1024, "HX"))
        return "Mac HFS+";
    if (at(32, "NXSB"))
        return "Mac APFS";
    return nullptr;
}

// A GPT partition type's name: Overrides when it says more than the file system on it would (an EFI system partition
// holds FAT32); otherwise it names a partition whose file system is not recognised. Name null: no name of its own.
struct IDEX_GPT_TYPE {
    UCHAR Guid[4]; // the first bytes of the type GUID, as stored
    const char *Name;
    bool Overrides;
};
static const IDEX_GPT_TYPE IdexGptTypes[] = {
    {{0x28, 0x73, 0x2A, 0xC1}, "EFI", true},          // C12A7328-...
    {{0x16, 0xE3, 0xC9, 0xE3}, nullptr, true},        // E3C9E316-...: Microsoft reserved, no file system
    {{0xAF, 0x3D, 0xC6, 0x0F}, "Linux", false},       // 0FC63DAF-...
    {{0x6D, 0xFD, 0x57, 0x06}, "Linux swap", false},  // 0657FD6D-...
    {{0xEF, 0x57, 0x34, 0x7C}, "Mac APFS", false},    // 7C3457EF-...
    {{0x00, 0x53, 0x46, 0x48}, "Mac HFS+", false},    // 48465300-...
};

static const IDEX_GPT_TYPE *IdexGptType(const UCHAR *Guid) {
    for (auto &t : IdexGptTypes) {
        if (Guid[0] == t.Guid[0] && Guid[1] == t.Guid[1] && Guid[2] == t.Guid[2] && Guid[3] == t.Guid[3])
            return &t;
    }
    return nullptr;
}

// An MBR partition type's name, for a partition whose file system is not recognised.
static const char *IdexMbrTypeName(UCHAR Type) {
    switch (Type) {
    case 0x07:
        return "NTFS/exFAT";
    case 0x0B:
    case 0x0C:
        return "FAT32";
    case 0x04:
    case 0x06:
    case 0x0E:
        return "FAT16";
    case 0x82:
        return "Linux swap";
    case 0x83:
        return "Linux";
    case 0x8E:
        return "Linux LVM";
    case 0xAF:
        return "Mac";
    case 0xFD:
        return "Linux RAID";
    default:
        return "unknown";
    }
}

static bool IdexSameText(const char *a, const char *b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

ULONG IdexDiskDescribe(ULONGLONG DiskSectors, const DISK_FORMAT_IO *Io, PUCHAR Scratch, char *Out, ULONG Size) {
    ULONG n = 0;
    auto append = [&](const char *text) {
        for (ULONG i = 0; text[i] && n + 1 < Size; i++)
            Out[n++] = text[i];
        Out[n] = 0;
    };
    // The list of names after "GPT: " / "MBR: ", each once.
    const char *names[4];
    ULONG count = 0;
    auto name = [&](const char *text) {
        for (ULONG i = 0; i < count; i++) {
            if (IdexSameText(names[i], text))
                return;
        }
        append(count ? ", " : "");
        append(text);
        names[count++] = text;
    };
    Out[0] = 0;
    if (!NT_SUCCESS(Io->Read(Io->Context, 0, Scratch, 4))) {
        append("unreadable");
        return 0;
    }
    if (const char *whole = IdexFileSystemName(Scratch)) { // a file system's boot sector at sector 0: no table
        append(whole);
        append(", no partition table");
        return DISK_FORMAT_WHOLE_DISK_FS;
    }
    if (Scratch[510] != 0x55 || Scratch[511] != 0xAA) {
        append("unknown");
        return 0;
    }
    // Up to four partitions: their starts, and their names from the table.
    ULONGLONG starts[4];
    const char *typeNames[4];
    bool overrides[4];
    ULONG parts = 0;
    if (Scratch[446 + 4] == 0xEE) {
        append("GPT: ");
        if (NT_SUCCESS(Io->Read(Io->Context, 1, Scratch, 1)) && Scratch[0] == 'E' && Scratch[1] == 'F') {
            ULONGLONG entries = *(const ULONGLONG *)(Scratch + 72);
            ULONG entrySize = *(const ULONG *)(Scratch + 84);
            if (entrySize == 128 && entries < DiskSectors && NT_SUCCESS(Io->Read(Io->Context, entries, Scratch, 4))) {
                for (ULONG i = 0; i < 16 && parts < 4; i++) {
                    const UCHAR *e = Scratch + 128 * i;
                    ULONGLONG start = *(const ULONGLONG *)(e + 32);
                    if (!start || start >= DiskSectors)
                        continue;
                    const IDEX_GPT_TYPE *t = IdexGptType(e);
                    typeNames[parts] = t ? t->Name : "unknown";
                    overrides[parts] = t && t->Overrides;
                    starts[parts++] = start;
                }
            }
        }
    } else {
        append("MBR: ");
        for (ULONG i = 0; i < 4; i++) {
            const UCHAR *e = Scratch + 446 + 16 * i;
            ULONG start = *(const ULONG *)(e + 8);
            if (e[4] && start && start < DiskSectors) {
                typeNames[parts] = IdexMbrTypeName(e[4]);
                overrides[parts] = false;
                starts[parts++] = start;
            }
        }
    }
    // Each partition by its file system, unless its type says more.
    for (ULONG i = 0; i < parts; i++) {
        const char *fs = NT_SUCCESS(Io->Read(Io->Context, starts[i], Scratch, 4)) ? IdexFileSystemName(Scratch)
                                                                                   : nullptr;
        const char *label = overrides[i] || !fs ? typeNames[i] : fs;
        if (label)
            name(label);
    }
    if (!count)
        append("no partitions");
    return DISK_FORMAT_PC_TABLE;
}

// --- The GPT

// 16 bytes from a xorshift generator seeded by Seed, marked as a version 4 GUID.
static void Guid(PUCHAR Out, ULONG *Seed) {
    for (ULONG i = 0; i < 16; i++) {
        *Seed ^= *Seed << 13;
        *Seed ^= *Seed >> 17;
        *Seed ^= *Seed << 5;
        Out[i] = (UCHAR)*Seed;
    }
    Out[7] = (UCHAR)((Out[7] & 0x0F) | 0x40);
    Out[8] = (UCHAR)((Out[8] & 0x3F) | 0x80);
}

// The protective MBR, both GPT headers and both entry arrays (128 entries of 128 bytes), one basic-data partition.
static NTSTATUS WriteGpt(ULONGLONG DiskSectors, ULONGLONG Start, ULONGLONG Sectors, ULONG Serial,
                         const DISK_FORMAT_IO *Io, PUCHAR Scratch) {
    static const UCHAR basicData[16] = {0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
                                        0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};
    ULONG seed = Serial | 1;
    UCHAR diskGuid[16];
    Guid(diskGuid, &seed);
    // The entry array (32 sectors) at Scratch + 1024.
    PUCHAR entries = Scratch + 1024;
    RtlZeroMemory(Scratch, 1024 + 32 * 512);
    RtlCopyMemory(entries, basicData, 16);
    Guid(entries + 16, &seed);
    DiskFormatPut64(entries + 32, Start);
    DiskFormatPut64(entries + 40, Start + Sectors - 1);
    const char label[] = "OXOS";
    for (ULONG i = 0; label[i]; i++)
        DiskFormatPut16(entries + 56 + 2 * i, (UCHAR)label[i]);
    ULONG entriesCrc = IdexCrc32(entries, 32 * 512);
    // The headers at Scratch + 512 (primary, then rewritten for the backup).
    auto header = [&](ULONGLONG self, ULONGLONG other, ULONGLONG entriesLba) {
        PUCHAR h = Scratch + 512;
        RtlZeroMemory(h, 512);
        RtlCopyMemory(h, "EFI PART", 8);
        DiskFormatPut32(h + 8, 0x00010000);
        DiskFormatPut32(h + 12, 92);
        DiskFormatPut64(h + 24, self);
        DiskFormatPut64(h + 32, other);
        DiskFormatPut64(h + 40, 34);
        DiskFormatPut64(h + 48, DiskSectors - 34);
        RtlCopyMemory(h + 56, diskGuid, 16);
        DiskFormatPut64(h + 72, entriesLba);
        DiskFormatPut32(h + 80, 128);
        DiskFormatPut32(h + 84, 128);
        DiskFormatPut32(h + 88, entriesCrc);
        DiskFormatPut32(h + 16, IdexCrc32(h, 92));
    };
    // The protective MBR at Scratch.
    PUCHAR mbr = Scratch;
    PUCHAR e = mbr + 446;
    e[1] = 0x00;
    e[2] = 0x02;
    e[4] = 0xEE;
    e[5] = e[6] = e[7] = 0xFF;
    DiskFormatPut32(e + 8, 1);
    DiskFormatPut32(e + 12, DiskSectors - 1 > 0xFFFFFFFF ? 0xFFFFFFFF : (ULONG)(DiskSectors - 1));
    mbr[510] = 0x55;
    mbr[511] = 0xAA;
    // The backup first (entries before the last sector, then its header), the primary last.
    NTSTATUS status = Io->Write(Io->Context, DiskSectors - 33, entries, 32);
    if (NT_SUCCESS(status)) {
        header(DiskSectors - 1, 1, DiskSectors - 33);
        status = Io->Write(Io->Context, DiskSectors - 1, Scratch + 512, 1);
    }
    if (NT_SUCCESS(status))
        status = Io->Write(Io->Context, 2, entries, 32);
    if (NT_SUCCESS(status)) {
        header(1, DiskSectors - 1, 2);
        status = Io->Write(Io->Context, 0, Scratch, 2); // the MBR and the primary header
    }
    return status;
}

NTSTATUS IdexDiskFormatEXFAT(ULONGLONG DiskSectors, ULONG Serial, ULONG Timestamp, const DISK_FORMAT_FILE *File,
                             const DISK_FORMAT_IO *Io, PUCHAR Scratch, DISK_FORMAT_RESULT *Result) {
    if (DiskSectors < (1ull << 21)) // under 1 GiB: not a disk OXOS formats
        return (NTSTATUS)0xC000000D; // STATUS_INVALID_PARAMETER
    // The partition: from sector 2048 up to the backup GPT (32 sectors of entries and its header), in whole MiB.
    ULONGLONG start = DISK_FORMAT_PARTITION_START, sectors = (DiskSectors - 34 + 1 - start) & ~2047ull;
    NTSTATUS status = EXFATFormatVolume(start, sectors, Serial, Timestamp, File, Io, Scratch, Result);
    // The space before the partition (the reserved-area copy included), then the GPT.
    if (NT_SUCCESS(status))
        status = IdexDiskWriteZeros(Io, Scratch, 34, start - 34);
    if (NT_SUCCESS(status))
        status = WriteGpt(DiskSectors, start, sectors, Serial, Io, Scratch);
    return status;
}
