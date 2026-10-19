// FATX file system driver, internal definitions (spec/stock/04 §6, spec/stock/04b). The same dispatcher serves exFAT
// volumes (spec/oxos-internal/exfat-driver.md): the volume and directory layers branch on FATX_VOLUME::Exfat.
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

// A directory entry as the layers exchange it: the FATX fields, plus what an exFAT entry set adds.
constexpr UCHAR EXFAT_NOFATCHAIN = 0x01; // the file's clusters are consecutive; its FAT entries are not used
struct FATX_ENTRY : FATX_DIRENT {
    UCHAR ExFlags;
    UCHAR ExEntries;       // entries in the set (file, stream, names)
    ULONG ExAllocClusters; // clusters allocated (DataLength rounded up)
    ULONG ExDirLength;     // directories: DataLength
};

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
    UCHAR ExFlags, ExEntries;             // exFAT (FATX_ENTRY)
    ULONG ExAllocClusters, ExDirLength;
};

// Per file object (FsContext2): directory enumeration state.
struct FATX_CCB {
    ULONG NextIndex;
    BOOLEAN HaveMask;
    UCHAR MaskLength;
    CHAR Mask[FATX_MAX_NAME + 2];
};

struct FATX_ALLOC {
    ULONG FreeClusters;
    ULONG AllocationHint;
};
struct EXFAT_DISK;

struct FATX_VOLUME {
    PDEVICE_OBJECT TargetDevice; // where the blocks are (exFAT: the exFAT partition)
    PDEVICE_OBJECT MountTarget;  // the device mounted (FATX: TargetDevice; exFAT: the folder partition)
    BOOLEAN Exfat;
    ULONG FirstClusterNumber;    // 1 (FATX) or 2 (exFAT)
    FATX_ALLOC *Alloc;           // FATX: OwnAlloc; exFAT: shared by the disk's folder partitions
    FATX_ALLOC OwnAlloc;
    EXFAT_DISK *Ex;
    ULONG PartitionNumber;       // exFAT: the folder partition's number
    LONGLONG PartitionLength;
    ULONG SectorSize;
    UCHAR ClusterShift;
    UCHAR FatEntryShift; // 1 = FATX16, 2 = FATX32
    BOOLEAN Dismounted;
    ULONG BytesPerCluster;
    ULONG NumberOfClusters; // data clusters are numbered FirstClusterNumber..FirstClusterNumber+NumberOfClusters-1
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

NTSTATUS FatxNextCluster(FATX_VOLUME *v, FATX_FCB *f, ULONG Cluster, ULONG *Next);
void FatxVolumeSize(FATX_VOLUME *v, ULONG *Total, ULONG *Free, ULONG *SectorsPerUnit);

// Directory layer (fatx/dir.cpp; exFAT in fatx/exfat.cpp)
NTSTATUS FatxMapDirent(FATX_VOLUME *v, FATX_FCB *dir, ULONG Index, FATX_DIRENT **Entry, LONGLONG *BlockOffset,
                       bool Extend);
NTSTATUS FatxWriteDirent(FATX_VOLUME *v, LONGLONG BlockOffset, FATX_DIRENT *Entry);
NTSTATUS FatxLookup(FATX_VOLUME *v, FATX_FCB *dir, const OBJECT_STRING *Name, FATX_ENTRY *Found, ULONG *Index);
NTSTATUS FatxReadEntry(FATX_VOLUME *v, FATX_FCB *dir, ULONG Index, FATX_ENTRY *Entry);
NTSTATUS FatxNextEntry(FATX_VOLUME *v, FATX_FCB *dir, ULONG *Index, FATX_ENTRY *Entry); // END_OF_FILE at the end
NTSTATUS FatxInitDirectory(FATX_VOLUME *v, ULONG *First); // one cluster for a new directory
NTSTATUS FatxRemoveDirent(FATX_VOLUME *v, FATX_FCB *f);   // drop the entry only (a move)
NTSTATUS FatxCreateDirent(FATX_VOLUME *v, FATX_FCB *dir, const OBJECT_STRING *Name, UCHAR Attributes,
                          ULONG FirstCluster, ULONG *Index);
NTSTATUS FatxFlushFcb(FATX_VOLUME *v, FATX_FCB *f);
NTSTATUS FatxDeleteDirent(FATX_VOLUME *v, FATX_FCB *f);
NTSTATUS FatxIsDirectoryEmpty(FATX_VOLUME *v, FATX_FCB *dir);
FATX_FCB *FatxFindOpenFcb(FATX_FCB *dir, const CHAR *Name, ULONG Length);
FATX_FCB *FatxCreateFcb(FATX_FCB *Parent, const FATX_ENTRY *e, ULONG Index);
void FatxDereferenceFcb(FATX_FCB *f);
NTSTATUS FatxSetFileSize(FATX_VOLUME *v, FATX_FCB *f, ULONG NewSize);

// exFAT (fatx/exfat.cpp)
NTSTATUS ExfatMountVolume(PDEVICE_OBJECT Target, PCSZ Folder);
NTSTATUS ExfatLookup(FATX_VOLUME *v, FATX_FCB *dir, const OBJECT_STRING *Name, FATX_ENTRY *Found, ULONG *Index);
NTSTATUS ExfatReadEntry(FATX_VOLUME *v, FATX_FCB *dir, ULONG Index, FATX_ENTRY *Entry);
NTSTATUS ExfatNextEntry(FATX_VOLUME *v, FATX_FCB *dir, ULONG *Index, FATX_ENTRY *Entry);
NTSTATUS ExfatCreateDirent(FATX_VOLUME *v, FATX_FCB *dir, const OBJECT_STRING *Name, UCHAR Attributes,
                           ULONG FirstCluster, ULONG *Index);
NTSTATUS ExfatFlushFcb(FATX_VOLUME *v, FATX_FCB *f);
NTSTATUS ExfatRemoveDirent(FATX_VOLUME *v, FATX_FCB *f);
NTSTATUS ExfatIsDirectoryEmpty(FATX_VOLUME *v, FATX_FCB *dir);
NTSTATUS ExfatInitDirectory(FATX_VOLUME *v, ULONG *First);
NTSTATUS ExfatMakeChain(FATX_VOLUME *v, FATX_FCB *f); // a NoFatChain file gets FAT entries (before it changes size)
NTSTATUS ExfatFreeFile(FATX_VOLUME *v, FATX_FCB *f);
bool ExfatClusterFree(FATX_VOLUME *v, ULONG Cluster, NTSTATUS *Status);
NTSTATUS ExfatMarkCluster(FATX_VOLUME *v, ULONG Cluster, bool Used);
NTSTATUS ExfatVolumeTransfer(FATX_VOLUME *v, PIRP Irp);
void ExfatVolumeSize(FATX_VOLUME *v, ULONG *Total, ULONG *Free, ULONG *SectorsPerUnit);

extern DRIVER_OBJECT FatxDriverObject;
static inline FATX_VOLUME *FatxVolume(PDEVICE_OBJECT dev) { return (FATX_VOLUME *)dev->DeviceExtension; }
