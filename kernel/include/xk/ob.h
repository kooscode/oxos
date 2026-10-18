// Object manager (spec/stock/10). Object names are ANSI counted strings on the Xbox.
#pragma once
#include <xk/ke.h>
#include <xk/mm.h>

typedef PVOID HANDLE, *PHANDLE;
typedef const char *PCSZ;
typedef WCHAR *PWSTR;
typedef const WCHAR *PCWSTR;

typedef struct _STRING {
    USHORT Length;
    USHORT MaximumLength;
    PCHAR Buffer;
} STRING, ANSI_STRING, OBJECT_STRING, *PSTRING, *PANSI_STRING, *POBJECT_STRING;

typedef struct _UNICODE_STRING {
    USHORT Length;
    USHORT MaximumLength;
    PWSTR Buffer;
} UNICODE_STRING, *PUNICODE_STRING;

typedef struct _OBJECT_ATTRIBUTES {
    HANDLE RootDirectory;
    POBJECT_STRING ObjectName;
    ULONG Attributes;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;

constexpr ULONG OBJ_INHERIT = 0x02, OBJ_PERMANENT = 0x10, OBJ_EXCLUSIVE = 0x20, OBJ_CASE_INSENSITIVE = 0x40,
                OBJ_OPENIF = 0x80;

#define NtCurrentThread() ((HANDLE)-2)
#define ObDosDevicesDirectory() ((HANDLE)-3)
#define ObWin32NamedObjectsDirectory() ((HANDLE)-4)

struct _OBJECT_TYPE;
typedef PVOID(NTAPI *OB_ALLOCATE_METHOD)(SIZE_T NumberOfBytes, ULONG Tag);
typedef void(NTAPI *OB_FREE_METHOD)(PVOID Pointer);
typedef void(NTAPI *OB_CLOSE_METHOD)(PVOID Object, ULONG SystemHandleCount);
typedef void(NTAPI *OB_DELETE_METHOD)(PVOID Object);
typedef NTSTATUS(NTAPI *OB_PARSE_METHOD)(PVOID ParseObject, struct _OBJECT_TYPE *ObjectType, ULONG Attributes,
                                         POBJECT_STRING CompleteName, POBJECT_STRING RemainingName, PVOID Context,
                                         PVOID *Object);

typedef struct _OBJECT_TYPE {
    OB_ALLOCATE_METHOD AllocateProcedure;
    OB_FREE_METHOD FreeProcedure;
    OB_CLOSE_METHOD CloseProcedure;
    OB_DELETE_METHOD DeleteProcedure;
    OB_PARSE_METHOD ParseProcedure;
    PVOID DefaultObject; // >= 0: offset of the waitable header inside the body; else an absolute object
    ULONG PoolTag;
} OBJECT_TYPE, *POBJECT_TYPE;
XK_SIZE(OBJECT_TYPE, 0x1C);

// 16 bytes in front of every object body.
typedef struct _OBJECT_HEADER {
    LONG PointerCount;
    LONG HandleCount;
    POBJECT_TYPE Type;
    ULONG Flags; // OB_FLAG_*
} OBJECT_HEADER, *POBJECT_HEADER;
XK_SIZE(OBJECT_HEADER, 0x10);

constexpr ULONG OB_FLAG_NAMED_OBJECT = 1, OB_FLAG_PERMANENT_OBJECT = 2, OB_FLAG_ATTACHED_OBJECT = 4;

// 16 more bytes in front of named objects.
typedef struct _OBJECT_HEADER_NAME_INFO {
    struct _OBJECT_HEADER_NAME_INFO *ChainLink;
    struct _OBJECT_DIRECTORY *Directory;
    OBJECT_STRING Name;
} OBJECT_HEADER_NAME_INFO, *POBJECT_HEADER_NAME_INFO;
XK_SIZE(OBJECT_HEADER_NAME_INFO, 0x10);

constexpr ULONG OB_NUMBER_HASH_BUCKETS = 11;
typedef struct _OBJECT_DIRECTORY {
    POBJECT_HEADER_NAME_INFO HashBuckets[OB_NUMBER_HASH_BUCKETS];
} OBJECT_DIRECTORY, *POBJECT_DIRECTORY;

typedef struct _OBJECT_SYMBOLIC_LINK {
    PVOID LinkTargetObject; // the target, or the device a path target continues into
    OBJECT_STRING LinkTarget;
    BOOLEAN PathLink; // (ours) the target names a path inside LinkTargetObject: lookups reparse LinkTarget
} OBJECT_SYMBOLIC_LINK, *POBJECT_SYMBOLIC_LINK;

typedef struct _OBJECT_DIRECTORY_INFORMATION {
    OBJECT_STRING Name;
    ULONG Type;
} OBJECT_DIRECTORY_INFORMATION, *POBJECT_DIRECTORY_INFORMATION;

typedef struct _OBJECT_HANDLE_TABLE {
    LONG HandleCount;
    LONG FirstFreeTableEntry;
    HANDLE NextHandleNeedingPool;
    PVOID **RootTable;
    PVOID *BuiltinRootTable[8];
} OBJECT_HANDLE_TABLE;

static inline POBJECT_HEADER OBJECT_TO_OBJECT_HEADER(PVOID o) { return (POBJECT_HEADER)o - 1; }
static inline POBJECT_HEADER_NAME_INFO OBJECT_HEADER_TO_NAME_INFO(POBJECT_HEADER h) {
    return (POBJECT_HEADER_NAME_INFO)h - 1;
}

XBAPI OBJECT_TYPE ObDirectoryObjectType, ObSymbolicLinkObjectType, PsThreadObjectType, ExEventObjectType,
    ExMutantObjectType, ExSemaphoreObjectType, ExTimerObjectType;
XBAPI OBJECT_HANDLE_TABLE ObpObjectHandleTable;

XBAPI NTSTATUS NTAPI ObCreateObject(POBJECT_TYPE ObjectType, POBJECT_ATTRIBUTES ObjectAttributes, ULONG ObjectBodySize,
                                    PVOID *Object);
XBAPI NTSTATUS NTAPI ObInsertObject(PVOID Object, POBJECT_ATTRIBUTES ObjectAttributes, ULONG ObjectPointerBias,
                                    PHANDLE Handle);
XBAPI void FASTCALL ObfReferenceObject(PVOID Object);
XBAPI void FASTCALL ObfDereferenceObject(PVOID Object);
XBAPI NTSTATUS NTAPI ObReferenceObjectByPointer(PVOID Object, POBJECT_TYPE ObjectType);
XBAPI NTSTATUS NTAPI ObReferenceObjectByHandle(HANDLE Handle, POBJECT_TYPE ObjectType, PVOID *ReturnedObject);
XBAPI NTSTATUS NTAPI ObReferenceObjectByName(POBJECT_STRING ObjectName, ULONG Attributes, POBJECT_TYPE ObjectType,
                                             PVOID ParseContext, PVOID *Object);
XBAPI NTSTATUS NTAPI ObOpenObjectByName(POBJECT_ATTRIBUTES ObjectAttributes, POBJECT_TYPE ObjectType,
                                        PVOID ParseContext, PHANDLE Handle);
XBAPI NTSTATUS NTAPI ObOpenObjectByPointer(PVOID Object, POBJECT_TYPE ObjectType, PHANDLE Handle);
XBAPI void NTAPI ObMakeTemporaryObject(PVOID Object);
XBAPI NTSTATUS NTAPI NtClose(HANDLE Handle);
XBAPI NTSTATUS NTAPI NtDuplicateObject(HANDLE SourceHandle, PHANDLE TargetHandle, ULONG Options);

void ObInitSystem();
NTSTATUS ObpCreateSymbolicLink(POBJECT_STRING LinkName, POBJECT_STRING TargetName);
// Waitable dispatcher header for an object (applies the type's DefaultObject rule).
PVOID ObpGetWaitableObject(PVOID Object);

constexpr NTSTATUS STATUS_OBJECT_NAME_EXISTS = 0x40000000, STATUS_INVALID_HANDLE = (NTSTATUS)0xC0000008,
                   STATUS_OBJECT_TYPE_MISMATCH = (NTSTATUS)0xC0000024,
                   STATUS_OBJECT_NAME_INVALID = (NTSTATUS)0xC0000033,
                   STATUS_OBJECT_NAME_NOT_FOUND = (NTSTATUS)0xC0000034,
                   STATUS_OBJECT_NAME_COLLISION = (NTSTATUS)0xC0000035,
                   STATUS_OBJECT_PATH_NOT_FOUND = (NTSTATUS)0xC000003A,
                   STATUS_INSUFFICIENT_RESOURCES_OB = (NTSTATUS)0xC000009A,
                   STATUS_NO_MORE_ENTRIES = (NTSTATUS)0x8000001A, STATUS_BUFFER_TOO_SMALL = (NTSTATUS)0xC0000023;

// Rtl strings
XBAPI void NTAPI RtlInitAnsiString(PANSI_STRING DestinationString, PCSZ SourceString);
XBAPI void NTAPI RtlInitUnicodeString(PUNICODE_STRING DestinationString, PCWSTR SourceString);
XBAPI BOOLEAN NTAPI RtlEqualString(const ANSI_STRING *String1, const ANSI_STRING *String2, BOOLEAN CaseInSensitive);
XBAPI LONG NTAPI RtlCompareString(const ANSI_STRING *String1, const ANSI_STRING *String2, BOOLEAN CaseInSensitive);
XBAPI CHAR NTAPI RtlUpperChar(CHAR Character);

// Executive objects and Ps (spec/stock/10 §5-6)
XBAPI NTSTATUS NTAPI NtCreateEvent(PHANDLE EventHandle, POBJECT_ATTRIBUTES ObjectAttributes, EVENT_TYPE EventType,
                                   BOOLEAN InitialState);
XBAPI NTSTATUS NTAPI NtSetEvent(HANDLE EventHandle, PLONG PreviousState);
XBAPI NTSTATUS NTAPI NtCreateSemaphore(PHANDLE SemaphoreHandle, POBJECT_ATTRIBUTES ObjectAttributes, LONG InitialCount,
                                       LONG MaximumCount);
XBAPI NTSTATUS NTAPI NtReleaseSemaphore(HANDLE SemaphoreHandle, LONG ReleaseCount, PLONG PreviousCount);
XBAPI NTSTATUS NTAPI NtCreateMutant(PHANDLE MutantHandle, POBJECT_ATTRIBUTES ObjectAttributes, BOOLEAN InitialOwner);
XBAPI NTSTATUS NTAPI NtWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable, PLARGE_INTEGER Timeout);
XBAPI NTSTATUS NTAPI NtWaitForSingleObjectEx(HANDLE Handle, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable,
                                             PLARGE_INTEGER Timeout);
XBAPI NTSTATUS NTAPI NtWaitForMultipleObjectsEx(ULONG Count, HANDLE Handles[], WAIT_TYPE WaitType,
                                                KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Timeout);
XBAPI NTSTATUS NTAPI NtCreateDirectoryObject(PHANDLE DirectoryHandle, POBJECT_ATTRIBUTES ObjectAttributes);
XBAPI NTSTATUS NTAPI NtQueryDirectoryObject(HANDLE DirectoryHandle, PVOID Buffer, ULONG Length, BOOLEAN RestartScan,
                                            PULONG Context, PULONG ReturnLength);
XBAPI NTSTATUS NTAPI NtOpenSymbolicLinkObject(PHANDLE LinkHandle, POBJECT_ATTRIBUTES ObjectAttributes);
XBAPI NTSTATUS NTAPI NtQuerySymbolicLinkObject(HANDLE LinkHandle, POBJECT_STRING LinkTarget, PULONG ReturnedLength);
typedef void(NTAPI *PPS_APC_ROUTINE)(PVOID ApcArgument1, PVOID ApcArgument2, PVOID ApcArgument3);
XBAPI NTSTATUS NTAPI NtQueueApcThread(HANDLE ThreadHandle, PPS_APC_ROUTINE ApcRoutine, PVOID ApcArgument1,
                                      PVOID ApcArgument2, PVOID ApcArgument3);
XBAPI NTSTATUS NTAPI NtSuspendThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount);
XBAPI NTSTATUS NTAPI NtResumeThread(HANDLE ThreadHandle, PULONG PreviousSuspendCount);
XBAPI NTSTATUS NTAPI PsCreateSystemThread(PHANDLE ThreadHandle, PHANDLE ThreadId, PVOID StartRoutine,
                                          PVOID StartContext, BOOLEAN DebuggerThread);
XBAPI NTSTATUS NTAPI PsCreateSystemThreadEx(PHANDLE ThreadHandle, ULONG ThreadExtensionSize, ULONG KernelStackSize,
                                            ULONG TlsDataSize, PHANDLE ThreadId, PVOID StartContext1,
                                            PVOID StartContext2, BOOLEAN CreateSuspended, BOOLEAN DebuggerThread,
                                            PKSYSTEM_ROUTINE SystemRoutine);
XBAPI void NTAPI PsTerminateSystemThread(NTSTATUS ExitStatus);
typedef void(NTAPI *PCREATE_THREAD_NOTIFY_ROUTINE)(PVOID Thread, HANDLE ThreadId, BOOLEAN Create);
XBAPI NTSTATUS NTAPI PsSetCreateThreadNotifyRoutine(PCREATE_THREAD_NOTIFY_ROUTINE NotifyRoutine);
