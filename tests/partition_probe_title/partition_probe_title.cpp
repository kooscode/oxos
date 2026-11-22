// Partition probe title (spec/oxos_internal/45): asks the running kernel, through the ordinary disk device controls, for the disk's size and for the start
// and length of \Device\Harddisk0\Partition1 to Partition14, keeps the answers in PartitionProbe and then calls FinishProbe, where the observer
// (gdb on xemu's GDB server, or tests/fatx_layout_test.py) reads them. On partitions 6-14 it also reads oxos_probe.txt, and where that file is there (a test
// disk), writes oxos_probe_write.bin over several clusters and reads it back (spec 45 §9). Elsewhere it changes nothing on the disk; it works under any
// kernel.
#include <hal/xbox.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include <cstdio>
#include <cstdlib>
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
    NTSTATUS MarkerStatus[PROBE_PARTITIONS]; // partitions 6-14: reading oxos_probe.txt (0 for the others)
    char Marker[PROBE_PARTITIONS][16];       // its first 16 bytes
    NTSTATUS WriteStatus[PROBE_PARTITIONS];  // writing and reading back oxos_probe_write.bin (1: what was read back differs)
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

// Opens \Device\Harddisk0\PartitionN\Name for reading, or (Write) creates it anew.
NTSTATUS OpenFileOn(ULONG Number, const char *Name, bool Write, HANDLE *Handle) {
    char path[80];
    std::snprintf(path, sizeof(path), "\\Device\\Harddisk0\\Partition%lu\\%s", Number, Name);
    ANSI_STRING name;
    RtlInitAnsiString(&name, path);
    OBJECT_ATTRIBUTES attributes;
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
    IO_STATUS_BLOCK iosb;
    return NtCreateFile(Handle, (Write ? GENERIC_WRITE : 0) | GENERIC_READ | SYNCHRONIZE, &attributes, &iosb, nullptr, FILE_ATTRIBUTE_NORMAL, 0,
                        Write ? FILE_OVERWRITE_IF : FILE_OPEN, FILE_SYNCHRONOUS_IO_NONALERT | FILE_NON_DIRECTORY_FILE);
}

// The written file: 640 KB, more than two clusters of 256 KB; each 32-bit word holds its own offset plus the partition number.
constexpr ULONG WRITE_BYTES = 640 * 1024;

// Reads oxos_probe.txt on partition Number and, where it is there, writes oxos_probe_write.bin and reads it back.
void ProbeFiles(ULONG Number, ULONG *Buffer) {
    HANDLE handle;
    IO_STATUS_BLOCK iosb;
    NTSTATUS &marker = PartitionProbe.MarkerStatus[Number];
    marker = OpenFileOn(Number, "oxos_probe.txt", false, &handle);
    if (!NT_SUCCESS(marker)) {
        return;
    }
    marker = NtReadFile(handle, nullptr, nullptr, nullptr, &iosb, PartitionProbe.Marker[Number], sizeof(PartitionProbe.Marker[Number]), nullptr);
    NtClose(handle);
    NTSTATUS &written = PartitionProbe.WriteStatus[Number];
    for (ULONG i = 0; i < WRITE_BYTES / 4; i++) {
        Buffer[i] = i * 4 + Number;
    }
    written = OpenFileOn(Number, "oxos_probe_write.bin", true, &handle);
    if (NT_SUCCESS(written)) {
        written = NtWriteFile(handle, nullptr, nullptr, nullptr, &iosb, Buffer, WRITE_BYTES, nullptr);
        NtClose(handle);
    }
    if (NT_SUCCESS(written)) {
        written = OpenFileOn(Number, "oxos_probe_write.bin", false, &handle);
    }
    if (NT_SUCCESS(written)) {
        std::memset(Buffer, 0, WRITE_BYTES);
        written = NtReadFile(handle, nullptr, nullptr, nullptr, &iosb, Buffer, WRITE_BYTES, nullptr);
        NtClose(handle);
        for (ULONG i = 0; NT_SUCCESS(written) && i < WRITE_BYTES / 4; i++) {
            if (Buffer[i] != i * 4 + Number) {
                written = 1;
            }
        }
    }
}

} // namespace

int main() {
    for (ULONG n = 0; n < PROBE_PARTITIONS; n++) {
        ProbePartition(n);
    }
    auto buffer = static_cast<ULONG *>(std::malloc(WRITE_BYTES));
    for (ULONG n = 6; buffer && n < PROBE_PARTITIONS; n++) {
        ProbeFiles(n, buffer);
    }
    std::free(buffer);
    std::memcpy(PartitionProbe.Magic, "OXOSPRB1", 8);
    FinishProbe();
    for (;;) {
        Sleep(1000);
    }
}
