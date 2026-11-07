// The hard disk's layout (spec/cerbios/03, spec/oxos_internal/05_exfat_storage_spec.md): retail FATX partitions (the
// fixed table, Cerbios's larger-disk defaults or a PARTINFO table), or an exFAT volume (GPT, MBR or the whole disk).
// Read once, at the first open of a partition or the power-cycle count.
#include <xk/storage.h>
#include <xk/serial.h>
#include <xk/runtime.h>
#include <xk/disk.h>

// The whole disk without remapping (not named): the layout is read through it. IdexDiskLayoutKnown is set under
// IdexDiskLayoutLock.
static PDEVICE_OBJECT IdexDiskPhysical;
static KEVENT IdexDiskLayoutLock;
static volatile LONG IdexDiskLayoutKnown;

IDE_PARTITION IdexDiskPartitions[15];

// exFAT layout: a GPT or MBR disk whose first data partition holds exFAT, or exFAT on the whole disk with its FAT at
// sector 2048 or later.
BOOLEAN IdexDiskEXFATLayout;
PDEVICE_OBJECT IdexDiskEXFATVolume; // the exFAT partition (not named)

// A disk with a PC partition table (GPT or an MBR with an exFAT/NTFS entry) on which no exFAT volume was found: it gets
// no partitions and refuses writes, rather than being taken for a FATX disk whose fixed layout would write into it
// (the refurb record in sector 3 lands in the GPT's entry array).
BOOLEAN IdexDiskForeignLayout;

void IdexDiskLayoutInit() {
    IdexDiskCreateDevice(0x81, 0, (LONGLONG)IdexDiskLengthInSectors << 9, &IdexDiskPhysical, IDE_KIND_PHYSICAL);
    KeInitializeEvent(&IdexDiskLayoutLock, SynchronizationEvent, TRUE);
}

static ULONG IdexCrc32(const UCHAR *p, ULONG n) {
    ULONG crc = 0xFFFFFFFF;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xEDB88320 & (0 - (crc & 1)));
    }
    return ~crc;
}

// Layout reads: a failed read is tried twice more (10 ms apart), so one bad read at power-on cannot change the layout.
NTSTATUS IdexDiskReadPhysical(ULONGLONG Lba, PVOID Buffer, ULONG Length) {
    LARGE_INTEGER o;
    NTSTATUS status = STATUS_SUCCESS;
    for (ULONG attempt = 0; attempt < 3; attempt++) {
        if (attempt) {
            XK_SERIAL("disk: layout read of sector %llu failed (%#lx), retrying\n", Lba, status);
            KeStallExecutionProcessor(10000);
        }
        o.QuadPart = (LONGLONG)(Lba << 9);
        status = IoSynchronousFsdRequest(IRP_MJ_READ, IdexDiskPhysical, Buffer, Length, &o);
        if (NT_SUCCESS(status))
            break;
    }
    return status;
}

// The first basic-data partition of a GPT whose header (at Lba) and entry array check out. With Lenient, an entry array
// that fails its CRC is used too (the caller then insists on an exFAT boot sector at the partition's start).
static bool IdexDiskReadGpt(ULONGLONG Lba, PUCHAR Sector, ULONGLONG *Start, ULONGLONG *Last, bool Lenient = false) {
    static const UCHAR basicData[16] = {0xA2, 0xA0, 0xD0, 0xEB, 0xE5, 0xB9, 0x33, 0x44,
                                        0x87, 0xC0, 0x68, 0xB6, 0xB7, 0x26, 0x99, 0xC7};
    if (!NT_SUCCESS(IdexDiskReadPhysical(Lba, Sector, 512)) || RtlCompareMemory(Sector, "EFI PART", 8) != 8)
        return false;
    ULONG size = *(PULONG)(Sector + 12), crc = *(PULONG)(Sector + 16);
    if (size < 92 || size > 512)
        return false;
    *(PULONG)(Sector + 16) = 0;
    if (IdexCrc32(Sector, size) != crc)
        return false;
    ULONGLONG entriesLba = *(ULONGLONG *)(Sector + 72);
    ULONG count = *(PULONG)(Sector + 80), entrySize = *(PULONG)(Sector + 84), entriesCrc = *(PULONG)(Sector + 88);
    if (entrySize < 128 || entrySize > 512 || count == 0 || count * entrySize > 0x10000)
        return false;
    ULONG bytes = (count * entrySize + 511) & ~511u;
    auto entries = (PUCHAR)ExAllocatePoolWithTag(bytes, 'tpgI');
    if (!entries)
        return false;
    bool found = false;
    if (NT_SUCCESS(IdexDiskReadPhysical(entriesLba, entries, bytes)) &&
        (Lenient || IdexCrc32(entries, count * entrySize) == entriesCrc)) {
        for (ULONG i = 0; i < count && !found; i++) {
            PUCHAR e = entries + i * entrySize;
            if (RtlCompareMemory(e, basicData, 16) == 16) {
                *Start = *(ULONGLONG *)(e + 32);
                *Last = *(ULONGLONG *)(e + 40);
                found = *Last >= *Start;
            }
        }
    }
    ExFreePool(entries);
    return found;
}

// FATX partitions without a table (spec/cerbios/03 §3, Cerbios's default 5): the retail five; on a disk larger than the
// retail 8 GB one, F: from E:'s end up to sector 0x0FFFFFFF (or the disk's end) and G: from there to the disk's end,
// each capped at 0x74000000 sectors and used only when larger than 0x200000 sectors (1 GiB).
static void IdexDiskDefaultPartitions() {
    constexpr ULONGLONG RETAIL_END = 0xEE8AB0, LBA28_END = 0x0FFFFFFF, CAP = 0x74000000, MIN = 0x200000;
    IdexDiskPartitions[1] = {0x55F400, 0x9896B0}; // E:
    IdexDiskPartitions[2] = {0x465400, 0xFA000};  // C:
    for (ULONG i = 0; i < 3; i++)                 // X:, Y:, Z:
        IdexDiskPartitions[3 + i] = {0x400 + i * 0x177000, 0x177000};
    ULONGLONG n = IdexDiskLengthInSectors;
    if (n <= RETAIL_END)
        return;
    auto place = [&](ULONG number, ULONGLONG start, ULONGLONG end) {
        ULONGLONG length = end > start ? end - start : 0;
        if (length > CAP)
            length = CAP;
        if (length > MIN)
            IdexDiskPartitions[number] = {start, length};
        return start + length;
    };
    ULONGLONG fEnd = place(6, RETAIL_END, n < LBA28_END ? n : LBA28_END);
    place(7, fEnd, n);
}

// A PARTINFO table (spec/cerbios/03 §2) at sector 0, 8 sectors before the end or the last sector replaces the whole
// table.
static bool IdexDiskReadPartitionTable(PUCHAR Sector) {
    ULONGLONG n = IdexDiskLengthInSectors;
    const ULONGLONG places[] = {0, n - 8, n - 1};
    for (ULONGLONG lba : places) {
        if (!NT_SUCCESS(IdexDiskReadPhysical(lba, Sector, 512)) ||
            RtlCompareMemory(Sector, "****PARTINFO****", 16) != 16)
            continue;
        RtlZeroMemory(IdexDiskPartitions, sizeof(IdexDiskPartitions));
        for (ULONG i = 0; i < 14; i++) {
            PUCHAR e = Sector + 0x30 + 0x20 * i;
            ULONGLONG start = *(PULONG)(e + 0x14) | (ULONGLONG)*(PUSHORT)(e + 0x1C) << 32;
            ULONGLONG length = *(PULONG)(e + 0x18) | (ULONGLONG)*(PUSHORT)(e + 0x1E) << 32;
            if ((*(PULONG)(e + 0x10) & 0x80000000) && length && start < n && length <= n - start)
                IdexDiskPartitions[i + 1] = {start, length};
        }
        XK_SERIAL("disk: partition table at sector %llu\n", lba);
        return true;
    }
    return false;
}

// Reads the layout once, before the first partition is used.
void IdexDiskDetectLayout() {
    if (IdexDiskLayoutKnown)
        return;
    KeWaitForSingleObject(&IdexDiskLayoutLock, Executive, KernelMode, FALSE, nullptr);
    if (!IdexDiskLayoutKnown) {
        auto sector = (PUCHAR)ExAllocatePoolWithTag(512, 'tyLI');
        ULONGLONG start = 0, last = 0;
        bool found = false, pcTable = false;
        if (sector && NT_SUCCESS(IdexDiskReadPhysical(0, sector, 512)) && *(PUSHORT)(sector + 510) == 0xAA55 &&
            RtlCompareMemory(sector, "****PARTINFO****", 16) != 16) {
            if (sector[446 + 4] == 0xEE) { // protective MBR: GPT, the primary header, else the backup
                pcTable = true;
                const ULONGLONG headers[] = {1, IdexDiskLengthInSectors - 1};
                for (ULONG pass = 0; pass < 2 && !found; pass++) { // checked entry arrays first, then unchecked ones
                    for (ULONGLONG lba : headers) {
                        if (!found && IdexDiskReadGpt(lba, sector, &start, &last, pass == 1)) {
                            found = true;
                            if (pass == 1)
                                XK_SERIAL("disk: GPT entry array at header %llu fails its CRC; using it\n", lba);
                        }
                    }
                }
            } else {
                for (ULONG i = 0; i < 4 && !found; i++) {
                    PUCHAR e = sector + 446 + 16 * i;
                    if (e[4] == 0x07 && *(PULONG)(e + 12)) {
                        start = *(PULONG)(e + 8);
                        last = start + *(PULONG)(e + 12) - 1;
                        found = pcTable = true;
                    }
                }
            }
        }
        // The partition must start after the reserved-area copy, end inside the disk and hold exFAT.
        found = found && start >= 2048 && last < IdexDiskLengthInSectors &&
                NT_SUCCESS(IdexDiskReadPhysical(start, sector, 512)) &&
                RtlCompareMemory(sector + 3, "EXFAT   ", 8) == 8;
        // Else exFAT on the whole disk, no partition table (as a PC formats a bare disk): used when its FAT starts at
        // sector 2048 or later, so the reserved-area copy (1024-2047) sits in the unused space after the two 12-sector
        // boot regions. Boot sector: VolumeLength (sectors) at 72, FatOffset (sectors) at 80.
        if (!found && sector && NT_SUCCESS(IdexDiskReadPhysical(0, sector, 512)) &&
            RtlCompareMemory(sector + 3, "EXFAT   ", 8) == 8) {
            ULONGLONG length = *(ULONGLONG *)(sector + 72);
            ULONG fatOffset = *(PULONG)(sector + 80);
            XK_SERIAL("disk: exFAT on the whole disk: %#llx sectors, FAT at sector %#lx\n", length, fatOffset);
            start = 0;
            last = length - 1;
            found = fatOffset >= 2048 && length > fatOffset && length <= IdexDiskLengthInSectors;
        }
        if (found && NT_SUCCESS(IdexDiskCreateDevice(0x80, (LONGLONG)(start << 9), (LONGLONG)((last - start + 1) << 9),
                                                     &IdexDiskEXFATVolume, IDE_KIND_VOLUME)))
            IdexDiskEXFATLayout = TRUE;
        if (!IdexDiskEXFATLayout && pcTable) { // a PC disk without a usable exFAT volume: hands off
            IdexDiskForeignLayout = TRUE;
            XK_SERIAL("disk: PC partition table without a usable exFAT volume: no partitions, writes refused\n");
        } else if (!IdexDiskEXFATLayout) { // FATX: the disk's table, else the default layout
            if (!sector || !IdexDiskReadPartitionTable(sector))
                IdexDiskDefaultPartitions();
            for ([[maybe_unused]] ULONG i = 1; i < 15; i++) {
                if (IdexDiskPartitions[i].Length) {
                    XK_SERIAL("disk: partition %lu at sector %#llx, %#llx sectors\n", i, IdexDiskPartitions[i].Start,
                              IdexDiskPartitions[i].Length);
                }
            }
        }
        if (sector)
            ExFreePool(sector);
        IdexDiskLayoutKnown = 1;
    }
    KeSetEvent(&IdexDiskLayoutLock, 0, FALSE);
}
