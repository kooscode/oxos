// HDDFS, the hard disk's file system driver (src/filesystems/hddfs/; the files named below are there): one driver for
// FATX volumes (spec/stock/04 §6, spec/stock/04b) and the folder partitions of an exFAT disk
// (spec/oxos_internal/07_exfat_driver.md). Titles see every volume as "FATX".
//
// Names say which format code serves: HDDFS for what both share (HDDFSLookup, HDDFS_VOLUME; dispatch.cpp, volume.cpp,
// open_files.cpp), FATX for FATX only (FATXLookup, *_fatx.cpp), EXFAT for exFAT only (EXFATWalk, EXFATFlags,
// *_exfat.cpp). The format names are in capitals so they cannot be read as the NT executive's Ex prefix
// (ExAllocatePoolWithTag).
#pragma once
#include <xk/filesystems.h>
#include <xk/storage.h>
#include <xk/runtime.h>

// --- FATX's on-disk format. Its name limits apply on exFAT volumes too: titles expect them.

constexpr ULONG FATX_SIGNATURE = 0x58544146; // "FATX"
constexpr ULONG FATX_MAX_NAME = 42, FATX_MAX_PATH = 250;
constexpr UCHAR FATX_DIRENT_NEVER_USED = 0x00, FATX_DIRENT_NEVER_USED2 = 0xFF, FATX_DIRENT_DELETED = 0xE5;

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

// --- Shared by both formats

// FAT entry values (the same on FATX32 and exFAT) and the block size of the cache.
constexpr ULONG HDDFS_CLUSTER_FREE = 0, HDDFS_CLUSTER_LAST = 0xFFFFFFFF, HDDFS_CLUSTER_BAD = 0xFFFFFFF7;
constexpr ULONG HDDFS_BLOCK_SIZE = 0x1000;

// The results of a create (NT's values; the file attributes are in xk/io.h).
constexpr ULONG FILE_SUPERSEDED = 0, FILE_OPENED = 1, FILE_CREATED = 2, FILE_OVERWRITTEN = 3;

// EXFATFlags: the file's clusters are consecutive; its FAT entries are not used.
constexpr UCHAR EXFAT_NOFATCHAIN = 0x01;

// A directory entry as the layers exchange it: the FATX fields, plus what an exFAT entry set adds.
struct HDDFS_ENTRY : FATX_DIRENT {
    UCHAR EXFATFlags;         // EXFAT_NOFATCHAIN
    UCHAR EXFATEntries;       // entries in the set (file, stream, names)
    ULONG EXFATAllocClusters; // clusters allocated (DataLength rounded up)
    ULONG EXFATDirLength;     // directories: DataLength
    ULONG FileSizeHigh;       // exFAT files of 4 GB or more: the size's high dword (read-only)
};

constexpr ULONG FCB_DIRECTORY = 0x02, FCB_DELETE_PENDING = 0x10, FCB_ROOT = 0x20, FCB_VOLUME = 0x40,
                FCB_DIRENT_DIRTY = 0x80,
                FCB_EXFAT_DISK_ROOT = 0x100; // the exFAT root directory: no length, its FAT chain ends it

struct HDDFS_FCB {
    LIST_ENTRY SiblingLink;
    LIST_ENTRY ChildList;
    HDDFS_FCB *Parent;
    ULONG Flags;
    LONG ReferenceCount; // file objects not yet closed, plus open children
    // Titles' XGetFilePhysicalSortKey reads the first cluster at FsContext + 0x1C on a "FATX" volume (and returns 0
    // when bit 0 of the first byte is set: SiblingLink is a pointer, so it never is). spec/oxos_internal/25.
    ULONG FirstCluster;
    LONG CleanupCount; // file objects not yet cleaned up
    SHARE_ACCESS ShareAccess;
    UCHAR Attributes;
    UCHAR NameLength;
    CHAR Name[FATX_MAX_NAME];
    ULONG FileSize;
    ULONG DirentIndex;                   // position of the entry in the parent directory
    ULONG CreationTime, LastWriteTime, LastAccessTime;
    ULONG HintClusterIndex, HintCluster; // last chain position looked up
    UCHAR EXFATFlags, EXFATEntries;      // exFAT (HDDFS_ENTRY)
    ULONG EXFATAllocClusters, EXFATDirLength;
    ULONG FileSizeHigh;                  // exFAT: files of 4 GB or more (read-only: FATX sizes are 32-bit)
};

XK_OFFSET(HDDFS_FCB, FirstCluster, 0x1C);

inline ULONGLONG HDDFSFileSize64(const HDDFS_FCB *f) {
    return (ULONGLONG)f->FileSizeHigh << 32 | f->FileSize;
}

// Per file object (FsContext2): directory enumeration state.
struct HDDFS_CCB {
    ULONG NextIndex;
    BOOLEAN HaveMask;
    UCHAR MaskLength;
    CHAR Mask[FATX_MAX_NAME + 2];
};

struct HDDFS_ALLOC {
    ULONG FreeClusters;
    ULONG AllocationHint;
};
struct EXFAT_DISK;

// A format's directory operations (directory_fatx.cpp, directory_exfat.cpp), chosen when the volume is mounted.
struct HDDFS_VOLUME;
struct HDDFS_FORMAT {
    NTSTATUS (*Lookup)(HDDFS_VOLUME *v, HDDFS_FCB *dir, const OBJECT_STRING *Name, HDDFS_ENTRY *Found, ULONG *Index);
    NTSTATUS (*ReadEntry)(HDDFS_VOLUME *v, HDDFS_FCB *dir, ULONG Index, HDDFS_ENTRY *Entry);
    NTSTATUS (*NextEntry)(HDDFS_VOLUME *v, HDDFS_FCB *dir, ULONG *Index, HDDFS_ENTRY *Entry);
    NTSTATUS (*InitDirectory)(HDDFS_VOLUME *v, ULONG *First);
    NTSTATUS (*RemoveDirent)(HDDFS_VOLUME *v, HDDFS_FCB *f);
    NTSTATUS (*CreateDirent)(HDDFS_VOLUME *v, HDDFS_FCB *dir, const OBJECT_STRING *Name, UCHAR Attributes,
                             ULONG FirstCluster, ULONG *Index);
    NTSTATUS (*FlushFcb)(HDDFS_VOLUME *v, HDDFS_FCB *f);
    NTSTATUS (*DeleteDirent)(HDDFS_VOLUME *v, HDDFS_FCB *f);
    NTSTATUS (*IsDirectoryEmpty)(HDDFS_VOLUME *v, HDDFS_FCB *dir);
};

struct HDDFS_VOLUME {
    PDEVICE_OBJECT TargetDevice; // where the blocks are (exFAT: the exFAT partition)
    PDEVICE_OBJECT MountTarget;  // the device mounted (FATX: TargetDevice; exFAT: the folder partition)
    BOOLEAN IsEXFAT;
    const HDDFS_FORMAT *Format;  // its directory operations
    ULONG FirstClusterNumber;    // 1 (FATX) or 2 (exFAT)
    HDDFS_ALLOC *Alloc;          // FATX: OwnAlloc; exFAT: shared by the disk's folder partitions
    HDDFS_ALLOC OwnAlloc;
    EXFAT_DISK *EXFATDisk;
    ULONG PartitionNumber;       // exFAT: the folder partition's number
    ULONG UsedClusters;          // exFAT X/Y/Z: clusters in the folder (their size is fixed at 750 MB)
    ULONG OwnClusters;           // exFAT X/Y/Z: of those, the folder's own directory clusters (at the last count)
    LONGLONG ReportedLength;     // exFAT: the folder partition's reported length
    LONGLONG PartitionLength;
    ULONG SectorSize;
    UCHAR ClusterShift;
    UCHAR FatEntryShift;    // 1 = FATX16, 2 = FATX32
    BOOLEAN Dismounted;
    ULONG BytesPerCluster;
    ULONG NumberOfClusters; // data clusters are numbered FirstClusterNumber..FirstClusterNumber+NumberOfClusters-1
    ULONG FatByteOffset;
    LONGLONG DataByteOffset;
    ULONG SerialNumber;
    HDDFS_FCB RootFcb;
    HDDFS_FCB VolumeFcb;              // opens of the volume itself
    FSCACHE_EXTENSION CacheExtension; // a FATX partition's key in the file system cache
    // The key its blocks are cached under: &CacheExtension on FATX; on exFAT one key for the whole disk, shared by
    // every folder volume (C:, E:, X:, ...), so a block (the allocation bitmap, the FAT) has one cached copy. With
    // a key each, a folder volume wrote back its stale copy of a bitmap sector over another's change.
    FSCACHE_EXTENSION *Cache;
};

// --- Volume layer (volume.cpp): shared by both formats. One lock serves all volumes; blocks come from the file system
// cache (filesystems/cache.cpp) and a block mapped while the lock is held stays usable until the lock is released.

void HDDFSForgetVolume(HDDFS_VOLUME *v);
void HDDFSAcquireVolume(HDDFS_VOLUME *v);
void HDDFSReleaseVolume(HDDFS_VOLUME *v);
NTSTATUS HDDFSDeviceIo(HDDFS_VOLUME *v, UCHAR Major, LONGLONG Offset, PVOID Buffer, ULONG Length);
NTSTATUS HDDFSMapBlock(HDDFS_VOLUME *v, LONGLONG Offset, PUCHAR *Data);
NTSTATUS HDDFSWriteBlockRange(HDDFS_VOLUME *v, LONGLONG BlockOffset, ULONG Start, ULONG Length);
void HDDFSInvalidateRange(HDDFS_VOLUME *v, LONGLONG Offset, ULONG Length);

// Clusters: the FAT, allocation and the chain of a file.
NTSTATUS HDDFSGetFatEntry(HDDFS_VOLUME *v, ULONG Cluster, ULONG *Value);
NTSTATUS HDDFSSetFatEntry(HDDFS_VOLUME *v, ULONG Cluster, ULONG Value);
NTSTATUS HDDFSAllocateClusters(HDDFS_VOLUME *v, ULONG PreviousLast, ULONG Count, ULONG *First);
NTSTATUS HDDFSFreeChain(HDDFS_VOLUME *v, ULONG First, ULONG Limit = ~0u);
NTSTATUS HDDFSFileChain(HDDFS_VOLUME *v, HDDFS_FCB *f, ULONG *Count, ULONG *Last);
NTSTATUS HDDFSClusterAtIndex(HDDFS_VOLUME *v, HDDFS_FCB *f, ULONG Index, ULONG *Cluster);
NTSTATUS HDDFSChainLength(HDDFS_VOLUME *v, ULONG First, ULONG *Count, ULONG *Last);
NTSTATUS HDDFSNextCluster(HDDFS_VOLUME *v, HDDFS_FCB *f, ULONG Cluster, ULONG *Next);
LONGLONG HDDFSClusterOffset(HDDFS_VOLUME *v, ULONG Cluster);
bool HDDFSIsValidCluster(HDDFS_VOLUME *v, ULONG Cluster);
void HDDFSVolumeSize(HDDFS_VOLUME *v, ULONG *Total, ULONG *Free, ULONG *SectorsPerUnit);
NTSTATUS HDDFSSetFileSize(HDDFS_VOLUME *v, HDDFS_FCB *f, ULONG NewSize); // grow or shrink the file's chain

// Names (FATX's rules, on both formats) and timestamps (FATX's format in the FCB).
bool HDDFSIsValidName(const OBJECT_STRING *Name);
bool HDDFSNameEqual(const CHAR *a, ULONG alen, const CHAR *b, ULONG blen);
bool HDDFSWildcardMatch(const CHAR *mask, ULONG mlen, const CHAR *name, ULONG nlen);
ULONG HDDFSCurrentTimestamp();
void HDDFSTimestampToTime(ULONG Stamp, PLARGE_INTEGER Time);
ULONG HDDFSTimeToTimestamp(const LARGE_INTEGER *Time);

// --- Open files (open_files.cpp): the FCB tree.

HDDFS_FCB *HDDFSFindOpenFcb(HDDFS_FCB *dir, const CHAR *Name, ULONG Length);
HDDFS_FCB *HDDFSCreateFcb(HDDFS_FCB *Parent, const HDDFS_ENTRY *e, ULONG Index);
void HDDFSDereferenceFcb(HDDFS_FCB *f);

// --- Directory layer: each operation goes to the volume's format (HDDFS_VOLUME::Format).

// An entry by name; STATUS_OBJECT_NAME_NOT_FOUND if there is none.
inline NTSTATUS HDDFSLookup(HDDFS_VOLUME *v, HDDFS_FCB *dir, const OBJECT_STRING *Name, HDDFS_ENTRY *Found,
                            ULONG *Index) {
    return v->Format->Lookup(v, dir, Name, Found, Index);
}

// The entry at Index (an index Lookup, NextEntry or CreateDirent returned).
inline NTSTATUS HDDFSReadEntry(HDDFS_VOLUME *v, HDDFS_FCB *dir, ULONG Index, HDDFS_ENTRY *Entry) {
    return v->Format->ReadEntry(v, dir, Index, Entry);
}

// The first visible entry at *Index or after it; STATUS_END_OF_FILE after the last.
inline NTSTATUS HDDFSNextEntry(HDDFS_VOLUME *v, HDDFS_FCB *dir, ULONG *Index, HDDFS_ENTRY *Entry) {
    return v->Format->NextEntry(v, dir, Index, Entry);
}

// One cluster for a new directory.
inline NTSTATUS HDDFSInitDirectory(HDDFS_VOLUME *v, ULONG *First) {
    return v->Format->InitDirectory(v, First);
}

// Drop the entry only (a move).
inline NTSTATUS HDDFSRemoveDirent(HDDFS_VOLUME *v, HDDFS_FCB *f) {
    return v->Format->RemoveDirent(v, f);
}

// A new entry; *Index is where it is.
inline NTSTATUS HDDFSCreateDirent(HDDFS_VOLUME *v, HDDFS_FCB *dir, const OBJECT_STRING *Name, UCHAR Attributes,
                                  ULONG FirstCluster, ULONG *Index) {
    return v->Format->CreateDirent(v, dir, Name, Attributes, FirstCluster, Index);
}

// Write the FCB's size, cluster, attributes and times back into its entry.
inline NTSTATUS HDDFSFlushFcb(HDDFS_VOLUME *v, HDDFS_FCB *f) {
    return v->Format->FlushFcb(v, f);
}

// Delete the entry and free the file's clusters.
inline NTSTATUS HDDFSDeleteDirent(HDDFS_VOLUME *v, HDDFS_FCB *f) {
    return v->Format->DeleteDirent(v, f);
}

// STATUS_SUCCESS, or STATUS_DIRECTORY_NOT_EMPTY.
inline NTSTATUS HDDFSIsDirectoryEmpty(HDDFS_VOLUME *v, HDDFS_FCB *dir) {
    return v->Format->IsDirectoryEmpty(v, dir);
}

// --- FATX: its directory entries (directory_fatx.cpp) and its mount (volume_fatx.cpp).

NTSTATUS FATXLookup(HDDFS_VOLUME *v, HDDFS_FCB *dir, const OBJECT_STRING *Name, HDDFS_ENTRY *Found, ULONG *Index);
NTSTATUS FATXReadEntry(HDDFS_VOLUME *v, HDDFS_FCB *dir, ULONG Index, HDDFS_ENTRY *Entry);
NTSTATUS FATXNextEntry(HDDFS_VOLUME *v, HDDFS_FCB *dir, ULONG *Index, HDDFS_ENTRY *Entry);
NTSTATUS FATXInitDirectory(HDDFS_VOLUME *v, ULONG *First);
NTSTATUS FATXRemoveDirent(HDDFS_VOLUME *v, HDDFS_FCB *f);
NTSTATUS FATXCreateDirent(HDDFS_VOLUME *v, HDDFS_FCB *dir, const OBJECT_STRING *Name, UCHAR Attributes,
                          ULONG FirstCluster, ULONG *Index);
NTSTATUS FATXFlushFcb(HDDFS_VOLUME *v, HDDFS_FCB *f);
NTSTATUS FATXDeleteDirent(HDDFS_VOLUME *v, HDDFS_FCB *f);
NTSTATUS FATXIsDirectoryEmpty(HDDFS_VOLUME *v, HDDFS_FCB *dir);
NTSTATUS FATXMountVolume(PDEVICE_OBJECT TargetDevice);
extern const HDDFS_FORMAT FATXFormat;

// --- exFAT (spec/oxos_internal/07_exfat_driver.md): its entry sets and folder walks (directory_exfat.cpp), its
// allocation bitmap, folder partitions and mount (volume_exfat.cpp).

// The exFAT disk all folder volumes share (volume_exfat.cpp).
struct EXFAT_DISK {
    BOOLEAN Mounted;
    UCHAR ClusterShift;
    ULONG BytesPerCluster, ClusterCount, FatByteOffset, Serial;
    LONGLONG HeapByteOffset, BitmapByteOffset;
    HDDFS_ALLOC Alloc;
    USHORT Upcase[256];
    HDDFS_FCB Root; // the volume's root directory (it has no entry set)
};
extern EXFAT_DISK EXFATDisk;

// Directory entry types and an entry set as read from the disk.
constexpr UCHAR EXFAT_ENTRY_END = 0x00, EXFAT_ENTRY_BITMAP = 0x81, EXFAT_ENTRY_UPCASE = 0x82, EXFAT_ENTRY_FILE = 0x85,
                EXFAT_ENTRY_STREAM = 0xC0, EXFAT_ENTRY_NAME = 0xC1;
constexpr ULONG EXFAT_MAX_SET = 19; // file + stream + 17 name entries (255 characters)
struct EXFAT_SET {
    UCHAR e[EXFAT_MAX_SET][32];
    ULONG Count;
};

NTSTATUS EXFATLookup(HDDFS_VOLUME *v, HDDFS_FCB *dir, const OBJECT_STRING *Name, HDDFS_ENTRY *Found, ULONG *Index);
NTSTATUS EXFATReadEntry(HDDFS_VOLUME *v, HDDFS_FCB *dir, ULONG Index, HDDFS_ENTRY *Entry);
NTSTATUS EXFATNextEntry(HDDFS_VOLUME *v, HDDFS_FCB *dir, ULONG *Index, HDDFS_ENTRY *Entry);
NTSTATUS EXFATInitDirectory(HDDFS_VOLUME *v, ULONG *First);
NTSTATUS EXFATRemoveDirent(HDDFS_VOLUME *v, HDDFS_FCB *f);
NTSTATUS EXFATCreateDirent(HDDFS_VOLUME *v, HDDFS_FCB *dir, const OBJECT_STRING *Name, UCHAR Attributes,
                           ULONG FirstCluster, ULONG *Index);
NTSTATUS EXFATFlushFcb(HDDFS_VOLUME *v, HDDFS_FCB *f);
NTSTATUS EXFATDeleteDirent(HDDFS_VOLUME *v, HDDFS_FCB *f);
NTSTATUS EXFATIsDirectoryEmpty(HDDFS_VOLUME *v, HDDFS_FCB *dir);
NTSTATUS EXFATMakeChain(HDDFS_VOLUME *v, HDDFS_FCB *f); // a NoFatChain file gets FAT entries (before it changes size)
bool EXFATClusterFree(HDDFS_VOLUME *v, ULONG Cluster, NTSTATUS *Status);
bool EXFATMayAllocate(HDDFS_VOLUME *v, ULONG Count);    // X/Y/Z hold at most their retail 750 MB
NTSTATUS EXFATMarkCluster(HDDFS_VOLUME *v, ULONG Cluster, bool Used);
NTSTATUS EXFATVolumeTransfer(HDDFS_VOLUME *v, PIRP Irp);
void EXFATVolumeSize(HDDFS_VOLUME *v, ULONG *Total, ULONG *Free, ULONG *SectorsPerUnit);
NTSTATUS EXFATMountVolume(PDEVICE_OBJECT Target, PCSZ Folder);
extern const HDDFS_FORMAT EXFATFormat;

// Shared by the two exFAT files: directory_exfat.cpp's slots and folder walks, volume_exfat.cpp's cluster helpers.
NTSTATUS EXFATMapSlot(HDDFS_VOLUME *v, HDDFS_FCB *dir, ULONG Slot, PUCHAR *Entry, LONGLONG *Block);
bool EXFATIsCacheFolder(HDDFS_VOLUME *v); // X:, Y: or Z:
NTSTATUS EXFATWalk(HDDFS_VOLUME *v, HDDFS_FCB *dir, bool Delete, ULONG Depth, ULONG *Used);
NTSTATUS EXFATCountFolder(HDDFS_VOLUME *v);
NTSTATUS EXFATZeroCluster(HDDFS_VOLUME *v, ULONG Cluster);
NTSTATUS EXFATFreeFile(HDDFS_VOLUME *v, HDDFS_FCB *f);

// --- Requests (dispatch.cpp): the driver every volume of either format belongs to.

extern DRIVER_OBJECT HDDFSDriverObject;
static inline HDDFS_VOLUME *HDDFSVolume(PDEVICE_OBJECT dev) {
    return (HDDFS_VOLUME *)dev->DeviceExtension;
}
