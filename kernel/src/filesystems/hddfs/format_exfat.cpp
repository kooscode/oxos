// Formatting an exFAT volume (spec/oxos_internal/30_disk_format.md): the boot regions, the FAT, the allocation bitmap,
// the up-case table and the root with the folders C E F X Y Z OXOS (and one file), laid out as the exFAT specification
// describes; the code and the up-case table's rules are OXOS's own. storage/disk_format.cpp puts the GPT around it.
// Every sector goes out through the caller's DISK_FORMAT_IO.
#include <xk/disk_format.h>
#include <xk/runtime.h>

constexpr NTSTATUS STATUS_INVALID_PARAMETER_ = (NTSTATUS)0xC000000D, STATUS_NAME_INVALID_ = (NTSTATUS)0xC0000033,
                   STATUS_DISK_FULL_ = (NTSTATUS)0xC000007F;
constexpr ULONG SCRATCH_SECTORS = DISK_FORMAT_SCRATCH / 512;

ULONG EXFATTimestamp(ULONG Year, ULONG Month, ULONG Day, ULONG Hour, ULONG Minute, ULONG Second) {
    if (Year < 1980)
        Year = 1980;
    return ((Year - 1980) << 25) | (Month << 21) | (Day << 16) | (Hour << 11) | (Minute << 5) | (Second / 2);
}

// --- The up-case table (OXOS's rules: Basic Latin, Latin-1, Latin Extended-A, Greek, Cyrillic, full-width Latin)

static USHORT Upcase(USHORT c) {
    if ((c >= 'a' && c <= 'z') || (c >= 0xE0 && c <= 0xFE && c != 0xF7) || (c >= 0x3B1 && c <= 0x3CB && c != 0x3C2) ||
        (c >= 0x430 && c <= 0x44F) || (c >= 0xFF41 && c <= 0xFF5A))
        return (USHORT)(c - 0x20);
    if (c == 0xFF)
        return 0x178;
    if (c == 0x3C2)
        return 0x3A3; // final sigma
    if (c >= 0x450 && c <= 0x45F)
        return (USHORT)(c - 0x50);
    if (c >= 0x100 && c <= 0x17E && c != 0x131) { // Latin Extended-A: capital and small letters in pairs
        bool oddSmall = c <= 0x137 || (c >= 0x14A && c <= 0x177);
        bool evenSmall = (c >= 0x139 && c <= 0x148) || c >= 0x179;
        if ((oddSmall && (c & 1)) || (evenSmall && !(c & 1)))
            return (USHORT)(c - 1);
    }
    return c;
}

// The compressed table (0xFFFF n: the next n characters map to themselves) into Table; returns its length in bytes.
static ULONG BuildUpcase(PUCHAR Table) {
    ULONG n = 0, run = 0;
    auto flush = [&]() {
        if (run) {
            DiskFormatPut16(Table + n, 0xFFFF);
            DiskFormatPut16(Table + n + 2, (USHORT)run);
            n += 4;
            run = 0;
        }
    };
    for (ULONG c = 0; c <= 0xFFFF; c++) {
        USHORT up = Upcase((USHORT)c);
        if (up == c && run < 0xFFFF) {
            run++;
            continue;
        }
        flush();
        if (up == c) {
            run = 1;
        } else {
            DiskFormatPut16(Table + n, up);
            n += 2;
        }
    }
    flush();
    return n;
}

static ULONG TableChecksum(const UCHAR *p, ULONG n) {
    ULONG sum = 0;
    for (ULONG i = 0; i < n; i++)
        sum = ((sum & 1) ? 0x80000000 : 0) + (sum >> 1) + p[i];
    return sum;
}

// --- Directory entries

constexpr UCHAR ENTRY_BITMAP = 0x81, ENTRY_UPCASE = 0x82, ENTRY_LABEL = 0x83, ENTRY_FILE = 0x85, ENTRY_STREAM = 0xC0,
                ENTRY_NAME = 0xC1;

static USHORT NameHash(const char *Name, ULONG Length) {
    USHORT hash = 0;
    for (ULONG i = 0; i < Length; i++) {
        USHORT c = Upcase((UCHAR)Name[i]);
        hash = (USHORT)(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + (c & 0xFF));
        hash = (USHORT)(((hash & 1) ? 0x8000 : 0) + (hash >> 1) + (c >> 8));
    }
    return hash;
}

// A file or folder's entry set (File, Stream extension, Name entries) at At; returns the number of entries.
static ULONG EntrySet(PUCHAR At, const char *Name, ULONG Length, bool Folder, ULONG FirstCluster,
                         ULONGLONG Size, ULONG Timestamp) {
    ULONG names = (Length + 14) / 15, count = 2 + names;
    RtlZeroMemory(At, count * 32);
    At[0] = ENTRY_FILE;
    At[1] = (UCHAR)(count - 1);
    DiskFormatPut16(At + 4, Folder ? 0x10 : 0x20);
    DiskFormatPut32(At + 8, Timestamp);
    DiskFormatPut32(At + 12, Timestamp);
    DiskFormatPut32(At + 16, Timestamp);
    PUCHAR s = At + 32;
    s[0] = ENTRY_STREAM;
    s[1] = 0x01; // allocation possible, FAT chain
    s[3] = (UCHAR)Length;
    DiskFormatPut16(s + 4, NameHash(Name, Length));
    DiskFormatPut64(s + 8, Size);
    DiskFormatPut32(s + 20, FirstCluster);
    DiskFormatPut64(s + 24, Size);
    for (ULONG i = 0; i < names; i++) {
        PUCHAR e = At + 64 + 32 * i;
        e[0] = ENTRY_NAME;
        for (ULONG k = 0; k < 15 && i * 15 + k < Length; k++)
            DiskFormatPut16(e + 2 + 2 * k, (UCHAR)Name[i * 15 + k]);
    }
    USHORT sum = 0;
    for (ULONG i = 0; i < count * 32; i++) {
        if (i != 2 && i != 3)
            sum = (USHORT)(((sum & 1) ? 0x8000 : 0) + (sum >> 1) + At[i]);
    }
    DiskFormatPut16(At + 2, sum);
    return count;
}

// --- The volume's contents: the folders and the file, as a small tree of nodes

constexpr ULONG MAX_NODES = 16;
struct FORMAT_NODE {
    char Name[DISK_FORMAT_NAME + 1];
    ULONG Length;
    LONG Parent; // -1 = the root
    bool Folder;
    ULONG FirstCluster, Clusters;
    ULONGLONG Size;
};

struct FORMAT_TREE {
    FORMAT_NODE Nodes[MAX_NODES];
    ULONG Count;
};

static bool SameName(const char *a, ULONG an, const char *b, ULONG bn) {
    if (an != bn)
        return false;
    for (ULONG i = 0; i < an; i++) {
        if (Upcase((UCHAR)a[i]) != Upcase((UCHAR)b[i]))
            return false;
    }
    return true;
}

// The node named Name under Parent, added when missing; -1 when the tree is full or the name is not allowed.
static LONG AddNode(FORMAT_TREE *t, LONG Parent, const char *Name, ULONG Length, bool Folder) {
    for (ULONG i = 0; i < t->Count; i++) {
        if (t->Nodes[i].Parent == Parent && SameName(t->Nodes[i].Name, t->Nodes[i].Length, Name, Length))
            return t->Nodes[i].Folder == Folder ? (LONG)i : -1;
    }
    if (t->Count == MAX_NODES || !Length || Length > DISK_FORMAT_NAME)
        return -1;
    for (ULONG i = 0; i < Length; i++) {
        char c = Name[i];
        if (c < 0x20 || c > 0x7E || c == '"' || c == '*' || c == '/' || c == ':' || c == '<' || c == '>' ||
            c == '?' || c == '\\' || c == '|')
            return -1;
    }
    FORMAT_NODE *n = &t->Nodes[t->Count];
    RtlZeroMemory(n, sizeof(*n));
    RtlCopyMemory(n->Name, Name, Length);
    n->Length = Length;
    n->Parent = Parent;
    n->Folder = Folder;
    return (LONG)t->Count++;
}

// Root, folders and the file in one directory cluster each (a folder's entries: at most 16 sets of up to 7 entries,
// 3.5 KB, less than any cluster).
static bool BuildTree(FORMAT_TREE *t, const DISK_FORMAT_FILE *File) {
    static const char *const folders[] = {"C", "E", "F", "X", "Y", "Z", "OXOS"};
    t->Count = 0;
    for (const char *name : folders) {
        ULONG n = 0;
        while (name[n])
            n++;
        AddNode(t, -1, name, n, true);
    }
    if (!File)
        return true;
    LONG parent = -1;
    const char *p = File->Path;
    for (;;) {
        ULONG n = 0;
        while (p[n] && p[n] != '\\')
            n++;
        bool last = !p[n];
        parent = AddNode(t, parent, p, n, !last);
        if (parent < 0)
            return false;
        if (last)
            break;
        p += n + 1;
    }
    t->Nodes[parent].Size = File->Length;
    return File->Length != 0;
}

// --- Formatting

struct FORMAT_GEOMETRY {
    ULONGLONG Start, Sectors; // the partition
    ULONG SpcShift, Spc;      // sectors per cluster
    ULONG FatOffset, FatLength, Heap, Count;
};

static void Geometry(ULONGLONG Start, ULONGLONG Sectors, FORMAT_GEOMETRY *g) {
    g->Start = Start;
    g->Sectors = Sectors;
    g->SpcShift = g->Sectors < (32ull << 21) ? 6 : 8; // 32 KB below 32 GB, else 128 KB
    g->Spc = 1u << g->SpcShift;
    g->FatOffset = 2048;
    ULONGLONG count = (g->Sectors - g->FatOffset) >> g->SpcShift;
    for (int pass = 0; pass < 3; pass++) {
        ULONGLONG fatBytes = (count + 2) * 4;
        g->FatLength = (ULONG)(((fatBytes + 511) / 512 + g->Spc - 1) & ~(ULONGLONG)(g->Spc - 1));
        g->Heap = g->FatOffset + g->FatLength;
        count = (g->Sectors - g->Heap) >> g->SpcShift;
    }
    g->Count = count > 0xFFFFFFF5 ? 0xFFFFFFF5 : (ULONG)count;
}

// A cluster's first disk sector.
static ULONGLONG ClusterLba(const FORMAT_GEOMETRY *g, ULONG Cluster) {
    return g->Start + g->Heap + ((ULONGLONG)(Cluster - 2) << g->SpcShift);
}

// The clusters handed out, in order from cluster 2: each run is chained in the FAT.
struct FORMAT_RUNS {
    ULONG First[MAX_NODES + 3], Count[MAX_NODES + 3];
    ULONG Runs, Next;
};

static ULONG Allocate(FORMAT_RUNS *r, ULONG Clusters) {
    r->First[r->Runs] = r->Next;
    r->Count[r->Runs++] = Clusters;
    r->Next += Clusters;
    return r->Next - Clusters;
}

static ULONG FatEntry(const FORMAT_RUNS *r, ULONG Cluster) {
    if (Cluster == 0)
        return 0xFFFFFFF8; // media type
    if (Cluster == 1)
        return 0xFFFFFFFF;
    for (ULONG i = 0; i < r->Runs; i++) {
        if (Cluster >= r->First[i] && Cluster < r->First[i] + r->Count[i])
            return Cluster + 1 == r->First[i] + r->Count[i] ? 0xFFFFFFFF : Cluster + 1;
    }
    return 0;
}

// The FAT (entries for the runs, zeros after them) and the allocation bitmap (clusters 2 to Next - 1 in use).
static NTSTATUS WriteFatAndBitmap(const FORMAT_GEOMETRY *g, const FORMAT_RUNS *r, ULONG BitmapCluster,
                                     const DISK_FORMAT_IO *Io, PUCHAR Scratch) {
    ULONGLONG fatLba = g->Start + g->FatOffset;
    for (ULONG done = 0; done < g->FatLength;) {
        ULONG n = g->FatLength - done < SCRATCH_SECTORS ? g->FatLength - done : SCRATCH_SECTORS;
        ULONG firstEntry = done * 128;
        if (firstEntry >= r->Next) { // only zeros from here
            NTSTATUS status = IdexDiskWriteZeros(Io, Scratch, fatLba + done, g->FatLength - done);
            if (!NT_SUCCESS(status))
                return status;
            break;
        }
        for (ULONG i = 0; i < n * 128; i++)
            DiskFormatPut32(Scratch + 4 * i, FatEntry(r, firstEntry + i));
        NTSTATUS status = Io->Write(Io->Context, fatLba + done, Scratch, n);
        if (!NT_SUCCESS(status))
            return status;
        done += n;
    }
    // The bitmap: bit (c - 2) for cluster c.
    ULONG bytes = (g->Count + 7) / 8, sectors = (bytes + 511) / 512;
    ULONGLONG lba = ClusterLba(g, BitmapCluster);
    for (ULONG done = 0; done < sectors;) {
        ULONG n = sectors - done < SCRATCH_SECTORS ? sectors - done : SCRATCH_SECTORS;
        RtlZeroMemory(Scratch, DISK_FORMAT_SCRATCH);
        for (ULONG c = 2 + done * 4096; c < r->Next && c < 2 + (done + n) * 4096; c++)
            Scratch[(c - 2 - done * 4096) / 8] |= (UCHAR)(1 << ((c - 2) & 7));
        NTSTATUS status = Io->Write(Io->Context, lba + done, Scratch, n);
        if (!NT_SUCCESS(status))
            return status;
        done += n;
    }
    return STATUS_SUCCESS;
}

// One directory cluster: Entries bytes from Scratch, the rest zeros.
static NTSTATUS WriteDirectory(const FORMAT_GEOMETRY *g, ULONG Cluster, ULONG Bytes, const DISK_FORMAT_IO *Io,
                                  PUCHAR Scratch) {
    ULONG sectors = (Bytes + 511) / 512;
    NTSTATUS status = Io->Write(Io->Context, ClusterLba(g, Cluster), Scratch, sectors);
    if (NT_SUCCESS(status) && sectors < g->Spc)
        status = IdexDiskWriteZeros(Io, Scratch, ClusterLba(g, Cluster) + sectors, g->Spc - sectors);
    return status;
}

// A folder's entries: the sets of the nodes whose parent is Parent.
static ULONG FolderEntries(const FORMAT_TREE *t, LONG Parent, PUCHAR At, ULONG Timestamp) {
    ULONG n = 0;
    for (ULONG i = 0; i < t->Count; i++) {
        const FORMAT_NODE *e = &t->Nodes[i];
        if (e->Parent == Parent)
            n += EntrySet(At + n * 32, e->Name, e->Length, e->Folder, e->FirstCluster, e->Size, Timestamp);
    }
    return n * 32;
}

// The boot region (12 sectors) at Lba: boot sector, 8 extended boot sectors, OEM parameters, a reserved sector and
// the checksum sector.
static NTSTATUS WriteBootRegion(const FORMAT_GEOMETRY *g, ULONG Serial, ULONG RootCluster, ULONGLONG Lba,
                                   const DISK_FORMAT_IO *Io, PUCHAR Scratch) {
    RtlZeroMemory(Scratch, 12 * 512);
    PUCHAR b = Scratch;
    b[0] = 0xEB;
    b[1] = 0x76;
    b[2] = 0x90;
    RtlCopyMemory(b + 3, "EXFAT   ", 8);
    DiskFormatPut64(b + 64, g->Start);
    DiskFormatPut64(b + 72, g->Sectors);
    DiskFormatPut32(b + 80, g->FatOffset);
    DiskFormatPut32(b + 84, g->FatLength);
    DiskFormatPut32(b + 88, g->Heap);
    DiskFormatPut32(b + 92, g->Count);
    DiskFormatPut32(b + 96, RootCluster);
    DiskFormatPut32(b + 100, Serial);
    DiskFormatPut16(b + 104, 0x0100); // revision 1.00
    b[108] = 9;             // 512-byte sectors
    b[109] = (UCHAR)g->SpcShift;
    b[110] = 1;    // one FAT
    b[111] = 0x80; // drive select
    for (ULONG i = 120; i < 510; i++)
        b[i] = 0xF4; // boot code: hlt
    b[510] = 0x55;
    b[511] = 0xAA;
    for (ULONG s = 1; s <= 8; s++)
        DiskFormatPut32(Scratch + s * 512 + 508, 0xAA550000);
    ULONG sum = 0;
    for (ULONG i = 0; i < 11 * 512; i++) {
        if (i != 106 && i != 107 && i != 112)
            sum = ((sum & 1) ? 0x80000000 : 0) + (sum >> 1) + Scratch[i];
    }
    for (ULONG i = 0; i < 128; i++)
        DiskFormatPut32(Scratch + 11 * 512 + 4 * i, sum);
    return Io->Write(Io->Context, Lba, Scratch, 12);
}

NTSTATUS EXFATFormatVolume(ULONGLONG Start, ULONGLONG Sectors, ULONG Serial, ULONG Timestamp,
                           const DISK_FORMAT_FILE *File, const DISK_FORMAT_IO *Io, PUCHAR Scratch,
                           DISK_FORMAT_RESULT *Result) {
    if (Sectors < (1ull << 20)) // under 512 MiB
        return STATUS_INVALID_PARAMETER_;
    FORMAT_GEOMETRY g;
    Geometry(Start, Sectors, &g);
    FORMAT_TREE tree; // 1.5 KB of stack
    if (!BuildTree(&tree, File))
        return STATUS_NAME_INVALID_;
    ULONG clusterBytes = 512u << g.SpcShift;

    // Clusters in order: the bitmap, the up-case table, the root, then each folder and the file.
    FORMAT_RUNS runs = {};
    runs.Next = 2;
    ULONG bitmapBytes = (g.Count + 7) / 8;
    ULONG bitmapCluster = Allocate(&runs, (bitmapBytes + clusterBytes - 1) / clusterBytes);
    ULONG upcaseBytes = BuildUpcase(Scratch);
    ULONG upcaseSum = TableChecksum(Scratch, upcaseBytes);
    ULONG upcaseCluster = Allocate(&runs, (upcaseBytes + clusterBytes - 1) / clusterBytes);
    ULONG rootCluster = Allocate(&runs, 1);
    for (ULONG i = 0; i < tree.Count; i++) {
        FORMAT_NODE *n = &tree.Nodes[i];
        n->Clusters = n->Folder ? 1 : (ULONG)((n->Size + clusterBytes - 1) / clusterBytes);
        n->FirstCluster = Allocate(&runs, n->Clusters);
        if (n->Folder)
            n->Size = clusterBytes;
    }
    if (runs.Next - 2 > g.Count)
        return STATUS_DISK_FULL_;

    // The up-case table (still in Scratch).
    ULONG upcaseSectors = (upcaseBytes + 511) / 512;
    RtlZeroMemory(Scratch + upcaseBytes, upcaseSectors * 512 - upcaseBytes);
    NTSTATUS status = Io->Write(Io->Context, ClusterLba(&g, upcaseCluster), Scratch, upcaseSectors);
    if (NT_SUCCESS(status))
        status = WriteFatAndBitmap(&g, &runs, bitmapCluster, Io, Scratch);

    // The root: label, bitmap, up-case table, then the folders.
    if (NT_SUCCESS(status)) {
        RtlZeroMemory(Scratch, DISK_FORMAT_SCRATCH);
        PUCHAR e = Scratch;
        e[0] = ENTRY_LABEL;
        e[1] = 4;
        const char label[] = "OXOS";
        for (ULONG i = 0; i < 4; i++)
            DiskFormatPut16(e + 2 + 2 * i, (UCHAR)label[i]);
        e += 32;
        e[0] = ENTRY_BITMAP;
        DiskFormatPut32(e + 20, bitmapCluster);
        DiskFormatPut64(e + 24, bitmapBytes);
        e += 32;
        e[0] = ENTRY_UPCASE;
        DiskFormatPut32(e + 4, upcaseSum);
        DiskFormatPut32(e + 20, upcaseCluster);
        DiskFormatPut64(e + 24, upcaseBytes);
        ULONG bytes = 96 + FolderEntries(&tree, -1, Scratch + 96, Timestamp);
        status = WriteDirectory(&g, rootCluster, bytes, Io, Scratch);
    }
    // Each folder, and the file's data.
    for (ULONG i = 0; i < tree.Count && NT_SUCCESS(status); i++) {
        const FORMAT_NODE *n = &tree.Nodes[i];
        if (n->Folder) {
            RtlZeroMemory(Scratch, DISK_FORMAT_SCRATCH);
            ULONG bytes = FolderEntries(&tree, (LONG)i, Scratch, Timestamp);
            status = WriteDirectory(&g, n->FirstCluster, bytes ? bytes : 512, Io, Scratch);
            continue;
        }
        ULONG whole = File->Length / 512, tail = File->Length % 512;
        ULONGLONG lba = ClusterLba(&g, n->FirstCluster);
        if (whole)
            status = Io->Write(Io->Context, lba, File->Data, whole);
        if (NT_SUCCESS(status) && tail) {
            RtlZeroMemory(Scratch, 512);
            RtlCopyMemory(Scratch, File->Data + whole * 512, tail);
            status = Io->Write(Io->Context, lba + whole, Scratch, 1);
        }
    }

    // The boot regions last: the backup, then the main one.
    if (NT_SUCCESS(status))
        status = WriteBootRegion(&g, Serial, rootCluster, g.Start + 12, Io, Scratch);
    if (NT_SUCCESS(status))
        status = WriteBootRegion(&g, Serial, rootCluster, g.Start, Io, Scratch);
    if (NT_SUCCESS(status) && Result) {
        Result->PartitionStart = g.Start;
        Result->PartitionSectors = g.Sectors;
        Result->ClusterShift = g.SpcShift + 9;
        Result->ClusterCount = g.Count;
    }
    return status;
}
