// Log files in C:\OXOS\log (spec/oxos_internal/35): lines of text appended to a named file, for what a console without a serial port should still keep (the
// serial port and its in-memory copy, in SERIAL=1 builds only, are the debug log: serial_log.cpp). A log over its size limit is rotated: NAME_1.log is the
// newest old part. Resident; file I/O, so PASSIVE_LEVEL only.
#include <diagnostics/diagnostics.h>
#include <core/core.h>
#include <io/io.h>
#include <objects/objects.h>
#include <runtime/runtime.h>

namespace xk::diagnostics {

namespace {

constexpr char OXOS_FOLDER[] = "\\Device\\Harddisk0\\Partition2\\OXOS";
constexpr char LOG_FOLDER[] = "\\Device\\Harddisk0\\Partition2\\OXOS\\log";
constexpr char CACHE_FOLDER[] = "\\Device\\Harddisk0\\Partition2\\OXOS\\cache"; // the kernel's own caches (extension loader, cache partitions)
constexpr char EXTENSIONS_FOLDER[] = "\\Device\\Harddisk0\\Partition2\\OXOS\\extensions"; // ready for the first extension (spec/oxos_internal/34 §3)
// A log is rotated before a line would take it past this size: 10 MB, and 4 KB in self-test builds, where the FATX suite rotates one
// (tests/kernel_self_tests/fatx.cpp).
constexpr ULONG LOG_ROTATE_BYTES = XK_BOOT_SELFTEST ? 0x1000 : 10 * 1024 * 1024;

// The path of part Part of log LogName: NAME.log for part 0, NAME_N.log for part N. False when it does not fit.
bool LogPath(char (&Path)[96], const char *LogName, ULONG Part) {
    int n = Part ? snprintf(Path, sizeof(Path), "%s\\%s_%lu.log", LOG_FOLDER, LogName, Part) : snprintf(Path, sizeof(Path), "%s\\%s.log", LOG_FOLDER,
                 LogName);
    return n > 0 && n < (int)sizeof(Path);
}

// Opens Path (Access, CreateDisposition, CreateOptions as for NtCreateFile); the handle, or null.
HANDLE Open(const char *Path, ACCESS_MASK Access, ULONG Disposition, ULONG Options) {
    HANDLE handle;
    return NT_SUCCESS(io::CreateFile(Path, Access, 0, Disposition, Options, &handle)) ? handle : nullptr;
}

// Renames From to To, replacing To.
void Rename(const char *From, const char *To) {
    HANDLE file = Open(From, DELETE, FILE_OPEN, FILE_NON_DIRECTORY_FILE);
    if (!file)
        return;
    io::RenameFile(file, To, true);
    NtClose(file);
}

// NAME_N.log becomes NAME_N+1.log for every old part, newest last, then NAME.log becomes NAME_1.log.
void Rotate(const char *LogName) {
    char from[96], to[96];
    ULONG parts = 0;
    while (LogPath(from, LogName, parts + 1)) {
        HANDLE part = Open(from, FILE_READ_ATTRIBUTES, FILE_OPEN, FILE_NON_DIRECTORY_FILE);
        if (!part)
            break;
        NtClose(part);
        parts++;
    }
    for (ULONG n = parts + 1; n >= 1; n--) {
        if (LogPath(from, LogName, n - 1) && LogPath(to, LogName, n))
            Rename(from, to);
    }
}

} // namespace

void CreateOXOSFolders() {
    const char *const folders[] = {OXOS_FOLDER, LOG_FOLDER, EXTENSIONS_FOLDER, CACHE_FOLDER};
    for (const char *name : folders) {
        HANDLE folder = Open(name, FILE_READ_DATA, FILE_OPEN_IF, FILE_DIRECTORY_FILE);
        if (folder)
            NtClose(folder);
    }
}

void AppendLogLine(const char *LogName, const char *Format, ...) {
    if (KeGetCurrentIrql() != PASSIVE_LEVEL)
        return;
    char line[256];
    __builtin_va_list args;
    __builtin_va_start(args, Format);
    int length = vsnprintf(line, sizeof(line) - 2, Format, args);
    __builtin_va_end(args);
    if (length < 0 || length > (int)sizeof(line) - 3)
        length = sizeof(line) - 3;
    line[length++] = '\r';
    line[length++] = '\n';

    char path[96];
    if (!LogPath(path, LogName, 0))
        return;
    for (int pass = 0; pass < 2; pass++) {
        HANDLE file = Open(path, GENERIC_WRITE, FILE_OPEN_IF, FILE_NON_DIRECTORY_FILE);
        if (!file) {
            // The folders are made at every cold boot (CreateOXOSFolders); a log written before that, or after they were deleted, makes them here.
            CreateOXOSFolders();
            file = Open(path, GENERIC_WRITE, FILE_OPEN_IF, FILE_NON_DIRECTORY_FILE);
        }
        if (!file)
            return; // no C: or a disk that refuses: nothing is kept
        IO_STATUS_BLOCK iosb;
        ULONG info[6] = {}; // FILE_STANDARD_INFORMATION: EndOfFile at +8
        NTSTATUS status = NtQueryInformationFile(file, &iosb, info, sizeof(info), FileStandardInformation);
        bool full = NT_SUCCESS(status) && pass == 0 && info[2] && (info[3] || info[2] + length > LOG_ROTATE_BYTES);
        if (NT_SUCCESS(status) && !full) {
            LARGE_INTEGER end;
            end.QuadPart = info[2] | (LONGLONG)info[3] << 32;
            NtWriteFile(file, nullptr, nullptr, nullptr, &iosb, line, length, &end);
        }
        NtClose(file);
        if (!full)
            return;
        Rotate(LogName);
    }
}

} // namespace xk::diagnostics
