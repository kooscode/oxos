// FTP server (nxdk, open source): the console's disks over the network, for copying, moving and deleting files with
// any FTP client (FileZilla, curl, lftp, Windows Explorer). No log-in: any user name and password are accepted.
//
//   /            the drives: C, E, F, G, X, Y, Z (those that exist)
//   /F/games     F:\games (\Device\Harddisk0\Partition6\games)
//
// Commands: USER PASS SYST FEAT OPTS PWD CWD CDUP TYPE MODE STRU PASV EPSV PORT LIST NLST MLSD MLST RETR STOR APPE
// REST SIZE MDTM DELE MKD RMD RNFR RNTO NOOP ABOR QUIT, SITE REBOOT (cold reboot) and SITE POWEROFF.
// RNTO moves a file or folder to another folder of the same drive. Passive data ports are 50000-50099.
// The address (DHCP, else 192.168.1.223) is on the screen and, with a SERIAL=1 kernel, on the debug serial port.
// The C++ headers first: lwIP's socket macros (bind, connect, ...) would break them.
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <hal/debug.h>
#include <hal/video.h>
#include <lwip/sockets.h>
#include <lwip/netif.h>
#include <lwip/netifapi.h>
#include <lwip/dhcp.h>
#include <nxdk/net.h>
#include <windows.h>
#include <xboxkrnl/xboxkrnl.h>


extern "C" struct netif *g_pnetif; // nxdk's network interface (lib/nxdk/net.c)

namespace {

constexpr unsigned short ControlPort = 21, DataPortFirst = 50000, DataPorts = 100;
constexpr unsigned BufferSize = 64 * 1024;
constexpr int MaxSessions = 8;
constexpr NTSTATUS STATUS_NOT_SAME_DEVICE = (NTSTATUS)0xC00000D4;
constexpr ULONG FallbackIp = 0xDF01A8C0; // 192.168.1.223, little-endian as nxdk wants it

// --- Output: the screen and the debug serial port -----------------------------------------------------------------

CRITICAL_SECTION PrintLock;

void Log(const char *format, ...) {
    char line[300];
    va_list ap;
    va_start(ap, format);
    std::vsnprintf(line, sizeof(line), format, ap);
    va_end(ap);
    EnterCriticalSection(&PrintLock);
    debugPrint("%s\n", line);
    DbgPrint("ftp: %s\n", line);
    LeaveCriticalSection(&PrintLock);
}

// --- Paths --------------------------------------------------------------------------------------------------------

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
constexpr char Drives[] = "CEFGXYZ";

// An FTP path as its parts: {"F", "games", "halo"}; parts[0] is the drive.
using Parts = std::vector<std::string>;

// cwd + arg, with "." and ".." resolved; both slash kinds accepted. False for a name with a character that cannot
// be in a file name.
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

NTSTATUS Open(HANDLE *handle, const std::string &path, ACCESS_MASK access, ULONG disposition, ULONG options,
              ULONG share = FILE_SHARE_READ | FILE_SHARE_WRITE) {
    ANSI_STRING name;
    OBJECT_ATTRIBUTES attributes;
    IO_STATUS_BLOCK iosb;
    RtlInitAnsiString(&name, path.c_str());
    InitializeObjectAttributes(&attributes, &name, OBJ_CASE_INSENSITIVE, nullptr, nullptr);
    return NtCreateFile(handle, access | SYNCHRONIZE, &attributes, &iosb, nullptr, FILE_ATTRIBUTE_NORMAL, share,
                        disposition, options | FILE_SYNCHRONOUS_IO_NONALERT);
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
        for (const char *d = Drives; *d; d++)
            if (Stat(Parts{std::string(1, *d)}).exists)
                list.push_back({std::string(1, *d), true, 0, 0});
        return STATUS_SUCCESS;
    }
    HANDLE folder;
    NTSTATUS status = Open(&folder, NtPath(p), FILE_LIST_DIRECTORY, FILE_OPEN, FILE_DIRECTORY_FILE);
    if (!NT_SUCCESS(status))
        return status;
    alignas(8) unsigned char buffer[4096];
    for (bool first = true;; first = false) {
        IO_STATUS_BLOCK iosb;
        status = NtQueryDirectoryFile(folder, nullptr, nullptr, nullptr, &iosb, buffer, sizeof(buffer),
                                      FileDirectoryInformation, nullptr, first);
        if (!NT_SUCCESS(status))
            break; // STATUS_NO_MORE_FILES at the end
        for (ULONG offset = 0;;) {
            auto e = reinterpret_cast<FILE_DIRECTORY_INFORMATION *>(buffer + offset);
            list.push_back({std::string(e->FileName, e->FileNameLength),
                            (e->FileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0, e->EndOfFile.QuadPart,
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

const char *StatusText(NTSTATUS s) {
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
    default: return "Failed";
    }
}

// --- Sessions -----------------------------------------------------------------------------------------------------

volatile LONG NextDataPort = 0, SessionCount = 0;

struct Session {
    int control;
    Parts cwd;
    int passive = -1;          // listening data socket (PASV/EPSV)
    sockaddr_in active = {};   // PORT address
    bool haveActive = false;
    LONGLONG restart = 0;      // REST
    bool haveFrom = false;     // RNFR
    Parts renameFrom;
    char *buffer;
    std::string pending;       // received control bytes not yet a line
    char peer[20];

    void Reply(int code, const char *format, ...) {
        char line[512];
        int n = std::snprintf(line, sizeof(line), "%d ", code);
        va_list ap;
        va_start(ap, format);
        n += std::vsnprintf(line + n, sizeof(line) - n - 2, format, ap);
        va_end(ap);
        if (n > (int)sizeof(line) - 3)
            n = sizeof(line) - 3;
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
        for (int tries = 0; tries < DataPorts; tries++) {
            unsigned short port = DataPortFirst + (unsigned)InterlockedIncrement(&NextDataPort) % DataPorts;
            int s = lwip_socket(AF_INET, SOCK_STREAM, 0);
            sockaddr_in a = {};
            a.sin_family = AF_INET;
            a.sin_port = htons(port);
            a.sin_addr.s_addr = INADDR_ANY;
            if (lwip_bind(s, reinterpret_cast<sockaddr *>(&a), sizeof(a)) == 0 && lwip_listen(s, 1) == 0) {
                passive = s;
                unsigned ip = ntohl(local.sin_addr.s_addr);
                if (extended)
                    Reply(229, "Entering Extended Passive Mode (|||%u|)", port);
                else
                    Reply(227, "Entering Passive Mode (%u,%u,%u,%u,%u,%u)", ip >> 24, ip >> 16 & 255, ip >> 8 & 255,
                          ip & 255, port >> 8, port & 255);
                return;
            }
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
        if (info.exists && !info.directory)
            list.push_back({p.back(), false, info.size, info.written});
        else if (!info.exists || !NT_SUCCESS(ListFolder(p, list))) {
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
            if (kind == 1)
                std::snprintf(line, sizeof(line), "%s\r\n", e.name.c_str());
            else if (kind == 2)
                std::snprintf(line, sizeof(line), "type=%s;size=%lld;modify=%s; %s\r\n", e.directory ? "dir" : "file",
                              e.size, mdtm, e.name.c_str());
            else
                std::snprintf(line, sizeof(line), "%s 1 xbox xbox %13lld %s %s\r\n",
                              e.directory ? "drwxrwxrwx" : "-rw-rw-rw-", e.directory ? 0LL : e.size, when,
                              e.name.c_str());
            out += line;
        }
        bool ok = SendAll(data, out.data(), (int)out.size());
        lwip_close(data);
        if (ok)
            Reply(226, "%u entries", (unsigned)list.size());
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
            Reply(550, "%s", StatusText(status));
            return;
        }
        int data = OpenData();
        if (data < 0) {
            NtClose(h);
            return;
        }
        Reply(150, "Sending %s", p.back().c_str());
        LARGE_INTEGER offset;
        offset.QuadPart = restart;
        restart = 0;
        bool ok = true;
        for (;;) {
            IO_STATUS_BLOCK iosb;
            status = NtReadFile(h, nullptr, nullptr, nullptr, &iosb, buffer, BufferSize, &offset);
            if (!NT_SUCCESS(status) || !iosb.Information) {
                ok = status == STATUS_END_OF_FILE || NT_SUCCESS(status);
                break;
            }
            if (!SendAll(data, buffer, (int)iosb.Information)) {
                ok = false;
                break;
            }
            offset.QuadPart += iosb.Information;
        }
        NtClose(h);
        lwip_close(data);
        if (ok)
            Reply(226, "Done");
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
        NTSTATUS status = Open(&h, NtPath(p), GENERIC_WRITE, resume ? FILE_OPEN_IF : FILE_OVERWRITE_IF,
                               FILE_NON_DIRECTORY_FILE, FILE_SHARE_READ);
        if (!NT_SUCCESS(status)) {
            Reply(550, "%s", StatusText(status));
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
        Reply(150, "Receiving %s", p.back().c_str());
        LONGLONG total = 0;
        bool ok = true, more = true;
        while (more) {
            unsigned filled = 0;
            while (filled < BufferSize) { // whole buffers: large, aligned disk writes
                int n = lwip_recv(data, buffer + filled, BufferSize - filled, 0);
                if (n <= 0) {
                    more = false;
                    ok = n == 0;
                    break;
                }
                filled += n;
            }
            if (filled) {
                IO_STATUS_BLOCK iosb;
                status = NtWriteFile(h, nullptr, nullptr, nullptr, &iosb, buffer, filled, &offset);
                if (!NT_SUCCESS(status)) {
                    ok = false;
                    break;
                }
                offset.QuadPart += filled;
                total += filled;
            }
        }
        IO_STATUS_BLOCK iosb;
        NtFlushBuffersFile(h, &iosb);
        NtClose(h);
        lwip_close(data);
        if (ok) {
            Log("%s stored %s (%lld bytes)", peer, FtpPath(p).c_str(), total);
            Reply(226, "Stored %lld bytes", total);
        } else {
            Log("%s store FAILED %s (status %08lx)", peer, FtpPath(p).c_str(), (unsigned long)status);
            Reply(NT_SUCCESS(status) ? 426 : 552, "%s", NT_SUCCESS(status) ? "Connection lost" : StatusText(status));
        }
    }

    void Command(const std::string &line) {
        std::size_t sp = line.find(' ');
        std::string verb = line.substr(0, sp), arg = sp == std::string::npos ? "" : line.substr(sp + 1);
        for (char &c : verb)
            c &= ~0x20;
        Parts p;
        if (verb != "RNTO")
            haveFrom = verb == "RNFR" ? haveFrom : false;

        if (verb == "USER")
            Reply(331, "Any password will do");
        else if (verb == "PASS")
            Reply(230, "Logged in");
        else if (verb == "SYST")
            Reply(215, "UNIX Type: L8");
        else if (verb == "FEAT") {
            static const char features[] = "211-Features:\r\n SIZE\r\n MDTM\r\n REST STREAM\r\n PASV\r\n EPSV\r\n"
                                           " MLST type*;size*;modify*;\r\n211 End\r\n";
            SendAll(control, features, sizeof(features) - 1);
        } else if (verb == "OPTS" || verb == "MODE" || verb == "STRU" || verb == "ALLO")
            Reply(200, "OK");
        else if (verb == "TYPE")
            Reply(200, "Type set (always binary)");
        else if (verb == "NOOP")
            Reply(200, "OK");
        else if (verb == "PWD" || verb == "XPWD")
            Reply(257, "\"%s\" is the current folder", FtpPath(cwd).c_str());
        else if (verb == "CWD" || verb == "XCWD" || verb == "CDUP" || verb == "XCUP") {
            if (Path(verb == "CDUP" || verb == "XCUP" ? ".." : arg, p)) {
                Info info = Stat(p);
                if (info.exists && info.directory) {
                    cwd = p;
                    Reply(250, "Now in %s", FtpPath(cwd).c_str());
                } else {
                    Reply(550, "No such folder");
                }
            }
        } else if (verb == "PASV")
            Passive(false);
        else if (verb == "EPSV")
            Passive(true);
        else if (verb == "PORT") {
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
        } else if (verb == "LIST")
            SendList(arg, 0);
        else if (verb == "NLST")
            SendList(arg, 1);
        else if (verb == "MLSD")
            SendList(arg, 2);
        else if (verb == "MLST") {
            if (Path(arg, p)) {
                Info info = Stat(p);
                if (!info.exists) {
                    Reply(550, "No such file or folder");
                } else {
                    char mdtm[20], text[400];
                    TimeText(info.written, nullptr, 0, mdtm, sizeof(mdtm));
                    std::snprintf(text, sizeof(text), "250-Listing\r\n type=%s;size=%lld;modify=%s; %s\r\n250 End\r\n",
                                  info.directory ? "dir" : "file", info.size, mdtm, FtpPath(p).c_str());
                    SendAll(control, text, (int)std::strlen(text));
                }
            }
        } else if (verb == "RETR")
            Retrieve(arg);
        else if (verb == "STOR")
            Store(arg, false);
        else if (verb == "APPE")
            Store(arg, true);
        else if (verb == "REST") {
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
                Log("%s delete %s: %s", peer, FtpPath(p).c_str(), NT_SUCCESS(status) ? "ok" : StatusText(status));
                if (NT_SUCCESS(status))
                    Reply(250, "Deleted");
                else
                    Reply(550, "%s", StatusText(status));
            }
        } else if (verb == "MKD" || verb == "XMKD") {
            if (Path(arg, p)) {
                NTSTATUS status = p.size() < 2 ? STATUS_ACCESS_DENIED : MakeFolder(p);
                Log("%s mkdir %s: %s", peer, FtpPath(p).c_str(), NT_SUCCESS(status) ? "ok" : StatusText(status));
                if (NT_SUCCESS(status))
                    Reply(257, "\"%s\" created", FtpPath(p).c_str());
                else
                    Reply(550, "%s", StatusText(status));
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
                Log("%s move %s -> %s: %s", peer, FtpPath(renameFrom).c_str(), FtpPath(p).c_str(),
                    NT_SUCCESS(status) ? "ok" : StatusText(status));
                if (NT_SUCCESS(status))
                    Reply(250, "Renamed");
                else
                    Reply(550, "%s", StatusText(status));
            }
            haveFrom = false;
        } else if (verb == "ABOR")
            Reply(226, "Nothing to abort");
        else if (verb == "SITE") {
            std::string what = arg;
            for (char &c : what)
                c &= ~0x20;
            if (what == "REBOOT") {
                Reply(200, "Rebooting");
                Log("%s: reboot", peer);
                HalReturnToFirmware(HalRebootRoutine);
            } else if (what == "POWEROFF") {
                Reply(200, "Powering off");
                Log("%s: power off", peer);
                HalInitiateShutdown();
            } else {
                Reply(504, "SITE REBOOT or SITE POWEROFF");
            }
        } else
            Reply(502, "%s not implemented", verb.c_str());
    }

    void Run() {
        Reply(220, "OXOS FTP server");
        std::string line;
        while (ReadLine(line)) {
            if (line.empty())
                continue;
            std::string verb = line.substr(0, 4);
            for (char &c : verb)
                c &= ~0x20;
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
    delete[] session->buffer;
    delete session;
    InterlockedDecrement(&SessionCount);
    return 0;
}

} // namespace

int main() {
    XVideoSetMode(640, 480, 32, REFRESH_DEFAULT);
    InitializeCriticalSection(&PrintLock);
    debugPrint("OXOS FTP server\n\n");

    nx_net_parameters_t dhcp = {};
    dhcp.ipv4_mode = NX_NET_DHCP;
    dhcp.ipv6_mode = NX_NET_DHCP;
    int r = nxNetInit(&dhcp);
    if (r == -2) { // no DHCP answer in 10 s: a fixed address
        netifapi_dhcp_stop(g_pnetif);
        ip4_addr_t ip, mask, gateway;
        ip.addr = FallbackIp;
        IP4_ADDR(&mask, 255, 255, 255, 0);
        IP4_ADDR(&gateway, 192, 168, 1, 1);
        netifapi_netif_set_addr(g_pnetif, &ip, &mask, &gateway);
        Log("no DHCP answer: fixed address");
    } else if (r != 0) {
        Log("network start failed (%d)", r);
        for (;;)
            Sleep(1000);
    }
    Log("ftp://%s/  (any user name and password)", ip4addr_ntoa(netif_ip4_addr(g_pnetif)));

    int listener = lwip_socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in a = {};
    a.sin_family = AF_INET;
    a.sin_port = htons(ControlPort);
    a.sin_addr.s_addr = INADDR_ANY;
    if (lwip_bind(listener, reinterpret_cast<sockaddr *>(&a), sizeof(a)) != 0 || lwip_listen(listener, 4) != 0) {
        Log("cannot listen on port %u", ControlPort);
        for (;;)
            Sleep(1000);
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
        session->buffer = new char[BufferSize];
        std::snprintf(session->peer, sizeof(session->peer), "%s", inet_ntoa(from.sin_addr));
        Log("%s connected", session->peer);
        InterlockedIncrement(&SessionCount);
        HANDLE thread = CreateThread(nullptr, 64 * 1024, SessionThread, session, 0, nullptr);
        if (thread)
            CloseHandle(thread);
        else
            SessionThread(session);
    }
}
