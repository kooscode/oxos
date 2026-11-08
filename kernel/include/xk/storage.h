// Storage: the IDE channel, hard disk and DVD drivers (spec/stock/04) and virtual discs (spec/oxos_internal/16).
#pragma once
#include <xk/io.h>

typedef BOOLEAN(NTAPI *PIDE_INTERRUPT_ROUTINE)();
typedef void(NTAPI *PIDE_FINISHIO_ROUTINE)();
typedef BOOLEAN(NTAPI *PIDE_POLL_RESET_COMPLETE_ROUTINE)();
typedef void(NTAPI *PIDE_TIMEOUT_EXPIRED_ROUTINE)();
typedef void(NTAPI *PIDE_START_PACKET_ROUTINE)(PDEVICE_OBJECT DeviceObject, PIRP Irp);
typedef void(NTAPI *PIDE_START_NEXT_PACKET_ROUTINE)();

// Exported as IdexChannelObject (ordinal 357). 4-byte packed: the timer sits at 0x6C.
#pragma pack(push, 4)
typedef struct _IDE_CHANNEL_OBJECT {
    PIDE_INTERRUPT_ROUTINE InterruptRoutine;
    PIDE_FINISHIO_ROUTINE FinishIoRoutine;
    PIDE_POLL_RESET_COMPLETE_ROUTINE PollResetCompleteRoutine;
    PIDE_TIMEOUT_EXPIRED_ROUTINE TimeoutExpiredRoutine;
    PIDE_START_PACKET_ROUTINE StartPacketRoutine;
    PIDE_START_NEXT_PACKET_ROUTINE StartNextPacketRoutine;
    KIRQL InterruptIrql;
    BOOLEAN ExpectingBusMasterInterrupt;
    BOOLEAN StartPacketBusy;
    BOOLEAN StartPacketRequested;
    UCHAR Timeout;
    UCHAR IoRetries;
    UCHAR MaximumIoRetries;
    PIRP CurrentIrp;
    KDEVICE_QUEUE DeviceQueue;
    ULONG PhysicalRegionDescriptorTablePhysical;
    KDPC TimerDpc;
    KDPC FinishDpc;
    KTIMER Timer;
    KINTERRUPT InterruptObject;
} IDE_CHANNEL_OBJECT, *PIDE_CHANNEL_OBJECT;
#pragma pack(pop)
XK_OFFSET(IDE_CHANNEL_OBJECT, InterruptIrql, 0x18);
XK_OFFSET(IDE_CHANNEL_OBJECT, CurrentIrp, 0x20);
XK_OFFSET(IDE_CHANNEL_OBJECT, PhysicalRegionDescriptorTablePhysical, 0x30);
XK_OFFSET(IDE_CHANNEL_OBJECT, FinishDpc, 0x50);
XK_OFFSET(IDE_CHANNEL_OBJECT, Timer, 0x6C);
XK_OFFSET(IDE_CHANNEL_OBJECT, InterruptObject, 0x94);

XBAPI IDE_CHANNEL_OBJECT IdexChannelObject;

typedef struct _PARTITION_INFORMATION {
    LARGE_INTEGER StartingOffset;
    LARGE_INTEGER PartitionLength;
    ULONG HiddenSectors;
    ULONG PartitionNumber;
    UCHAR PartitionType;
    BOOLEAN BootIndicator;
    BOOLEAN RecognizedPartition;
    BOOLEAN RewritePartition;
} PARTITION_INFORMATION, *PPARTITION_INFORMATION;

#pragma pack(push, 4)

typedef struct _IDE_DISK_EXTENSION {
    PDEVICE_OBJECT DeviceObject;
    PARTITION_INFORMATION PartitionInformation;
} IDE_DISK_EXTENSION, *PIDE_DISK_EXTENSION;
#pragma pack(pop)
XK_SIZE(PARTITION_INFORMATION, 0x20);
XK_SIZE(IDE_DISK_EXTENSION, 0x24);

typedef struct _DISK_GEOMETRY {
    LARGE_INTEGER Cylinders;
    ULONG MediaType;
    ULONG TracksPerCylinder;
    ULONG SectorsPerTrack;
    ULONG BytesPerSector;
} DISK_GEOMETRY, *PDISK_GEOMETRY;
XK_SIZE(DISK_GEOMETRY, 0x18);

constexpr ULONG IOCTL_DISK_GET_DRIVE_GEOMETRY = 0x70000, IOCTL_DISK_VERIFY = 0x70014,
                IOCTL_DISK_GET_PARTITION_INFO = 0x74004, IOCTL_IDE_PASS_THROUGH = 0x4D028,
                IOCTL_CDROM_CHECK_VERIFY = 0x24800;
constexpr NTSTATUS STATUS_IO_DEVICE_ERROR = (NTSTATUS)0xC0000185, STATUS_IO_TIMEOUT = (NTSTATUS)0xC00000B5,
                   STATUS_DEVICE_NOT_READY = (NTSTATUS)0xC00000A3, STATUS_BUFFER_TOO_SMALL_IO = (NTSTATUS)0xC0000023;

// IOCTL_IDE_PASS_THROUGH buffer (input = output). On return the register bytes hold the drive's error (Features),
// count, LBA bytes and status (Command).
typedef struct _IDEREGS {
    UCHAR bFeaturesReg, bSectorCountReg, bSectorNumberReg, bCylLowReg, bCylHighReg, bDriveHeadReg, bCommandReg,
        bHostSendsData;
} IDEREGS;
typedef struct _ATA_PASS_THROUGH {
    IDEREGS IdeReg;
    ULONG DataBufferSize;
    PVOID DataBuffer;
} ATA_PASS_THROUGH;
XK_SIZE(ATA_PASS_THROUGH, 0x10);
constexpr NTSTATUS STATUS_REQUEST_ABORTED = (NTSTATUS)0xC0000240; // the drive stayed busy: the channel was reset

void IdexChannelReset();                        // software reset of both devices (ide_channel.cpp)
void IdexDiskComputePassword(PUCHAR Password); // the drive password from the IDENTIFY model and serial (disk.cpp)

// The disk (spec/stock/04 §4): partition devices, size, addressing, identity, security state, cache partitions.
constexpr ULONG IDE_MAX_PARTITIONS = 20; // Partition0 (the whole disk) to Partition19
XBAPI ULONGLONG IdexDiskLengthInSectors; // 64-bit: 48-bit drives (spec/oxos_internal/04_lba48.md)
XBAPI BOOLEAN IdexDiskLba48;             // the drive supports 48-bit addressing
XBAPI STRING HalDiskModelNumber, HalDiskSerialNumber;
XBAPI BOOLEAN IdexDiskSecurityUnlocked;
extern volatile UCHAR IdexDiskLastStatus; // the disk's status register at its last start-up step (diagnostics)
XBAPI ULONG HalDiskCachePartitionCount;

// exFAT layout (spec/oxos_internal/05_exfat_storage_spec.md): set before the first partition lookup.
extern BOOLEAN IdexDiskEXFATLayout;
extern PDEVICE_OBJECT IdexDiskEXFATVolume;    // the exFAT partition
PCSZ IdexDiskFolderOf(PDEVICE_OBJECT Device); // "E", "C", ... for a folder partition, else nullptr

// ATA task file.
constexpr USHORT IDE_DATA = 0x1F0, IDE_ERROR = 0x1F1, IDE_FEATURES = 0x1F1, IDE_COUNT = 0x1F2, IDE_LBA0 = 0x1F3,
                 IDE_LBA1 = 0x1F4, IDE_LBA2 = 0x1F5, IDE_DRIVE = 0x1F6, IDE_STATUS = 0x1F7, IDE_COMMAND = 0x1F7,
                 IDE_ALT_STATUS = 0x3F6, IDE_CONTROL = 0x3F6, IDE_BM_COMMAND = 0xFF60, IDE_BM_STATUS = 0xFF62,
                 IDE_BM_PRDT = 0xFF64;
constexpr UCHAR IDE_STATUS_ERR = 0x01, IDE_STATUS_DRQ = 0x08, IDE_STATUS_DF = 0x20, IDE_STATUS_DRDY = 0x40,
                IDE_STATUS_BSY = 0x80;

void IdexChannelInit();
void NTAPI IdexChannelStartPacket(PDEVICE_OBJECT DeviceObject, PIRP Irp);
void NTAPI IdexChannelStartNextPacket();
bool IdexWaitForStatus(UCHAR mask, UCHAR value, ULONG timeoutUs);
bool IdexProgramDma(PVOID Buffer, ULONG Length);
void IdexDiskInit();
void IdexCdRomInit();

// Shared between the IDE drivers and the virtual disc driver (ide_channel.cpp, dvd_drive.cpp, disk.cpp).
UCHAR IdexGetCompletionStatus(PUCHAR BusMasterStatus);
extern PDEVICE_OBJECT IdexCdRomDeviceObject;
void IdexCdRomEmulatedSecurityReply(const UCHAR *cdb, PUCHAR data, ULONG length, NTSTATUS *status, ULONG *got);
bool IdexCdRomIsSecurityCommand(const UCHAR *cdb, const UCHAR *data, ULONG length);
void IdexDiskCountPowerCycle();
[[noreturn]] void IdexFatalError(ULONG Code); // the fatal error screen (core/fatal.cpp)

// The virtual disc driver's hooks in the DVD driver (virtual_disc.cpp), and its restart: the disc images mounted
// before a quick reboot are mounted again (the title launcher calls it).
bool VdServeCdRom(PIRP Irp, NTSTATUS *Status);
bool VdDvdImageMounted();
void VdRestoreMounts();
void VdInitialize(); // start-up: the virtual disc control device (the IDE start-up calls it)
