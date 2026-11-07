// exFAT volumes: the folder partitions of an exFAT disk (spec/oxos_internal/07_exfat_driver.md,
// 05_exfat_storage_spec.md). Each folder partition mounts as its own volume whose root is that folder; they share one
// EXFAT_DISK (geometry, up-case table, free count, block cache key). Here: the allocation bitmap, the volume handle
// and size, and the mount.
#include <xk/hddfs.h>
#include <xk/serial.h>

EXFAT_DISK EXFATDisk;
// The exFAT disk's one key in the file system cache, shared by all its folder volumes (HDDFS_VOLUME::Cache).
static FSCACHE_EXTENSION EXFATDiskCache;

// --- Allocation bitmap

static NTSTATUS EXFATBitmapByte(HDDFS_VOLUME *v, ULONG Cluster, LONGLONG *Block, PUCHAR *Byte) {
    LONGLONG at = v->EXFATDisk->BitmapByteOffset + ((Cluster - 2) >> 3);
    *Block = at & ~(LONGLONG)(HDDFS_BLOCK_SIZE - 1);
    PUCHAR data;
    NTSTATUS status = HDDFSMapBlock(v, *Block, &data);
    if (NT_SUCCESS(status))
        *Byte = data + (ULONG)(at - *Block);
    return status;
}

bool EXFATClusterFree(HDDFS_VOLUME *v, ULONG Cluster, NTSTATUS *Status) {
    LONGLONG block;
    PUCHAR byte;
    *Status = EXFATBitmapByte(v, Cluster, &block, &byte);
    return NT_SUCCESS(*Status) && !(*byte & (1 << ((Cluster - 2) & 7)));
}

NTSTATUS EXFATMarkCluster(HDDFS_VOLUME *v, ULONG Cluster, bool Used) {
    LONGLONG block;
    PUCHAR byte, base;
    NTSTATUS status = EXFATBitmapByte(v, Cluster, &block, &byte);
    if (NT_SUCCESS(status))
        status = HDDFSMapBlock(v, block, &base);
    if (!NT_SUCCESS(status))
        return status;
    UCHAR bit = (UCHAR)(1 << ((Cluster - 2) & 7));
    *byte = Used ? *byte | bit : *byte & ~bit;
    return HDDFSWriteBlockRange(v, block, (ULONG)(byte - base), 1);
}

NTSTATUS EXFATZeroCluster(HDDFS_VOLUME *v, ULONG Cluster) {
    LONGLONG base = HDDFSClusterOffset(v, Cluster);
    NTSTATUS status = STATUS_SUCCESS;
    for (ULONG off = 0; NT_SUCCESS(status) && off < v->BytesPerCluster; off += HDDFS_BLOCK_SIZE) {
        PUCHAR data;
        status = HDDFSMapBlock(v, base + off, &data);
        if (NT_SUCCESS(status)) {
            RtlZeroMemory(data, HDDFS_BLOCK_SIZE);
            status = HDDFSWriteBlockRange(v, base + off, 0, HDDFS_BLOCK_SIZE);
        }
    }
    return status;
}

// A NoFatChain allocation gets its FAT entries, so it can grow or shrink out of place.
NTSTATUS EXFATMakeChain(HDDFS_VOLUME *v, HDDFS_FCB *f) {
    if (!(f->EXFATFlags & EXFAT_NOFATCHAIN))
        return STATUS_SUCCESS;
    for (ULONG i = 0; i < f->EXFATAllocClusters; i++) {
        ULONG c = f->FirstCluster + i;
        NTSTATUS status = HDDFSSetFatEntry(v, c, i + 1 < f->EXFATAllocClusters ? c + 1 : HDDFS_CLUSTER_LAST);
        if (!NT_SUCCESS(status))
            return status;
    }
    f->EXFATFlags &= ~EXFAT_NOFATCHAIN;
    f->HintCluster = 0;
    f->Flags |= FCB_DIRENT_DIRTY;
    return STATUS_SUCCESS;
}

NTSTATUS EXFATFreeFile(HDDFS_VOLUME *v, HDDFS_FCB *f) {
    if (!f->FirstCluster)
        return STATUS_SUCCESS;
    if (!(f->EXFATFlags & EXFAT_NOFATCHAIN))
        return HDDFSFreeChain(v, f->FirstCluster, f->EXFATAllocClusters);
    for (ULONG i = 0; i < f->EXFATAllocClusters; i++) {
        NTSTATUS status = EXFATMarkCluster(v, f->FirstCluster + i, false);
        if (!NT_SUCCESS(status))
            return status;
        v->Alloc->FreeClusters++;
        v->UsedClusters--;
    }
    if (f->FirstCluster < v->Alloc->AllocationHint)
        v->Alloc->AllocationHint = f->FirstCluster;
    return STATUS_SUCCESS;
}

// --- Volume handle and size (spec §5-6 of the storage spec)

// A title formats its cache partition (X/Y/Z) through the volume handle: a FATX header written at offset 0, then the
// allocation table and the root directory, then a dismount. A folder partition has no sectors of its own, so the
// header write empties the folder and the metadata writes are dropped; reads show a freshly formatted FATX volume.
// E:, C: and F: refuse to be formatted this way.

// The serial and sectors per cluster of the last header written to X/Y/Z, shown by later mounts and kept across
// quick reboots (a title formats, starts another title or the dashboard, and is started again).
XK_PERSIST static ULONG EXFATFormatSerial[7];
XK_PERSIST static ULONG EXFATFormatSpc[7];

// The FATX cluster size a folder partition shows: the one its last format chose, else 16 KB.
static ULONG EXFATFormattedClusterBytes(HDDFS_VOLUME *v) {
    ULONG spc = EXFATFormatSpc[v->PartitionNumber];
    return (spc ? spc : 32) * 512;
}

// The layout of a freshly formatted FATX volume of the reported length (as FATX mounts it: a 4 KB header, the
// allocation table rounded to 4 KB, then the clusters, the first holding the root directory).
static void EXFATFormattedLayout(HDDFS_VOLUME *v, ULONG *EntrySize, ULONG *RootAt, ULONG *Clusters = nullptr) {
    ULONG bytes = EXFATFormattedClusterBytes(v);
    ULONG entries = (ULONG)(v->ReportedLength / bytes) + 1;
    *EntrySize = entries < 0xFFF0 ? 2 : 4;
    *RootAt = 0x1000 + ((entries * *EntrySize + 0xFFF) & ~0xFFFu);
    if (Clusters)
        *Clusters = (ULONG)((v->ReportedLength - *RootAt) / bytes);
}

NTSTATUS EXFATVolumeTransfer(HDDFS_VOLUME *v, PIRP Irp) {
    PIO_STACK_LOCATION s = IoGetCurrentIrpStackLocation(Irp);
    ULONG length = s->Parameters.Read.Length;
    LONGLONG offset = s->Parameters.Read.ByteOffset.QuadPart;
    auto buffer = (PUCHAR)Irp->UserBuffer;
    ULONG entrySize, rootAt;
    EXFATFormattedLayout(v, &entrySize, &rootAt);
    LONGLONG metaEnd = rootAt + EXFATFormattedClusterBytes(v);
    NTSTATUS status = STATUS_SUCCESS;
    HDDFSAcquireVolume(v);
    if (offset < 0 || offset + length > v->ReportedLength) {
        status = STATUS_INVALID_PARAMETER;
    } else if (s->MajorFunction == IRP_MJ_READ) {
        for (ULONG i = 0; i < length; i++) {
            LONGLONG at = offset + i;
            UCHAR b = 0;
            if (at < 16) {
                ULONG header[4] = {FATX_SIGNATURE,
                                   EXFATFormatSerial[v->PartitionNumber] ? EXFATFormatSerial[v->PartitionNumber]
                                                                         : v->SerialNumber,
                                   EXFATFormattedClusterBytes(v) / 512, 1};
                b = ((PUCHAR)header)[at];
            } else if (at >= 0x1000 && at < 0x1000 + 2 * entrySize) {
                b = (at - 0x1000) == 0 ? 0xF8 : 0xFF; // entry 0 (media), entry 1 (the root's end of chain)
            } else if (at >= rootAt && at < metaEnd) {
                b = 0xFF; // empty root directory
            }
            buffer[i] = b;
        }
    } else if (offset == 0 && length >= 16 && *(PULONG)buffer == FATX_SIGNATURE) {
        if (!EXFATIsCacheFolder(v) || !IsListEmpty(&v->RootFcb.ChildList)) {
            status = STATUS_ACCESS_DENIED;
        } else {
            ULONG used = 0;
            status = EXFATWalk(v, &v->RootFcb, true, 0, &used);
            if (NT_SUCCESS(status))
                status = EXFATCountFolder(v);
            if (NT_SUCCESS(status)) {
                EXFATFormatSerial[v->PartitionNumber] = v->SerialNumber = ((PULONG)buffer)[1];
                ULONG spc = ((PULONG)buffer)[2]; // a FATX mount accepts powers of two from 4 KB to 64 KB clusters
                EXFATFormatSpc[v->PartitionNumber] = (spc >= 8 && spc <= 128 && !(spc & (spc - 1))) ? spc : 0;
            }
        }
    } else if (offset + length > metaEnd || !EXFATIsCacheFolder(v)) {
        status = STATUS_ACCESS_DENIED; // only the format's own metadata writes are expected
    }
    HDDFSReleaseVolume(v);
    Irp->IoStatus.Status = status;
    Irp->IoStatus.Information = NT_SUCCESS(status) ? length : 0;
    IofCompleteRequest(Irp, 0);
    return status;
}

// E: and C: in 16 KB units, the volume capped at 2 TB; F: everything. X/Y/Z as a FATX volume of their retail 750 MB
// with the cluster size of their last format (16 KB until formatted): its clusters, less the root directory's and
// those the folder's files take (the exFAT folder's own directory clusters are not counted).
void EXFATVolumeSize(HDDFS_VOLUME *v, ULONG *Total, ULONG *Free, ULONG *SectorsPerUnit) {
    constexpr ULONGLONG unit = 0x4000, cap = 2ull << 40;
    ULONGLONG total = (ULONGLONG)v->EXFATDisk->ClusterCount << v->ClusterShift;
    ULONGLONG free = (ULONGLONG)v->Alloc->FreeClusters << v->ClusterShift;
    if (EXFATIsCacheFolder(v)) {
        ULONG bytes = EXFATFormattedClusterBytes(v), entrySize, rootAt, clusters;
        EXFATFormattedLayout(v, &entrySize, &rootAt, &clusters);
        ULONG content = v->UsedClusters > v->OwnClusters ? v->UsedClusters - v->OwnClusters : 0;
        ULONGLONG used = (((ULONGLONG)content << v->ClusterShift) + bytes - 1) / bytes + 1;
        ULONGLONG avail = free / bytes;
        *Total = clusters;
        *Free = (ULONG)(used >= clusters ? 0 : clusters - used < avail ? clusters - used : avail);
        *SectorsPerUnit = bytes / v->SectorSize;
        return;
    } else if (v->PartitionNumber <= 2 && total > cap) {
        total = cap;
    }
    if (free > total)
        free = total;
    *Total = total / unit > 0xFFFFFFFF ? 0xFFFFFFFF : (ULONG)(total / unit);
    *Free = free / unit > 0xFFFFFFFF ? 0xFFFFFFFF : (ULONG)(free / unit);
    *SectorsPerUnit = unit / v->SectorSize;
}

// --- Mount

static void EXFATInitFcb(HDDFS_FCB *f, ULONG Flags) {
    RtlZeroMemory(f, sizeof(*f));
    InitializeListHead(&f->SiblingLink);
    InitializeListHead(&f->ChildList);
    f->Flags = Flags;
    f->ReferenceCount = 1; // permanent
}

// A boot region (12 sectors at Base: the main one at 0, the backup at sector 12) whose boot sector is exFAT's and
// whose checksum (sector 11) is right: its boot sector copied to Boot.
static bool EXFATReadBootRegion(HDDFS_VOLUME *v, ULONG Base, PUCHAR Boot, NTSTATUS *Status) {
    ULONG sum = 0;
    for (ULONG i = 0; i < 12 * 512; i++) {
        PUCHAR data;
        *Status = HDDFSMapBlock(v, (Base + i) & ~(HDDFS_BLOCK_SIZE - 1), &data);
        if (!NT_SUCCESS(*Status))
            return false;
        data += (Base + i) & (HDDFS_BLOCK_SIZE - 1);
        if (i < 512)
            Boot[i] = *data;
        if (i < 11 * 512 && i != 106 && i != 107 && i != 112)
            sum = ((sum >> 1) | (sum << 31)) + *data;
        else if (i >= 11 * 512 && data[0] != (UCHAR)(sum >> ((i & 3) * 8)))
            return false; // the checksum sector repeats the sum
    }
    return RtlCompareMemory(Boot + 3, "EXFAT   ", 8) == 8 && *(USHORT *)(Boot + 510) == 0xAA55;
}

// Read the boot sector, the root's bitmap and up-case entries, and count the free clusters (the lock is held).
static NTSTATUS EXFATMountDisk(HDDFS_VOLUME *v) {
    EXFAT_DISK *x = v->EXFATDisk;
    // The main boot region, else the backup: a kernel that takes a whole-disk exFAT drive for a FATX disk writes its
    // refurbishment record into sector 3 (spec/oxos_internal/05 §2).
    UCHAR b[512];
    NTSTATUS status;
    if (!EXFATReadBootRegion(v, 0, b, &status)) {
        if (!NT_SUCCESS(status))
            return status;
        XK_SERIAL("exfat: the main boot region is damaged; using the backup\n");
        if (!EXFATReadBootRegion(v, 12 * 512, b, &status))
            return NT_SUCCESS(status) ? STATUS_UNRECOGNIZED_VOLUME : status;
        // Repair the main region from the backup, as fsck.exfat does, so a PC mounts the disk again. The backup is
        // never written: an interrupted repair leaves a main region that fails its checksum and is repaired again.
        for (ULONG sector = 0; sector < 12 && NT_SUCCESS(status); sector++) {
            UCHAR copy[512];
            PUCHAR data;
            status = HDDFSMapBlock(v, (12 + sector) * 512, &data);
            if (!NT_SUCCESS(status))
                break;
            RtlCopyMemory(copy, data, 512);
            status = HDDFSMapBlock(v, sector * 512, &data);
            if (!NT_SUCCESS(status))
                break;
            RtlCopyMemory(data, copy, 512);
            status = HDDFSWriteBlockRange(v, sector * 512 & ~(HDDFS_BLOCK_SIZE - 1),
                                          sector * 512 & (HDDFS_BLOCK_SIZE - 1), 512);
        }
        XK_SERIAL("exfat: main boot region restored from the backup: %#lx\n", status);
        status = STATUS_SUCCESS; // a failed repair still mounts from the backup
    }
    UCHAR sectorShift = b[108], clusterShift = (UCHAR)(b[108] + b[109]);
    if (sectorShift != 9 || clusterShift < 12 || clusterShift > 25)
        return STATUS_UNRECOGNIZED_VOLUME;
    ULONG fatOffset = *(ULONG *)(b + 80), fatLength = *(ULONG *)(b + 84), heap = *(ULONG *)(b + 88);
    ULONG count = *(ULONG *)(b + 92), root = *(ULONG *)(b + 96), serial = *(ULONG *)(b + 100);
    USHORT flags = *(USHORT *)(b + 106);
    UCHAR fats = b[110];
    if (!count || (fats != 1 && fats != 2) ||
        ((ULONGLONG)heap << 9) + ((ULONGLONG)count << clusterShift) > (ULONGLONG)v->PartitionLength)
        return STATUS_UNRECOGNIZED_VOLUME;
    x->ClusterShift = clusterShift;
    x->BytesPerCluster = 1u << clusterShift;
    x->ClusterCount = count;
    x->FatByteOffset = (fatOffset + (fats == 2 && (flags & 1) ? fatLength : 0)) << 9;
    x->HeapByteOffset = (LONGLONG)heap << 9;
    x->Serial = serial;
    v->ClusterShift = clusterShift;
    v->BytesPerCluster = x->BytesPerCluster;
    v->NumberOfClusters = count;
    v->FatByteOffset = x->FatByteOffset;
    v->FatEntryShift = 2;
    v->DataByteOffset = x->HeapByteOffset;
    for (ULONG i = 0; i < 256; i++)
        x->Upcase[i] = (USHORT)i;
    EXFATInitFcb(&x->Root, FCB_DIRECTORY | FCB_ROOT | FCB_EXFAT_DISK_ROOT);
    x->Root.Attributes = FILE_ATTRIBUTE_DIRECTORY;
    x->Root.FirstCluster = root;
    if (!HDDFSIsValidCluster(v, root))
        return STATUS_UNRECOGNIZED_VOLUME;

    // The root's allocation bitmap (its clusters must be consecutive) and up-case table.
    ULONG bitmapFirst = 0, upcaseFirst = 0;
    ULONGLONG bitmapLength = 0, upcaseLength = 0;
    for (ULONG slot = 0;; slot++) {
        PUCHAR e;
        LONGLONG block;
        status = EXFATMapSlot(v, &x->Root, slot, &e, &block);
        if (status == STATUS_END_OF_FILE || (NT_SUCCESS(status) && e[0] == EXFAT_ENTRY_END))
            break;
        if (!NT_SUCCESS(status))
            return status;
        if (e[0] == EXFAT_ENTRY_BITMAP && !bitmapFirst) {
            bitmapFirst = *(ULONG *)(e + 20);
            bitmapLength = *(ULONGLONG *)(e + 24);
        } else if (e[0] == EXFAT_ENTRY_UPCASE) {
            upcaseFirst = *(ULONG *)(e + 20);
            upcaseLength = *(ULONGLONG *)(e + 24);
        }
    }
    if (!HDDFSIsValidCluster(v, bitmapFirst) || bitmapLength < (count + 7) / 8)
        return STATUS_UNRECOGNIZED_VOLUME;
    ULONG bitmapClusters = (ULONG)((bitmapLength + x->BytesPerCluster - 1) >> clusterShift);
    for (ULONG i = 0, c = bitmapFirst; i + 1 < bitmapClusters; i++, c++) {
        ULONG next;
        status = HDDFSGetFatEntry(v, c, &next);
        if (!NT_SUCCESS(status) || next != c + 1)
            return STATUS_UNRECOGNIZED_VOLUME;
    }
    x->BitmapByteOffset = HDDFSClusterOffset(v, bitmapFirst);
    if (HDDFSIsValidCluster(v, upcaseFirst)) {
        // The mapping of characters 0-255 (the compressed form: 0xFFFF n = n characters map to themselves).
        HDDFS_FCB table;
        EXFATInitFcb(&table, 0);
        table.FirstCluster = upcaseFirst;
        // Its chain's length.
        table.EXFATAllocClusters = (ULONG)((upcaseLength + x->BytesPerCluster - 1) >> clusterShift);
        ULONG ch = 0;
        for (ULONG at = 0; at + 1 < upcaseLength && ch < 256; at += 2) {
            ULONG cluster;
            status = HDDFSClusterAtIndex(v, &table, at >> clusterShift, &cluster);
            if (!NT_SUCCESS(status))
                break;
            LONGLONG pos = HDDFSClusterOffset(v, cluster) + (at & (x->BytesPerCluster - 1));
            PUCHAR data;
            status = HDDFSMapBlock(v, pos & ~(LONGLONG)(HDDFS_BLOCK_SIZE - 1), &data);
            if (!NT_SUCCESS(status))
                break;
            USHORT value = *(USHORT *)(data + (pos & (HDDFS_BLOCK_SIZE - 1)));
            if (value == 0xFFFF && at + 3 < upcaseLength) {
                at += 2;
                pos += 2;
                status = HDDFSMapBlock(v, pos & ~(LONGLONG)(HDDFS_BLOCK_SIZE - 1), &data);
                if (!NT_SUCCESS(status))
                    break;
                ch += *(USHORT *)(data + (pos & (HDDFS_BLOCK_SIZE - 1)));
            } else {
                x->Upcase[ch++] = value;
            }
        }
        status = STATUS_SUCCESS;
    }

    // Free clusters, and the first free one.
    x->Alloc.FreeClusters = 0;
    x->Alloc.AllocationHint = 0;
    for (ULONG done = 0; done < count;) {
        LONGLONG at = x->BitmapByteOffset + (done >> 3);
        PUCHAR data;
        status = HDDFSMapBlock(v, at, &data);
        if (!NT_SUCCESS(status))
            return status;
        // A dword (32 clusters) at a time: the bitmap of a large volume is megabytes long.
        for (ULONG i = 0; i < HDDFS_BLOCK_SIZE / 4 && done < count; i++) {
            ULONG word = ((PULONG)data)[i], n = count - done < 32 ? count - done : 32;
            if (n < 32)
                word |= ~0u << n; // past the last cluster: not free
            if (word != ~0u) {
                if (!x->Alloc.FreeClusters)
                    x->Alloc.AllocationHint = done + 2 + __builtin_ctz(~word);
                x->Alloc.FreeClusters += 32 - __builtin_popcount(word);
            }
            done += n;
        }
    }
    x->Mounted = TRUE;
    return STATUS_SUCCESS;
}

NTSTATUS EXFATMountVolume(PDEVICE_OBJECT Target, PCSZ Folder) {
    PDEVICE_OBJECT disk = IdexDiskEXFATVolume;
    PARTITION_INFORMATION volume, target;
    NTSTATUS status = IoSynchronousDeviceIoControlRequest(IOCTL_DISK_GET_PARTITION_INFO, disk, nullptr, 0, &volume,
                                                          sizeof(volume), nullptr, FALSE);
    if (NT_SUCCESS(status)) {
        status = IoSynchronousDeviceIoControlRequest(IOCTL_DISK_GET_PARTITION_INFO, Target, nullptr, 0, &target,
                                                     sizeof(target), nullptr, FALSE);
    }
    if (!NT_SUCCESS(status))
        return status;
    PDEVICE_OBJECT dev;
    status = IoCreateDevice(&HDDFSDriverObject, sizeof(HDDFS_VOLUME), nullptr, 8 /*FILE_DEVICE_DISK_FILE_SYSTEM*/,
                            FALSE, &dev);
    if (!NT_SUCCESS(status))
        return status;
    dev->StackSize = (CHAR)(dev->StackSize +
                            (disk->StackSize > Target->StackSize ? disk->StackSize : Target->StackSize));
    if (dev->AlignmentRequirement < disk->AlignmentRequirement)
        dev->AlignmentRequirement = disk->AlignmentRequirement;
    dev->Flags |= disk->Flags & 0x40;
    dev->SectorSize = 512;
    HDDFS_VOLUME *v = HDDFSVolume(dev);
    v->TargetDevice = disk;
    v->MountTarget = Target;
    ObfReferenceObject(Target); // released by the delete routine (HDDFSDeleteVolume), also when the mount fails
    v->IsEXFAT = TRUE;
    v->Format = &EXFATFormat;
    v->FirstClusterNumber = 2;
    v->EXFATDisk = &EXFATDisk;
    v->Alloc = &EXFATDisk.Alloc;
    v->PartitionNumber = target.PartitionNumber;
    v->PartitionLength = volume.PartitionLength.QuadPart;
    v->ReportedLength = target.PartitionLength.QuadPart;
    v->SectorSize = 512;
    v->CacheExtension = {disk, volume.PartitionLength, 512};
    EXFATDiskCache = v->CacheExtension;
    v->Cache = &EXFATDiskCache;
    EXFATInitFcb(&v->RootFcb, FCB_DIRECTORY | FCB_ROOT);
    EXFATInitFcb(&v->VolumeFcb, FCB_VOLUME);
    v->RootFcb.Attributes = FILE_ATTRIBUTE_DIRECTORY;

    HDDFSAcquireVolume(v);
    status = EXFATDisk.Mounted ? STATUS_SUCCESS : EXFATMountDisk(v);
    if (NT_SUCCESS(status)) {
        v->ClusterShift = EXFATDisk.ClusterShift;
        v->BytesPerCluster = EXFATDisk.BytesPerCluster;
        v->NumberOfClusters = EXFATDisk.ClusterCount;
        v->FatByteOffset = EXFATDisk.FatByteOffset;
        v->FatEntryShift = 2;
        v->DataByteOffset = EXFATDisk.HeapByteOffset;
        v->SerialNumber = EXFATDisk.Serial;
        // The folder: found in the volume's root, or created there.
        ULONG length = 0;
        while (Folder[length])
            length++;
        OBJECT_STRING name = {(USHORT)length, (USHORT)length, (PCHAR)Folder};
        HDDFS_ENTRY e;
        ULONG index;
        status = EXFATLookup(v, &EXFATDisk.Root, &name, &e, &index);
        if (status == STATUS_OBJECT_NAME_NOT_FOUND) {
            ULONG first = 0;
            status = EXFATInitDirectory(v, &first);
            if (NT_SUCCESS(status))
                status = EXFATCreateDirent(v, &EXFATDisk.Root, &name, FILE_ATTRIBUTE_DIRECTORY, first, &index);
            if (NT_SUCCESS(status))
                status = EXFATReadEntry(v, &EXFATDisk.Root, index, &e);
        }
        if (NT_SUCCESS(status) && !(e.FileAttributes & FILE_ATTRIBUTE_DIRECTORY))
            status = STATUS_UNRECOGNIZED_VOLUME;
        if (NT_SUCCESS(status)) {
            HDDFS_FCB *r = &v->RootFcb;
            r->FirstCluster = e.FirstCluster;
            r->EXFATFlags = e.EXFATFlags;
            r->EXFATEntries = e.EXFATEntries;
            r->EXFATAllocClusters = e.EXFATAllocClusters;
            r->EXFATDirLength = e.EXFATDirLength;
            r->DirentIndex = index;
            r->NameLength = e.FileNameLength;
            RtlCopyMemory(r->Name, e.FileName, e.FileNameLength);
            r->CreationTime = e.CreationTime;
            r->LastWriteTime = e.LastWriteTime;
            r->LastAccessTime = e.LastAccessTime;
            if (EXFATFormatSerial[v->PartitionNumber])
                v->SerialNumber = EXFATFormatSerial[v->PartitionNumber];
            if (EXFATIsCacheFolder(v))
                status = EXFATCountFolder(v);
        }
    }
    HDDFSReleaseVolume(v);
    if (!NT_SUCCESS(status)) {
        XK_SERIAL("exfat: mounting folder %s failed: %#lx\n", Folder, status);
        HDDFSForgetVolume(v);
        IoDeleteDevice(dev);
        return STATUS_UNRECOGNIZED_VOLUME;
    }
    dev->Flags &= ~DO_DEVICE_INITIALIZING;
    Target->MountedOrSelfDevice = dev;
    return STATUS_SUCCESS;
}
