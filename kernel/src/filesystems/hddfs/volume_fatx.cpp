// FATX volumes: reading the boot sector and the FAT size at mount (spec/stock/04 §6), and counting the free clusters.
#include <filesystems/hddfs/hddfs.h>
#include <diagnostics/diagnostics.h>
#include <io/io.h>

namespace xk::filesystems {

static void FATXInitFcb(HDDFS_FCB *f, ULONG Flags) {
    memset(f, 0, sizeof(*f));
    InitializeListHead(&f->SiblingLink);
    InitializeListHead(&f->ChildList);
    f->Flags = Flags;
    f->ReferenceCount = 1; // permanent
}

NTSTATUS FATXMountVolume(PDEVICE_OBJECT TargetDevice) {
    DISK_GEOMETRY geometry;
    PARTITION_INFORMATION partition;
    NTSTATUS status = IoSynchronousDeviceIoControlRequest(IOCTL_DISK_GET_DRIVE_GEOMETRY, TargetDevice, nullptr, 0, &geometry, sizeof(geometry), nullptr, FALSE);
    if (NT_SUCCESS(status)) {
        status = IoSynchronousDeviceIoControlRequest(IOCTL_DISK_GET_PARTITION_INFO, TargetDevice, nullptr, 0, &partition, sizeof(partition), nullptr, FALSE);
    }
    if (!NT_SUCCESS(status))
        return status;

    PDEVICE_OBJECT dev;
    status = IoCreateDevice(&HDDFSDriverObject, sizeof(HDDFS_VOLUME), nullptr, 8 /*FILE_DEVICE_DISK_FILE_SYSTEM*/, FALSE, &dev);
    if (!NT_SUCCESS(status))
        return status;
    dev->StackSize = (CHAR)(dev->StackSize + TargetDevice->StackSize);
    if (dev->AlignmentRequirement < TargetDevice->AlignmentRequirement)
        dev->AlignmentRequirement = TargetDevice->AlignmentRequirement;
    dev->Flags |= TargetDevice->Flags & 0x40;
    dev->SectorSize = geometry.BytesPerSector;
    HDDFS_VOLUME *v = HDDFSVolume(dev);
    v->TargetDevice = v->MountTarget = TargetDevice;
    ObfReferenceObject(TargetDevice); // released by the delete routine (HDDFSDeleteVolume), also when the mount fails
    v->FirstClusterNumber = 1;
    v->Format = &FATXFormat;
    v->Alloc = &v->OwnAlloc;
    v->PartitionLength = partition.PartitionLength.QuadPart;
    v->SectorSize = geometry.BytesPerSector;
    v->CacheExtension = {TargetDevice, partition.PartitionLength, geometry.BytesPerSector};
    v->Cache = &v->CacheExtension;
    FATXInitFcb(&v->RootFcb, FCB_DIRECTORY | FCB_ROOT);
    FATXInitFcb(&v->VolumeFcb, FCB_VOLUME);
    v->RootFcb.Attributes = FILE_ATTRIBUTE_DIRECTORY;

    // Reserved area / FAT alignment: max(4 KB, sector size rounded up to 4 KB).
    ULONG align = (v->SectorSize + 0xFFF) & ~0xFFFu;
    ULONG reserved = align > 0x1000 ? align : 0x1000;
    status = STATUS_UNRECOGNIZED_VOLUME;
    PUCHAR boot;
    HDDFSAcquireVolume(v); // the block cache is shared
    if (v->PartitionLength > reserved && NT_SUCCESS(HDDFSMapBlock(v, 0, &boot)) &&
        *(ULONG *)boot == FATX_SIGNATURE) {
        ULONG spc = *(ULONG *)(boot + 8), root = *(ULONG *)(boot + 0xC);
        v->SerialNumber = *(ULONG *)(boot + 4);
        // Any power of two up to 32 MB clusters, as the retail arithmetic allows: the retail kernel stops at 128 sectors (spec/stock/04 §6), disk formatters
        // for disks over 2 TB write up to 2 MB (spec/oxos_internal/45 §8.5).
        ULONG sectorShift = 0;
        while ((1u << sectorShift) < v->SectorSize)
            sectorShift++;
        bool spcOk = spc && !(spc & (spc - 1)) && spc <= (0x2000000u >> sectorShift);
        v->BytesPerCluster = spcOk ? spc << sectorShift : 0;
        while (spcOk && (1u << v->ClusterShift) < v->BytesPerCluster)
            v->ClusterShift++;
        if (spcOk && v->BytesPerCluster >= align) {
            // The cluster count as 32 bits, as the reference takes it (spec/oxos_internal/45 §8.5); the FAT's size and the data in 64 bits.
            ULONG entries = (ULONG)(v->PartitionLength >> v->ClusterShift) + 1;
            v->FatEntryShift = entries < 0xFFF0 ? 1 : 2;
            ULONGLONG fatSize = (((ULONGLONG)entries << v->FatEntryShift) + align - 1) & ~(ULONGLONG)(align - 1);
            if ((LONGLONG)fatSize < v->PartitionLength - reserved) {
                ULONGLONG clusters = (ULONGLONG)(v->PartitionLength - reserved - (LONGLONG)fatSize) >> v->ClusterShift;
                v->FatByteOffset = reserved;
                v->DataByteOffset = reserved + (ULONG)fatSize;
                v->NumberOfClusters = (ULONG)clusters;
                v->RootFcb.FirstCluster = root;
                if (clusters <= 0x0FFFFFFF && HDDFSIsValidCluster(v, root)) // at most 0x0FFFFFFF clusters (spec/oxos_internal/45 §8.5)
                    status = STATUS_SUCCESS;
            }
        }
    }
    // Count free clusters (FileFsSizeInformation) and find the first free one.
    for (ULONG c = 1; NT_SUCCESS(status) && c <= v->NumberOfClusters; c++) {
        ULONG e;
        status = HDDFSGetFatEntry(v, c, &e);
        if (NT_SUCCESS(status) && e == HDDFS_CLUSTER_FREE) {
            if (!v->Alloc->FreeClusters)
                v->Alloc->AllocationHint = c;
            v->Alloc->FreeClusters++;
        }
    }
    HDDFSReleaseVolume(v);
    if (!NT_SUCCESS(status)) {
        HDDFSForgetVolume(v);
        IoDeleteDevice(dev);
        return STATUS_UNRECOGNIZED_VOLUME;
    }
    dev->Flags &= ~DO_DEVICE_INITIALIZING;
    TargetDevice->MountedOrSelfDevice = dev;
    return STATUS_SUCCESS;
}

} // namespace xk::filesystems
