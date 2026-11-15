// Object manager (spec/stock/10). Object names are ANSI counted strings on the Xbox.
#pragma once
#include <core/core.h>
#include <memory/memory.h>
#include <runtime/runtime.h>

typedef struct _OBJECT_ATTRIBUTES {
    HANDLE RootDirectory;
    POBJECT_STRING ObjectName;
    ULONG Attributes;
} OBJECT_ATTRIBUTES, *POBJECT_ATTRIBUTES;

constexpr ULONG OBJ_INHERIT = 0x02, OBJ_PERMANENT = 0x10, OBJ_EXCLUSIVE = 0x20, OBJ_CASE_INSENSITIVE = 0x40, OBJ_OPENIF = 0x80;

#define NtCurrentThread() ((HANDLE)-2)
#define ObDosDevicesDirectory() ((HANDLE)-3)
#define ObWin32NamedObjectsDirectory() ((HANDLE)-4)

struct _OBJECT_TYPE;
typedef PVOID(NTAPI *OB_ALLOCATE_METHOD)(SIZE_T NumberOfBytes, ULONG Tag);
typedef void(NTAPI *OB_FREE_METHOD)(PVOID Pointer);
typedef void(NTAPI *OB_CLOSE_METHOD)(PVOID Object, ULONG SystemHandleCount);
typedef void(NTAPI *OB_DELETE_METHOD)(PVOID Object);
typedef NTSTATUS(NTAPI *OB_PARSE_METHOD)(PVOID ParseObject, struct _OBJECT_TYPE *ObjectType, ULONG Attributes, POBJECT_STRING CompleteName,
                                         POBJECT_STRING RemainingName, PVOID Context, PVOID *Object);

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

static inline POBJECT_HEADER OBJECT_TO_OBJECT_HEADER(PVOID o) {
    return (POBJECT_HEADER)o - 1;
}

static inline POBJECT_HEADER_NAME_INFO OBJECT_HEADER_TO_NAME_INFO(POBJECT_HEADER h) {
    return (POBJECT_HEADER_NAME_INFO)h - 1;
}

// The object types and the handle table (exported).
XBAPI OBJECT_TYPE ObDirectoryObjectType, ObSymbolicLinkObjectType, PsThreadObjectType, ExEventObjectType, ExMutantObjectType, ExSemaphoreObjectType,
    ExTimerObjectType;
XBAPI OBJECT_HANDLE_TABLE ObpObjectHandleTable;

XBAPI NTSTATUS NTAPI ObCreateObject(POBJECT_TYPE ObjectType, POBJECT_ATTRIBUTES ObjectAttributes, ULONG ObjectBodySize, PVOID *Object);
XBAPI NTSTATUS NTAPI ObInsertObject(PVOID Object, POBJECT_ATTRIBUTES ObjectAttributes, ULONG ObjectPointerBias, PHANDLE Handle);
XBAPI void FASTCALL ObfReferenceObject(PVOID Object);
XBAPI void FASTCALL ObfDereferenceObject(PVOID Object);
XBAPI NTSTATUS NTAPI ObReferenceObjectByPointer(PVOID Object, POBJECT_TYPE ObjectType);
XBAPI NTSTATUS NTAPI ObReferenceObjectByHandle(HANDLE Handle, POBJECT_TYPE ObjectType, PVOID *ReturnedObject);
XBAPI NTSTATUS NTAPI ObReferenceObjectByName(POBJECT_STRING ObjectName, ULONG Attributes, POBJECT_TYPE ObjectType, PVOID ParseContext, PVOID *Object);
XBAPI NTSTATUS NTAPI ObOpenObjectByName(POBJECT_ATTRIBUTES ObjectAttributes, POBJECT_TYPE ObjectType, PVOID ParseContext, PHANDLE Handle);
XBAPI NTSTATUS NTAPI ObOpenObjectByPointer(PVOID Object, POBJECT_TYPE ObjectType, PHANDLE Handle);
XBAPI void NTAPI ObMakeTemporaryObject(PVOID Object);
XBAPI NTSTATUS NTAPI NtClose(HANDLE Handle);
XBAPI NTSTATUS NTAPI NtDuplicateObject(HANDLE SourceHandle, PHANDLE TargetHandle, ULONG Options);

namespace xk::objects {

void InitObjectManager();
NTSTATUS CreateSymbolicLink(POBJECT_STRING LinkName, POBJECT_STRING TargetName);

// Names as the object manager keeps them (a length and the text, no 0 needed), made from a C string; OXOS's own code passes C strings to the functions below.
inline OBJECT_STRING ObjectName(const char *Text) {
    USHORT length = (USHORT)strlen(Text);
    return {length, (USHORT)(length + 1), (PCHAR)Text};
}
POBJECT_DIRECTORY CreateDirectory(const char *Path); // a permanent object directory (\Device, \Device\Virtual0), or null; object_manager.cpp
// Waitable dispatcher header for an object (applies the type's DefaultObject rule).
PVOID GetWaitableObject(PVOID Object);

} // namespace xk::objects

constexpr NTSTATUS STATUS_OBJECT_NAME_EXISTS = 0x40000000, STATUS_INVALID_HANDLE = (NTSTATUS)0xC0000008, STATUS_OBJECT_TYPE_MISMATCH = (NTSTATUS)0xC0000024,
                   STATUS_OBJECT_NAME_INVALID = (NTSTATUS)0xC0000033, STATUS_OBJECT_NAME_NOT_FOUND = (NTSTATUS)0xC0000034,
                   STATUS_OBJECT_NAME_COLLISION = (NTSTATUS)0xC0000035, STATUS_OBJECT_PATH_NOT_FOUND = (NTSTATUS)0xC000003A,
                   STATUS_INSUFFICIENT_RESOURCES_OB = (NTSTATUS)0xC000009A, STATUS_NO_MORE_ENTRIES = (NTSTATUS)0x8000001A,
                   STATUS_BUFFER_TOO_SMALL = (NTSTATUS)0xC0000023;

// Executive objects and Ps (spec/stock/10 §5-6)
XBAPI NTSTATUS NTAPI NtCreateEvent(PHANDLE EventHandle, POBJECT_ATTRIBUTES ObjectAttributes, EVENT_TYPE EventType, BOOLEAN InitialState);
XBAPI NTSTATUS NTAPI NtSetEvent(HANDLE EventHandle, PLONG PreviousState);
XBAPI NTSTATUS NTAPI NtCreateSemaphore(PHANDLE SemaphoreHandle, POBJECT_ATTRIBUTES ObjectAttributes, LONG InitialCount, LONG MaximumCount);
XBAPI NTSTATUS NTAPI NtReleaseSemaphore(HANDLE SemaphoreHandle, LONG ReleaseCount, PLONG PreviousCount);
XBAPI NTSTATUS NTAPI NtCreateMutant(PHANDLE MutantHandle, POBJECT_ATTRIBUTES ObjectAttributes, BOOLEAN InitialOwner);
XBAPI NTSTATUS NTAPI NtWaitForSingleObject(HANDLE Handle, BOOLEAN Alertable, PLARGE_INTEGER Timeout);
XBAPI NTSTATUS NTAPI NtWaitForSingleObjectEx(HANDLE Handle, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable, PLARGE_INTEGER Timeout);
XBAPI NTSTATUS NTAPI NtWaitForMultipleObjectsEx(ULONG Count, HANDLE Handles[], WAIT_TYPE WaitType, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable,
                                                PLARGE_INTEGER Timeout);
XBAPI NTSTATUS NTAPI NtCreateDirectoryObject(PHANDLE DirectoryHandle, POBJECT_ATTRIBUTES ObjectAttributes);
XBAPI NTSTATUS NTAPI NtQueryDirectoryObject(HANDLE DirectoryHandle, PVOID Buffer, ULONG Length, BOOLEAN RestartScan, PULONG Context, PULONG ReturnLength);
XBAPI NTSTATUS NTAPI NtOpenSymbolicLinkObject(PHANDLE LinkHandle, POBJECT_ATTRIBUTES ObjectAttributes);
XBAPI NTSTATUS NTAPI NtQuerySymbolicLinkObject(HANDLE LinkHandle, POBJECT_STRING LinkTarget, PULONG ReturnedLength);

namespace xk::objects {
extern volatile ULONG TraceSymbolicLink;
} // namespace xk::objects
// The executive objects' query results and the timer APC (objects/handle_objects.cpp).
struct EVENT_BASIC_INFORMATION {
    EVENT_TYPE EventType;
    LONG EventState;
};
struct MUTANT_BASIC_INFORMATION {
    LONG CurrentCount;
    BOOLEAN OwnedByCaller;
    BOOLEAN AbandonedState;
};
struct SEMAPHORE_BASIC_INFORMATION {
    LONG CurrentCount;
    LONG MaximumCount;
};
typedef void(NTAPI *PTIMER_APC_ROUTINE)(PVOID TimerContext, ULONG TimerLowValue, LONG TimerHighValue);
struct TIMER_BASIC_INFORMATION {
    LARGE_INTEGER RemainingTime;
    BOOLEAN TimerState;
};

XBAPI NTSTATUS NTAPI NtCreateTimer(PHANDLE TimerHandle, POBJECT_ATTRIBUTES ObjectAttributes, TIMER_TYPE TimerType);
XBAPI NTSTATUS NTAPI NtSetTimerEx(HANDLE TimerHandle, PLARGE_INTEGER DueTime, PTIMER_APC_ROUTINE TimerApcRoutine, KPROCESSOR_MODE ApcMode, PVOID TimerContext,
                                  BOOLEAN ResumeTimer, LONG Period, PBOOLEAN PreviousState);
XBAPI NTSTATUS NTAPI NtCancelTimer(HANDLE TimerHandle, PBOOLEAN CurrentState);
XBAPI NTSTATUS NTAPI NtQueryTimer(HANDLE TimerHandle, TIMER_BASIC_INFORMATION *TimerInformation);
XBAPI NTSTATUS NTAPI NtQueryEvent(HANDLE EventHandle, EVENT_BASIC_INFORMATION *EventInformation);
XBAPI NTSTATUS NTAPI NtClearEvent(HANDLE EventHandle);
XBAPI NTSTATUS NTAPI NtPulseEvent(HANDLE EventHandle, PLONG PreviousState);
XBAPI NTSTATUS NTAPI NtQueryMutant(HANDLE MutantHandle, MUTANT_BASIC_INFORMATION *MutantInformation);
XBAPI NTSTATUS NTAPI NtReleaseMutant(HANDLE MutantHandle, PLONG PreviousCount);
XBAPI NTSTATUS NTAPI NtQuerySemaphore(HANDLE SemaphoreHandle, SEMAPHORE_BASIC_INFORMATION *SemaphoreInformation);
XBAPI NTSTATUS NTAPI NtSignalAndWaitForSingleObjectEx(HANDLE SignalHandle, HANDLE WaitHandle, KPROCESSOR_MODE WaitMode, BOOLEAN Alertable,
                                                      PLARGE_INTEGER Timeout);
XBAPI NTSTATUS NTAPI NtYieldExecution();
XBAPI NTSTATUS NTAPI NtOpenDirectoryObject(PHANDLE DirectoryHandle, POBJECT_ATTRIBUTES ObjectAttributes);
