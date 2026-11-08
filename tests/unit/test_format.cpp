// Formatting, deleting all partitions and recognising a blank disk (spec/oxos_internal/30_disk_format.md;
// kernel/src/storage/disk_format.cpp, kernel/src/filesystems/hddfs/format_exfat.cpp), against image files: the exFAT
// volume in one file (checked with the host's fsck.exfat), the sectors around it in memory.
#include "kernel_test.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include <fcntl.h>
#include <unistd.h>

#include <xk/disk_format.h>

namespace {
// A disk of Sectors sectors: the partition (from sector 2048 on) in a sparse file, everything else in a map.
struct Disk {
    ULONGLONG Sectors;
    std::string Dir, Volume;
    int Fd = -1;
    std::map<ULONGLONG, std::vector<UCHAR>> Outside;
    bool FailReads = false;
    DISK_FORMAT_IO Io;
    std::vector<UCHAR> Scratch = std::vector<UCHAR>(DISK_FORMAT_SCRATCH);

    explicit Disk(ULONGLONG sectors) : Sectors(sectors) {
        char tmpl[] = "/tmp/oxos-format-XXXXXX";
        Dir = mkdtemp(tmpl);
        Volume = Dir + "/volume.img";
        Fd = open(Volume.c_str(), O_RDWR | O_CREAT | O_TRUNC | O_LARGEFILE, 0600);
        // As large as the space for the partition (sparse), so fsck sees the volume's full length.
        EXPECT_EQ(ftruncate64(Fd, (off64_t)(sectors - 34 - DISK_FORMAT_PARTITION_START) * 512), 0);
        Io = {this, Write, Read};
    }
    ~Disk() {
        close(Fd);
        unlink(Volume.c_str());
        rmdir(Dir.c_str());
    }
    bool InVolume(ULONGLONG lba) const {
        return lba >= DISK_FORMAT_PARTITION_START && lba < Sectors - 34;
    }
    static NTSTATUS Write(PVOID context, ULONGLONG lba, const void *data, ULONG count) {
        auto d = (Disk *)context;
        EXPECT_LE(lba + count, d->Sectors) << "wrote past the disk's end";
        for (ULONG i = 0; i < count; i++) {
            const UCHAR *p = (const UCHAR *)data + 512 * i;
            if (d->InVolume(lba + i)) {
                EXPECT_EQ(pwrite64(d->Fd, p, 512, (off64_t)(lba + i - DISK_FORMAT_PARTITION_START) * 512), 512);
            } else {
                d->Outside[lba + i].assign(p, p + 512);
            }
        }
        return STATUS_SUCCESS;
    }
    static NTSTATUS Read(PVOID context, ULONGLONG lba, void *data, ULONG count) {
        auto d = (Disk *)context;
        if (d->FailReads)
            return (NTSTATUS)0xC0000185;
        for (ULONG i = 0; i < count; i++) {
            UCHAR *p = (UCHAR *)data + 512 * i;
            memset(p, 0, 512);
            if (d->InVolume(lba + i)) {
                EXPECT_GE(pread64(d->Fd, p, 512, (off64_t)(lba + i - DISK_FORMAT_PARTITION_START) * 512), 0);
            } else if (d->Outside.count(lba + i)) {
                memcpy(p, d->Outside[lba + i].data(), 512);
            }
        }
        return STATUS_SUCCESS;
    }
    std::vector<UCHAR> Sector(ULONGLONG lba) {
        std::vector<UCHAR> s(512);
        Read(this, lba, s.data(), 1);
        return s;
    }
    // fsck.exfat's exit code on the volume (-1 when the tool is missing).
    int Fsck() {
        if (system("command -v fsck.exfat >/dev/null 2>&1 || test -x /usr/sbin/fsck.exfat") != 0)
            return -1;
        std::string cmd = "PATH=$PATH:/usr/sbin fsck.exfat -n " + Volume + " >" + Dir + "/fsck.txt 2>&1";
        int rc = system(cmd.c_str());
        if (rc != 0) {
            std::string out = "cat " + Dir + "/fsck.txt";
            system(out.c_str());
        }
        return WEXITSTATUS(rc);
    }
};

ULONGLONG U64(const UCHAR *p) {
    ULONGLONG v;
    memcpy(&v, p, 8);
    return v;
}

ULONG U32(const UCHAR *p) {
    ULONG v;
    memcpy(&v, p, 4);
    return v;
}

constexpr ULONG STAMP = 0x5B670000; // 2025-11-07

// A minimal exFAT reader over the volume: the entries of a directory cluster, and a file's data.
struct Reader {
    Disk &D;
    ULONG Heap, Shift, Root;
    explicit Reader(Disk &d) : D(d) {
        auto b = D.Sector(DISK_FORMAT_PARTITION_START);
        Heap = U32(&b[88]);
        Shift = b[108] + b[109];
        Root = U32(&b[96]);
    }
    std::vector<UCHAR> Cluster(ULONG c, ULONG bytes) {
        std::vector<UCHAR> out(bytes);
        off64_t at = ((off64_t)Heap << 9) + ((off64_t)(c - 2) << Shift);
        EXPECT_EQ(pread64(D.Fd, out.data(), bytes, at), (ssize_t)bytes);
        return out;
    }
    // Finds Name in the directory at cluster Dir: its first cluster and size.
    bool Find(ULONG dir, const std::string &name, ULONG *first, ULONGLONG *size) {
        auto entries = Cluster(dir, 1u << Shift);
        for (size_t i = 0; i + 32 <= entries.size(); i += 32) {
            if (entries[i] == 0)
                break;
            if (entries[i] != 0x85)
                continue;
            const UCHAR *s = &entries[i + 32];
            std::string got;
            for (ULONG k = 0; k < s[3]; k++)
                got += (char)entries[i + 64 + 32 * (k / 15) + 2 + 2 * (k % 15)];
            if (got == name) {
                *first = U32(s + 20);
                *size = U64(s + 24);
                return true;
            }
        }
        return false;
    }
};
} // namespace

TEST(Format, EightGigabyteDiskPassesFsck) {
    Disk d(0x1000000); // 8 GiB
    DISK_FORMAT_RESULT r = {};
    ASSERT_EQ(IdexDiskFormatEXFAT(d.Sectors, 0x12345678, STAMP, nullptr, &d.Io, d.Scratch.data(), &r), STATUS_SUCCESS);
    EXPECT_EQ(r.PartitionStart, 2048u);
    EXPECT_EQ(r.ClusterShift, 15u); // 32 KB below 32 GB
    int rc = d.Fsck();
    if (rc < 0)
        GTEST_SKIP() << "fsck.exfat not installed";
    EXPECT_EQ(rc, 0);
}

TEST(Format, TwoTerabyteDiskWithAFilePassesFsck) {
    Disk d(0xE8E088B0ull); // a 2 TB disk
    std::vector<UCHAR> data(586752 + 123);
    for (size_t i = 0; i < data.size(); i++)
        data[i] = (UCHAR)(i * 7 + 3);
    DISK_FORMAT_FILE file = {"E\\ftp\\ftp.xbe", data.data(), (ULONG)data.size()};
    DISK_FORMAT_RESULT r = {};
    ASSERT_EQ(IdexDiskFormatEXFAT(d.Sectors, 0xCAFEF00D, STAMP, &file, &d.Io, d.Scratch.data(), &r), STATUS_SUCCESS);
    EXPECT_EQ(r.ClusterShift, 17u); // 128 KB
    int rc = d.Fsck();
    if (rc < 0)
        GTEST_SKIP() << "fsck.exfat not installed";
    EXPECT_EQ(rc, 0);
}

TEST(Format, TheFoldersAndTheFileAreThere) {
    Disk d(0x2000000); // 16 GiB
    std::vector<UCHAR> data(70000);
    for (size_t i = 0; i < data.size(); i++)
        data[i] = (UCHAR)(i ^ (i >> 8));
    DISK_FORMAT_FILE file = {"E\\ftp\\ftp.xbe", data.data(), (ULONG)data.size()};
    ASSERT_EQ(IdexDiskFormatEXFAT(d.Sectors, 1, STAMP, &file, &d.Io, d.Scratch.data(), nullptr), STATUS_SUCCESS);
    Reader rd(d);
    ULONG first;
    ULONGLONG size;
    for (const char *name : {"C", "E", "F", "X", "Y", "Z", "OXOS"})
        EXPECT_TRUE(rd.Find(rd.Root, name, &first, &size)) << name;
    ULONG e, ftp;
    ASSERT_TRUE(rd.Find(rd.Root, "E", &e, &size));
    ASSERT_TRUE(rd.Find(e, "ftp", &ftp, &size));
    ASSERT_TRUE(rd.Find(ftp, "ftp.xbe", &first, &size));
    EXPECT_EQ(size, data.size());
    auto got = rd.Cluster(first, (ULONG)data.size());
    EXPECT_EQ(got, data);
}

TEST(Format, TheGptChecksOut) {
    Disk d(0x1000000);
    ASSERT_EQ(IdexDiskFormatEXFAT(d.Sectors, 7, STAMP, nullptr, &d.Io, d.Scratch.data(), nullptr), STATUS_SUCCESS);
    auto mbr = d.Sector(0);
    EXPECT_EQ(mbr[446 + 4], 0xEE);
    EXPECT_EQ(mbr[510], 0x55);
    EXPECT_EQ(mbr[511], 0xAA);
    for (ULONGLONG at : {1ull, d.Sectors - 1}) {
        auto h = d.Sector(at);
        ASSERT_EQ(memcmp(h.data(), "EFI PART", 8), 0) << at;
        ULONG crc = U32(&h[16]);
        memset(&h[16], 0, 4);
        EXPECT_EQ(IdexCrc32(h.data(), 92), crc) << at;
        EXPECT_EQ(U64(&h[24]), at);
        std::vector<UCHAR> entries;
        for (ULONG i = 0; i < 32; i++) {
            auto s = d.Sector(U64(&h[72]) + i);
            entries.insert(entries.end(), s.begin(), s.end());
        }
        EXPECT_EQ(IdexCrc32(entries.data(), (ULONG)entries.size()), U32(&h[88])) << at;
        EXPECT_EQ(U64(&entries[32]), 2048u);
        EXPECT_LE(U64(&entries[40]), d.Sectors - 34);
    }
    auto boot = d.Sector(2048);
    EXPECT_EQ(memcmp(boot.data() + 3, "EXFAT   ", 8), 0);
}

TEST(Format, BlankFormattedAndPartitionsDeleted) {
    Disk d(0x1000000);
    EXPECT_TRUE(IdexDiskIsBlank(d.Sectors, &d.Io, d.Scratch.data())) << "a new disk";
    ASSERT_EQ(IdexDiskFormatEXFAT(d.Sectors, 9, STAMP, nullptr, &d.Io, d.Scratch.data(), nullptr), STATUS_SUCCESS);
    EXPECT_FALSE(IdexDiskIsBlank(d.Sectors, &d.Io, d.Scratch.data())) << "formatted";
    ASSERT_EQ(IdexDiskDeleteAllPartitions(d.Sectors, nullptr, 0, 0, &d.Io, d.Scratch.data()), STATUS_SUCCESS);
    EXPECT_TRUE(IdexDiskIsBlank(d.Sectors, &d.Io, d.Scratch.data())) << "partitions deleted";
}

TEST(Format, AFatxDiskIsNotBlankAndDeletingClearsIt) {
    Disk d(0x1000000);
    std::vector<UCHAR> fatx(512);
    memcpy(fatx.data(), "FATX", 4);
    Disk::Write(&d, 0x55F400, fatx.data(), 1); // E:
    Disk::Write(&d, 0x465400, fatx.data(), 1); // C:
    Disk::Write(&d, 0x177400, fatx.data(), 1); // Y:
    EXPECT_FALSE(IdexDiskIsBlank(d.Sectors, &d.Io, d.Scratch.data()));
    ULONGLONG extra = 0x900000;
    Disk::Write(&d, extra, fatx.data(), 1);
    ASSERT_EQ(IdexDiskDeleteAllPartitions(d.Sectors, &extra, 1, 7, &d.Io, d.Scratch.data()), STATUS_SUCCESS);
    EXPECT_TRUE(IdexDiskIsBlank(d.Sectors, &d.Io, d.Scratch.data()));
    for (ULONGLONG lba : {0x55F400ull, 0x465400ull, 0x177400ull, extra})
        EXPECT_EQ(d.Sector(lba)[0], 0) << std::hex << lba;
}

TEST(Format, AFailedReadIsNotBlank) {
    Disk d(0x1000000);
    d.FailReads = true;
    EXPECT_FALSE(IdexDiskIsBlank(d.Sectors, &d.Io, d.Scratch.data()));
}

TEST(Format, BadFilePathsAreRefused) {
    Disk d(0x1000000);
    UCHAR byte = 1;
    for (const char *path : {"E\\a:b", "E\\", "C", "E\\ftp\\x.xbe\\"}) {
        DISK_FORMAT_FILE file = {path, &byte, 1};
        EXPECT_NE(IdexDiskFormatEXFAT(d.Sectors, 1, STAMP, &file, &d.Io, d.Scratch.data(), nullptr), STATUS_SUCCESS)
            << path;
    }
}

TEST(Format, TheMarkerSetsTheCountdown) {
    Disk d(0x1000000);
    EXPECT_EQ(IdexDiskBlankCountdown(&d.Io, d.Scratch.data(), DISK_FORMAT_COUNTDOWN), DISK_FORMAT_COUNTDOWN) << "no marker: the default";
    for (ULONG seconds : {0u, 5u, 30u}) {
        ASSERT_EQ(IdexDiskDeleteAllPartitions(d.Sectors, nullptr, 0, seconds, &d.Io, d.Scratch.data()),
                  STATUS_SUCCESS);
        EXPECT_TRUE(IdexDiskIsBlank(d.Sectors, &d.Io, d.Scratch.data())) << "the marker leaves the disk blank";
        EXPECT_EQ(IdexDiskBlankCountdown(&d.Io, d.Scratch.data(), DISK_FORMAT_COUNTDOWN), seconds);
    }
    ASSERT_EQ(IdexDiskDeleteAllPartitions(d.Sectors, nullptr, 0, 100000, &d.Io, d.Scratch.data()), STATUS_SUCCESS);
    EXPECT_EQ(IdexDiskBlankCountdown(&d.Io, d.Scratch.data(), DISK_FORMAT_COUNTDOWN), DISK_FORMAT_MAX_COUNTDOWN);
    ASSERT_EQ(IdexDiskFormatEXFAT(d.Sectors, 1, STAMP, nullptr, &d.Io, d.Scratch.data(), nullptr), STATUS_SUCCESS);
    EXPECT_EQ(IdexDiskBlankCountdown(&d.Io, d.Scratch.data(), DISK_FORMAT_COUNTDOWN), DISK_FORMAT_COUNTDOWN) << "the format clears it";
}

namespace {
// Sector 0 as an MBR with one partition of Type at Start; a file system's signature written at the partition.
void Mbr(Disk &d, UCHAR type, ULONG start) {
    std::vector<UCHAR> s(512);
    s[446 + 4] = type;
    memcpy(&s[446 + 8], &start, 4);
    ULONG length = 0x100000;
    memcpy(&s[446 + 12], &length, 4);
    s[510] = 0x55;
    s[511] = 0xAA;
    Disk::Write(&d, 0, s.data(), 1);
}

void Signature(Disk &d, ULONGLONG lba, ULONG offset, const char *text, size_t length) {
    std::vector<UCHAR> s(2048);
    memcpy(&s[offset], text, length);
    Disk::Write(&d, lba, s.data(), 4);
}

std::string Describe(Disk &d, ULONG *flags = nullptr) {
    char out[64];
    ULONG f = IdexDiskDescribe(d.Sectors, &d.Io, d.Scratch.data(), out, sizeof(out));
    if (flags)
        *flags = f;
    return out;
}
} // namespace

TEST(Format, DescribeNamesWhatIsOnTheDisk) {
    ULONG flags;
    {
        Disk d(0x1000000);
        Mbr(d, 0x07, 2048);
        Signature(d, 2048, 3, "NTFS    ", 8);
        EXPECT_EQ(Describe(d, &flags), "MBR: NTFS");
        EXPECT_EQ(flags, DISK_FORMAT_PC_TABLE);
    }
    {
        Disk d(0x1000000);
        Mbr(d, 0x83, 2048);
        Signature(d, 2048, 1080, "\x53\xEF", 2);
        EXPECT_EQ(Describe(d), "MBR: Linux ext2/3/4");
    }
    {
        Disk d(0x1000000);
        Mbr(d, 0x83, 2048); // no file system written: the type names it
        EXPECT_EQ(Describe(d), "MBR: Linux");
    }
    {
        Disk d(0x1000000);
        Signature(d, 0, 82, "FAT32   ", 8);
        EXPECT_EQ(Describe(d, &flags), "FAT32, no partition table");
        EXPECT_EQ(flags, DISK_FORMAT_WHOLE_DISK_FS);
    }
    {
        Disk d(0x1000000); // our own format: GPT with exFAT
        ASSERT_EQ(IdexDiskFormatEXFAT(d.Sectors, 1, STAMP, nullptr, &d.Io, d.Scratch.data(), nullptr),
                  STATUS_SUCCESS);
        EXPECT_EQ(Describe(d), "GPT: exFAT");
    }
    {
        Disk d(0x1000000); // a blank disk
        EXPECT_EQ(Describe(d, &flags), "unknown");
        EXPECT_EQ(flags, 0u);
    }
}
