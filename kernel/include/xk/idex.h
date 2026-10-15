// IDE channel, hard disk and DVD drivers (spec/04).
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
constexpr NTSTATUS STATUS_REQUEST_ABORTED = (NTSTATUS)0xC0000240;
void IdexChannelReset();
void IdexDiskComputePassword(PUCHAR Password);

// Fixed retail partition table (spec/04 §4); slot 0 is the whole disk.
constexpr ULONG IDE_MAX_PARTITIONS = 20;
XBAPI ULONG IdexDiskLengthInSectors;
XBAPI BOOLEAN IdexDiskSecurityUnlocked;
XBAPI STRING HalDiskModelNumber, HalDiskSerialNumber;
XBAPI ULONG HalDiskCachePartitionCount;

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
[[noreturn]] void IdexFatalError(ULONG Code);
