// FATX file system driver, internal definitions (spec/stock/04 §6, spec/stock/04b).
#pragma once
#include <xk/fsc.h>
#include <xk/idex.h>
#include <xk/rtl.h>

constexpr ULONG FATX_SIGNATURE = 0x58544146; // "FATX"
constexpr ULONG FATX_MAX_NAME = 42, FATX_MAX_PATH = 250;
constexpr UCHAR FATX_DIRENT_NEVER_USED = 0x00, FATX_DIRENT_NEVER_USED2 = 0xFF, FATX_DIRENT_DELETED = 0xE5;
constexpr ULONG FATX_CLUSTER_FREE = 0, FATX_CLUSTER_LAST = 0xFFFFFFFF, FATX_CLUSTER_BAD = 0xFFFFFFF7;
constexpr ULONG FATX_BLOCK_SIZE = 0x1000;

constexpr UCHAR FILE_ATTRIBUTE_READONLY = 0x01, FILE_ATTRIBUTE_HIDDEN = 0x02, FILE_ATTRIBUTE_SYSTEM = 0x04,
                FILE_ATTRIBUTE_DIRECTORY = 0x10, FILE_ATTRIBUTE_ARCHIVE = 0x20;
constexpr ULONG FILE_ATTRIBUTE_NORMAL = 0x80;
constexpr ULONG FILE_SUPERSEDED = 0, FILE_OPENED = 1, FILE_CREATED = 2, FILE_OVERWRITTEN = 3;

struct FATX_DIRENT {
    UCHAR FileNameLength;
    UCHAR FileAttributes;
    CHAR FileName[FATX_MAX_NAME];
    ULONG FirstCluster;
    ULONG FileSize;
    ULONG CreationTime;
    ULONG LastWriteTime;
    ULONG LastAccessTime;
};
static_assert(sizeof(FATX_DIRENT) == 64);

constexpr ULONG FCB_DIRECTORY = 0x02, FCB_DELETE_PENDING = 0x10, FCB_ROOT = 0x20, FCB_VOLUME = 0x40,
                FCB_DIRENT_DIRTY = 0x80;

struct FATX_FCB {
    LIST_ENTRY SiblingLink;
    LIST_ENTRY ChildList;
    FATX_FCB *Parent;
    ULONG Flags;
    LONG ReferenceCount; // file objects not yet closed, plus open children
    LONG CleanupCount;   // file objects not yet cleaned up
    SHARE_ACCESS ShareAccess;
    UCHAR Attributes;
    UCHAR NameLength;
    CHAR Name[FATX_MAX_NAME];
    ULONG FirstCluster;
    ULONG FileSize;
    ULONG DirentIndex; // position of the entry in the parent directory
    ULONG CreationTime, LastWriteTime, LastAccessTime;
    ULONG HintClusterIndex, HintCluster; // last chain position looked up
};

// Per file object (FsContext2): directory enumeration state.
struct FATX_CCB {
    ULONG NextIndex;
    BOOLEAN HaveMask;
    UCHAR MaskLength;
    CHAR Mask[FATX_MAX_NAME + 2];
};

struct FATX_VOLUME {
    PDEVICE_OBJECT TargetDevice;
    LONGLONG PartitionLength;
    ULONG SectorSize;
    UCHAR ClusterShift;
    UCHAR FatEntryShift; // 1 = FATX16, 2 = FATX32
    BOOLEAN Dismounted;
    ULONG BytesPerCluster;
    ULONG NumberOfClusters; // data clusters are numbered 1..NumberOfClusters
    ULONG FreeClusters;
    ULONG AllocationHint;
    ULONG FatByteOffset;
    LONGLONG DataByteOffset;
    ULONG SerialNumber;
    FATX_FCB RootFcb;
    FATX_FCB VolumeFcb; // opens of the volume itself
    FSCACHE_EXTENSION CacheExtension; // the volume's key in the file system cache
};

// Volume layer (fatx/volume.cpp). One lock serves all FATX volumes; blocks come from the file system cache
// (io/fsc.cpp) and a block mapped while the lock is held stays usable until the lock is released.
void FatxInitialize();
void FatxForgetVolume(FATX_VOLUME *v);
void FatxAcquireVolume(FATX_VOLUME *v);
void FatxReleaseVolume(FATX_VOLUME *v);
NTSTATUS FatxDeviceIo(FATX_VOLUME *v, UCHAR Major, LONGLONG Offset, PVOID Buffer, ULONG Length);
NTSTATUS FatxMapBlock(FATX_VOLUME *v, LONGLONG Offset, PUCHAR *Data);
NTSTATUS FatxWriteBlockRange(FATX_VOLUME *v, LONGLONG BlockOffset, ULONG Start, ULONG Length);
void FatxInvalidateRange(FATX_VOLUME *v, LONGLONG Offset, ULONG Length);
NTSTATUS FatxGetFatEntry(FATX_VOLUME *v, ULONG Cluster, ULONG *Value);
NTSTATUS FatxSetFatEntry(FATX_VOLUME *v, ULONG Cluster, ULONG Value);
NTSTATUS FatxAllocateClusters(FATX_VOLUME *v, ULONG PreviousLast, ULONG Count, ULONG *First);
NTSTATUS FatxFreeChain(FATX_VOLUME *v, ULONG First);
NTSTATUS FatxClusterAtIndex(FATX_VOLUME *v, FATX_FCB *f, ULONG Index, ULONG *Cluster);
NTSTATUS FatxChainLength(FATX_VOLUME *v, ULONG First, ULONG *Count, ULONG *Last);
LONGLONG FatxClusterOffset(FATX_VOLUME *v, ULONG Cluster);
bool FatxIsValidCluster(FATX_VOLUME *v, ULONG Cluster);
ULONG FatxCurrentTimestamp();
void FatxTimestampToTime(ULONG Stamp, PLARGE_INTEGER Time);
ULONG FatxTimeToTimestamp(const LARGE_INTEGER *Time);
bool FatxIsValidName(const OBJECT_STRING *Name);
bool FatxNameEqual(const CHAR *a, ULONG alen, const CHAR *b, ULONG blen);
bool FatxWildcardMatch(const CHAR *mask, ULONG mlen, const CHAR *name, ULONG nlen);
NTSTATUS FatxMountVolume(PDEVICE_OBJECT TargetDevice);

// Directory layer (fatx/dir.cpp)
NTSTATUS FatxMapDirent(FATX_VOLUME *v, FATX_FCB *dir, ULONG Index, FATX_DIRENT **Entry, LONGLONG *BlockOffset,
                       bool Extend);
NTSTATUS FatxWriteDirent(FATX_VOLUME *v, LONGLONG BlockOffset, FATX_DIRENT *Entry);
NTSTATUS FatxLookup(FATX_VOLUME *v, FATX_FCB *dir, const OBJECT_STRING *Name, FATX_DIRENT *Found, ULONG *Index);
NTSTATUS FatxCreateDirent(FATX_VOLUME *v, FATX_FCB *dir, const OBJECT_STRING *Name, UCHAR Attributes,
                          ULONG FirstCluster, ULONG *Index);
NTSTATUS FatxFlushFcb(FATX_VOLUME *v, FATX_FCB *f);
NTSTATUS FatxDeleteDirent(FATX_VOLUME *v, FATX_FCB *f);
NTSTATUS FatxIsDirectoryEmpty(FATX_VOLUME *v, FATX_FCB *dir);
FATX_FCB *FatxFindOpenFcb(FATX_FCB *dir, const CHAR *Name, ULONG Length);
FATX_FCB *FatxCreateFcb(FATX_FCB *Parent, const FATX_DIRENT *e, ULONG Index);
void FatxDereferenceFcb(FATX_FCB *f);
NTSTATUS FatxSetFileSize(FATX_VOLUME *v, FATX_FCB *f, ULONG NewSize);

extern DRIVER_OBJECT FatxDriverObject;
static inline FATX_VOLUME *FatxVolume(PDEVICE_OBJECT dev) { return (FATX_VOLUME *)dev->DeviceExtension; }
