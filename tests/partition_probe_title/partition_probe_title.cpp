// Partition probe title (spec/oxos_internal/45): asks the running kernel, through the ordinary disk device controls, for the disk's size and for the start
// and length of \Device\Harddisk0\Partition1 to Partition14, keeps the answers in PartitionProbe and then calls FinishProbe, where the observer
// (gdb on xemu's GDB server) reads them. It changes nothing on the disk and works under any kernel.
#include <hal/xbox.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <cstdio>
#include <cstring>

namespace {

constexpr ULONG PROBE_PARTITIONS = 15; // Partition0 (the whole disk) to Partition14

// One partition's answer.
struct PROBE_ENTRY {
    NTSTATUS OpenStatus;      // NtOpenFile of the partition device
    NTSTATUS InfoStatus;      // IOCTL_DISK_GET_PARTITION_INFO
    ULONGLONG StartingOffset; // bytes from the disk's start
    ULONGLONG Length;         // bytes
};

} // namespace

// The answers, read by the observer at FinishProbe: a magic, the disk geometry and the fifteen partitions.
struct PROBE_RESULT {
    char Magic[8];                        // "OXOSPRB1"
    NTSTATUS GeometryStatus;              // IOCTL_DISK_GET_DRIVE_GEOMETRY on Partition0
    ULONGLONG Cylinders;                  // its cylinders
    ULONG TracksPerCylinder;              // its tracks per cylinder
    ULONG SectorsPerTrack;                // its sectors per track
    ULONG BytesPerSector;                 // its sector size
    PROBE_ENTRY Entry[PROBE_PARTITIONS];  // Partition0 to Partition14
};

extern "C" {
PROBE_RESULT PartitionProbe; // the answers (a C name, so the observer finds it by symbol)
}

// The observer's stop: called once every answer is in PartitionProbe.
extern "C" __attribute__((noinline)) void FinishProbe() {
    __asm__ volatile("" ::: "memory");
}

namespace {

// Opens \Device\Harddisk0\PartitionN for reading.
NTSTATUS OpenPartition(ULONG Number, HANDLE *Handle) {
    char path[48];
    std::snprintf(path, sizeof(path), "\\Device\\Harddisk0\\Partition%lu", Number);
    ANSI_STRING name;
    RtlInitAnsiString(&name, path);
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
    IO_STATUS_BLOCK iosb;
    return NtOpenFile(Handle, GENERIC_READ | SYNCHRONIZE, &attributes, &iosb, FILE_SHARE_READ | FILE_SHARE_WRITE, FILE_SYNCHRONOUS_IO_NONALERT);
}

// Asks one partition device for its place on the disk (and, for Partition0, the disk's geometry).
void ProbePartition(ULONG Number) {
    PROBE_ENTRY &entry = PartitionProbe.Entry[Number];
    HANDLE handle;
    entry.OpenStatus = OpenPartition(Number, &handle);
    if (!NT_SUCCESS(entry.OpenStatus)) {
        return;
    }
    IO_STATUS_BLOCK iosb;
    PARTITION_INFORMATION info = {};
    entry.InfoStatus = NtDeviceIoControlFile(handle, nullptr, nullptr, nullptr, &iosb, IOCTL_DISK_GET_PARTITION_INFO, nullptr, 0, &info, sizeof(info));
    if (NT_SUCCESS(entry.InfoStatus)) {
        entry.StartingOffset = info.StartingOffset.QuadPart;
        entry.Length = info.PartitionLength.QuadPart;
    }
    if (Number == 0) {
        DISK_GEOMETRY geometry = {};
        PartitionProbe.GeometryStatus =
            NtDeviceIoControlFile(handle, nullptr, nullptr, nullptr, &iosb, IOCTL_DISK_GET_DRIVE_GEOMETRY, nullptr, 0, &geometry, sizeof(geometry));
        PartitionProbe.Cylinders = geometry.Cylinders.QuadPart;
        PartitionProbe.TracksPerCylinder = geometry.TracksPerCylinder;
        PartitionProbe.SectorsPerTrack = geometry.SectorsPerTrack;
        PartitionProbe.BytesPerSector = geometry.BytesPerSector;
    }
    NtClose(handle);
}

} // namespace

int main() {
    for (ULONG n = 0; n < PROBE_PARTITIONS; n++) {
        ProbePartition(n);
    }
    std::memcpy(PartitionProbe.Magic, "OXOSPRB1", 8);
    FinishProbe();
    for (;;) {
        Sleep(1000);
    }
}
