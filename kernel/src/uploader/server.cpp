// The upload server (spec/oxos_internal/31_upload_server.md §3): HTTP/1.1 on the kernel's network stack, one request
// per connection: the page, folder listings, uploads written to the disk as they arrive, creating a folder, renaming
// and deleting a file or folder (a folder only when empty), and launching an XBE.
// Boot-only.
//
// Each connection's request moves through phases: its head (request line and headers), then an upload's body, then
// the reply, queued to the connection as its send buffer has room. Everything the server keeps is in one block,
// allocated when it starts and freed by UploadStop.
#include <xk/uploader.h>
#include <xk/network.h>
#include <xk/bootscreen.h>
#include <xk/core.h>
#include <xk/io.h>
#include <xk/memory.h>
#include <xk/objects.h>
#include <xk/runtime.h>
#include <xk/serial.h>
#include <xk/storage.h>

#pragma code_seg(".xkb")
#pragma const_seg(".xka$y")

constexpr ULONG HEAD_SIZE = 4096, CHUNK_SIZE = 0x20000, JSON_SIZE = 0x8000, PATH_SIZE = 260, STATUS_SIZE = 64;
constexpr NTSTATUS STATUS_NO_MORE_FILES_ = (NTSTATUS)0x80000006, STATUS_NO_SUCH_FILE_ = (NTSTATUS)0xC000000F,
                   STATUS_DIRECTORY_NOT_EMPTY_ = (NTSTATUS)0xC0000101,
                   STATUS_OBJECT_NAME_COLLISION_ = (NTSTATUS)0xC0000035;

enum UPLOADP_PHASE : ULONG {
    UploadpIdle,      // no request
    UploadpReadHead,  // reading the request line and headers
    UploadpReadBody,  // writing an upload's body to its file (or reading it past an error)
    UploadpQueueReply, // queuing the reply
    UploadpClosing    // the reply is queued and the connection closing
};

struct UPLOADP_REQUEST {
    UPLOADP_PHASE Phase;
    LONG Connection;
    char Head[HEAD_SIZE]; // the request line and headers (HeadLength bytes)
    ULONG HeadLength;
    char Path[PATH_SIZE];   // the request's path= value, as the page sends it ("/E/Apps/x.xbe")
    char Shown[PATH_SIZE];  // Path as messages show it: E:\Apps on a FATX disk, /E/Apps on an exFAT one
    char NtPath[PATH_SIZE]; // the same as an NT path
    // An upload: its file, the bytes still to come, the bytes not yet written, and the first error.
    HANDLE File;
    ULONGLONG Remaining, Received;
    UCHAR Chunk[CHUNK_SIZE];
    ULONG ChunkUsed;
    NTSTATUS Error;
    bool Launch; // the server returns the path once this reply has gone out
    // The reply: its header, then its body (the page, or Json), each queued as far as the connection takes it.
    char Header[256];
    ULONG HeaderLength, HeaderSent;
    const char *Body;
    ULONG BodyLength, BodySent;
    char Json[JSON_SIZE];
};

struct UPLOADP_SERVER {
    UPLOADP_REQUEST Requests[NET_TCP_CONNECTIONS];
    char Status[STATUS_SIZE];   // the screen's status line
    char Shown[STATUS_SIZE + 32]; // what the screen shows now (address and status), to redraw only on a change
    char Launch[PATH_SIZE];     // the NT path of the XBE to launch, once its reply has gone out
    bool LaunchReady;
};

static UPLOADP_SERVER *UploadpServer; // null until the server starts
volatile ULONG UploadServing;         // 1 while the server runs (read by the boot tests)

// --- The screen's status line

static void UploadpStatus(const char *Format, ...) {
    va_list args;
    __builtin_va_start(args, Format);
    RtlVsnprintf(UploadpServer->Status, STATUS_SIZE, Format, args);
    __builtin_va_end(args);
}

// "Prefix path", keeping the path's end when the line would not fit the screen.
static void UploadpStatusPath(const char *Prefix, const char *Path) {
    ULONG room = 45, used = 0, length = 0;
    while (Prefix[used])
        used++;
    while (Path[length])
        length++;
    if (used + 1 + length <= room)
        return UploadpStatus("%s %s", Prefix, Path);
    ULONG keep = room > used + 4 ? room - used - 4 : 0;
    UploadpStatus("%s ...%s", Prefix, Path + length - (keep < length ? keep : length));
}

static void UploadpUpdateScreen() {
    char address[20];
    NetAddressText(address, sizeof(address));
    const char *network = NetState() == NetNoLink   ? "Connect a network cable."
                          : NetState() == NetAsking ? "Asking the network for an address..."
                                                    : nullptr;
    const char *status = UploadpServer->Status[0] ? UploadpServer->Status : network;
    char shown[sizeof(UploadpServer->Shown)];
    RtlSnprintf(shown, sizeof(shown), "%s|%s", address, status ? status : "Waiting for a browser.");
    for (ULONG i = 0; i < sizeof(shown); i++) {
        if (shown[i] != UploadpServer->Shown[i]) {
            RtlCopyMemory(UploadpServer->Shown, shown, sizeof(shown));
            XkShowFileManagerScreen(address, status ? status : "Waiting for a browser.");
            return;
        }
        if (!shown[i])
            return;
    }
}

// --- Paths

// A hexadecimal digit's value, or -1.
static int UploadpHexDigit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f')
        return (c | 0x20) - 'a' + 10;
    return -1;
}

// Text starts with Word.
static bool UploadpIs(const char *Text, const char *Word) {
    ULONG i = 0;
    while (Word[i] && Text[i] == Word[i])
        i++;
    return !Word[i];
}

// The decoded value of query parameter Key ("path=", "name=") in Target ("/api/list?path=%2FE%2FApps"), or false.
static bool UploadpQueryValue(const char *Target, const char *Key, char *Out, ULONG Size) {
    const char *p = Target;
    while (*p && *p != ' ' && !((*p == '?' || *p == '&') && UploadpIs(p + 1, Key)))
        p++;
    if (*p != '?' && *p != '&')
        return false;
    for (p++; *Key; Key++)
        p++;
    ULONG n = 0;
    for (; *p && *p != '&' && *p != ' '; p++) {
        char c = *p;
        if (c == '%' && UploadpHexDigit(p[1]) >= 0 && UploadpHexDigit(p[2]) >= 0) {
            c = (char)(UploadpHexDigit(p[1]) << 4 | UploadpHexDigit(p[2]));
            p += 2;
        } else if (c == '+') {
            c = ' ';
        }
        if (n + 1 >= Size)
            return false;
        Out[n++] = c;
    }
    Out[n] = 0;
    return n > 0;
}

// A character FATX allows in a name (exFAT allows a few more; FATX's rule holds on both).
static bool UploadpNameCharacter(char c) {
    return (UCHAR)c >= 0x20 && c != '"' && c != '*' && c != '/' && c != ':' && c != '<' && c != '>' && c != '?' &&
           c != '\\' && c != '|';
}

// The NT path of a page path ("/E/Apps/x.xbe" -> "\Device\Harddisk0\Partition1\Apps\x.xbe"); false for an unknown
// drive, an empty part, "." or "..", or a character FATX does not allow. "/" alone is the list of drives.
static bool UploadpNtPath(const char *Path, char *Out, ULONG Size) {
    static const char drives[] = "CEFGXYZ";
    static const UCHAR partitions[] = {2, 1, 6, 7, 3, 4, 5};
    if (Path[0] != '/' || !Path[1] || (Path[2] && Path[2] != '/'))
        return false;
    ULONG drive = 0;
    char letter = (char)(Path[1] & ~0x20);
    while (drives[drive] && drives[drive] != letter)
        drive++;
    if (!drives[drive])
        return false;
    ULONG n = (ULONG)RtlSnprintf(Out, Size, "\\Device\\Harddisk0\\Partition%u", partitions[drive]);
    const ULONG prefix = n;
    const char *p = Path + 2;
    while (*p) { // each "/part"
        p++;
        ULONG length = 0;
        while (p[length] && p[length] != '/')
            length++;
        if (!length && !p[length]) // a trailing slash
            break;
        bool dots = length <= 2 && p[0] == '.' && (length == 1 || p[1] == '.');
        if (!length || dots || n + 1 + length + 1 > Size)
            return false;
        Out[n++] = '\\';
        for (ULONG i = 0; i < length; i++) {
            if (!UploadpNameCharacter(p[i]))
                return false;
            Out[n++] = p[i];
        }
        p += length;
    }
    if (n == prefix && n + 1 < Size) // a drive alone: its root folder (without the '\\', the partition itself)
        Out[n++] = '\\';
    Out[n] = 0;
    return true;
}

// Opens or creates NtPath; Size, when not 0, is the allocation a new file starts with (an upload's length: its
// clusters are allocated once, not with every write).
static NTSTATUS UploadpOpen(const char *NtPath, ACCESS_MASK Access, ULONG Disposition, ULONG Options, HANDLE *h,
                            ULONGLONG Size = 0) {
    ANSI_STRING name;
    RtlInitAnsiString(&name, NtPath);
    OBJECT_ATTRIBUTES a = {nullptr, &name, OBJ_CASE_INSENSITIVE};
    IO_STATUS_BLOCK iosb;
    LARGE_INTEGER allocation;
    allocation.QuadPart = (LONGLONG)Size;
    return NtCreateFile(h, Access | SYNCHRONIZE, &a, &iosb, Size ? &allocation : nullptr, FILE_ATTRIBUTE_NORMAL,
                        FILE_SHARE_READ | FILE_SHARE_WRITE, Disposition, Options | FILE_SYNCHRONOUS_IO_NONALERT);
}

// The folders on the way to NtPath's last part, created where missing.
static NTSTATUS UploadpMakeFolders(char *NtPath) {
    ULONG skip = 0, separators = 0;
    for (; NtPath[skip] && separators < 4; skip++) // past "\Device\Harddisk0\PartitionN\"
        separators += NtPath[skip] == '\\';
    for (char *p = NtPath + skip; *p; p++) {
        if (*p != '\\')
            continue;
        *p = 0;
        HANDLE h;
        NTSTATUS status = UploadpOpen(NtPath, GENERIC_READ, FILE_OPEN_IF, FILE_DIRECTORY_FILE, &h);
        *p = '\\';
        if (!NT_SUCCESS(status))
            return status;
        NtClose(h);
    }
    return STATUS_SUCCESS;
}

// --- Replies

static void UploadpReply(UPLOADP_REQUEST *r, ULONG Code, const char *Type, const char *Body, ULONG Length) {
    const char *reason = Code == 200 ? "OK" : Code == 201 ? "Created" : Code == 400 ? "Bad Request"
                         : Code == 403 ? "Forbidden" : Code == 404 ? "Not Found" : Code == 405 ? "Method Not Allowed"
                         : Code == 409 ? "Conflict"
                         : Code == 411 ? "Length Required" : Code == 431 ? "Request Header Fields Too Large"
                                                                         : "Internal Server Error";
    r->HeaderLength = (ULONG)RtlSnprintf(r->Header, sizeof(r->Header),
                                         "HTTP/1.1 %lu %s\r\nContent-Type: %s\r\nContent-Length: %lu\r\n"
                                         "Cache-Control: no-store\r\nConnection: close\r\n\r\n",
                                         Code, reason, Type, Length);
    r->HeaderSent = r->BodySent = 0;
    r->Body = Body;
    r->BodyLength = Length;
    r->Phase = UploadpQueueReply;
}

// A plain-text reply: the message is kept in Json.
static void UploadpText(UPLOADP_REQUEST *r, ULONG Code, const char *Format, ...) {
    va_list args;
    __builtin_va_start(args, Format);
    ULONG n = (ULONG)RtlVsnprintf(r->Json, sizeof(r->Json), Format, args);
    __builtin_va_end(args);
    UploadpReply(r, Code, "text/plain; charset=utf-8", r->Json, n);
}

// --- Listing a folder

// Appends "name" (JSON-escaped) to Json at *n.
static void UploadpJsonName(UPLOADP_REQUEST *r, ULONG *n, const char *Name, ULONG Length) {
    for (ULONG i = 0; i < Length && *n + 8 < sizeof(r->Json); i++) {
        UCHAR c = (UCHAR)Name[i];
        if (c == '"' || c == '\\')
            r->Json[(*n)++] = '\\';
        if (c < 0x20)
            c = '?';
        r->Json[(*n)++] = (char)c;
    }
}

static void UploadpList(UPLOADP_REQUEST *r) {
    ULONG n = 0;
    auto entry = [&](const char *name, ULONG length, bool folder, ULONGLONG size) {
        if (n + length + 64 >= sizeof(r->Json)) // a folder too large for one reply: the rest is left out
            return;
        n += (ULONG)RtlSnprintf(r->Json + n, sizeof(r->Json) - n, "%s{\"name\":\"", n > 1 ? "," : "");
        UploadpJsonName(r, &n, name, length);
        n += (ULONG)RtlSnprintf(r->Json + n, sizeof(r->Json) - n, "\",\"dir\":%s,\"size\":%llu}",
                                folder ? "true" : "false", size);
    };
    r->Json[n++] = '[';
    if (r->Path[0] == '/' && !r->Path[1]) { // the drives that exist
        static const char drives[] = "CEFGXYZ";
        for (ULONG i = 0; drives[i]; i++) {
            char path[3] = {'/', drives[i], 0}, nt[64];
            HANDLE h;
            if (UploadpNtPath(path, nt, sizeof(nt)) && NT_SUCCESS(UploadpOpen(nt, GENERIC_READ, FILE_OPEN,
                                                                              FILE_DIRECTORY_FILE, &h))) {
                NtClose(h);
                entry(path + 1, 1, true, 0);
            }
        }
    } else {
        HANDLE h;
        NTSTATUS status = UploadpOpen(r->NtPath, GENERIC_READ, FILE_OPEN, FILE_DIRECTORY_FILE, &h);
        if (!NT_SUCCESS(status))
            return UploadpText(r, 404, "%s: not a folder (%08lx)", r->Shown, status);
        UCHAR buffer[2048];
        for (bool first = true;; first = false) {
            IO_STATUS_BLOCK iosb;
            status = NtQueryDirectoryFile(h, nullptr, nullptr, nullptr, &iosb, buffer, sizeof(buffer),
                                          FileDirectoryInformation, nullptr, first);
            if (first && status == STATUS_NO_SUCH_FILE_) // an empty folder: NT's answer to the first query
                status = STATUS_NO_MORE_FILES_;
            if (!NT_SUCCESS(status))
                break;
            // FILE_DIRECTORY_INFORMATION: next entry at 0, size at 0x28, attributes at 0x38, name length at 0x3C,
            // the name at 0x40.
            for (PUCHAR e = buffer;; e += *(PULONG)e) {
                bool folder = *(PULONG)(e + 0x38) & FILE_ATTRIBUTE_DIRECTORY;
                entry((const char *)(e + 0x40), *(PULONG)(e + 0x3C), folder, *(ULONGLONG *)(e + 0x28));
                if (!*(PULONG)e)
                    break;
            }
        }
        NtClose(h);
        if (status != STATUS_NO_MORE_FILES_)
            return UploadpText(r, 500, "%s: listing failed (%08lx)", r->Shown, status);
    }
    r->Json[n++] = ']';
    UploadpReply(r, 200, "application/json", r->Json, n);
}

// --- The disk

// A page path as messages show it: a FATX disk has partitions, so "/E/Apps" reads "E:\Apps" ("/E" reads "E:\"); an
// exFAT disk has one volume whose top-level folders stand for the drives, so its paths stay as they are.
static void UploadpShowPath(const char *Path, char *Out, ULONG Size) {
    ULONG i = 0, o = 0;
    if (!IdexDiskEXFATLayout && Path[0] == '/' && Path[1] && Path[1] != '/' && (!Path[2] || Path[2] == '/') &&
        Size > 3) {
        Out[o++] = Path[1];
        Out[o++] = ':';
        Out[o++] = '\\';
        i = Path[2] ? 3 : 2;
        for (; Path[i] && o + 1 < Size; i++)
            Out[o++] = Path[i] == '/' ? '\\' : Path[i];
    } else {
        for (; Path[i] && o + 1 < Size; i++)
            Out[o++] = Path[i];
    }
    Out[o] = 0;
}

// The disk's file system and size, for the page's header and its folder buttons (spec/oxos_internal/31 §4):
// {"fs":"exFAT","bytes":2000398934016}. "fs" is "FATX", "exFAT", or "none" (a PC disk without an exFAT volume).
static void UploadpDisk(UPLOADP_REQUEST *r) {
    const char *fs = IdexDiskEXFATLayout ? "exFAT" : IdexDiskForeignLayout ? "none" : "FATX";
    ULONG n = (ULONG)RtlSnprintf(r->Json, sizeof(r->Json), "{\"fs\":\"%s\",\"bytes\":%llu}", fs,
                                 IdexDiskLengthInSectors * 512);
    UploadpReply(r, 200, "application/json", r->Json, n);
}

// What a top-level folder is, for a refusal: a drive on a FATX disk, a folder of the volume on an exFAT one.
static const char *UploadpTopName() {
    return IdexDiskEXFATLayout ? "a top-level folder" : "a drive";
}

// --- Creating a folder

// Creates the folder at the request's path, and the folders on the way to it; one that exists already: 409.
static void UploadpCreateFolder(UPLOADP_REQUEST *r) {
    NTSTATUS status = UploadpMakeFolders(r->NtPath);
    HANDLE h = nullptr;
    if (NT_SUCCESS(status))
        status = UploadpOpen(r->NtPath, GENERIC_READ, FILE_CREATE, FILE_DIRECTORY_FILE, &h);
    if (!NT_SUCCESS(status)) {
        UploadpStatusPath("New folder failed:", r->Shown);
        if (status == STATUS_OBJECT_NAME_COLLISION_)
            return UploadpText(r, 409, "%s: already exists", r->Shown);
        return UploadpText(r, 500, "%s: cannot create it (%08lx)", r->Shown, status);
    }
    NtClose(h);
    UploadpStatusPath("New folder", r->Shown);
    UploadpText(r, 201, "Created %s", r->Shown);
}

// --- Renaming

// Renames the file or folder at the request's path to Name, in the same folder. A drive is refused (403), a name in use
// too (409).
static void UploadpRename(UPLOADP_REQUEST *r, const char *Target) {
    if (!r->Path[2] || !r->Path[3]) // "/E" or "/E/"
        return UploadpText(r, 403, "Cannot rename %s (%s)", r->Shown, UploadpTopName());
    char name[PATH_SIZE];
    bool valid = UploadpQueryValue(Target, "name=", name, sizeof(name));
    for (ULONG i = 0; valid && name[i]; i++)
        valid = UploadpNameCharacter(name[i]);
    if (!valid || (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))))
        return UploadpText(r, 400, "Bad name");
    HANDLE h;
    NTSTATUS status = UploadpOpen(r->NtPath, DELETE, FILE_OPEN, 0, &h);
    if (NT_SUCCESS(status)) {
        FILE_RENAME_INFORMATION info = {FALSE, nullptr, {}};
        RtlInitAnsiString(&info.FileName, name);
        IO_STATUS_BLOCK iosb;
        status = NtSetInformationFile(h, &iosb, &info, sizeof(info), FileRenameInformation);
        NtClose(h);
    }
    if (!NT_SUCCESS(status)) {
        UploadpStatusPath("Rename failed:", r->Shown);
        if (status == STATUS_OBJECT_NAME_COLLISION_)
            return UploadpText(r, 409, "%s: already exists", name);
        return UploadpText(r, 500, "%s: rename failed (%08lx)", r->Shown, status);
    }
    UploadpStatusPath("Renamed", r->Shown);
    UploadpText(r, 200, "Renamed %s to %s", r->Shown, name);
}

// --- Deleting

// Deletes the file or empty folder at the request's path. A drive's own folder is refused, and a folder with
// anything in it is left as it is (409): the page deletes nothing recursively.
static void UploadpDelete(UPLOADP_REQUEST *r) {
    if (!r->Path[2] || !r->Path[3]) // "/E" or "/E/"
        return UploadpText(r, 403, "Cannot delete %s (%s)", r->Shown, UploadpTopName());
    ANSI_STRING name;
    RtlInitAnsiString(&name, r->NtPath);
    OBJECT_ATTRIBUTES a = {nullptr, &name, OBJ_CASE_INSENSITIVE};
    NTSTATUS status = NtDeleteFile(&a);
    if (!NT_SUCCESS(status)) {
        UploadpStatusPath("Delete failed:", r->Shown);
        if (status == STATUS_DIRECTORY_NOT_EMPTY_)
            return UploadpText(r, 409, "%s: the folder is not empty", r->Shown);
        return UploadpText(r, 500, "%s: delete failed (%08lx)", r->Shown, status);
    }
    UploadpStatusPath("Deleted", r->Shown);
    UploadpText(r, 200, "Deleted %s", r->Shown);
}

// --- Uploads

// The upload's buffered bytes go to its file (after an error they are dropped).
static void UploadpWrite(UPLOADP_REQUEST *r) {
    if (r->ChunkUsed && NT_SUCCESS(r->Error)) {
        IO_STATUS_BLOCK iosb;
        r->Error = NtWriteFile(r->File, nullptr, nullptr, nullptr, &iosb, r->Chunk, r->ChunkUsed, nullptr);
    }
    r->Received += r->ChunkUsed;
    r->ChunkUsed = 0;
}

static void UploadpEndUpload(UPLOADP_REQUEST *r) {
    UploadpWrite(r);
    if (r->File) {
        NtClose(r->File);
        r->File = nullptr;
    }
    if (!NT_SUCCESS(r->Error)) {
        UploadpStatusPath("Upload failed:", r->Shown);
        return UploadpText(r, 500, "%s: write failed (%08lx)", r->Shown, r->Error);
    }
    UploadpStatusPath("Saved", r->Shown);
    UploadpText(r, 201, "%s: %llu bytes", r->Shown, r->Received);
}

static void UploadpStartUpload(UPLOADP_REQUEST *r, ULONGLONG Length, bool Continue) {
    r->Error = UploadpMakeFolders(r->NtPath);
    if (NT_SUCCESS(r->Error))
        r->Error = UploadpOpen(r->NtPath, GENERIC_WRITE, FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE, &r->File, Length);
    if (!NT_SUCCESS(r->Error))
        r->File = nullptr;
    r->Remaining = Length;
    r->Received = r->ChunkUsed = 0;
    r->Phase = UploadpReadBody;
    UploadpStatusPath("Receiving", r->Shown);
    if (Continue) { // curl and others wait for this before they send a large body
        static const char go[] = "HTTP/1.1 100 Continue\r\n\r\n";
        NetTcpSend(r->Connection, go, sizeof(go) - 1);
    }
}

// --- Requests

// Header Name's value in the head (case-insensitive name), or null.
static const char *UploadpHeader(const UPLOADP_REQUEST *r, const char *Name) {
    for (ULONG i = 0; i + 2 < r->HeadLength; i++) {
        if (r->Head[i] != '\n')
            continue;
        ULONG k = 0;
        while (Name[k] && (r->Head[i + 1 + k] | 0x20) == (Name[k] | 0x20))
            k++;
        if (!Name[k] && r->Head[i + 1 + k] == ':') {
            const char *v = r->Head + i + 2 + k;
            while (*v == ' ')
                v++;
            return v;
        }
    }
    return nullptr;
}

// The head has arrived (Body: the bytes after it in Head, BodyLength of them): what the request asks for.
static void UploadpDispatch(UPLOADP_REQUEST *r, const char *Body, ULONG BodyLength) {
    const char *target = r->Head;
    while (*target && *target != ' ')
        target++;
    while (*target == ' ')
        target++;
    bool get = UploadpIs(r->Head, "GET "), put = UploadpIs(r->Head, "PUT "), post = UploadpIs(r->Head, "POST "),
         remove = UploadpIs(r->Head, "DELETE ");
    if (get && (UploadpIs(target, "/ ") || UploadpIs(target, "/index.html"))) {
        ULONG length;
        const char *page = UploadPage(&length);
        return UploadpReply(r, 200, "text/html; charset=utf-8", page, length);
    }
    if (get && UploadpIs(target, "/api/disk "))
        return UploadpDisk(r);
    bool list = UploadpIs(target, "/api/list?"), file = UploadpIs(target, "/api/file?"),
         folder = UploadpIs(target, "/api/folder?"), rename = UploadpIs(target, "/api/rename?"),
         launch = UploadpIs(target, "/api/launch?");
    if (!list && !file && !folder && !rename && !launch)
        return UploadpText(r, 404, "No such page");
    if ((list && !get) || (file && !put && !remove) || (folder && !put) || ((rename || launch) && !post))
        return UploadpText(r, 405, "Wrong method");
    if (!UploadpQueryValue(target, "path=", r->Path, sizeof(r->Path)))
        return UploadpText(r, 400, "Bad path");
    UploadpShowPath(r->Path, r->Shown, sizeof(r->Shown));
    bool drives = list && r->Path[0] == '/' && !r->Path[1]; // the list of drives
    if (!drives && !UploadpNtPath(r->Path, r->NtPath, sizeof(r->NtPath)))
        return UploadpText(r, 400, "Bad path");
    if (list)
        return UploadpList(r);
    if (folder)
        return UploadpCreateFolder(r);
    if (rename)
        return UploadpRename(r, target);
    if (remove)
        return UploadpDelete(r);
    if (launch) {
        UploadpStatusPath("Launching", r->Shown);
        r->Launch = true;
        return UploadpText(r, 200, "Launching %s", r->Shown);
    }
    const char *length = UploadpHeader(r, "Content-Length");
    if (!length)
        return UploadpText(r, 411, "Content-Length needed");
    ULONGLONG bytes = 0;
    for (; *length >= '0' && *length <= '9'; length++)
        bytes = bytes * 10 + (ULONGLONG)(*length - '0');
    const char *expect = UploadpHeader(r, "Expect");
    UploadpStartUpload(r, bytes, expect && UploadpIs(expect, "100-continue"));
    // Body bytes that came with the head.
    ULONG n = BodyLength < r->Remaining ? BodyLength : (ULONG)r->Remaining;
    RtlCopyMemory(r->Chunk, Body, n);
    r->ChunkUsed = n;
    r->Remaining -= n;
}

// A launch whose reply has gone out (or whose browser has gone): the server returns its path.
static void UploadpLaunchNow(UPLOADP_REQUEST *r) {
    if (UploadpServer->LaunchReady)
        return;
    RtlCopyMemory(UploadpServer->Launch, r->NtPath, PATH_SIZE);
    UploadpServer->LaunchReady = true;
    r->Launch = false;
}

// One step of a request: whatever its connection has for it, and whatever its reply can queue.
static void UploadpStep(UPLOADP_REQUEST *r) {
    if (!NetTcpOpen(r->Connection)) { // closed or reset
        if (r->Launch && r->Phase == UploadpClosing)
            UploadpLaunchNow(r);
        if (r->File)
            NtClose(r->File);
        r->File = nullptr;
        r->Phase = UploadpIdle;
        return;
    }
    if (r->Phase == UploadpReadHead) {
        r->HeadLength += NetTcpReceive(r->Connection, r->Head + r->HeadLength, HEAD_SIZE - 1 - r->HeadLength);
        r->Head[r->HeadLength] = 0;
        for (ULONG i = 3; i < r->HeadLength; i++) {
            if (r->Head[i - 3] == '\r' && r->Head[i - 2] == '\n' && r->Head[i - 1] == '\r' && r->Head[i] == '\n') {
                return UploadpDispatch(r, r->Head + i + 1, r->HeadLength - i - 1);
            }
        }
        if (r->HeadLength == HEAD_SIZE - 1)
            return UploadpText(r, 431, "Request head too large");
        if (NetTcpPeerClosed(r->Connection))
            NetTcpClose(r->Connection);
        return;
    }
    if (r->Phase == UploadpReadBody) {
        while (r->Remaining) {
            ULONG room = CHUNK_SIZE - r->ChunkUsed;
            ULONG n = NetTcpReceive(r->Connection, r->Chunk + r->ChunkUsed,
                                    room < r->Remaining ? room : (ULONG)r->Remaining);
            if (!n)
                break;
            r->ChunkUsed += n;
            r->Remaining -= n;
            if (r->ChunkUsed == CHUNK_SIZE)
                UploadpWrite(r);
        }
        if (!r->Remaining)
            UploadpEndUpload(r);
        else if (NetTcpPeerClosed(r->Connection)) // the browser gave up
            NetTcpClose(r->Connection);
        return;
    }
    if (r->Phase == UploadpQueueReply) {
        r->HeaderSent += NetTcpSend(r->Connection, r->Header + r->HeaderSent, r->HeaderLength - r->HeaderSent);
        if (r->HeaderSent == r->HeaderLength)
            r->BodySent += NetTcpSend(r->Connection, r->Body + r->BodySent, r->BodyLength - r->BodySent);
        if (r->HeaderSent == r->HeaderLength && r->BodySent == r->BodyLength) {
            NetTcpClose(r->Connection);
            r->Phase = UploadpClosing;
        }
        return;
    }
    if (r->Phase == UploadpClosing && r->Launch && NetTcpAllSent(r->Connection))
        UploadpLaunchNow(r);
}

// --- Entry points

bool UploadServe(NTSTATUS LastLaunch, char *LaunchPath, ULONG Size) {
    if (!UploadpServer) {
        UploadpServer = (UPLOADP_SERVER *)MmAllocateSystemMemory(sizeof(UPLOADP_SERVER), PAGE_READWRITE);
        if (!UploadpServer)
            return false;
        RtlZeroMemory(UploadpServer, sizeof(UPLOADP_SERVER));
        if (!NetStart()) {
            MmFreeSystemMemory(UploadpServer, sizeof(UPLOADP_SERVER));
            UploadpServer = nullptr;
            return false;
        }
        HalpStopSplash(); // the screen is the server's; the boot-only pages it runs from stay
    }
    if (LastLaunch)
        UploadpStatus("Launch failed (%08lx)", LastLaunch);
    UploadpServer->LaunchReady = false;
    UploadServing = 1;
    XK_SERIAL("upload: serving the File Manager\n");
    for (;;) {
        bool busy = NetPoll();
        for (LONG id; (id = NetTcpAccept()) >= 0;) {
            UPLOADP_REQUEST *free = nullptr;
            for (ULONG i = 0; i < NET_TCP_CONNECTIONS && !free; i++) {
                if (UploadpServer->Requests[i].Phase == UploadpIdle)
                    free = &UploadpServer->Requests[i];
            }
            if (!free) { // more connections than requests cannot happen: the stack has as many of each
                NetTcpClose(id);
                continue;
            }
            free->Phase = UploadpReadHead;
            free->Connection = id;
            free->HeadLength = 0;
            free->Launch = false;
        }
        for (ULONG i = 0; i < NET_TCP_CONNECTIONS; i++) {
            if (UploadpServer->Requests[i].Phase != UploadpIdle)
                UploadpStep(&UploadpServer->Requests[i]);
        }
        UploadpUpdateScreen();
        if (UploadpServer->LaunchReady) {
            UploadServing = 0;
            RtlSnprintf(LaunchPath, Size, "%s", UploadpServer->Launch);
            XK_SERIAL("upload: launching %s\n", LaunchPath);
            return true;
        }
        if (busy) // frames are arriving: straight on, a pause would hold the sender back
            continue;
        LARGE_INTEGER pause;
        pause.QuadPart = -10000; // 1 ms
        KeDelayExecutionThread(KernelMode, FALSE, &pause);
    }
}

void UploadStop() {
    if (!UploadpServer)
        return;
    NetStop();
    XkCloseFileManagerScreen();
    for (ULONG i = 0; i < NET_TCP_CONNECTIONS; i++) {
        if (UploadpServer->Requests[i].File)
            NtClose(UploadpServer->Requests[i].File);
    }
    MmFreeSystemMemory(UploadpServer, sizeof(UPLOADP_SERVER));
    UploadpServer = nullptr;
}
