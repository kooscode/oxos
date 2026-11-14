// The upload server (spec/oxos_internal/31_upload_server.md §3): HTTP/1.1 on the kernel's network stack, one request
// per connection: the page, folder listings, uploads written to the disk as they arrive, creating a folder, renaming
// and deleting a file or folder (a folder only when empty), and launching an XBE.
// Boot-only.
//
// Each connection's request moves through phases: its head (request line and headers), then an upload's body, then
// the reply, queued to the connection as its send buffer has room. Everything the server keeps is in one block,
// allocated when it starts and freed by Stop.
#include <webserver/webserver.h>
#include <user_interface/user_interface.h>
#include <network/network.h>
#include <boot_splash/boot_splash.h>
#include <core/core.h>
#include <io/io.h>
#include <memory/memory.h>
#include <objects/objects.h>
#include <runtime/runtime.h>
#include <hardware/chipset/serial.h>
#include <storage/storage.h>

namespace xk::webserver {

#pragma code_seg(".xkb")
#pragma const_seg(".xka$y")

constexpr ULONG HEAD_SIZE = 4096, CHUNK_SIZE = 0x20000, JSON_SIZE = 0x8000, PATH_SIZE = 260, STATUS_SIZE = 64;
constexpr NTSTATUS STATUS_NO_MORE_FILES_ = (NTSTATUS)0x80000006, STATUS_NO_SUCH_FILE_ = (NTSTATUS)0xC000000F,
                   STATUS_DIRECTORY_NOT_EMPTY_ = (NTSTATUS)0xC0000101,
                   STATUS_OBJECT_NAME_COLLISION_ = (NTSTATUS)0xC0000035;

enum UPLOADP_PHASE : ULONG {
    Idle,       // no request
    ReadHead,   // reading the request line and headers
    ReadBody,   // writing an upload's body to its file (or reading it past an error)
    QueueReply, // queuing the reply
    Closing     // the reply is queued and the connection closing
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

static UPLOADP_SERVER *Server; // null until the server starts
volatile ULONG Serving;        // 1 while the server runs (read by the boot tests)

// --- The screen's status line

static void ShowStatus(const char *Format, ...) {
    va_list args;
    __builtin_va_start(args, Format);
    RtlVsnprintf(Server->Status, STATUS_SIZE, Format, args);
    __builtin_va_end(args);
}

// "Prefix path", keeping the path's end when the line would not fit the screen.
static void StatusPath(const char *Prefix, const char *Path) {
    ULONG room = 45, used = 0, length = 0;
    while (Prefix[used])
        used++;
    while (Path[length])
        length++;
    if (used + 1 + length <= room)
        return ShowStatus("%s %s", Prefix, Path);
    ULONG keep = room > used + 4 ? room - used - 4 : 0;
    ShowStatus("%s ...%s", Prefix, Path + length - (keep < length ? keep : length));
}

static void UpdateScreen() {
    char address[20];
    network::IpAddressText(address, sizeof(address));
    const char *network = network::NetworkState() == NetNoLink   ? "Connect a network cable."
                          : network::NetworkState() == NetAsking ? "Asking the network for an address..."
                                                    : nullptr;
    const char *status = Server->Status[0] ? Server->Status : network;
    char shown[sizeof(Server->Shown)];
    RtlSnprintf(shown, sizeof(shown), "%s|%s", address, status ? status : "Waiting for a browser.");
    for (ULONG i = 0; i < sizeof(shown); i++) {
        if (shown[i] != Server->Shown[i]) {
            RtlCopyMemory(Server->Shown, shown, sizeof(shown));
            user_interface::ShowFileManagerScreen(address, status ? status : "Waiting for a browser.");
            return;
        }
        if (!shown[i])
            return;
    }
}

// --- Paths

// A hexadecimal digit's value, or -1.
static int HexDigit(char c) {
    if (c >= '0' && c <= '9')
        return c - '0';
    if ((c | 0x20) >= 'a' && (c | 0x20) <= 'f')
        return (c | 0x20) - 'a' + 10;
    return -1;
}

// Text starts with Word.
static bool IsWord(const char *Text, const char *Word) {
    ULONG i = 0;
    while (Word[i] && Text[i] == Word[i])
        i++;
    return !Word[i];
}

// The decoded value of query parameter Key ("path=", "name=") in Target ("/api/list?path=%2FE%2FApps"), or false.
static bool QueryValue(const char *Target, const char *Key, char *Out, ULONG Size) {
    const char *p = Target;
    while (*p && *p != ' ' && !((*p == '?' || *p == '&') && IsWord(p + 1, Key)))
        p++;
    if (*p != '?' && *p != '&')
        return false;
    for (p++; *Key; Key++)
        p++;
    ULONG n = 0;
    for (; *p && *p != '&' && *p != ' '; p++) {
        char c = *p;
        if (c == '%' && HexDigit(p[1]) >= 0 && HexDigit(p[2]) >= 0) {
            c = (char)(HexDigit(p[1]) << 4 | HexDigit(p[2]));
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
static bool NameCharacter(char c) {
    return (UCHAR)c >= 0x20 && c != '"' && c != '*' && c != '/' && c != ':' && c != '<' && c != '>' && c != '?' &&
           c != '\\' && c != '|';
}

// The NT path of a page path ("/E/Apps/x.xbe" -> "\Device\Harddisk0\Partition1\Apps\x.xbe"); false for an unknown
// drive, an empty part, "." or "..", or a character FATX does not allow. "/" alone is the list of drives.
static bool NtPathOf(const char *Path, char *Out, ULONG Size) {
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
            if (!NameCharacter(p[i]))
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
static NTSTATUS OpenFile(const char *NtPath, ACCESS_MASK Access, ULONG Disposition, ULONG Options, HANDLE *h,
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
static NTSTATUS MakeFolders(char *NtPath) {
    ULONG skip = 0, separators = 0;
    for (; NtPath[skip] && separators < 4; skip++) // past "\Device\Harddisk0\PartitionN\"
        separators += NtPath[skip] == '\\';
    for (char *p = NtPath + skip; *p; p++) {
        if (*p != '\\')
            continue;
        *p = 0;
        HANDLE h;
        NTSTATUS status = OpenFile(NtPath, GENERIC_READ, FILE_OPEN_IF, FILE_DIRECTORY_FILE, &h);
        *p = '\\';
        if (!NT_SUCCESS(status))
            return status;
        NtClose(h);
    }
    return STATUS_SUCCESS;
}

// --- Replies

static void Reply(UPLOADP_REQUEST *r, ULONG Code, const char *Type, const char *Body, ULONG Length) {
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
    r->Phase = QueueReply;
}

// A plain-text reply: the message is kept in Json.
static void ReplyText(UPLOADP_REQUEST *r, ULONG Code, const char *Format, ...) {
    va_list args;
    __builtin_va_start(args, Format);
    ULONG n = (ULONG)RtlVsnprintf(r->Json, sizeof(r->Json), Format, args);
    __builtin_va_end(args);
    Reply(r, Code, "text/plain; charset=utf-8", r->Json, n);
}

// --- Listing a folder

// Appends "name" (JSON-escaped) to Json at *n.
static void JsonName(UPLOADP_REQUEST *r, ULONG *n, const char *Name, ULONG Length) {
    for (ULONG i = 0; i < Length && *n + 8 < sizeof(r->Json); i++) {
        UCHAR c = (UCHAR)Name[i];
        if (c == '"' || c == '\\')
            r->Json[(*n)++] = '\\';
        if (c < 0x20)
            c = '?';
        r->Json[(*n)++] = (char)c;
    }
}

static void HandleList(UPLOADP_REQUEST *r) {
    ULONG n = 0;
    auto entry = [&](const char *name, ULONG length, bool folder, ULONGLONG size) {
        if (n + length + 64 >= sizeof(r->Json)) // a folder too large for one reply: the rest is left out
            return;
        n += (ULONG)RtlSnprintf(r->Json + n, sizeof(r->Json) - n, "%s{\"name\":\"", n > 1 ? "," : "");
        JsonName(r, &n, name, length);
        n += (ULONG)RtlSnprintf(r->Json + n, sizeof(r->Json) - n, "\",\"dir\":%s,\"size\":%llu}",
                                folder ? "true" : "false", size);
    };
    r->Json[n++] = '[';
    if (r->Path[0] == '/' && !r->Path[1]) { // the drives that exist
        static const char drives[] = "CEFGXYZ";
        for (ULONG i = 0; drives[i]; i++) {
            char path[3] = {'/', drives[i], 0}, nt[64];
            HANDLE h;
            if (NtPathOf(path, nt, sizeof(nt)) && NT_SUCCESS(OpenFile(nt, GENERIC_READ, FILE_OPEN,
                                                                      FILE_DIRECTORY_FILE, &h))) {
                NtClose(h);
                entry(path + 1, 1, true, 0);
            }
        }
    } else {
        HANDLE h;
        NTSTATUS status = OpenFile(r->NtPath, GENERIC_READ, FILE_OPEN, FILE_DIRECTORY_FILE, &h);
        if (!NT_SUCCESS(status))
            return ReplyText(r, 404, "%s: not a folder (%08lx)", r->Shown, status);
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
            return ReplyText(r, 500, "%s: listing failed (%08lx)", r->Shown, status);
    }
    r->Json[n++] = ']';
    Reply(r, 200, "application/json", r->Json, n);
}

// --- The disk

// A page path as messages show it: a FATX disk has partitions, so "/E/Apps" reads "E:\Apps" ("/E" reads "E:\"); an
// exFAT disk has one volume whose top-level folders stand for the drives, so its paths stay as they are.
static void ShowPath(const char *Path, char *Out, ULONG Size) {
    ULONG i = 0, o = 0;
    if (!storage::DiskHasEXFATLayout && Path[0] == '/' && Path[1] && Path[1] != '/' &&
        (!Path[2] || Path[2] == '/') && Size > 3) {
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
static void HandleDiskInfo(UPLOADP_REQUEST *r) {
    const char *fs = storage::DiskHasEXFATLayout ? "exFAT" : storage::DiskHasForeignLayout ? "none" : "FATX";
    ULONG n = (ULONG)RtlSnprintf(r->Json, sizeof(r->Json), "{\"fs\":\"%s\",\"bytes\":%llu}", fs,
                                 hardware::ide::DiskLengthInSectors * 512);
    Reply(r, 200, "application/json", r->Json, n);
}

// What a top-level folder is, for a refusal: a drive on a FATX disk, a folder of the volume on an exFAT one.
static const char *TopName() {
    return storage::DiskHasEXFATLayout ? "a top-level folder" : "a drive";
}

// --- Creating a folder

// Creates the folder at the request's path, and the folders on the way to it; one that exists already: 409.
static void HandleCreateFolder(UPLOADP_REQUEST *r) {
    NTSTATUS status = MakeFolders(r->NtPath);
    HANDLE h = nullptr;
    if (NT_SUCCESS(status))
        status = OpenFile(r->NtPath, GENERIC_READ, FILE_CREATE, FILE_DIRECTORY_FILE, &h);
    if (!NT_SUCCESS(status)) {
        StatusPath("New folder failed:", r->Shown);
        if (status == STATUS_OBJECT_NAME_COLLISION_)
            return ReplyText(r, 409, "%s: already exists", r->Shown);
        return ReplyText(r, 500, "%s: cannot create it (%08lx)", r->Shown, status);
    }
    NtClose(h);
    StatusPath("New folder", r->Shown);
    ReplyText(r, 201, "Created %s", r->Shown);
}

// --- Renaming

// Renames the file or folder at the request's path to Name, in the same folder. A drive is refused (403), a name in use
// too (409).
static void HandleRename(UPLOADP_REQUEST *r, const char *Target) {
    if (!r->Path[2] || !r->Path[3]) // "/E" or "/E/"
        return ReplyText(r, 403, "Cannot rename %s (%s)", r->Shown, TopName());
    char name[PATH_SIZE];
    bool valid = QueryValue(Target, "name=", name, sizeof(name));
    for (ULONG i = 0; valid && name[i]; i++)
        valid = NameCharacter(name[i]);
    if (!valid || (name[0] == '.' && (!name[1] || (name[1] == '.' && !name[2]))))
        return ReplyText(r, 400, "Bad name");
    HANDLE h;
    NTSTATUS status = OpenFile(r->NtPath, DELETE, FILE_OPEN, 0, &h);
    if (NT_SUCCESS(status)) {
        FILE_RENAME_INFORMATION info = {FALSE, nullptr, {}};
        RtlInitAnsiString(&info.FileName, name);
        IO_STATUS_BLOCK iosb;
        status = NtSetInformationFile(h, &iosb, &info, sizeof(info), FileRenameInformation);
        NtClose(h);
    }
    if (!NT_SUCCESS(status)) {
        StatusPath("Rename failed:", r->Shown);
        if (status == STATUS_OBJECT_NAME_COLLISION_)
            return ReplyText(r, 409, "%s: already exists", name);
        return ReplyText(r, 500, "%s: rename failed (%08lx)", r->Shown, status);
    }
    StatusPath("Renamed", r->Shown);
    ReplyText(r, 200, "Renamed %s to %s", r->Shown, name);
}

// --- Deleting

// Deletes the file or empty folder at the request's path. A drive's own folder is refused, and a folder with
// anything in it is left as it is (409): the page deletes nothing recursively.
static void HandleDelete(UPLOADP_REQUEST *r) {
    if (!r->Path[2] || !r->Path[3]) // "/E" or "/E/"
        return ReplyText(r, 403, "Cannot delete %s (%s)", r->Shown, TopName());
    ANSI_STRING name;
    RtlInitAnsiString(&name, r->NtPath);
    OBJECT_ATTRIBUTES a = {nullptr, &name, OBJ_CASE_INSENSITIVE};
    NTSTATUS status = NtDeleteFile(&a);
    if (!NT_SUCCESS(status)) {
        StatusPath("Delete failed:", r->Shown);
        if (status == STATUS_DIRECTORY_NOT_EMPTY_)
            return ReplyText(r, 409, "%s: the folder is not empty", r->Shown);
        return ReplyText(r, 500, "%s: delete failed (%08lx)", r->Shown, status);
    }
    StatusPath("Deleted", r->Shown);
    ReplyText(r, 200, "Deleted %s", r->Shown);
}

// --- Uploads

// The upload's buffered bytes go to its file (after an error they are dropped).
static void HandleWrite(UPLOADP_REQUEST *r) {
    if (r->ChunkUsed && NT_SUCCESS(r->Error)) {
        IO_STATUS_BLOCK iosb;
        r->Error = NtWriteFile(r->File, nullptr, nullptr, nullptr, &iosb, r->Chunk, r->ChunkUsed, nullptr);
    }
    r->Received += r->ChunkUsed;
    r->ChunkUsed = 0;
}

static void EndUpload(UPLOADP_REQUEST *r) {
    HandleWrite(r);
    if (r->File) {
        NtClose(r->File);
        r->File = nullptr;
    }
    if (!NT_SUCCESS(r->Error)) {
        StatusPath("Upload failed:", r->Shown);
        return ReplyText(r, 500, "%s: write failed (%08lx)", r->Shown, r->Error);
    }
    StatusPath("Saved", r->Shown);
    ReplyText(r, 201, "%s: %llu bytes", r->Shown, r->Received);
}

static void StartUpload(UPLOADP_REQUEST *r, ULONGLONG Length, bool Continue) {
    r->Error = MakeFolders(r->NtPath);
    if (NT_SUCCESS(r->Error))
        r->Error = OpenFile(r->NtPath, GENERIC_WRITE, FILE_OVERWRITE_IF, FILE_NON_DIRECTORY_FILE, &r->File, Length);
    if (!NT_SUCCESS(r->Error))
        r->File = nullptr;
    r->Remaining = Length;
    r->Received = r->ChunkUsed = 0;
    r->Phase = ReadBody;
    StatusPath("Receiving", r->Shown);
    if (Continue) { // curl and others wait for this before they send a large body
        static const char go[] = "HTTP/1.1 100 Continue\r\n\r\n";
        network::SendTcp(r->Connection, go, sizeof(go) - 1);
    }
}

// --- Requests

// Header Name's value in the head (case-insensitive name), or null.
static const char *RequestHeader(const UPLOADP_REQUEST *r, const char *Name) {
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
static void DispatchRequest(UPLOADP_REQUEST *r, const char *Body, ULONG BodyLength) {
    const char *target = r->Head;
    while (*target && *target != ' ')
        target++;
    while (*target == ' ')
        target++;
    bool get = IsWord(r->Head, "GET "), put = IsWord(r->Head, "PUT "), post = IsWord(r->Head, "POST "),
         remove = IsWord(r->Head, "DELETE ");
    if (get && (IsWord(target, "/ ") || IsWord(target, "/index.html"))) {
        ULONG length;
        const char *page = FileManagerPage(&length);
        return Reply(r, 200, "text/html; charset=utf-8", page, length);
    }
    if (get && IsWord(target, "/api/disk "))
        return HandleDiskInfo(r);
    bool list = IsWord(target, "/api/list?"), file = IsWord(target, "/api/file?"),
         folder = IsWord(target, "/api/folder?"), rename = IsWord(target, "/api/rename?"),
         launch = IsWord(target, "/api/launch?");
    if (!list && !file && !folder && !rename && !launch)
        return ReplyText(r, 404, "No such page");
    if ((list && !get) || (file && !put && !remove) || (folder && !put) || ((rename || launch) && !post))
        return ReplyText(r, 405, "Wrong method");
    if (!QueryValue(target, "path=", r->Path, sizeof(r->Path)))
        return ReplyText(r, 400, "Bad path");
    ShowPath(r->Path, r->Shown, sizeof(r->Shown));
    bool drives = list && r->Path[0] == '/' && !r->Path[1]; // the list of drives
    if (!drives && !NtPathOf(r->Path, r->NtPath, sizeof(r->NtPath)))
        return ReplyText(r, 400, "Bad path");
    if (list)
        return HandleList(r);
    if (folder)
        return HandleCreateFolder(r);
    if (rename)
        return HandleRename(r, target);
    if (remove)
        return HandleDelete(r);
    if (launch) {
        StatusPath("Launching", r->Shown);
        r->Launch = true;
        return ReplyText(r, 200, "Launching %s", r->Shown);
    }
    const char *length = RequestHeader(r, "Content-Length");
    if (!length)
        return ReplyText(r, 411, "Content-Length needed");
    ULONGLONG bytes = 0;
    for (; *length >= '0' && *length <= '9'; length++)
        bytes = bytes * 10 + (ULONGLONG)(*length - '0');
    const char *expect = RequestHeader(r, "Expect");
    StartUpload(r, bytes, expect && IsWord(expect, "100-continue"));
    // Body bytes that came with the head.
    ULONG n = BodyLength < r->Remaining ? BodyLength : (ULONG)r->Remaining;
    RtlCopyMemory(r->Chunk, Body, n);
    r->ChunkUsed = n;
    r->Remaining -= n;
}

// A launch whose reply has gone out (or whose browser has gone): the server returns its path.
static void LaunchNow(UPLOADP_REQUEST *r) {
    if (Server->LaunchReady)
        return;
    RtlCopyMemory(Server->Launch, r->NtPath, PATH_SIZE);
    Server->LaunchReady = true;
    r->Launch = false;
}

// One step of a request: whatever its connection has for it, and whatever its reply can queue.
static void StepRequest(UPLOADP_REQUEST *r) {
    if (!network::IsTcpOpen(r->Connection)) { // closed or reset
        if (r->Launch && r->Phase == Closing)
            LaunchNow(r);
        if (r->File)
            NtClose(r->File);
        r->File = nullptr;
        r->Phase = Idle;
        return;
    }
    if (r->Phase == ReadHead) {
        r->HeadLength += network::ReceiveTcp(r->Connection, r->Head + r->HeadLength, HEAD_SIZE - 1 - r->HeadLength);
        r->Head[r->HeadLength] = 0;
        for (ULONG i = 3; i < r->HeadLength; i++) {
            if (r->Head[i - 3] == '\r' && r->Head[i - 2] == '\n' && r->Head[i - 1] == '\r' && r->Head[i] == '\n') {
                return DispatchRequest(r, r->Head + i + 1, r->HeadLength - i - 1);
            }
        }
        if (r->HeadLength == HEAD_SIZE - 1)
            return ReplyText(r, 431, "Request head too large");
        if (network::TcpPeerClosed(r->Connection))
            network::CloseTcp(r->Connection);
        return;
    }
    if (r->Phase == ReadBody) {
        while (r->Remaining) {
            ULONG room = CHUNK_SIZE - r->ChunkUsed;
            ULONG n = network::ReceiveTcp(r->Connection, r->Chunk + r->ChunkUsed,
                                              room < r->Remaining ? room : (ULONG)r->Remaining);
            if (!n)
                break;
            r->ChunkUsed += n;
            r->Remaining -= n;
            if (r->ChunkUsed == CHUNK_SIZE)
                HandleWrite(r);
        }
        if (!r->Remaining)
            EndUpload(r);
        else if (network::TcpPeerClosed(r->Connection)) // the browser gave up
            network::CloseTcp(r->Connection);
        return;
    }
    if (r->Phase == QueueReply) {
        r->HeaderSent +=
            network::SendTcp(r->Connection, r->Header + r->HeaderSent, r->HeaderLength - r->HeaderSent);
        if (r->HeaderSent == r->HeaderLength)
            r->BodySent += network::SendTcp(r->Connection, r->Body + r->BodySent, r->BodyLength - r->BodySent);
        if (r->HeaderSent == r->HeaderLength && r->BodySent == r->BodyLength) {
            network::CloseTcp(r->Connection);
            r->Phase = Closing;
        }
        return;
    }
    if (r->Phase == Closing && r->Launch && network::TcpAllSent(r->Connection))
        LaunchNow(r);
}

// --- Entry points

bool Start(NTSTATUS LastLaunch, char *LaunchPath, ULONG Size) {
    if (!Server) {
        Server = (UPLOADP_SERVER *)MmAllocateSystemMemory(sizeof(UPLOADP_SERVER), PAGE_READWRITE);
        if (!Server)
            return false;
        RtlZeroMemory(Server, sizeof(UPLOADP_SERVER));
        if (!network::StartNetwork()) {
            MmFreeSystemMemory(Server, sizeof(UPLOADP_SERVER));
            Server = nullptr;
            return false;
        }
        boot_splash::StopSplash(); // the screen is the server's; the boot-only pages it runs from stay
    }
    if (LastLaunch)
        ShowStatus("Launch failed (%08lx)", LastLaunch);
    Server->LaunchReady = false;
    Serving = 1;
    XK_SERIAL("upload: serving the File Manager\n");
    for (;;) {
        bool busy = network::PollNetwork();
        for (LONG id; (id = network::AcceptTcp()) >= 0;) {
            UPLOADP_REQUEST *free = nullptr;
            for (ULONG i = 0; i < NET_TCP_CONNECTIONS && !free; i++) {
                if (Server->Requests[i].Phase == Idle)
                    free = &Server->Requests[i];
            }
            if (!free) { // more connections than requests cannot happen: the stack has as many of each
                network::CloseTcp(id);
                continue;
            }
            free->Phase = ReadHead;
            free->Connection = id;
            free->HeadLength = 0;
            free->Launch = false;
        }
        for (ULONG i = 0; i < NET_TCP_CONNECTIONS; i++) {
            if (Server->Requests[i].Phase != Idle)
                StepRequest(&Server->Requests[i]);
        }
        UpdateScreen();
        if (Server->LaunchReady) {
            Serving = 0;
            RtlSnprintf(LaunchPath, Size, "%s", Server->Launch);
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

void Stop() {
    if (!Server)
        return;
    network::StopNetwork();
    user_interface::CloseFileManagerScreen();
    for (ULONG i = 0; i < NET_TCP_CONNECTIONS; i++) {
        if (Server->Requests[i].File)
            NtClose(Server->Requests[i].File);
    }
    MmFreeSystemMemory(Server, sizeof(UPLOADP_SERVER));
    Server = nullptr;
}

} // namespace xk::webserver
