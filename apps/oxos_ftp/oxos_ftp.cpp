// FTP server (nxdk + SDL3, open source): the console's disks over the network, for copying, moving and deleting files with any FTP client (FileZilla, curl,
// lftp, Windows Explorer). No log-in: any user name and password are accepted.
//
//   /            the drives: C, E, F, G, X, Y, Z (those that exist)
//   /F/games     F:\games (\Device\Harddisk0\Partition6\games)
//
// Commands: USER PASS SYST FEAT OPTS PWD CWD CDUP TYPE MODE STRU PASV EPSV PORT LIST NLST MLSD MLST RETR STOR APPE REST SIZE MDTM DELE MKD RMD RNFR RNTO NOOP
// ABOR QUIT, SITE REBOOT (cold reboot), SITE POWEROFF, SITE LAUNCH PATH (starts an XBE, or a folder's default.xbe, as a dashboard does), and on OXOS SITE
// FORMAT-EXFAT YES (the hard disk becomes one exFAT volume; the server keeps itself on it and restarts from it) and SITE DELETE-ALL-PARTITIONS YES [SECONDS]
// (the next boot finds a blank disk and formats it as exFAT after a countdown of SECONDS, default 0). RNTO moves a file or folder to another folder of the same
// drive. Passive data ports are lwIP's 49152-65535. The address (DHCP, else 192.168.1.222 after 30 s) is on the screen and, with a SERIAL=1 kernel, on the
// debug serial port. The screen (and the debug serial port) shows every command as it arrives and what it did. BACK on any controller reboots the console (as
// SITE REBOOT). The main thread owns the screen and the controllers (SDL3, tools/sdl_screen) and starts titles; the network runs in a thread of its own, and
// each client in another; an upload's disk writes in one more. While files move, the top right of the screen shows the speeds (UL / DL, MB/s), filled as the
// files progress (a download's size is known; an upload's when the client sent ALLO with it). The C++ headers first: lwIP's socket macros (bind, connect,
// ...) would break them.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <hal/xbox.h>
#include <lwip/sockets.h>
#include <lwip/netif.h>
#include <lwip/netifapi.h>
#include <lwip/dhcp.h>
#include <nxdk/net.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>

#include "sdl_screen.h"

extern "C" struct netif *g_pnetif; // nxdk's network interface (lib/nxdk/net.c)

namespace {

// The server's name, version and build time, in the 220 greeting and on the screen: "OXOS FTP server 0.2.0 (2026-11-24 02:13:27)". The build time changes
// with every build, so a client can tell which build answers. The version is MAJOR.MINOR.PATCH: raise it when the server's behaviour changes.
constexpr char ServerVersion[] = "OXOS FTP server 0.2.0";

struct SERVER_NAME {
    char Text[48];
};

// ServerVersion and the compiler's build time (__DATE__ "Nov 24 2026", __TIME__ "02:13:27") as "... (2026-11-24 02:13:27)", at compile time.
constexpr SERVER_NAME MakeServerName() {
    constexpr char months[] = "JanFebMarAprMayJunJulAugSepOctNovDec";
    const char *date = __DATE__, *time = __TIME__;
    int month = 1;
    for (int i = 0; i < 12; i++) {
        if (date[0] == months[3 * i] && date[1] == months[3 * i + 1] && date[2] == months[3 * i + 2])
            month = i + 1;
    }
    SERVER_NAME name{};
    int at = 0;
    for (const char *c = ServerVersion; *c; c++)
        name.Text[at++] = *c;
    const char stamp[] = {' ', '(', date[7], date[8], date[9], date[10], '-', (char)('0' + month / 10), (char)('0' + month % 10), '-',
                          date[4] == ' ' ? '0' : date[4], date[5], ' ', time[0], time[1], time[2], time[3], time[4], time[5], time[6], time[7], ')'};
    for (char c : stamp)
        name.Text[at++] = c;
    return name;
}

constexpr SERVER_NAME ServerNameText = MakeServerName();
constexpr const char *ServerName = ServerNameText.Text;

constexpr unsigned short ControlPort = 21;
constexpr int MaxSessions = 8;
constexpr SIZE_T SessionStack = 64 * 1024; // each client's thread, and the network's
constexpr NTSTATUS STATUS_NOT_SAME_DEVICE = (NTSTATUS)0xC00000D4;
// 192.168.1.222, little-endian as nxdk wants it: the address the netboot BIOS uses for this console, so free on a network set up for it (.223 was in use on the
// test network).
constexpr ULONG FallbackIp = 0xDE01A8C0;
constexpr ULONG DhcpWaitMs = 30000; // nxdk waits 10 s; after a title switch the link can take longer to come back

// Upper-cases ASCII letters only (FTP verbs and SITE commands; '-' and digits stay as they are).
void ToUpper(std::string &s) {
    for (char &c : s) {
        if (c >= 'a' && c <= 'z')
            c = (char)(c - 0x20);
    }
}

// --- OXOS's own kernel exports (spec/oxos_internal/30_disk_format.md), found by ordinal in the running kernel's
// export table, so the server still runs on other kernels (they have no such ordinals)

constexpr ULONG OrdinalFormatDisk = 400, OrdinalDeleteAllPartitions = 401;
typedef NTSTATUS(NTAPI *FormatDiskRoutine)(PCSTR KeepPath);
typedef NTSTATUS(NTAPI *DeleteAllPartitionsRoutine)(ULONG CountdownSeconds);

void *KernelExport(ULONG ordinal) {
    auto base = (const UCHAR *)0x80010000;
    if (!MmIsAddressValid((PVOID)base) || *(const USHORT *)base != 0x5A4D) // "MZ"
        return nullptr;
    const UCHAR *pe = base + *(const ULONG *)(base + 0x3C);
    ULONG directory = *(const ULONG *)(pe + 24 + 96); // the optional header's export directory
    if (!directory)
        return nullptr;
    const UCHAR *e = base + directory;
    ULONG first = *(const ULONG *)(e + 16), count = *(const ULONG *)(e + 20);
    if (ordinal < first || ordinal - first >= count)
        return nullptr;
    ULONG rva = ((const ULONG *)(base + *(const ULONG *)(e + 28)))[ordinal - first];
    return rva ? (void *)(base + rva) : nullptr;
}

// --- Output: the screen and the debug serial port -----------------------------------------------------------------

CRITICAL_SECTION PrintLock; // one line at a time: every session thread logs

void Log(const char *format, ...) {
    char line[300];
    va_list ap;
    va_start(ap, format);
    std::vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    EnterCriticalSection(&PrintLock);
    sdl_screen::Print("%s\n", line);
    DbgPrint("ftp: %s\n", line);
    LeaveCriticalSection(&PrintLock);
}

// --- Transfer speeds and progress on the screen: the sessions count the bytes of the files they send and receive, the main thread shows them at the top
// right once a second while transfers run

volatile LONG UploadedBytes, DownloadedBytes;   // running totals, modulo 2^32: only a second's difference is used
volatile LONG Uploads, Downloads;               // transfers in progress
volatile LONG UploadKiBDone, UploadKiBTotal;    // of the uploads whose size is known: KiB done and in all (progress)
volatile LONG DownloadKiBDone, DownloadKiBTotal; // the same for the downloads
constexpr DWORD SpeedIntervalMs = 1000;         // how often the speeds are measured and shown

// One transfer, counted in Uploads or Downloads while it lives; Count adds the file bytes it has moved. Size 0: not known (no progress shown for it).
class TransferInProgress {
public:
    TransferInProgress(bool upload, LONGLONG start, LONGLONG size) : upload(upload), done(start), size(size) {
        InterlockedIncrement(upload ? &Uploads : &Downloads);
        if (size > 0) {
            InterlockedExchangeAdd(upload ? &UploadKiBTotal : &DownloadKiBTotal, (LONG)(size >> 10));
            InterlockedExchangeAdd(upload ? &UploadKiBDone : &DownloadKiBDone, (LONG)(done >> 10));
        }
    }

    ~TransferInProgress() {
        if (size > 0) {
            InterlockedExchangeAdd(upload ? &UploadKiBTotal : &DownloadKiBTotal, -(LONG)(size >> 10));
            InterlockedExchangeAdd(upload ? &UploadKiBDone : &DownloadKiBDone, -(LONG)(done >> 10));
        }
        InterlockedDecrement(upload ? &Uploads : &Downloads);
    }

    void Count(unsigned bytes) {
        InterlockedExchangeAdd(upload ? &UploadedBytes : &DownloadedBytes, (LONG)bytes);
        LONGLONG before = done;
        done += bytes;
        if (size > 0)
            InterlockedExchangeAdd(upload ? &UploadKiBDone : &DownloadKiBDone, (LONG)((done >> 10) - (before >> 10)));
    }

private:
    bool upload;   // a STOR or APPE, else a RETR
    LONGLONG done; // the file's bytes so far, from the REST or APPE offset on
    LONGLONG size; // the file's size, 0 when not known
};

// One line of the box: "UL: 11.8 MB/s / ETA: 00:01:12" from the bytes of one interval (tenths in integers: nxdk's printf has no floating point), and its
// progress; the time left only when the size is known, from Smoothed (bytes a second, averaged over the last seconds so it does not jump each second).
int DescribeTransfers(char *out, int room, const char *name, ULONG bytes, DWORD ms, LONG kibDone, LONG kibTotal, int line, ULONG &smoothed) {
    ULONG perSecond = (ULONG)((ULONGLONG)bytes * 1000 / ms), tenths = perSecond / 100000;
    smoothed = smoothed ? (ULONG)(((ULONGLONG)smoothed * 3 + perSecond) / 4) : perSecond;
    sdl_screen::SetCornerFill(line, kibTotal > 0 ? (int)((LONGLONG)kibDone * 1000 / kibTotal) : -1);
    int n = std::snprintf(out, room, "%s: %lu.%lu MB/s", name, tenths / 10, tenths % 10);
    if (kibTotal > 0 && smoothed) {
        ULONG left = (ULONG)((ULONGLONG)(kibTotal > kibDone ? kibTotal - kibDone : 0) * 1024 / smoothed);
        n += std::snprintf(out + n, room - n, " / ETA: %02lu:%02lu:%02lu", left / 3600, left / 60 % 60, left % 60);
    }
    return n + std::snprintf(out + n, room - n, "\n");
}

// The main thread, every frame: once a second the speeds of the last second at the top right, filled by progress; nothing while no transfer runs.
void ShowTransferSpeeds() {
    static DWORD lastTime;
    static ULONG lastUploaded, lastDownloaded;
    static ULONG uploadSmoothed, downloadSmoothed; // bytes a second, for the time left; 0 again once no transfer runs
    DWORD now = GetTickCount();
    if (now - lastTime < SpeedIntervalMs)
        return;
    if (Uploads <= 0)
        uploadSmoothed = 0;
    if (Downloads <= 0)
        downloadSmoothed = 0;
    ULONG uploaded = (ULONG)UploadedBytes, downloaded = (ULONG)DownloadedBytes;
    char text[128] = "";
    int n = 0, line = 0;
    if (Uploads > 0)
        n += DescribeTransfers(text + n, sizeof(text) - n, "UL", uploaded - lastUploaded, now - lastTime, UploadKiBDone, UploadKiBTotal, line++,
                               uploadSmoothed);
    if (Downloads > 0)
        n += DescribeTransfers(text + n, sizeof(text) - n, "DL", downloaded - lastDownloaded, now - lastTime, DownloadKiBDone, DownloadKiBTotal, line++,
                               downloadSmoothed);
    sdl_screen::SetCornerText(text);
    lastTime = now;
    lastUploaded = uploaded;
    lastDownloaded = downloaded;
}

// --- An upload's disk writes, on a thread of their own

// The blocks of an upload on their way to the disk: the session thread receives into a free block and queues it, the writer thread writes the queued blocks
// in order and frees them. The disk writes one block by DMA while the session receives the next, so an upload runs at the network's speed, not the network's
// and the disk's one after the other (console, 2026-11-23: 800 MB took 70 s to receive and 29 s to write).
class DiskWriter {
public:
    static constexpr unsigned BlockSize = 256 * 1024; // the most a disk write takes
    static constexpr int Blocks = 6;                  // 1.5 MB per upload

    // Allocates the blocks and starts the writer thread for File from Offset on; false when there is no memory for them.
    bool Start(HANDLE file, LONGLONG offset) {
        this->file = file;
        position.QuadPart = offset;
        for (char *&block : blocks) {
            block = static_cast<char *>(std::malloc(BlockSize));
            if (!block)
                return false;
        }
        freeBlocks = CreateSemaphore(nullptr, Blocks, Blocks, nullptr);
        queuedBlocks = CreateSemaphore(nullptr, 0, Blocks, nullptr);
        if (!freeBlocks || !queuedBlocks)
            return false;
        thread = CreateThread(nullptr, WriterStack, Run, this, CREATE_SUSPENDED, nullptr);
        if (!thread)
            return false;
        // Above the session: the writer needs little CPU, but as soon as a write ends, so the disk does not wait for the session's time slice (console,
        // 2026-11-23: at the same priority a busy session doubled the time spent writing).
        SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST);
        ResumeThread(thread);
        return true;
    }

    ~DiskWriter() {
        for (char *block : blocks)
            std::free(block);
        for (HANDLE handle : {freeBlocks, queuedBlocks, thread}) {
            if (handle)
                CloseHandle(handle);
        }
    }

    // The next free block, after waiting for one; nullptr once a write has failed.
    char *TakeBlock() {
        WaitForSingleObject(freeBlocks, INFINITE);
        if (!NT_SUCCESS(status)) {
            ReleaseSemaphore(freeBlocks, 1, nullptr);
            return nullptr;
        }
        return blocks[next];
    }

    // The block TakeBlock gave, filled with Length bytes (more than 0), queued for the disk.
    void QueueBlock(unsigned length) {
        lengths[next] = length;
        next = (next + 1) % Blocks;
        ReleaseSemaphore(queuedBlocks, 1, nullptr);
    }

    // The block TakeBlock gave, back unused.
    void ReturnBlock() {
        ReleaseSemaphore(freeBlocks, 1, nullptr);
    }

    // Waits until every queued block is written and the writer thread has ended: the first failed write's status, else STATUS_SUCCESS.
    NTSTATUS Finish() {
        WaitForSingleObject(freeBlocks, INFINITE);
        lengths[next] = 0; // the end
        ReleaseSemaphore(queuedBlocks, 1, nullptr);
        WaitForSingleObject(thread, INFINITE);
        return status;
    }

private:
    static constexpr SIZE_T WriterStack = 16 * 1024;

    static DWORD WINAPI Run(LPVOID self) {
        static_cast<DiskWriter *>(self)->WriteQueuedBlocks();
        return 0;
    }

    // The writer thread: the queued blocks in order until the end; after a failed write the rest are only freed.
    void WriteQueuedBlocks() {
        for (int at = 0;; at = (at + 1) % Blocks) {
            WaitForSingleObject(queuedBlocks, INFINITE);
            if (!lengths[at])
                return;
            if (NT_SUCCESS(status)) {
                IO_STATUS_BLOCK iosb;
                NTSTATUS written = NtWriteFile(file, nullptr, nullptr, nullptr, &iosb, blocks[at], lengths[at], &position);
                if (NT_SUCCESS(written))
                    position.QuadPart += lengths[at];
                else
                    status = written;
            }
            ReleaseSemaphore(freeBlocks, 1, nullptr);
        }
    }

    HANDLE file = nullptr;          // the upload's file
    LARGE_INTEGER position = {};    // where the next block goes (the writer thread's)
    char *blocks[Blocks] = {};      // the blocks, used in turn
    unsigned lengths[Blocks] = {};  // each queued block's bytes; 0 ends the upload
    int next = 0;                   // the block TakeBlock gives next (the session thread's)
    HANDLE freeBlocks = nullptr;    // counts the free blocks
    HANDLE queuedBlocks = nullptr;  // counts the queued blocks
    HANDLE thread = nullptr;        // the writer thread
    volatile NTSTATUS status = STATUS_SUCCESS; // the first failed write's
};

// --- A download's disk reads, on a thread of their own

// The blocks of a download on their way to the network: the reader thread reads the file ahead into free blocks and queues them, the session thread sends
// the queued blocks in order and frees them. The disk reads the next block by DMA while the session sends one, as DiskWriter does for uploads (console,
// 2026-11-24: reading and sending one after the other, 800 MB went down at 8.9 MB/s against 11.8 up).
class DiskReader {
public:
    static constexpr unsigned BlockSize = 256 * 1024; // the most a disk read takes
    static constexpr int Blocks = 6;                  // 1.5 MB per download

    // Allocates the blocks and starts the reader thread for File from Offset on; false when there is no memory for them.
    bool Start(HANDLE file, LONGLONG offset) {
        this->file = file;
        position.QuadPart = offset;
        for (char *&block : blocks) {
            block = static_cast<char *>(std::malloc(BlockSize));
            if (!block)
                return false;
        }
        freeBlocks = CreateSemaphore(nullptr, Blocks, Blocks, nullptr);
        filledBlocks = CreateSemaphore(nullptr, 0, Blocks, nullptr);
        if (!freeBlocks || !filledBlocks)
            return false;
        thread = CreateThread(nullptr, ReaderStack, Run, this, CREATE_SUSPENDED, nullptr);
        if (!thread)
            return false;
        SetThreadPriority(thread, THREAD_PRIORITY_HIGHEST); // as DiskWriter's: the disk does not wait for the session's time slice
        ResumeThread(thread);
        return true;
    }

    // Stops the reader thread if it still runs (a download that ended early), then frees the blocks.
    ~DiskReader() {
        if (thread) {
            stopping = true;
            ReleaseSemaphore(freeBlocks, 1, nullptr); // wakes the thread if it waits for a free block
            WaitForSingleObject(thread, INFINITE);
        }
        for (char *block : blocks)
            std::free(block);
        for (HANDLE handle : {freeBlocks, filledBlocks, thread}) {
            if (handle)
                CloseHandle(handle);
        }
    }

    // The next block read, after waiting for it, in Block: its bytes, 0 at the end of the file, -1 when a read failed (Status says how). A block with
    // bytes goes back with ReturnBlock once sent.
    int TakeBlock(char *&block) {
        WaitForSingleObject(filledBlocks, INFINITE);
        block = blocks[next];
        if (!lengths[next])
            return NT_SUCCESS(status) ? 0 : -1;
        return (int)lengths[next];
    }

    void ReturnBlock() {
        next = (next + 1) % Blocks;
        ReleaseSemaphore(freeBlocks, 1, nullptr);
    }

    NTSTATUS Status() const {
        return status;
    }

private:
    static constexpr SIZE_T ReaderStack = 16 * 1024;

    static DWORD WINAPI Run(LPVOID self) {
        static_cast<DiskReader *>(self)->ReadBlocks();
        return 0;
    }

    // The reader thread: blocks in order until the end of the file or a failed read, which queues an empty block; stops early when told to.
    void ReadBlocks() {
        for (int at = 0;; at = (at + 1) % Blocks) {
            WaitForSingleObject(freeBlocks, INFINITE);
            if (stopping)
                return;
            IO_STATUS_BLOCK iosb;
            NTSTATUS read = NtReadFile(file, nullptr, nullptr, nullptr, &iosb, blocks[at], BlockSize, &position);
            lengths[at] = NT_SUCCESS(read) ? (unsigned)iosb.Information : 0;
            if (!NT_SUCCESS(read) && read != STATUS_END_OF_FILE)
                status = read;
            position.QuadPart += lengths[at];
            ReleaseSemaphore(filledBlocks, 1, nullptr);
            if (!lengths[at])
                return;
        }
    }

    HANDLE file = nullptr;          // the download's file
    LARGE_INTEGER position = {};    // where the next block is read from (the reader thread's)
    char *blocks[Blocks] = {};      // the blocks, used in turn
    unsigned lengths[Blocks] = {};  // each filled block's bytes; 0 ends the download
    int next = 0;                   // the block TakeBlock gives next (the session thread's)
    HANDLE freeBlocks = nullptr;    // counts the free blocks
    HANDLE filledBlocks = nullptr;  // counts the filled blocks
    HANDLE thread = nullptr;        // the reader thread
    volatile bool stopping = false; // the download ended early: the reader thread stops
    volatile NTSTATUS status = STATUS_SUCCESS; // a failed read's
};

// --- Starting a title: the main thread does it, after SDL and the USB host have stopped (sdl_screen::Close) --------

char LaunchPath[300];
volatile LONG LaunchRequested;
bool LaunchRebootOnFailure; // a cold reboot when the launch fails (after a format: the old layout is gone)

void RequestLaunch(const std::string &path, bool rebootOnFailure) {
    std::snprintf(LaunchPath, sizeof(LaunchPath), "%s", path.c_str());
    LaunchRebootOnFailure = rebootOnFailure;
    InterlockedExchange(&LaunchRequested, 1);
}

// Called by the main thread once a frame. Returns only when there was no request or the launch failed.
void LaunchIfRequested() {
    if (!InterlockedExchange(&LaunchRequested, 0))
        return;
    sdl_screen::Close();
    XLaunchXBE(LaunchPath);
    if (LaunchRebootOnFailure)
        HalReturnToFirmware(HalRebootRoutine);
    sdl_screen::Open(true);
    Log("launch FAILED: %s", LaunchPath);
}

// --- Paths --------------------------------------------------------------------------------------------------------

constexpr char Drives[] = "CEFGXYZ"; // the drives the root lists, in order

// The partition device behind a drive letter, or nullptr.
const char *PartitionOf(char letter) {
    switch (letter) {
    case 'C': return "\\Device\\Harddisk0\\Partition2";
    case 'E': return "\\Device\\Harddisk0\\Partition1";
    case 'X': return "\\Device\\Harddisk0\\Partition3";
    case 'Y': return "\\Device\\Harddisk0\\Partition4";
    case 'Z': return "\\Device\\Harddisk0\\Partition5";
    case 'F': return "\\Device\\Harddisk0\\Partition6";
    case 'G': return "\\Device\\Harddisk0\\Partition7";
    default: return nullptr;
    }
}

// An FTP path as its parts: {"F", "games", "halo"}; parts[0] is the drive.
using Parts = std::vector<std::string>;

// cwd + arg, with "." and ".." resolved; both slash kinds accepted. False for a name with a character that cannot be in a file name.
bool Resolve(const Parts &cwd, const std::string &arg, Parts &out) {
    out = (arg.empty() || (arg[0] != '/' && arg[0] != '\\')) ? cwd : Parts{};
    std::string part;
    for (std::size_t i = 0; i <= arg.size(); i++) {
        char ch = i < arg.size() ? arg[i] : '/';
        if (ch == '/' || ch == '\\') {
            if (part == "..") {
                if (!out.empty())
                    out.pop_back();
            } else if (!part.empty() && part != ".") {
                if (part.find_first_of(":*?\"<>|") != std::string::npos)
                    return false;
                out.push_back(part);
            }
            part.clear();
        } else {
            part += ch;
        }
    }
    if (!out.empty()) {
        if (out[0].size() != 1)
            return false;
        out[0][0] &= ~0x20; // drive letters in upper case
        if (!PartitionOf(out[0][0]))
            return false;
    }
    return true;
}

std::string FtpPath(const Parts &p) {
    std::string s;
    for (const auto &part : p)
        s += "/" + part;
    return s.empty() ? "/" : s;
}

// The kernel path; a drive's root ends with a backslash.
std::string NtPath(const Parts &p) {
    std::string s = PartitionOf(p[0][0]);
    if (p.size() == 1)
        return s + "\\";
    for (std::size_t i = 1; i < p.size(); i++)
        s += "\\" + p[i];
    return s;
}

// --- Files --------------------------------------------------------------------------------------------------------

NTSTATUS Open(HANDLE *handle, const std::string &path, ACCESS_MASK access, ULONG disposition, ULONG options, ULONG share = FILE_SHARE_READ | FILE_SHARE_WRITE) {
    ANSI_STRING name;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK iosb;
    RtlInitAnsiString(&name, path.c_str());
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
    return NtCreateFile(handle, access | SYNCHRONIZE, &attributes, &iosb, nullptr, FILE_ATTRIBUTE_NORMAL, share, disposition,
                        options | FILE_SYNCHRONOUS_IO_NONALERT);
}

struct Info {
    bool exists = false, directory = false;
    LONGLONG size = 0, written = 0; // written: 100 ns units since 1601
};

Info Stat(const Parts &p) {
    Info info;
    if (p.empty()) {
        info.exists = info.directory = true;
        return info;
    }
    ANSI_STRING name;
    OBJECT_ATTRIBUTES attributes;
    std::string path = NtPath(p);
    RtlInitAnsiString(&name, path.c_str());
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
    FILE_NETWORK_OPEN_INFORMATION n;
    if (NT_SUCCESS(NtQueryFullAttributesFile(&attributes, &n))) {
        info.exists = true;
        info.directory = n.FileAttributes & FILE_ATTRIBUTE_DIRECTORY;
        info.size = n.EndOfFile.QuadPart;
        info.written = n.LastWriteTime.QuadPart;
    } else if (p.size() == 1) { // a drive's root
        HANDLE h;
        if (NT_SUCCESS(Open(&h, path, FILE_LIST_DIRECTORY, FILE_OPEN, FILE_DIRECTORY_FILE))) {
            NtClose(h);
            info.exists = info.directory = true;
        }
    }
    return info;
}

struct Entry {
    std::string name;
    bool directory;
    LONGLONG size, written;
};

NTSTATUS ListFolder(const Parts &p, std::vector<Entry> &list) {
    list.clear();
    if (p.empty()) {
        for (const char *d = Drives; *d; d++) {
            if (Stat(Parts{std::string(1, *d)}).exists)
                list.push_back({std::string(1, *d), true, 0, 0});
        }
        return STATUS_SUCCESS;
    }
    HANDLE folder;
    NTSTATUS status = Open(&folder, NtPath(p), FILE_LIST_DIRECTORY, FILE_OPEN, FILE_DIRECTORY_FILE);
    if (!NT_SUCCESS(status))
        return status;
    alignas(8) unsigned char buffer[4096];
    for (bool first = true;; first = false) {
        IO_STATUS_BLOCK iosb;
        status = NtQueryDirectoryFile(folder, nullptr, nullptr, nullptr, &iosb, buffer, sizeof(buffer), FileDirectoryInformation, nullptr, first);
        if (!NT_SUCCESS(status))
            break; // STATUS_NO_MORE_FILES at the end
        for (ULONG offset = 0;;) {
            auto e = reinterpret_cast<FILE_DIRECTORY_INFORMATION *>(buffer + offset);
            list.push_back({std::string(e->FileName, e->FileNameLength), (e->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, e->EndOfFile.QuadPart,
                            e->LastWriteTime.QuadPart});
            if (!e->NextEntryOffset)
                break;
            offset += e->NextEntryOffset;
        }
    }
    NtClose(folder);
    return STATUS_SUCCESS;
}

NTSTATUS Delete(const Parts &p, bool directory) {
    HANDLE h;
    NTSTATUS status = Open(&h, NtPath(p), DELETE, FILE_OPEN, directory ? FILE_DIRECTORY_FILE : FILE_NON_DIRECTORY_FILE,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    if (!NT_SUCCESS(status))
        return status;
    FILE_DISPOSITION_INFORMATION del = {TRUE};
    IO_STATUS_BLOCK iosb;
    status = NtSetInformationFile(h, &iosb, &del, sizeof(del), FileDispositionInformation);
    NtClose(h);
    return status;
}

NTSTATUS MakeFolder(const Parts &p) {
    HANDLE h;
    NTSTATUS status = Open(&h, NtPath(p), FILE_LIST_DIRECTORY, FILE_CREATE, FILE_DIRECTORY_FILE);
    if (NT_SUCCESS(status))
        NtClose(h);
    return status;
}

// Rename or move (same drive): the new name as a full kernel path.
NTSTATUS Rename(const Parts &from, const Parts &to) {
    if (from.size() < 2 || to.size() < 2 || from[0] != to[0])
        return STATUS_NOT_SAME_DEVICE;
    HANDLE h;
    NTSTATUS status = Open(&h, NtPath(from), DELETE, FILE_OPEN, 0, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE);
    if (!NT_SUCCESS(status))
        return status;
    std::string target = NtPath(to);
    FILE_RENAME_INFORMATION rename = {};
    rename.ReplaceIfExists = FALSE;
    rename.RootDirectory = nullptr;
    RtlInitAnsiString(&rename.FileName, target.c_str());
    IO_STATUS_BLOCK iosb;
    status = NtSetInformationFile(h, &iosb, &rename, sizeof(rename), FileRenameInformation);
    NtClose(h);
    return status;
}

// "Mon DD HH:MM" (this year) or "Mon DD  YYYY", and "YYYYMMDDHHMMSS".
void TimeText(LONGLONG t, char *ls, std::size_t lsSize, char *mdtm, std::size_t mdtmSize) {
    static const char *months[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    TIME_FIELDS f = {};
    LARGE_INTEGER time;
    time.QuadPart = t ? t : 0x01D0000000000000LL; // no time: some date in 2014
    RtlTimeToTimeFields(&time, &f);
    int m = f.Month >= 1 && f.Month <= 12 ? f.Month - 1 : 0;
    if (ls)
        std::snprintf(ls, lsSize, "%s %2d  %4d", months[m], f.Day, f.Year);
    if (mdtm)
        std::snprintf(mdtm, mdtmSize, "%04d%02d%02d%02d%02d%02d", f.Year, f.Month, f.Day, f.Hour, f.Minute, f.Second);
}

// The words for the failures a client meets most, or null.
const char *StatusName(NTSTATUS s) {
    switch ((ULONG)s) {
    case 0xC0000034: return "No such file or folder";
    case 0xC000003A: return "Path not found";
    case 0xC0000035: return "Already exists";
    case 0xC0000043: return "File in use";
    case 0xC0000022: return "Access denied";
    case 0xC0000101: return "Folder not empty";
    case 0xC00000D4: return "Not the same drive";
    case 0xC000007F: return "Disk full";
    case 0xC0000033: return "Invalid name";
    case 0xC0000056: return "Being deleted (still open)";
    case 0xC0000121: return "Read-only";
    default: return nullptr;
    }
}

// A failure for a reply or the log: its words, else its status, so every failure can be traced.
std::string StatusText(NTSTATUS s) {
    if (const char *name = StatusName(s))
        return name;
    char text[32];
    std::snprintf(text, sizeof(text), "Failed (status %08lx)", (unsigned long)s);
    return text;
}

// --- Sessions -----------------------------------------------------------------------------------------------------

volatile LONG SessionCount = 0;

struct Session {
    int control;
    Parts cwd;
    int passive = -1;          // listening data socket (PASV/EPSV)
    sockaddr_in active = {};   // PORT address
    bool haveActive = false;
    LONGLONG restart = 0;      // REST
    LONGLONG announced = 0;    // ALLO: the next upload's size, for its progress on the screen; 0 when not sent
    bool haveFrom = false;     // RNFR
    Parts renameFrom;
    std::string pending;       // received control bytes not yet a line
    char peer[20];
    std::string lastCommand;   // the command being answered (for the screen log of failures)

    void Reply(int code, const char *format, ...) {
        char line[512];
        int n = std::snprintf(line, sizeof(line), "%d ", code);
        va_list ap;
        va_start(ap, format);
        n += std::vsnprintf(line + n, sizeof(line) - n - 2, format, ap);
        va_end(ap);
        if (n > (int)sizeof(line) - 3)
            n = sizeof(line) - 3;
        if (code >= 400) // every refusal and failure on the screen too, with the command it answers
            Log("%s: %s -> %.*s", peer, lastCommand.c_str(), n, line);
        line[n++] = '\r';
        line[n++] = '\n';
        SendAll(control, line, n);
    }

    static bool SendAll(int s, const char *data, int n) {
        while (n > 0) {
            int sent = lwip_send(s, data, n, 0);
            if (sent <= 0)
                return false;
            data += sent;
            n -= sent;
        }
        return true;
    }

    bool ReadLine(std::string &line) {
        for (;;) {
            std::size_t end = pending.find('\n');
            if (end != std::string::npos) {
                line = pending.substr(0, end);
                pending.erase(0, end + 1);
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                return true;
            }
            char chunk[512];
            int n = lwip_recv(control, chunk, sizeof(chunk), 0);
            if (n <= 0)
                return false;
            pending.append(chunk, n);
            if (pending.size() > 4096)
                return false;
        }
    }

    void ClosePassive() {
        if (passive >= 0)
            lwip_close(passive);
        passive = -1;
    }

    void Passive(bool extended) {
        ClosePassive();
        haveActive = false;
        sockaddr_in local = {};
        socklen_t length = sizeof(local);
        lwip_getsockname(control, reinterpret_cast<sockaddr *>(&local), &length);
        // Port 0: lwIP picks a free port in 49152-65535, skipping ports still in TIME_WAIT. (A closed data connection keeps its port for 2 minutes; a fixed set
        // of 100 ports ran out when copying many small files.)
        {
            int s = lwip_socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in a = {};
            a.sin_family = AF_INET;
            a.sin_port = 0;
            a.sin_addr.s_addr = INADDR_ANY;
            sockaddr_in bound = {};
            socklen_t boundLength = sizeof(bound);
            if (s >= 0 && lwip_bind(s, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0 && lwip_listen(s, 1) == 0 && lwip_getsockname(s,
                reinterpret_cast<sockaddr *>(&bound), &boundLength) == 0) {
                unsigned short port = ntohs(bound.sin_port);
                passive = s;
                unsigned ip = ntohl(local.sin_addr.s_addr);
                if (extended) {
                    Reply(229, "Entering Extended Passive Mode (|||%u|)", port);
                } else {
                    Reply(227, "Entering Passive Mode (%u,%u,%u,%u,%u,%u)", ip >> 24, ip >> 16 & 255, ip >> 8 & 255, ip & 255, port >> 8, port & 255);
                }
                return;
            }
            if (s >= 0)
                lwip_close(s);
        }
        Reply(425, "No data port free");
    }

    // The data connection for a transfer; -1 (and 425 sent) when there is none.
    int OpenData() {
        int s = -1;
        if (passive >= 0) {
            fd_set set;
            FD_ZERO(&set);
            FD_SET(passive, &set);
            timeval wait = {20, 0};
            if (lwip_select(passive + 1, &set, nullptr, nullptr, &wait) > 0)
                s = lwip_accept(passive, nullptr, nullptr);
            ClosePassive();
        } else if (haveActive) {
            s = lwip_socket(AF_INET, SOCK_STREAM, 0);
            if (lwip_connect(s, reinterpret_cast<sockaddr *>(&active), sizeof(active)) != 0) {
                lwip_close(s);
                s = -1;
            }
            haveActive = false;
        }
        if (s < 0)
            Reply(425, "Use PASV or PORT first");
        return s;
    }

    bool Path(const std::string &arg, Parts &out) {
        if (!Resolve(cwd, arg, out)) {
            Reply(553, "Invalid path");
            return false;
        }
        return true;
    }

    void SendList(const std::string &arg, int kind) { // 0 LIST, 1 NLST, 2 MLSD
        std::string a = arg;
        while (!a.empty() && a[0] == '-') { // ls options (LIST -la): ignored
            std::size_t sp = a.find(' ');
            a = sp == std::string::npos ? "" : a.substr(sp + 1);
        }
        Parts p;
        if (!Path(a, p))
            return;
        std::vector<Entry> list;
        Info info = Stat(p);
        if (info.exists && !info.directory) {
            list.push_back({p.back(), false, info.size, info.written});
        } else if (!info.exists || !NT_SUCCESS(ListFolder(p, list))) {
            Reply(550, "No such folder");
            return;
        }
        int data = OpenData();
        if (data < 0)
            return;
        Reply(150, "Listing %s", FtpPath(p).c_str());
        std::string out;
        for (const auto &e : list) {
            char line[400], when[32], mdtm[20];
            TimeText(e.written, when, sizeof(when), mdtm, sizeof(mdtm));
            if (kind == 1) {
                std::snprintf(line, sizeof(line), "%s\r\n", e.name.c_str());
            } else if (kind == 2) {
                std::snprintf(line, sizeof(line), "type=%s;size=%lld;modify=%s; %s\r\n", e.directory ? "dir" : "file", e.size, mdtm, e.name.c_str());
            } else {
                std::snprintf(line, sizeof(line), "%s 1 xbox xbox %13lld %s %s\r\n", e.directory ? "drwxrwxrwx" : "-rw-rw-rw-", e.directory ? 0LL : e.size,
                              when, e.name.c_str());
            }
            out += line;
        }
        bool ok = SendAll(data, out.data(), (int)out.size());
        lwip_close(data);
        if (ok) {
            Log("%s listed %u entries", peer, (unsigned)list.size());
            Reply(226, "%u entries", (unsigned)list.size());
        }
        else
            Reply(426, "Connection lost");
    }

    void Retrieve(const std::string &arg) {
        Parts p;
        if (!Path(arg, p))
            return;
        HANDLE h;
        NTSTATUS status = p.size() < 2 ? STATUS_OBJECT_NAME_NOT_FOUND
                                       : Open(&h, NtPath(p), GENERIC_READ, FILE_OPEN, FILE_NON_DIRECTORY_FILE);
        if (!NT_SUCCESS(status)) {
            Reply(550, "%s", StatusText(status).c_str());
            return;
        }
        int data = OpenData();
        if (data < 0) {
            NtClose(h);
            return;
        }
        LARGE_INTEGER offset;
        offset.QuadPart = restart;
        restart = 0;
        FILE_STANDARD_INFORMATION standard;
        IO_STATUS_BLOCK info;
        LONGLONG size = NT_SUCCESS(NtQueryInformationFile(h, &info, &standard, sizeof(standard), FileStandardInformation)) ? standard.EndOfFile.QuadPart : 0;
        bool ok = true;
        { // the reader stops and frees its blocks before the file closes
            DiskReader reader;
            if (!reader.Start(h, offset.QuadPart)) {
                NtClose(h);
                lwip_close(data);
                Reply(451, "Out of memory");
                return;
            }
            Reply(150, "Sending %s", p.back().c_str());
            TransferInProgress transfer(false, offset.QuadPart, size);
            for (;;) {
                char *block;
                int length = reader.TakeBlock(block);
                if (length <= 0) {
                    ok = length == 0;
                    break;
                }
                bool sent = SendAll(data, block, length);
                reader.ReturnBlock();
                if (!sent) {
                    ok = false;
                    break;
                }
                offset.QuadPart += length;
                transfer.Count((unsigned)length);
            }
        }
        NtClose(h);
        lwip_close(data);
        if (ok) {
            Log("%s sent %s (%lld bytes)", peer, FtpPath(p).c_str(), (long long)offset.QuadPart);
            Reply(226, "Done");
        }
        else
            Reply(426, "Transfer failed");
    }

    void Store(const std::string &arg, bool append) {
        Parts p;
        if (!Path(arg, p))
            return;
        if (p.size() < 2) {
            Reply(553, "Not in a folder of a drive");
            return;
        }
        HANDLE h;
        bool resume = restart || append;
        NTSTATUS status = Open(&h, NtPath(p), GENERIC_WRITE, resume ? FILE_OPEN_IF : FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE, FILE_SHARE_READ);
        if (!NT_SUCCESS(status)) {
            Reply(550, "%s", StatusText(status).c_str());
            return;
        }
        LARGE_INTEGER offset;
        offset.QuadPart = restart;
        if (append) {
            FILE_STANDARD_INFORMATION standard;
            IO_STATUS_BLOCK iosb;
            if (NT_SUCCESS(NtQueryInformationFile(h, &iosb, &standard, sizeof(standard), FileStandardInformation)))
                offset.QuadPart = standard.EndOfFile.QuadPart;
        }
        restart = 0;
        int data = OpenData();
        if (data < 0) {
            NtClose(h);
            return;
        }
        LONGLONG size = announced;
        announced = 0;
        DiskWriter writer;
        if (!writer.Start(h, offset.QuadPart)) {
            NtClose(h);
            lwip_close(data);
            Reply(451, "Out of memory");
            return;
        }
        Reply(150, "Receiving %s", p.back().c_str());
        LONGLONG total = 0;
        bool ok = true, more = true;
        TransferInProgress transfer(true, offset.QuadPart, size);
        while (more) {
            char *block = writer.TakeBlock();
            if (!block)
                break;
            unsigned filled = 0;
            while (filled < DiskWriter::BlockSize) { // whole blocks: large, aligned disk writes
                int n = lwip_recv(data, block + filled, DiskWriter::BlockSize - filled, 0);
                if (n <= 0) {
                    more = false;
                    ok = n == 0;
                    break;
                }
                filled += n;
            }
            if (!filled) {
                writer.ReturnBlock();
                continue;
            }
            writer.QueueBlock(filled);
            total += filled;
            transfer.Count(filled);
        }
        status = writer.Finish();
        if (!NT_SUCCESS(status))
            ok = false;
        IO_STATUS_BLOCK iosb;
        NtFlushBuffersFile(h, &iosb);
        NtClose(h);
        lwip_close(data);
        if (ok) {
            Log("%s stored %s (%lld bytes)", peer, FtpPath(p).c_str(), total);
            Reply(226, "Stored %lld bytes", total);
        } else {
            Log("%s store FAILED %s (status %08lx)", peer, FtpPath(p).c_str(), (unsigned long)status);
            Reply(NT_SUCCESS(status) ? 426 : 552, "%s", NT_SUCCESS(status) ? "Connection lost" : StatusText(status).c_str());
        }
    }

    // SITE LAUNCH: an XBE, or a folder whose default.xbe is started (an ISO's folder holds the ISO launcher as default.xbe). The title starts through the
    // launch data page and a quick reboot, as a dashboard starts it; the reply goes out first, since the connection ends with the server.
    void Launch(const std::string &arg) {
        Parts p;
        if (arg.empty() || !Path(arg, p) || p.empty()) {
            Reply(501, "SITE LAUNCH /F/path/default.xbe (or a folder holding a default.xbe)");
            return;
        }
        Info info = Stat(p);
        if (info.exists && info.directory) {
            p.push_back("default.xbe");
            info = Stat(p);
        }
        std::string name = p.back();
        ToUpper(name);
        if (!info.exists || info.directory) {
            Reply(550, "%s: no such XBE", FtpPath(p).c_str());
            return;
        }
        if (name.size() < 4 || name.compare(name.size() - 4, 4, ".XBE") != 0) {
            Reply(550, "%s is not an XBE", FtpPath(p).c_str());
            return;
        }
        std::string path = NtPath(p);
        Reply(200, "Launching %s", FtpPath(p).c_str());
        Log("%s: launch %s", peer, FtpPath(p).c_str());
        Sleep(300); // let the reply reach the client
        RequestLaunch(path, false);
    }

    // SITE FORMAT-EXFAT YES: the kernel formats the hard disk as exFAT with the folders C E F X Y Z and C\OXOS and keeps this server's XBE at the same place,
    // then the server starts again from the new disk (a title launch, so the kernel mounts the new layout). Without YES it only says what it would do.
    void FormatDisk(const std::string &arg) {
        auto format = (FormatDiskRoutine)KernelExport(OrdinalFormatDisk);
        if (!format) {
            Log("%s: format: this kernel has no OXOSFormatDisk", peer);
            Reply(502, "This kernel cannot format the disk (it needs OXOS)");
            return;
        }
        if (arg != "YES" && arg != "yes") {
            Log("%s: format not confirmed (send SITE FORMAT-EXFAT YES)", peer);
            Reply(200, "SITE FORMAT-EXFAT YES erases the whole hard disk and makes it one exFAT volume");
            return;
        }
        // The running XBE (XeImageFileName, not terminated), kept when it is on the hard disk.
        std::string self(XeImageFileName->Buffer, XeImageFileName->Length);
        const std::string disk = "\\Device\\Harddisk0\\";
        bool keep = self.compare(0, disk.size(), disk) == 0;
        Log("%s: format the hard disk as exFAT (keeping %s)", peer, keep ? self.c_str() : "nothing");
        NTSTATUS status = format(keep ? self.c_str() : nullptr);
        if (!NT_SUCCESS(status)) {
            Log("format FAILED: %08lx", (unsigned long)status);
            Reply(550, "Format failed: %s", StatusText(status).c_str());
            return;
        }
        Log("formatted as exFAT; %s", keep ? "restarting the server from the new disk" : "rebooting");
        Reply(200, "Formatted as exFAT; %s", keep ? "the server restarts from the new disk" : "rebooting");
        Sleep(300); // let the reply reach the client
        if (!keep)
            HalReturnToFirmware(HalRebootRoutine);
        RequestLaunch(self, true);
    }

    // SITE DELETE-ALL-PARTITIONS YES [SECONDS]: the kernel clears every partition table and header, then the console reboots; OXOS finds a blank disk at the
    // next boot and formats it as exFAT after SECONDS (default 0: at once) of countdown. Without YES it only says what it would do.
    void DeleteAllPartitions(const std::string &arg) {
        auto remove = (DeleteAllPartitionsRoutine)KernelExport(OrdinalDeleteAllPartitions);
        if (!remove) {
            Log("%s: delete partitions: this kernel has no OXOSDeleteAllPartitions", peer);
            Reply(502, "This kernel cannot delete partitions (it needs OXOS)");
            return;
        }
        std::size_t gap = arg.find(' ');
        std::string confirm = arg.substr(0, gap);
        ULONG seconds = gap == std::string::npos ? 0 : (ULONG)std::strtoul(arg.c_str() + gap + 1, nullptr, 10);
        if (confirm != "YES" && confirm != "yes") {
            Log("%s: partition delete not confirmed (send SITE DELETE-ALL-PARTITIONS YES)", peer);
            Reply(200, "SITE DELETE-ALL-PARTITIONS YES deletes every partition; OXOS formats the blank disk at boot");
            return;
        }
        Log("%s: delete all partitions (the next boot formats after %lu s)", peer, seconds);
        NTSTATUS status = remove(seconds);
        if (!NT_SUCCESS(status)) {
            Log("delete all partitions FAILED: %08lx", (unsigned long)status);
            Reply(550, "Deleting the partitions failed: %s", StatusText(status).c_str());
            return;
        }
        Log("all partitions deleted; rebooting");
        Reply(200, "All partitions deleted; rebooting");
        Sleep(300);
        HalReturnToFirmware(HalRebootRoutine);
    }

    void Command(const std::string &line) {
        lastCommand = line.compare(0, 5, "PASS ") == 0 ? "PASS" : line; // never a password on the screen
        Log("%s: %s", peer, lastCommand.c_str()); // every command on the screen as it arrives
        std::size_t sp = line.find(' ');
        std::string verb = line.substr(0, sp), arg = sp == std::string::npos ? "" : line.substr(sp + 1);
        ToUpper(verb);
        Parts p;
        if (verb != "RNTO")
            haveFrom = verb == "RNFR" ? haveFrom : false;

        if (verb == "USER") {
            Reply(331, "Any password will do");
        } else if (verb == "PASS") {
            Reply(230, "Logged in");
        } else if (verb == "SYST") {
            Reply(215, "UNIX Type: L8");
        } else if (verb == "FEAT") {
            static const char features[] = "211-Features:\r\n SIZE\r\n MDTM\r\n REST STREAM\r\n PASV\r\n EPSV\r\n"
                                           " MLST type*;size*;modify*;\r\n211 End\r\n";
            SendAll(control, features, sizeof(features) - 1);
        } else if (verb == "ALLO") {
            announced = std::strtoll(arg.c_str(), nullptr, 10);
            Reply(200, "OK");
        } else if (verb == "OPTS" || verb == "MODE" || verb == "STRU") {
            Reply(200, "OK");
        } else if (verb == "TYPE") {
            Reply(200, "Type set (always binary)");
        } else if (verb == "NOOP") {
            Reply(200, "OK");
        } else if (verb == "PWD" || verb == "XPWD") {
            Reply(257, "\"%s\" is the current folder", FtpPath(cwd).c_str());
        } else if (verb == "CWD" || verb == "XCWD" || verb == "CDUP" || verb == "XCUP") {
            if (Path(verb == "CDUP" || verb == "XCUP" ? ".." : arg, p)) {
                Info info = Stat(p);
                if (info.exists && info.directory) {
                    cwd = p;
                    Reply(250, "Now in %s", FtpPath(cwd).c_str());
                } else {
                    Reply(550, "No such folder");
                }
            }
        } else if (verb == "PASV") {
            Passive(false);
        } else if (verb == "EPSV") {
            Passive(true);
        } else if (verb == "PORT") {
            unsigned v[6];
            if (std::sscanf(arg.c_str(), "%u,%u,%u,%u,%u,%u", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6) {
                ClosePassive();
                active = {};
                active.sin_family = AF_INET;
                active.sin_addr.s_addr = htonl(v[0] << 24 | v[1] << 16 | v[2] << 8 | v[3]);
                active.sin_port = htons((unsigned short)(v[4] << 8 | v[5]));
                haveActive = true;
                Reply(200, "PORT OK");
            } else {
                Reply(501, "Bad PORT");
            }
        } else if (verb == "LIST") {
            SendList(arg, 0);
        } else if (verb == "NLST") {
            SendList(arg, 1);
        } else if (verb == "MLSD") {
            SendList(arg, 2);
        } else if (verb == "MLST") {
            if (Path(arg, p)) {
                Info info = Stat(p);
                if (!info.exists) {
                    Reply(550, "No such file or folder");
                } else {
                    char mdtm[20], text[400];
                    TimeText(info.written, nullptr, 0, mdtm, sizeof(mdtm));
                    std::snprintf(text, sizeof(text), "250-Listing\r\n type=%s;size=%lld;modify=%s; %s\r\n250 End\r\n", info.directory ? "dir" : "file",
                                  info.size, mdtm, FtpPath(p).c_str());
                    SendAll(control, text, (int)std::strlen(text));
                }
            }
        } else if (verb == "RETR") {
            Retrieve(arg);
        } else if (verb == "STOR") {
            Store(arg, false);
        } else if (verb == "APPE") {
            Store(arg, true);
        } else if (verb == "REST") {
            restart = std::strtoll(arg.c_str(), nullptr, 10);
            Reply(350, "Restarting at %lld", restart);
        } else if (verb == "SIZE" || verb == "MDTM") {
            if (Path(arg, p)) {
                Info info = Stat(p);
                if (!info.exists || info.directory) {
                    Reply(550, "No such file");
                } else if (verb == "SIZE") {
                    Reply(213, "%lld", info.size);
                } else {
                    char mdtm[20];
                    TimeText(info.written, nullptr, 0, mdtm, sizeof(mdtm));
                    Reply(213, "%s", mdtm);
                }
            }
        } else if (verb == "DELE" || verb == "RMD" || verb == "XRMD") {
            if (Path(arg, p)) {
                NTSTATUS status = p.size() < 2 ? STATUS_ACCESS_DENIED : Delete(p, verb != "DELE");
                Log("%s delete %s: %s", peer, FtpPath(p).c_str(), NT_SUCCESS(status) ? "ok" : StatusText(status).c_str());
                if (NT_SUCCESS(status))
                    Reply(250, "Deleted");
                else
                    Reply(550, "%s", StatusText(status).c_str());
            }
        } else if (verb == "MKD" || verb == "XMKD") {
            if (Path(arg, p)) {
                NTSTATUS status = p.size() < 2 ? STATUS_ACCESS_DENIED : MakeFolder(p);
                Log("%s mkdir %s: %s", peer, FtpPath(p).c_str(), NT_SUCCESS(status) ? "ok" : StatusText(status).c_str());
                if (NT_SUCCESS(status))
                    Reply(257, "\"%s\" created", FtpPath(p).c_str());
                else
                    Reply(550, "%s", StatusText(status).c_str());
            }
        } else if (verb == "RNFR") {
            if (Path(arg, p)) {
                if (p.size() >= 2 && Stat(p).exists) {
                    renameFrom = p;
                    haveFrom = true;
                    Reply(350, "Ready for RNTO");
                } else {
                    haveFrom = false;
                    Reply(550, "No such file or folder");
                }
            }
        } else if (verb == "RNTO") {
            if (!haveFrom) {
                Reply(503, "RNFR first");
            } else if (Path(arg, p)) {
                NTSTATUS status = Rename(renameFrom, p);
                Log("%s move %s -> %s: %s", peer, FtpPath(renameFrom).c_str(), FtpPath(p).c_str(), NT_SUCCESS(status) ? "ok" : StatusText(status).c_str());
                if (NT_SUCCESS(status))
                    Reply(250, "Renamed");
                else
                    Reply(550, "%s", StatusText(status).c_str());
            }
            haveFrom = false;
        } else if (verb == "ABOR") {
            Reply(226, "Nothing to abort");
        } else if (verb == "SITE") {
            std::size_t gap = arg.find(' ');
            std::string what = arg.substr(0, gap), rest = gap == std::string::npos ? "" : arg.substr(gap + 1);
            ToUpper(what);
            if (what == "LAUNCH") {
                Launch(rest);
            } else if (what == "REBOOT") {
                Reply(200, "Rebooting");
                Log("%s: reboot", peer);
                HalReturnToFirmware(HalRebootRoutine);
            } else if (what == "POWEROFF") {
                Reply(200, "Powering off");
                Log("%s: power off", peer);
                HalInitiateShutdown();
            } else if (what == "FORMAT-EXFAT") {
                FormatDisk(rest);
            } else if (what == "DELETE-ALL-PARTITIONS") {
                DeleteAllPartitions(rest);
            } else {
                Log("%s: unknown SITE command", peer);
                Reply(504, "SITE LAUNCH PATH, REBOOT, POWEROFF, FORMAT-EXFAT or DELETE-ALL-PARTITIONS");
            }
        } else {
            Reply(502, "%s not implemented", verb.c_str());
        }
    }

    void Run() {
        Reply(220, "%s", ServerName);
        std::string line;
        while (ReadLine(line)) {
            if (line.empty())
                continue;
            std::string verb = line.substr(0, 4);
            ToUpper(verb);
            if (verb == "QUIT") {
                Reply(221, "Bye");
                break;
            }
            Command(line);
        }
        ClosePassive();
    }
};

DWORD WINAPI SessionThread(LPVOID parameter) {
    auto session = static_cast<Session *>(parameter);
    session->Run();
    Log("%s disconnected", session->peer);
    lwip_close(session->control);
    delete session;
    InterlockedDecrement(&SessionCount);
    return 0;
}

// --- The network --------------------------------------------------------------------------------------------------

// Starts the network (DHCP, else the fixed address), then accepts clients, each in a thread of its own.
DWORD WINAPI ServerThread(LPVOID) {
    nx_net_parameters_t dhcp = {};
    dhcp.ipv4_mode = NX_NET_DHCP;
    dhcp.ipv6_mode = NX_NET_DHCP;
    int r = nxNetInit(&dhcp);
    if (r == -2) { // no DHCP answer in nxdk's 10 s: DHCP keeps running, so wait for it a little longer
        Log("waiting for DHCP...");
        for (ULONG waited = 10000; waited < DhcpWaitMs && !dhcp_supplied_address(g_pnetif); waited += 250)
            Sleep(250);
        if (dhcp_supplied_address(g_pnetif))
            r = 0;
    }
    if (r == -2) { // still no DHCP answer: a fixed address
        netifapi_dhcp_stop(g_pnetif);
        ip4_addr_t ip, mask, gateway;
        ip.addr = FallbackIp;
        IP4_ADDR(&mask, 255, 255, 255, 0);
        IP4_ADDR(&gateway, 192, 168, 1, 1);
        netifapi_netif_set_addr(g_pnetif, &ip, &mask, &gateway);
        Log("no DHCP answer: fixed address");
    } else if (r != 0) {
        Log("network start failed (%d)", r);
        return 1;
    }
    Log("ftp://%s/  (any user name and password)", ip4addr_ntoa(netif_ip4_addr(g_pnetif)));

    int listener = lwip_socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(ControlPort);
    a.sin_addr.s_addr = INADDR_ANY;
    if (lwip_bind(listener, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0 || lwip_listen(listener, 4) != 0) {
        Log("cannot listen on port %u", ControlPort);
        return 1;
    }
    for (;;) {
        sockaddr_in from = {};
        socklen_t length = sizeof(from);
        int s = lwip_accept(listener, reinterpret_cast<sockaddr *>(&from), &length);
        if (s < 0)
            continue;
        if (SessionCount >= MaxSessions) {
            static const char busy[] = "421 Too many connections\r\n";
            lwip_send(s, busy, sizeof(busy) - 1, 0);
            lwip_close(s);
            continue;
        }
        int one = 1;
        lwip_setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        auto session = new Session;
        session->control = s;
        std::snprintf(session->peer, sizeof(session->peer), "%s", inet_ntoa(from.sin_addr));
        Log("%s connected", session->peer);
        InterlockedIncrement(&SessionCount);
        HANDLE thread = CreateThread(nullptr, SessionStack, SessionThread, session, 0, nullptr);
        if (thread)
            CloseHandle(thread);
        else
            SessionThread(session);
    }
}

} // namespace

// The main thread: the screen, the controllers (BACK reboots the console cold, as SITE REBOOT: every device starts reset, and a netbooted console starts what
// the PC serves; quitting to the dashboard with a quick reboot hung the Xbox dashboard on the 1.4 console) and the titles SITE LAUNCH starts.
int main() {
    InitializeCriticalSection(&PrintLock);
    if (!sdl_screen::Open(true))
        DbgPrint("ftp: SDL: %s\n", SDL_GetError());
    Log("%s", ServerName);
    Log("(BACK: reboot)\n");
    if (HANDLE server = CreateThread(nullptr, SessionStack, ServerThread, nullptr, 0, nullptr))
        CloseHandle(server);
    for (;;) {
        sdl_screen::Poll();
        if (sdl_screen::Pressed(SDL_GAMEPAD_BUTTON_BACK)) {
            Log("BACK: reboot");
            HalReturnToFirmware(HalRebootRoutine);
        }
        LaunchIfRequested();
        ShowTransferSpeeds();
        sdl_screen::Present();
        if (!sdl_screen::IsOpen())
            Sleep(10);
    }
}
