// I/O manager (spec/stock/11).
#pragma once
#include <xk/ob.h>

typedef ULONG ACCESS_MASK;

typedef struct _IO_STATUS_BLOCK {
    union {
        NTSTATUS Status;
        PVOID Pointer;
    };
    ULONG_PTR Information;
} IO_STATUS_BLOCK, *PIO_STATUS_BLOCK;

typedef void(NTAPI *PIO_APC_ROUTINE)(PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, ULONG Reserved);

struct _DEVICE_OBJECT;
struct _IRP;
struct _FILE_OBJECT;
typedef NTSTATUS(NTAPI *PDRIVER_DISPATCH)(struct _DEVICE_OBJECT *DeviceObject, struct _IRP *Irp);
typedef void(NTAPI *PDRIVER_STARTIO)(struct _DEVICE_OBJECT *DeviceObject, struct _IRP *Irp);
typedef void(NTAPI *PDRIVER_DELETEDEVICE)(struct _DEVICE_OBJECT *DeviceObject);
typedef NTSTATUS(NTAPI *PDRIVER_DISMOUNTVOLUME)(struct _DEVICE_OBJECT *DeviceObject);
typedef NTSTATUS(NTAPI *PIO_COMPLETION_ROUTINE)(struct _DEVICE_OBJECT *DeviceObject, struct _IRP *Irp, PVOID Context);

constexpr ULONG FSCTL_DISMOUNT_VOLUME = 0x90020;
constexpr UCHAR IRP_MJ_CREATE = 0, IRP_MJ_CLOSE = 1, IRP_MJ_READ = 2, IRP_MJ_WRITE = 3,
                IRP_MJ_QUERY_INFORMATION = 4, IRP_MJ_SET_INFORMATION = 5, IRP_MJ_FLUSH_BUFFERS = 6,
                IRP_MJ_QUERY_VOLUME_INFORMATION = 7, IRP_MJ_DIRECTORY_CONTROL = 8, IRP_MJ_FILE_SYSTEM_CONTROL = 9,
                IRP_MJ_DEVICE_CONTROL = 0xA, IRP_MJ_INTERNAL_DEVICE_CONTROL = 0xB, IRP_MJ_SHUTDOWN = 0xC,
                IRP_MJ_CLEANUP = 0xD, IRP_MJ_MAXIMUM_FUNCTION = 0xD;

typedef struct _DRIVER_OBJECT {
    PDRIVER_STARTIO DriverStartIo;
    PDRIVER_DELETEDEVICE DriverDeleteDevice;
    PDRIVER_DISMOUNTVOLUME DriverDismountVolume;
    PDRIVER_DISPATCH MajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1];
} DRIVER_OBJECT, *PDRIVER_OBJECT;
XK_SIZE(DRIVER_OBJECT, 0x44);

constexpr ULONG DO_RAW_MOUNT_ONLY = 1, DO_EXCLUSIVE = 2, DO_DIRECT_IO = 4, DO_DEVICE_HAS_NAME = 8, DO_DEVICE_INITIALIZING = 0x10;
constexpr UCHAR DO_STARTIO_BUSY = 1, DO_STARTIO_REQUESTED = 2, DO_STARTIO_REQUESTED_BYKEY = 4;
constexpr UCHAR FILE_DEVICE_CD_ROM = 2, FILE_DEVICE_DISK = 7, FILE_DEVICE_UNKNOWN = 0x22,
                FILE_DEVICE_MEMORY_UNIT = 0x3A, FILE_DEVICE_MEDIA_BOARD = 0x3B;

typedef struct _DEVICE_OBJECT {
    CSHORT Type;
    USHORT Size;
    LONG ReferenceCount;
    PDRIVER_OBJECT DriverObject;
    struct _DEVICE_OBJECT *MountedOrSelfDevice;
    struct _IRP *CurrentIrp;
    ULONG Flags;
    PVOID DeviceExtension;
    UCHAR DeviceType;
    UCHAR StartIoFlags;
    CHAR StackSize;
    BOOLEAN DeletePending;
    ULONG SectorSize;
    ULONG AlignmentRequirement;
    KDEVICE_QUEUE DeviceQueue;
    KEVENT DeviceLock;
    ULONG StartIoKey;
} DEVICE_OBJECT, *PDEVICE_OBJECT;
XK_OFFSET(DEVICE_OBJECT, DeviceQueue, 0x28);
XK_OFFSET(DEVICE_OBJECT, DeviceLock, 0x34);
XK_SIZE(DEVICE_OBJECT, 0x48);

typedef struct _IO_COMPLETION_CONTEXT {
    PVOID Port;
    PVOID Key;
} IO_COMPLETION_CONTEXT, *PIO_COMPLETION_CONTEXT;

constexpr UCHAR FO_SYNCHRONOUS_IO = 1, FO_ALERTABLE_IO = 2, FO_NO_INTERMEDIATE_BUFFERING = 4,
                FO_SEQUENTIAL_ONLY = 8, FO_HANDLE_CREATED = 0x20, FO_RANDOM_ACCESS = 0x40;

#pragma pack(push, 4) // CurrentByteOffset sits at 0x14
typedef struct _FILE_OBJECT {
    CSHORT Type;
    UCHAR DeletePending : 1;
    UCHAR ReadAccess : 1;
    UCHAR WriteAccess : 1;
    UCHAR DeleteAccess : 1;
    UCHAR SharedRead : 1;
    UCHAR SharedWrite : 1;
    UCHAR SharedDelete : 1;
    UCHAR Reserved : 1;
    UCHAR Flags;
    PDEVICE_OBJECT DeviceObject;
    PVOID FsContext;
    PVOID FsContext2;
    NTSTATUS FinalStatus;
    LARGE_INTEGER CurrentByteOffset;
    struct _FILE_OBJECT *RelatedFileObject;
    PIO_COMPLETION_CONTEXT CompletionContext;
    LONG LockCount;
    KEVENT Lock;
    KEVENT Event;
} FILE_OBJECT, *PFILE_OBJECT;
#pragma pack(pop)
XK_OFFSET(FILE_OBJECT, CurrentByteOffset, 0x14);
XK_OFFSET(FILE_OBJECT, CompletionContext, 0x20);
XK_OFFSET(FILE_OBJECT, Event, 0x38);
XK_SIZE(FILE_OBJECT, 0x48);

typedef struct _SHARE_ACCESS {
    UCHAR OpenCount, Readers, Writers, Deleters, SharedRead, SharedWrite, SharedDelete;
} SHARE_ACCESS, *PSHARE_ACCESS;

typedef enum _FILE_INFORMATION_CLASS {
    FileDirectoryInformation = 1, FileFullDirectoryInformation, FileBothDirectoryInformation, FileBasicInformation,
    FileStandardInformation, FileInternalInformation, FileEaInformation, FileAccessInformation, FileNameInformation,
    FileRenameInformation, FileLinkInformation, FileNamesInformation, FileDispositionInformation,
    FilePositionInformation, FileFullEaInformation, FileModeInformation, FileAlignmentInformation,
    FileAllInformation, FileAllocationInformation, FileEndOfFileInformation, FileAlternateNameInformation,
    FileStreamInformation, FilePipeInformation, FilePipeLocalInformation, FilePipeRemoteInformation,
    FileMailslotQueryInformation, FileMailslotSetInformation, FileCompressionInformation, FileCopyOnWriteInformation,
    FileCompletionInformation, FileMoveClusterInformation, FileQuotaInformation, FileReparsePointInformation,
    FileNetworkOpenInformation, FileObjectIdInformation, FileTrackingInformation, FileMaximumInformation
} FILE_INFORMATION_CLASS;

typedef enum _FS_INFORMATION_CLASS {
    FileFsVolumeInformation = 1, FileFsLabelInformation, FileFsSizeInformation, FileFsDeviceInformation,
    FileFsAttributeInformation, FileFsControlInformation, FileFsFullSizeInformation, FileFsObjectIdInformation,
    FileFsMaximumInformation
} FS_INFORMATION_CLASS;

typedef union _FILE_SEGMENT_ELEMENT {
    PVOID Buffer;
    ULONG Alignment;
} FILE_SEGMENT_ELEMENT, *PFILE_SEGMENT_ELEMENT;

#pragma pack(push, 4) // Read.ByteOffset sits at +0xC
typedef struct _IO_STACK_LOCATION {
    UCHAR MajorFunction;
    UCHAR MinorFunction;
    UCHAR Flags;
    UCHAR Control;
    union {
        struct {
            ACCESS_MASK DesiredAccess;
            ULONG Options;
            USHORT FileAttributes;
            USHORT ShareAccess;
            POBJECT_STRING RemainingName;
        } Create;
        struct {
            ULONG Length;
            union {
                ULONG BufferOffset;
                PVOID CacheBuffer;
            };
            LARGE_INTEGER ByteOffset;
        } Read;
        struct {
            ULONG Length;
            union {
                ULONG BufferOffset;
                PVOID CacheBuffer;
            };
            LARGE_INTEGER ByteOffset;
        } Write;
        struct {
            ULONG Length;
            POBJECT_STRING FileName;
            FILE_INFORMATION_CLASS FileInformationClass;
        } QueryDirectory;
        struct {
            ULONG Length;
            FILE_INFORMATION_CLASS FileInformationClass;
        } QueryFile;
        struct {
            ULONG Length;
            FILE_INFORMATION_CLASS FileInformationClass;
            struct _FILE_OBJECT *FileObject;
        } SetFile;
        struct {
            ULONG Length;
            FS_INFORMATION_CLASS FsInformationClass;
        } QueryVolume;
        struct {
            ULONG OutputBufferLength;
            PVOID InputBuffer;
            ULONG InputBufferLength;
            ULONG IoControlCode;
        } DeviceIoControl;
        struct {
            ULONG OutputBufferLength;
            PVOID InputBuffer;
            ULONG InputBufferLength;
            ULONG FsControlCode;
        } FileSystemControl;
        struct {
            PVOID Argument1, Argument2, Argument3, Argument4;
        } Others;
    } Parameters;
    PDEVICE_OBJECT DeviceObject;
    struct _FILE_OBJECT *FileObject;
    PIO_COMPLETION_ROUTINE CompletionRoutine;
    PVOID Context;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;
#pragma pack(pop)
XK_OFFSET(IO_STACK_LOCATION, DeviceObject, 0x14);
XK_SIZE(IO_STACK_LOCATION, 0x24);

constexpr UCHAR SL_PENDING_RETURNED = 1, SL_INVOKE_ON_CANCEL = 0x20, SL_INVOKE_ON_SUCCESS = 0x40,
                SL_INVOKE_ON_ERROR = 0x80;
constexpr UCHAR SL_RESTART_SCAN = 1, SL_RETURN_SINGLE_ENTRY = 2, SL_INDEX_SPECIFIED = 4, SL_CASE_SENSITIVE = 0x80;

constexpr ULONG IRP_NOCACHE = 1, IRP_PAGING_IO = 2, IRP_SYNCHRONOUS_API = 4, IRP_CREATE_OPERATION = 8,
                IRP_READ_OPERATION = 0x10, IRP_WRITE_OPERATION = 0x20, IRP_CLOSE_OPERATION = 0x40,
                IRP_DEFER_IO_COMPLETION = 0x80, IRP_UNLOCK_USER_BUFFER = 0x200;

typedef struct _IRP {
    CSHORT Type;
    USHORT Size;
    ULONG Flags;
    LIST_ENTRY ThreadListEntry;
    IO_STATUS_BLOCK IoStatus;
    CHAR StackCount;
    CHAR CurrentLocation;
    BOOLEAN PendingReturned;
    BOOLEAN Cancel;
    PIO_STATUS_BLOCK UserIosb;
    PKEVENT UserEvent;
    union {
        struct {
            PIO_APC_ROUTINE UserApcRoutine;
            PVOID UserApcContext;
        } AsynchronousParameters;
        LARGE_INTEGER AllocationSize;
    } Overlay;
    PVOID UserBuffer;
    PFILE_SEGMENT_ELEMENT SegmentArray;
    ULONG LockedBufferLength;
    union {
        struct {
            union {
                KDEVICE_QUEUE_ENTRY DeviceQueueEntry;
                PVOID DriverContext[5];
            };
            PKTHREAD Thread;
            struct {
                LIST_ENTRY ListEntry;
                union {
                    struct _IO_STACK_LOCATION *CurrentStackLocation;
                    ULONG PacketType;
                };
            };
            struct _FILE_OBJECT *OriginalFileObject;
        } Overlay;
        KAPC Apc;
        PVOID CompletionKey;
    } Tail;
} IRP, *PIRP;
XK_OFFSET(IRP, Overlay, 0x28);
XK_OFFSET(IRP, Tail, 0x3C);
XK_OFFSET(IRP, Tail.Overlay.Thread, 0x50);
XK_OFFSET(IRP, Tail.Overlay.ListEntry, 0x54);
XK_OFFSET(IRP, Tail.Overlay.CurrentStackLocation, 0x5C);
XK_OFFSET(IRP, Tail.Overlay.OriginalFileObject, 0x60);
XK_SIZE(IRP, 0x68);

static inline PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP Irp) { return Irp->Tail.Overlay.CurrentStackLocation; }
static inline PIO_STACK_LOCATION IoGetNextIrpStackLocation(PIRP Irp) { return Irp->Tail.Overlay.CurrentStackLocation - 1; }
static inline void IoMarkIrpPending(PIRP Irp) { IoGetCurrentIrpStackLocation(Irp)->Control |= SL_PENDING_RETURNED; }
static inline void IoCopyCurrentIrpStackLocationToNext(PIRP Irp) {
    PIO_STACK_LOCATION cur = IoGetCurrentIrpStackLocation(Irp), next = cur - 1;
    __builtin_memcpy(next, cur, __builtin_offsetof(IO_STACK_LOCATION, CompletionRoutine));
    next->Control = 0;
}
static inline void IoSetCompletionRoutine(PIRP Irp, PIO_COMPLETION_ROUTINE Routine, PVOID Context, bool Success,
                                          bool Error, bool Cancel) {
    PIO_STACK_LOCATION next = IoGetNextIrpStackLocation(Irp);
    next->CompletionRoutine = Routine;
    next->Context = Context;
    next->Control = (Success ? SL_INVOKE_ON_SUCCESS : 0) | (Error ? SL_INVOKE_ON_ERROR : 0) |
                    (Cancel ? SL_INVOKE_ON_CANCEL : 0);
}

// Access rights, share modes, dispositions, create options.
constexpr ACCESS_MASK FILE_READ_DATA = 1, FILE_WRITE_DATA = 2, FILE_APPEND_DATA = 4, FILE_EXECUTE = 0x20,
                      FILE_READ_ATTRIBUTES = 0x80, FILE_WRITE_ATTRIBUTES = 0x100, DELETE = 0x10000,
                      SYNCHRONIZE = 0x100000, GENERIC_READ = 0x80000000, GENERIC_WRITE = 0x40000000;
constexpr ULONG FILE_SHARE_READ = 1, FILE_SHARE_WRITE = 2, FILE_SHARE_DELETE = 4;
constexpr ULONG FILE_SUPERSEDE = 0, FILE_OPEN = 1, FILE_CREATE = 2, FILE_OPEN_IF = 3, FILE_OVERWRITE = 4,
                FILE_OVERWRITE_IF = 5;
constexpr ULONG FILE_DIRECTORY_FILE = 1, FILE_WRITE_THROUGH = 2, FILE_SEQUENTIAL_ONLY = 4,
                FILE_NO_INTERMEDIATE_BUFFERING = 8, FILE_SYNCHRONOUS_IO_ALERT = 0x10,
                FILE_SYNCHRONOUS_IO_NONALERT = 0x20, FILE_NON_DIRECTORY_FILE = 0x40, FILE_RANDOM_ACCESS = 0x800,
                FILE_DELETE_ON_CLOSE = 0x1000;
constexpr ULONG IO_CHECK_CREATE_PARAMETERS = 0x200;
constexpr ULONG FILE_USE_FILE_POINTER_POSITION_HIGH = 0xFFFFFFFF, FILE_USE_FILE_POINTER_POSITION_LOW = 0xFFFFFFFE;

constexpr NTSTATUS STATUS_PENDING = 0x103, STATUS_INVALID_INFO_CLASS = (NTSTATUS)0xC0000003,
                   STATUS_INFO_LENGTH_MISMATCH = (NTSTATUS)0xC0000004, STATUS_INVALID_PARAMETER = (NTSTATUS)0xC000000D,
                   STATUS_NO_SUCH_DEVICE = (NTSTATUS)0xC000000E, STATUS_INVALID_DEVICE_REQUEST = (NTSTATUS)0xC0000010,
                   STATUS_END_OF_FILE = (NTSTATUS)0xC0000011, STATUS_MORE_PROCESSING_REQUIRED = (NTSTATUS)0xC0000016,
                   STATUS_ACCESS_DENIED = (NTSTATUS)0xC0000022, STATUS_SHARING_VIOLATION = (NTSTATUS)0xC0000043,
                   STATUS_INSUFFICIENT_RESOURCES = (NTSTATUS)0xC000009A,
                   STATUS_UNRECOGNIZED_VOLUME = (NTSTATUS)0xC000014F;

typedef struct _FILE_POSITION_INFORMATION {
    LARGE_INTEGER CurrentByteOffset;
} FILE_POSITION_INFORMATION;
typedef struct _FILE_MODE_INFORMATION {
    ULONG Mode;
} FILE_MODE_INFORMATION;
typedef struct _FILE_ALIGNMENT_INFORMATION {
    ULONG AlignmentRequirement;
} FILE_ALIGNMENT_INFORMATION;
typedef struct _FILE_COMPLETION_INFORMATION {
    HANDLE Port;
    PVOID Key;
} FILE_COMPLETION_INFORMATION;
typedef struct _FILE_DISPOSITION_INFORMATION {
    BOOLEAN DeleteFile;
} FILE_DISPOSITION_INFORMATION;
typedef struct _FILE_NETWORK_OPEN_INFORMATION {
    LARGE_INTEGER CreationTime, LastAccessTime, LastWriteTime, ChangeTime, AllocationSize, EndOfFile;
    ULONG FileAttributes;
} FILE_NETWORK_OPEN_INFORMATION, *PFILE_NETWORK_OPEN_INFORMATION;
typedef struct _FILE_IO_COMPLETION_INFORMATION {
    PVOID KeyContext;
    PVOID ApcContext;
    IO_STATUS_BLOCK IoStatusBlock;
} FILE_IO_COMPLETION_INFORMATION;

XBAPI OBJECT_TYPE IoDeviceObjectType, IoFileObjectType, IoCompletionObjectType;

XBAPI NTSTATUS NTAPI IoCreateDevice(PDRIVER_OBJECT DriverObject, ULONG DeviceExtensionSize, POBJECT_STRING DeviceName,
                                    ULONG DeviceType, BOOLEAN Exclusive, PDEVICE_OBJECT *DeviceObject);
XBAPI void NTAPI IoDeleteDevice(PDEVICE_OBJECT DeviceObject);
XBAPI NTSTATUS NTAPI IoCreateSymbolicLink(POBJECT_STRING SymbolicLinkName, POBJECT_STRING DeviceName);
XBAPI NTSTATUS NTAPI IoDeleteSymbolicLink(POBJECT_STRING SymbolicLinkName);
XBAPI PIRP NTAPI IoAllocateIrp(CCHAR StackSize);
XBAPI void NTAPI IoInitializeIrp(PIRP Irp, USHORT PacketSize, CCHAR StackSize);
XBAPI void NTAPI IoFreeIrp(PIRP Irp);
XBAPI NTSTATUS FASTCALL IofCallDriver(PDEVICE_OBJECT DeviceObject, PIRP Irp);
XBAPI void FASTCALL IofCompleteRequest(PIRP Irp, CCHAR PriorityBoost);
XBAPI PIRP NTAPI IoBuildAsynchronousFsdRequest(ULONG MajorFunction, PDEVICE_OBJECT DeviceObject, PVOID Buffer,
                                               ULONG Length, PLARGE_INTEGER StartingOffset,
                                               PIO_STATUS_BLOCK IoStatusBlock);
XBAPI PIRP NTAPI IoBuildSynchronousFsdRequest(ULONG MajorFunction, PDEVICE_OBJECT DeviceObject, PVOID Buffer,
                                              ULONG Length, PLARGE_INTEGER StartingOffset, PKEVENT Event,
                                              PIO_STATUS_BLOCK IoStatusBlock);
XBAPI PIRP NTAPI IoBuildDeviceIoControlRequest(ULONG IoControlCode, PDEVICE_OBJECT DeviceObject, PVOID InputBuffer,
                                               ULONG InputBufferLength, PVOID OutputBuffer, ULONG OutputBufferLength,
                                               BOOLEAN InternalDeviceIoControl, PKEVENT Event,
                                               PIO_STATUS_BLOCK IoStatusBlock);
XBAPI NTSTATUS NTAPI IoSynchronousFsdRequest(ULONG MajorFunction, PDEVICE_OBJECT DeviceObject, PVOID Buffer,
                                             ULONG Length, PLARGE_INTEGER StartingOffset);
NTSTATUS NTAPI IoFsControlDismountOnly(PDEVICE_OBJECT DeviceObject, PIRP Irp);
XBAPI NTSTATUS NTAPI IoSynchronousDeviceIoControlRequest(ULONG IoControlCode, PDEVICE_OBJECT DeviceObject,
                                                         PVOID InputBuffer, ULONG InputBufferLength,
                                                         PVOID OutputBuffer, ULONG OutputBufferLength,
                                                         PULONG ReturnedOutputBufferLength,
                                                         BOOLEAN InternalDeviceIoControl);
XBAPI void NTAPI IoStartPacket(PDEVICE_OBJECT DeviceObject, PIRP Irp, PULONG Key);
XBAPI void NTAPI IoStartNextPacket(PDEVICE_OBJECT DeviceObject);
XBAPI void NTAPI IoStartNextPacketByKey(PDEVICE_OBJECT DeviceObject, ULONG Key);
XBAPI NTSTATUS NTAPI IoCheckShareAccess(ACCESS_MASK DesiredAccess, ULONG DesiredShareAccess, PFILE_OBJECT FileObject,
                                        PSHARE_ACCESS ShareAccess, BOOLEAN Update);
XBAPI void NTAPI IoSetShareAccess(ACCESS_MASK DesiredAccess, ULONG DesiredShareAccess, PFILE_OBJECT FileObject,
                                  PSHARE_ACCESS ShareAccess);
XBAPI void NTAPI IoRemoveShareAccess(PFILE_OBJECT FileObject, PSHARE_ACCESS ShareAccess);
XBAPI NTSTATUS NTAPI IoInvalidDeviceRequest(PDEVICE_OBJECT DeviceObject, PIRP Irp);
XBAPI NTSTATUS NTAPI IoSetIoCompletion(PVOID IoCompletion, PVOID KeyContext, PVOID ApcContext, NTSTATUS IoStatus,
                                       ULONG_PTR IoStatusInformation);

XBAPI NTSTATUS NTAPI IoCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                                  PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
                                  ULONG ShareAccess, ULONG Disposition, ULONG CreateOptions, ULONG Options);
XBAPI NTSTATUS NTAPI NtCreateFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                                  PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER AllocationSize, ULONG FileAttributes,
                                  ULONG ShareAccess, ULONG CreateDisposition, ULONG CreateOptions);
XBAPI NTSTATUS NTAPI NtOpenFile(PHANDLE FileHandle, ACCESS_MASK DesiredAccess, POBJECT_ATTRIBUTES ObjectAttributes,
                                PIO_STATUS_BLOCK IoStatusBlock, ULONG ShareAccess, ULONG OpenOptions);
XBAPI NTSTATUS NTAPI NtReadFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                                PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
                                PLARGE_INTEGER ByteOffset);
XBAPI NTSTATUS NTAPI NtWriteFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                                 PIO_STATUS_BLOCK IoStatusBlock, PVOID Buffer, ULONG Length,
                                 PLARGE_INTEGER ByteOffset);
XBAPI NTSTATUS NTAPI NtDeviceIoControlFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
                                           PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, ULONG IoControlCode,
                                           PVOID InputBuffer, ULONG InputBufferLength, PVOID OutputBuffer,
                                           ULONG OutputBufferLength);
XBAPI NTSTATUS NTAPI NtFsControlFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine, PVOID ApcContext,
                                     PIO_STATUS_BLOCK IoStatusBlock, ULONG FsControlCode, PVOID InputBuffer,
                                     ULONG InputBufferLength, PVOID OutputBuffer, ULONG OutputBufferLength);
XBAPI NTSTATUS NTAPI NtQueryInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation,
                                            ULONG Length, FILE_INFORMATION_CLASS FileInformationClass);
XBAPI NTSTATUS NTAPI NtSetInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation,
                                          ULONG Length, FILE_INFORMATION_CLASS FileInformationClass);
XBAPI NTSTATUS NTAPI NtQueryVolumeInformationFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock,
                                                  PVOID FsInformation, ULONG Length,
                                                  FS_INFORMATION_CLASS FsInformationClass);
XBAPI NTSTATUS NTAPI NtQueryDirectoryFile(HANDLE FileHandle, HANDLE Event, PIO_APC_ROUTINE ApcRoutine,
                                          PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, PVOID FileInformation,
                                          ULONG Length, FILE_INFORMATION_CLASS FileInformationClass,
                                          POBJECT_STRING FileName, BOOLEAN RestartScan);
XBAPI NTSTATUS NTAPI NtFlushBuffersFile(HANDLE FileHandle, PIO_STATUS_BLOCK IoStatusBlock);
XBAPI NTSTATUS NTAPI NtCreateIoCompletion(PHANDLE IoCompletionHandle, ACCESS_MASK DesiredAccess,
                                          POBJECT_ATTRIBUTES ObjectAttributes, ULONG Count);
XBAPI NTSTATUS NTAPI NtSetIoCompletion(HANDLE IoCompletionHandle, PVOID KeyContext, PVOID ApcContext,
                                       NTSTATUS IoStatus, ULONG_PTR IoStatusInformation);
XBAPI NTSTATUS NTAPI NtRemoveIoCompletion(HANDLE IoCompletionHandle, PVOID *KeyContext, PVOID *ApcContext,
                                          PIO_STATUS_BLOCK IoStatusBlock, PLARGE_INTEGER Timeout);
XBAPI void NTAPI NtUserIoApcDispatcher(PVOID ApcContext, PIO_STATUS_BLOCK IoStatusBlock, ULONG Reserved);

// File-system registration for mounting (internal; FATX/XDVDFS/RAW arrive in milestone 3c).
typedef NTSTATUS (*IO_MOUNT_ROUTINE)(PDEVICE_OBJECT TargetDevice);
struct IO_FILE_SYSTEM {
    IO_MOUNT_ROUTINE Disk;  // FATX
    IO_MOUNT_ROUTINE CdRom; // XDVDFS (UDF is not in the stock retail path)
    IO_MOUNT_ROUTINE Raw;   // raw volume access when opening the device itself
};
extern IO_FILE_SYSTEM IopFileSystems;

void IoInitSystem();
void FsInitSystem(); // registers the file systems with the mount path
