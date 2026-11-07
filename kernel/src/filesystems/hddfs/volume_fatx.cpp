// FATX volumes: reading the boot sector and the FAT size at mount (spec/stock/04 §6), and counting the free
// clusters.
#include <xk/hddfs.h>
#include <xk/serial.h>
#include <xk/io.h>

static void FATXInitFcb(HDDFS_FCB *f, ULONG Flags) {
    RtlZeroMemory(f, sizeof(*f));
    InitializeListHead(&f->SiblingLink);
    InitializeListHead(&f->ChildList);
    f->Flags = Flags;
    f->ReferenceCount = 1; // permanent
}

NTSTATUS FATXMountVolume(PDEVICE_OBJECT TargetDevice) {
    DISK_GEOMETRY geometry;
    PARTITION_INFORMATION partition;
    NTSTATUS status = IoSynchronousDeviceIoControlRequest(IOCTL_DISK_GET_DRIVE_GEOMETRY, TargetDevice, nullptr, 0,
                                                          &geometry, sizeof(geometry), nullptr, FALSE);
    if (NT_SUCCESS(status)) {
        status = IoSynchronousDeviceIoControlRequest(IOCTL_DISK_GET_PARTITION_INFO, TargetDevice, nullptr, 0,
                                                     &partition, sizeof(partition), nullptr, FALSE);
    }
    if (!NT_SUCCESS(status))
        return status;

    PDEVICE_OBJECT dev;
    status = IoCreateDevice(&HDDFSDriverObject, sizeof(HDDFS_VOLUME), nullptr, 8 /*FILE_DEVICE_DISK_FILE_SYSTEM*/,
                            FALSE, &dev);
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
        bool spcOk = spc && spc <= 128 && !(spc & (spc - 1));
        ULONG sectorShift = 0;
        while ((1u << sectorShift) < v->SectorSize)
            sectorShift++;
        v->BytesPerCluster = spcOk ? spc << sectorShift : 0;
        while (spcOk && (1u << v->ClusterShift) < v->BytesPerCluster)
            v->ClusterShift++;
        if (spcOk && v->BytesPerCluster >= align) {
            ULONG entries = (ULONG)(v->PartitionLength >> v->ClusterShift) + 1;
            v->FatEntryShift = entries < 0xFFF0 ? 1 : 2;
            ULONG fatSize = ((entries << v->FatEntryShift) + align - 1) & ~(align - 1);
            if ((LONGLONG)fatSize < v->PartitionLength - reserved) {
                v->FatByteOffset = reserved;
                v->DataByteOffset = reserved + fatSize;
                v->NumberOfClusters = (ULONG)((v->PartitionLength - reserved - fatSize) >> v->ClusterShift);
                v->RootFcb.FirstCluster = root;
                if (HDDFSIsValidCluster(v, root))
                    status = STATUS_SUCCESS;
            }
        }
    }
    // Count free clusters (FileFsSizeInformation) and find the first free one.
    [[maybe_unused]] ULONG scanStart = KeTickCount;
    if (NT_SUCCESS(status)) {
        XK_SERIAL("fatx: mount %#llx bytes, cluster %#lx, %#lx clusters, FAT%u\n", v->PartitionLength,
                  v->BytesPerCluster, v->NumberOfClusters, 8u << v->FatEntryShift);
    }
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
    if (NT_SUCCESS(status))
        XK_SERIAL("fatx: %#lx free clusters counted in %lu ms\n", v->Alloc->FreeClusters, KeTickCount - scanStart);
    if (!NT_SUCCESS(status)) {
        HDDFSForgetVolume(v);
        IoDeleteDevice(dev);
        return STATUS_UNRECOGNIZED_VOLUME;
    }
    dev->Flags &= ~DO_DEVICE_INITIALIZING;
    TargetDevice->MountedOrSelfDevice = dev;
    return STATUS_SUCCESS;
}
