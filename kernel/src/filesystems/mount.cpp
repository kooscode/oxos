// Which file system mounts a volume: the I/O manager asks these routines when a device is first opened.
#include <xk/filesystems.h>
#include <xk/storage.h>
#include <xk/hddfs.h>

// CD-ROM: XDVDFS first, then UDF (DVD-video) when XDVDFS does not recognise the media (spec/stock/04c §2,
// spec/stock/04d).
static NTSTATUS IopMountCdRom(PDEVICE_OBJECT TargetDevice) {
    NTSTATUS status = XdvdfsMountVolume(TargetDevice);
    return status == STATUS_UNRECOGNIZED_VOLUME ? UdfMountVolume(TargetDevice) : status;
}

// Disks: the folder partitions of an exFAT disk mount as exFAT views; every other partition as FATX.
static NTSTATUS IopMountDisk(PDEVICE_OBJECT TargetDevice) {
    PCSZ folder = IdexDiskFolderOf(TargetDevice);
    return folder ? EXFATMountVolume(TargetDevice, folder) : FATXMountVolume(TargetDevice);
}

void FsInitSystem() {
    IopFileSystems.Disk = IopMountDisk;
    IopFileSystems.CdRom = IopMountCdRom;
    IopFileSystems.Raw = RawMountVolume;
}
