// Object manager (spec/10): headers, handle table, namespace, directories and symbolic links.
// The Ob "lock" is DISPATCH_LEVEL, as in the reference.
#include <xk/arch.h>
#include <xk/ob.h>
#include <xk/rtl.h>

static KEVENT ObpDefaultObject; // waiting on a non-waitable object completes at once

OBJECT_TYPE ObDirectoryObjectType = {ExAllocatePoolWithTag, ExFreePool, nullptr, nullptr, nullptr,
                                     &ObpDefaultObject, 'eriD'};
static void NTAPI ObpDeleteSymbolicLink(PVOID Object);
OBJECT_TYPE ObSymbolicLinkObjectType = {ExAllocatePoolWithTag, ExFreePool, nullptr, ObpDeleteSymbolicLink, nullptr,
                                        &ObpDefaultObject, 'bmyS'};

OBJECT_HANDLE_TABLE ObpObjectHandleTable;
static POBJECT_DIRECTORY ObpRootDirectoryObject, ObpDosDevicesDirectoryObject, ObpWin32NamedObjectsDirectoryObject;
static PVOID ObpDosDevicesDriveLetterMap[26]; // "\??\X:" fast path

PVOID ObpGetWaitableObject(PVOID Object) {
    PVOID d = OBJECT_TO_OBJECT_HEADER(Object)->Type->DefaultObject;
    return (LONG)d >= 0 ? (UCHAR *)Object + (ULONG)d : d;
}

// --- Handle table: RootTable[h >> 8][(h & 0xFC) / 4]; free entries hold the next free handle | 1.

static PVOID *ObpHandleEntry(ULONG h) { return &ObpObjectHandleTable.RootTable[h >> 8][(h & 0xFC) >> 2]; }

static bool ObpExtendHandleTable() {
    auto sub = (PVOID *)ExAllocatePoolWithTag(64 * sizeof(PVOID), 'tHbO');
    if (!sub)
        return false;
    ULONG next = (ULONG)ObpObjectHandleTable.NextHandleNeedingPool;
    if ((next & 0x7FF) == 0 && next != 0) { // root table full: grow it by 8 slots
        ULONG slots = next >> 8;
        auto root = (PVOID **)ExAllocatePoolWithTag((slots + 8) * sizeof(PVOID), 'rHbO');
        if (!root) {
            ExFreePool(sub);
            return false;
        }
        for (ULONG i = 0; i < slots; i++)
            root[i] = ObpObjectHandleTable.RootTable[i];
        if (ObpObjectHandleTable.RootTable != ObpObjectHandleTable.BuiltinRootTable)
            ExFreePool(ObpObjectHandleTable.RootTable);
        ObpObjectHandleTable.RootTable = root;
    }
    ObpObjectHandleTable.RootTable[next >> 8] = sub;
    for (ULONG i = 0; i < 63; i++)
        sub[i] = (PVOID)((next + (i + 1) * 4) | 1);
    sub[63] = (PVOID)0xFFFFFFFF;
    if (next == 0) { // handle 0 is never handed out
        ObpObjectHandleTable.FirstFreeTableEntry = (LONG)sub[0];
        sub[0] = nullptr;
    } else {
        ObpObjectHandleTable.FirstFreeTableEntry = (LONG)(next | 1);
    }
    ObpObjectHandleTable.NextHandleNeedingPool = (HANDLE)(next + 0x100);
    return true;
}

// At DISPATCH_LEVEL. The handle takes over one object reference from the caller.
static HANDLE ObpCreateObjectHandle(PVOID Object) {
    if (ObpObjectHandleTable.FirstFreeTableEntry == -1 && !ObpExtendHandleTable())
        return nullptr;
    ULONG h = (ULONG)ObpObjectHandleTable.FirstFreeTableEntry & ~1u;
    PVOID *entry = ObpHandleEntry(h);
    ObpObjectHandleTable.FirstFreeTableEntry = (LONG)*entry;
    *entry = Object;
    ObpObjectHandleTable.HandleCount++;
    OBJECT_TO_OBJECT_HEADER(Object)->HandleCount++;
    return (HANDLE)h;
}

static PVOID ObpLookupHandle(HANDLE Handle) {
    ULONG h = (ULONG)Handle & ~3u;
    if (h >= (ULONG)ObpObjectHandleTable.NextHandleNeedingPool)
        return nullptr;
    PVOID o = *ObpHandleEntry(h);
    return (o && !((ULONG)o & 1)) ? o : nullptr;
}

static PVOID ObpRemoveHandle(HANDLE Handle) {
    PVOID o = ObpLookupHandle(Handle);
    if (o) {
        ULONG h = (ULONG)Handle & ~3u;
        *ObpHandleEntry(h) = (PVOID)ObpObjectHandleTable.FirstFreeTableEntry;
        ObpObjectHandleTable.FirstFreeTableEntry = (LONG)(h | 1);
        ObpObjectHandleTable.HandleCount--;
    }
    return o;
}

// --- References

XBAPI void FASTCALL ObfReferenceObject(PVOID Object) {
    __atomic_add_fetch(&OBJECT_TO_OBJECT_HEADER(Object)->PointerCount, 1, __ATOMIC_SEQ_CST);
}

XBAPI void FASTCALL ObfDereferenceObject(PVOID Object) {
    POBJECT_HEADER h = OBJECT_TO_OBJECT_HEADER(Object);
    if (__atomic_sub_fetch(&h->PointerCount, 1, __ATOMIC_SEQ_CST) != 0)
        return;
    if (h->Type->DeleteProcedure)
        h->Type->DeleteProcedure(Object);
    h->Type->FreeProcedure((h->Flags & OB_FLAG_NAMED_OBJECT) ? (PVOID)OBJECT_HEADER_TO_NAME_INFO(h) : (PVOID)h);
}

XBAPI NTSTATUS NTAPI ObReferenceObjectByPointer(PVOID Object, POBJECT_TYPE ObjectType) {
    if (ObjectType && OBJECT_TO_OBJECT_HEADER(Object)->Type != ObjectType)
        return STATUS_OBJECT_TYPE_MISMATCH;
    ObfReferenceObject(Object);
    return STATUS_SUCCESS;
}

XBAPI NTSTATUS NTAPI ObReferenceObjectByHandle(HANDLE Handle, POBJECT_TYPE ObjectType, PVOID *ReturnedObject) {
    *ReturnedObject = nullptr;
    if (Handle == NtCurrentThread()) {
        if (ObjectType && ObjectType != &PsThreadObjectType)
            return STATUS_OBJECT_TYPE_MISMATCH;
        PVOID t = KiPcr.PrcbData.CurrentThread;
        ObfReferenceObject(t);
        *ReturnedObject = t;
        return STATUS_SUCCESS;
    }
    KIRQL old = KeRaiseIrqlToDpcLevel();
    PVOID o = ObpLookupHandle(Handle);
    if (o)
        ObfReferenceObject(o);
    KfLowerIrql(old);
    if (!o)
        return STATUS_INVALID_HANDLE;
    if (ObjectType && OBJECT_TO_OBJECT_HEADER(o)->Type != ObjectType) {
        ObfDereferenceObject(o);
        return STATUS_OBJECT_TYPE_MISMATCH;
    }
    *ReturnedObject = o;
    return STATUS_SUCCESS;
}

// --- Namespace

static ULONG ObpComputeHashIndex(const OBJECT_STRING *name) {
    ULONG h = 0;
    for (USHORT i = 0; i < name->Length; i++) {
        UCHAR c = (UCHAR)name->Buffer[i];
        if (c < 0x80)
            h = h * 3 + (c | 0x20) + (h >> 1);
    }
    return h % OB_NUMBER_HASH_BUCKETS;
}

static int ObpDriveLetterIndex(const OBJECT_STRING *name) {
    if (name->Length != 2 || name->Buffer[1] != ':')
        return -1;
    CHAR c = RtlUpperChar(name->Buffer[0]);
    return (c >= 'A' && c <= 'Z') ? c - 'A' : -1;
}

// Find `element` in `dir`; with `follow`, a symbolic link resolves to its target object.
static PVOID ObpLookupElementNameInDirectory(POBJECT_DIRECTORY dir, const OBJECT_STRING *element, bool follow) {
    if (dir == ObpDosDevicesDirectoryObject && follow) {
        int letter = ObpDriveLetterIndex(element);
        if (letter >= 0 && ObpDosDevicesDriveLetterMap[letter])
            return ObpDosDevicesDriveLetterMap[letter];
    }
    for (POBJECT_HEADER_NAME_INFO n = dir->HashBuckets[ObpComputeHashIndex(element)]; n; n = n->ChainLink) {
        if (!RtlEqualString(&n->Name, element, TRUE))
            continue;
        PVOID o = (POBJECT_HEADER)(n + 1) + 1;
        if (follow && OBJECT_TO_OBJECT_HEADER(o)->Type == &ObSymbolicLinkObjectType)
            o = ((POBJECT_SYMBOLIC_LINK)o)->LinkTargetObject;
        return o;
    }
    return nullptr;
}

// Split off the first path element of `rest` (no leading '\'); `rest` becomes what follows the '\'.
static void ObpSplitElement(OBJECT_STRING *rest, OBJECT_STRING *element) {
    USHORT i = 0;
    while (i < rest->Length && rest->Buffer[i] != '\\')
        i++;
    element->Buffer = rest->Buffer;
    element->Length = element->MaximumLength = i;
    if (i < rest->Length)
        i++; // skip the separator
    rest->Buffer += i;
    rest->Length -= i;
    rest->MaximumLength = rest->Length;
}

// Resolve the starting directory/object for a name (Root handle rules, spec/10 §3). Referenced on success.
static NTSTATUS ObpResolveRoot(HANDLE RootDirectory, OBJECT_STRING *rest, PVOID *start) {
    if (!RootDirectory) {
        if (!rest->Length || rest->Buffer[0] != '\\')
            return STATUS_OBJECT_NAME_INVALID;
        rest->Buffer++;
        rest->Length--;
        *start = ObpRootDirectoryObject;
    } else {
        if (rest->Length && rest->Buffer[0] == '\\')
            return STATUS_OBJECT_NAME_INVALID;
        if (RootDirectory == ObDosDevicesDirectory())
            *start = ObpDosDevicesDirectoryObject;
        else if (RootDirectory == ObWin32NamedObjectsDirectory())
            *start = ObpWin32NamedObjectsDirectoryObject;
        else if (!(*start = ObpLookupHandle(RootDirectory)))
            return STATUS_INVALID_HANDLE;
    }
    ObfReferenceObject(*start);
    return STATUS_SUCCESS;
}

static NTSTATUS ObpLookupObjectName(HANDLE RootDirectory, POBJECT_STRING ObjectName, ULONG Attributes,
                                    POBJECT_TYPE ObjectType, PVOID ParseContext, PVOID *Object) {
    *Object = nullptr;
    OBJECT_STRING rest = ObjectName ? *ObjectName : OBJECT_STRING{0, 0, nullptr};
    KIRQL old = KeRaiseIrqlToDpcLevel();
    PVOID current;
    NTSTATUS status = ObpResolveRoot(RootDirectory, &rest, &current);
    if (!NT_SUCCESS(status)) {
        KfLowerIrql(old);
        return status;
    }
    for (;;) {
        POBJECT_TYPE type = OBJECT_TO_OBJECT_HEADER(current)->Type;
        if (type != &ObDirectoryObjectType && type->ParseProcedure) {
            // Hand the rest of the path (with its leading '\') to the object's parse procedure.
            KfLowerIrql(old);
            OBJECT_STRING remaining = rest;
            // Give the parse procedure the separator in front of the rest: "\Device\CdRom0\" opens the
            // root directory ("\"), "\Device\CdRom0" the volume itself ("").
            if (ObjectName && remaining.Buffer > ObjectName->Buffer && remaining.Buffer[-1] == '\\') {
                remaining.Buffer--;
                remaining.Length++;
                remaining.MaximumLength = remaining.Length;
            }
            PVOID result = nullptr;
            status = type->ParseProcedure(current, ObjectType, Attributes, ObjectName, &remaining, ParseContext, &result);
            ObfDereferenceObject(current);
            if (!NT_SUCCESS(status))
                return status;
            if (ObjectType && OBJECT_TO_OBJECT_HEADER(result)->Type != ObjectType) {
                ObfDereferenceObject(result);
                return STATUS_OBJECT_TYPE_MISMATCH;
            }
            *Object = result;
            return STATUS_SUCCESS;
        }
        if (!rest.Length)
            break;
        if (type != &ObDirectoryObjectType) {
            status = STATUS_OBJECT_PATH_NOT_FOUND;
            break;
        }
        OBJECT_STRING element;
        ObpSplitElement(&rest, &element);
        if (!element.Length) {
            status = STATUS_OBJECT_NAME_INVALID;
            break;
        }
        bool follow = rest.Length || ObjectType != &ObSymbolicLinkObjectType;
        PVOID next = ObpLookupElementNameInDirectory((POBJECT_DIRECTORY)current, &element, follow);
        if (!next) {
            status = rest.Length ? STATUS_OBJECT_PATH_NOT_FOUND : STATUS_OBJECT_NAME_NOT_FOUND;
            break;
        }
        ObfReferenceObject(next);
        ObfDereferenceObject(current);
        current = next;
    }
    if (NT_SUCCESS(status) && ObjectType && OBJECT_TO_OBJECT_HEADER(current)->Type != ObjectType)
        status = STATUS_OBJECT_TYPE_MISMATCH;
    KfLowerIrql(old);
    if (!NT_SUCCESS(status)) {
        ObfDereferenceObject(current);
        return status;
    }
    *Object = current;
    return STATUS_SUCCESS;
}

// --- Creation and insertion

XBAPI NTSTATUS NTAPI ObCreateObject(POBJECT_TYPE ObjectType, POBJECT_ATTRIBUTES ObjectAttributes, ULONG ObjectBodySize,
                                    PVOID *Object) {
    *Object = nullptr;
    POBJECT_STRING name = ObjectAttributes ? ObjectAttributes->ObjectName : nullptr;
    if (!name || !name->Length) {
        auto h = (POBJECT_HEADER)ObjectType->AllocateProcedure(ObjectBodySize + sizeof(OBJECT_HEADER), ObjectType->PoolTag);
        if (!h)
            return STATUS_INSUFFICIENT_RESOURCES_OB;
        *h = {1, 0, ObjectType, 0};
        *Object = h + 1;
        return STATUS_SUCCESS;
    }
    // The object keeps only the final path element as its name.
    USHORT start = name->Length;
    while (start && name->Buffer[start - 1] != '\\')
        start--;
    USHORT len = name->Length - start;
    if (!len)
        return STATUS_OBJECT_NAME_INVALID;
    ULONG body = (ObjectBodySize + 3) & ~3u;
    auto info = (POBJECT_HEADER_NAME_INFO)ObjectType->AllocateProcedure(
        sizeof(OBJECT_HEADER_NAME_INFO) + sizeof(OBJECT_HEADER) + body + len, ObjectType->PoolTag);
    if (!info)
        return STATUS_INSUFFICIENT_RESOURCES_OB;
    auto h = (POBJECT_HEADER)(info + 1);
    *h = {1, 0, ObjectType, OB_FLAG_NAMED_OBJECT};
    info->ChainLink = nullptr;
    info->Directory = nullptr;
    info->Name.Buffer = (PCHAR)(h + 1) + body;
    info->Name.Length = info->Name.MaximumLength = len;
    RtlCopyMemory(info->Name.Buffer, name->Buffer + start, len);
    *Object = h + 1;
    return STATUS_SUCCESS;
}

static void ObpInsertIntoDirectory(POBJECT_DIRECTORY dir, PVOID Object) {
    POBJECT_HEADER h = OBJECT_TO_OBJECT_HEADER(Object);
    POBJECT_HEADER_NAME_INFO n = OBJECT_HEADER_TO_NAME_INFO(h);
    ULONG bucket = ObpComputeHashIndex(&n->Name);
    n->ChainLink = dir->HashBuckets[bucket];
    dir->HashBuckets[bucket] = n;
    n->Directory = dir;
    h->Flags |= OB_FLAG_ATTACHED_OBJECT;
    if (dir == ObpDosDevicesDirectoryObject) {
        int letter = ObpDriveLetterIndex(&n->Name);
        if (letter >= 0)
            ObpDosDevicesDriveLetterMap[letter] = h->Type == &ObSymbolicLinkObjectType
                                                      ? ((POBJECT_SYMBOLIC_LINK)Object)->LinkTargetObject
                                                      : Object;
    }
    ObfReferenceObject(dir);
    ObfReferenceObject(Object);
}

// At DISPATCH_LEVEL; the caller dereferences afterwards (the directory link reference and dir reference).
static void ObpDetachNamedObject(PVOID Object) {
    POBJECT_HEADER h = OBJECT_TO_OBJECT_HEADER(Object);
    POBJECT_HEADER_NAME_INFO n = OBJECT_HEADER_TO_NAME_INFO(h);
    POBJECT_DIRECTORY dir = n->Directory;
    POBJECT_HEADER_NAME_INFO *link = &dir->HashBuckets[ObpComputeHashIndex(&n->Name)];
    while (*link && *link != n)
        link = &(*link)->ChainLink;
    if (*link)
        *link = n->ChainLink;
    if (dir == ObpDosDevicesDirectoryObject) {
        int letter = ObpDriveLetterIndex(&n->Name);
        if (letter >= 0)
            ObpDosDevicesDriveLetterMap[letter] = nullptr;
    }
    n->Directory = nullptr;
    h->Flags &= ~OB_FLAG_ATTACHED_OBJECT;
}

static void ObpDetachAndRelease(PVOID Object) {
    POBJECT_DIRECTORY dir = OBJECT_HEADER_TO_NAME_INFO(OBJECT_TO_OBJECT_HEADER(Object))->Directory;
    ObpDetachNamedObject(Object);
    ObfDereferenceObject(Object);
    ObfDereferenceObject(dir);
}

XBAPI NTSTATUS NTAPI ObInsertObject(PVOID Object, POBJECT_ATTRIBUTES ObjectAttributes, ULONG ObjectPointerBias,
                                    PHANDLE Handle) {
    *Handle = nullptr;
    POBJECT_HEADER h = OBJECT_TO_OBJECT_HEADER(Object);
    NTSTATUS status = STATUS_SUCCESS;
    PVOID target = Object; // the object the new handle refers to
    KIRQL old = KeRaiseIrqlToDpcLevel();

    if (h->Flags & OB_FLAG_NAMED_OBJECT) {
        OBJECT_STRING rest = *ObjectAttributes->ObjectName;
        PVOID dir;
        status = ObpResolveRoot(ObjectAttributes->RootDirectory, &rest, &dir);
        // Walk to the parent directory (all elements but the last).
        while (NT_SUCCESS(status)) {
            OBJECT_STRING element;
            ObpSplitElement(&rest, &element);
            if (!element.Length) {
                status = STATUS_OBJECT_NAME_INVALID;
                break;
            }
            if (OBJECT_TO_OBJECT_HEADER(dir)->Type != &ObDirectoryObjectType) {
                status = STATUS_OBJECT_PATH_NOT_FOUND;
                break;
            }
            PVOID found = ObpLookupElementNameInDirectory((POBJECT_DIRECTORY)dir, &element, rest.Length != 0);
            if (!rest.Length) { // final element
                if (!found) {
                    ObpInsertIntoDirectory((POBJECT_DIRECTORY)dir, Object);
                } else if ((ObjectAttributes->Attributes & OBJ_OPENIF) && OBJECT_TO_OBJECT_HEADER(found)->Type == h->Type) {
                    target = found;
                    status = STATUS_OBJECT_NAME_EXISTS;
                } else {
                    status = STATUS_OBJECT_NAME_COLLISION;
                }
                break;
            }
            if (!found || OBJECT_TO_OBJECT_HEADER(found)->Type != &ObDirectoryObjectType) {
                status = STATUS_OBJECT_PATH_NOT_FOUND;
                break;
            }
            ObfReferenceObject(found);
            ObfDereferenceObject(dir);
            dir = found;
        }
        if (dir)
            ObfDereferenceObject(dir);
    }
    if (NT_SUCCESS(status)) {
        HANDLE handle = ObpCreateObjectHandle(target);
        if (!handle) {
            if (target == Object && (h->Flags & OB_FLAG_ATTACHED_OBJECT))
                ObpDetachAndRelease(Object);
            status = STATUS_INSUFFICIENT_RESOURCES_OB;
        } else {
            OBJECT_TO_OBJECT_HEADER(target)->PointerCount += ObjectPointerBias + 1;
            if (ObjectAttributes && (ObjectAttributes->Attributes & OBJ_PERMANENT))
                h->Flags |= OB_FLAG_PERMANENT_OBJECT;
            *Handle = handle;
        }
    }
    KfLowerIrql(old);
    ObfDereferenceObject(Object); // the creator's reference
    return status;
}

XBAPI NTSTATUS NTAPI ObOpenObjectByName(POBJECT_ATTRIBUTES ObjectAttributes, POBJECT_TYPE ObjectType,
                                        PVOID ParseContext, PHANDLE Handle) {
    *Handle = nullptr;
    PVOID o;
    NTSTATUS status = ObpLookupObjectName(ObjectAttributes->RootDirectory, ObjectAttributes->ObjectName,
                                          ObjectAttributes->Attributes, ObjectType, ParseContext, &o);
    if (!NT_SUCCESS(status))
        return status;
    KIRQL old = KeRaiseIrqlToDpcLevel();
    HANDLE h = ObpCreateObjectHandle(o);
    KfLowerIrql(old);
    if (!h) {
        ObfDereferenceObject(o);
        return STATUS_INSUFFICIENT_RESOURCES_OB;
    }
    *Handle = h;
    return STATUS_SUCCESS;
}

XBAPI NTSTATUS NTAPI ObOpenObjectByPointer(PVOID Object, POBJECT_TYPE ObjectType, PHANDLE Handle) {
    *Handle = nullptr;
    NTSTATUS status = ObReferenceObjectByPointer(Object, ObjectType);
    if (!NT_SUCCESS(status))
        return status;
    KIRQL old = KeRaiseIrqlToDpcLevel();
    HANDLE h = ObpCreateObjectHandle(Object);
    KfLowerIrql(old);
    if (!h) {
        ObfDereferenceObject(Object);
        return STATUS_INSUFFICIENT_RESOURCES_OB;
    }
    *Handle = h;
    return STATUS_SUCCESS;
}

XBAPI NTSTATUS NTAPI ObReferenceObjectByName(POBJECT_STRING ObjectName, ULONG Attributes, POBJECT_TYPE ObjectType,
                                             PVOID ParseContext, PVOID *Object) {
    return ObpLookupObjectName(nullptr, ObjectName, Attributes, ObjectType, ParseContext, Object);
}

XBAPI NTSTATUS NTAPI NtClose(HANDLE Handle) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    PVOID o = ObpRemoveHandle(Handle);
    if (!o) {
        KfLowerIrql(old);
        return STATUS_INVALID_HANDLE;
    }
    POBJECT_HEADER h = OBJECT_TO_OBJECT_HEADER(o);
    LONG handles = h->HandleCount--;
    if (h->Type->CloseProcedure) {
        KfLowerIrql(old);
        h->Type->CloseProcedure(o, handles);
        old = KeRaiseIrqlToDpcLevel();
    }
    if (h->HandleCount == 0 && (h->Flags & OB_FLAG_ATTACHED_OBJECT) && !(h->Flags & OB_FLAG_PERMANENT_OBJECT))
        ObpDetachAndRelease(o);
    KfLowerIrql(old);
    ObfDereferenceObject(o);
    return STATUS_SUCCESS;
}

XBAPI void NTAPI ObMakeTemporaryObject(PVOID Object) {
    KIRQL old = KeRaiseIrqlToDpcLevel();
    POBJECT_HEADER h = OBJECT_TO_OBJECT_HEADER(Object);
    h->Flags &= ~OB_FLAG_PERMANENT_OBJECT;
    if (h->HandleCount == 0 && (h->Flags & OB_FLAG_ATTACHED_OBJECT))
        ObpDetachAndRelease(Object);
    KfLowerIrql(old);
}

XBAPI NTSTATUS NTAPI NtDuplicateObject(HANDLE SourceHandle, PHANDLE TargetHandle, ULONG Options) {
    constexpr ULONG DUPLICATE_CLOSE_SOURCE = 1;
    PVOID o;
    NTSTATUS status = ObReferenceObjectByHandle(SourceHandle, nullptr, &o);
    if (!NT_SUCCESS(status)) {
        *TargetHandle = nullptr;
        return status;
    }
    KIRQL old = KeRaiseIrqlToDpcLevel();
    HANDLE h = ObpCreateObjectHandle(o);
    KfLowerIrql(old);
    if (!h) {
        ObfDereferenceObject(o);
        *TargetHandle = nullptr;
        return STATUS_INSUFFICIENT_RESOURCES_OB;
    }
    *TargetHandle = h;
    if (Options & DUPLICATE_CLOSE_SOURCE)
        NtClose(SourceHandle);
    return STATUS_SUCCESS;
}

// --- Directory and symbolic link objects

XBAPI NTSTATUS NTAPI NtCreateDirectoryObject(PHANDLE DirectoryHandle, POBJECT_ATTRIBUTES ObjectAttributes) {
    PVOID dir = nullptr;
    NTSTATUS status = ObCreateObject(&ObDirectoryObjectType, ObjectAttributes, sizeof(OBJECT_DIRECTORY), &dir);
    if (!NT_SUCCESS(status)) {
        *DirectoryHandle = nullptr;
        return status;
    }
    RtlZeroMemory(dir, sizeof(OBJECT_DIRECTORY));
    return ObInsertObject(dir, ObjectAttributes, 0, DirectoryHandle);
}

XBAPI NTSTATUS NTAPI NtOpenDirectoryObject(PHANDLE DirectoryHandle, POBJECT_ATTRIBUTES ObjectAttributes) {
    return ObOpenObjectByName(ObjectAttributes, &ObDirectoryObjectType, nullptr, DirectoryHandle);
}

// One entry per call; *Context is the running index.
XBAPI NTSTATUS NTAPI NtQueryDirectoryObject(HANDLE DirectoryHandle, PVOID Buffer, ULONG Length, BOOLEAN RestartScan,
                                            PULONG Context, PULONG ReturnLength) {
    POBJECT_DIRECTORY dir;
    NTSTATUS status = ObReferenceObjectByHandle(DirectoryHandle, &ObDirectoryObjectType, (PVOID *)&dir);
    if (!NT_SUCCESS(status))
        return status;
    ULONG wanted = RestartScan ? 0 : *Context, index = 0;
    POBJECT_HEADER_NAME_INFO found = nullptr;
    KIRQL old = KeRaiseIrqlToDpcLevel();
    for (ULONG b = 0; b < OB_NUMBER_HASH_BUCKETS && !found; b++)
        for (POBJECT_HEADER_NAME_INFO n = dir->HashBuckets[b]; n && !found; n = n->ChainLink)
            if (index++ == wanted)
                found = n;
    status = STATUS_NO_MORE_ENTRIES;
    if (found) {
        ULONG need = sizeof(OBJECT_DIRECTORY_INFORMATION) + found->Name.Length + 1;
        if (ReturnLength)
            *ReturnLength = need;
        if (Length < need) {
            status = STATUS_BUFFER_TOO_SMALL;
        } else {
            auto info = (POBJECT_DIRECTORY_INFORMATION)Buffer;
            info->Name.Buffer = (PCHAR)(info + 1);
            info->Name.Length = found->Name.Length;
            info->Name.MaximumLength = (USHORT)(found->Name.Length + 1);
            RtlCopyMemory(info->Name.Buffer, found->Name.Buffer, found->Name.Length);
            info->Name.Buffer[found->Name.Length] = 0;
            info->Type = ((POBJECT_HEADER)(found + 1))->Type->PoolTag;
            *Context = wanted + 1;
            status = STATUS_SUCCESS;
        }
    }
    KfLowerIrql(old);
    ObfDereferenceObject(dir);
    return status;
}

static void NTAPI ObpDeleteSymbolicLink(PVOID Object) {
    auto link = (POBJECT_SYMBOLIC_LINK)Object;
    if (link->LinkTargetObject)
        ObfDereferenceObject(link->LinkTargetObject);
    if (link->LinkTarget.Buffer)
        ExFreePool(link->LinkTarget.Buffer);
}

// Permanent link LinkName -> object named TargetName (which must already exist).
NTSTATUS ObpCreateSymbolicLink(POBJECT_STRING LinkName, POBJECT_STRING TargetName) {
    PVOID target;
    NTSTATUS status = ObReferenceObjectByName(TargetName, 0, nullptr, nullptr, &target);
    if (!NT_SUCCESS(status))
        return status;
    OBJECT_ATTRIBUTES attrs = {nullptr, LinkName, OBJ_PERMANENT | OBJ_CASE_INSENSITIVE};
    POBJECT_SYMBOLIC_LINK link;
    status = ObCreateObject(&ObSymbolicLinkObjectType, &attrs, sizeof(OBJECT_SYMBOLIC_LINK), (PVOID *)&link);
    if (!NT_SUCCESS(status)) {
        ObfDereferenceObject(target);
        return status;
    }
    link->LinkTargetObject = target;
    link->LinkTarget.Buffer = (PCHAR)ExAllocatePoolWithTag(TargetName->Length + 1, 'bmyS');
    if (!link->LinkTarget.Buffer) {
        ObfDereferenceObject(link);
        return STATUS_INSUFFICIENT_RESOURCES_OB;
    }
    RtlCopyMemory(link->LinkTarget.Buffer, TargetName->Buffer, TargetName->Length);
    link->LinkTarget.Buffer[TargetName->Length] = 0;
    link->LinkTarget.Length = TargetName->Length;
    link->LinkTarget.MaximumLength = (USHORT)(TargetName->Length + 1);
    HANDLE h;
    status = ObInsertObject(link, &attrs, 0, &h);
    if (NT_SUCCESS(status))
        NtClose(h);
    return status;
}

XBAPI NTSTATUS NTAPI NtOpenSymbolicLinkObject(PHANDLE LinkHandle, POBJECT_ATTRIBUTES ObjectAttributes) {
    return ObOpenObjectByName(ObjectAttributes, &ObSymbolicLinkObjectType, nullptr, LinkHandle);
}

XBAPI NTSTATUS NTAPI NtQuerySymbolicLinkObject(HANDLE LinkHandle, POBJECT_STRING LinkTarget, PULONG ReturnedLength) {
    POBJECT_SYMBOLIC_LINK link;
    NTSTATUS status = ObReferenceObjectByHandle(LinkHandle, &ObSymbolicLinkObjectType, (PVOID *)&link);
    if (!NT_SUCCESS(status))
        return status;
    USHORT len = link->LinkTarget.Length;
    if (ReturnedLength)
        *ReturnedLength = len;
    if (len > LinkTarget->MaximumLength) {
        status = STATUS_BUFFER_TOO_SMALL;
    } else {
        RtlCopyMemory(LinkTarget->Buffer, link->LinkTarget.Buffer, len);
        LinkTarget->Length = len;
        if (len < LinkTarget->MaximumLength)
            LinkTarget->Buffer[len] = 0;
    }
    ObfDereferenceObject(link);
    return status;
}

// --- Initialisation (spec/02 §2.4): root, \??, \Device, \Win32NamedObjects.

static POBJECT_DIRECTORY ObpCreatePermanentDirectory(PCSZ path) {
    OBJECT_STRING name;
    RtlInitAnsiString(&name, path);
    OBJECT_ATTRIBUTES attrs = {nullptr, &name, OBJ_PERMANENT | OBJ_CASE_INSENSITIVE};
    HANDLE h;
    if (!NT_SUCCESS(NtCreateDirectoryObject(&h, &attrs)))
        KeBugCheck(0x5E); // OBJECT_INITIALIZATION_FAILED
    POBJECT_DIRECTORY dir;
    ObReferenceObjectByHandle(h, &ObDirectoryObjectType, (PVOID *)&dir);
    NtClose(h);
    ObfDereferenceObject(dir); // kept alive by its directory link (permanent)
    return dir;
}

void ObInitSystem() {
    KeInitializeEvent(&ObpDefaultObject, NotificationEvent, TRUE);
    ObpObjectHandleTable.HandleCount = 0;
    ObpObjectHandleTable.FirstFreeTableEntry = -1;
    ObpObjectHandleTable.NextHandleNeedingPool = nullptr;
    ObpObjectHandleTable.RootTable = ObpObjectHandleTable.BuiltinRootTable;

    PVOID root;
    if (!NT_SUCCESS(ObCreateObject(&ObDirectoryObjectType, nullptr, sizeof(OBJECT_DIRECTORY), &root)))
        KeBugCheck(0x5E);
    RtlZeroMemory(root, sizeof(OBJECT_DIRECTORY));
    ObpRootDirectoryObject = (POBJECT_DIRECTORY)root; // holds its creation reference forever
    ObpDosDevicesDirectoryObject = ObpCreatePermanentDirectory("\\??");
    ObpCreatePermanentDirectory("\\Device");
    ObpWin32NamedObjectsDirectoryObject = ObpCreatePermanentDirectory("\\Win32NamedObjects");
}
