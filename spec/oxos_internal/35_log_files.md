# 35. Log files on the hard disk (OXOS's own)

Status: implemented 2026-11-14. Code: `kernel/src/diagnostics/file_log.cpp` (`diagnostics::AppendLogLine`, declared in `src/diagnostics/diagnostics.h`, and the
the kernel's debug log, `diagnostics::LogLine` in `kernel/src/diagnostics/serial_log.cpp`); the extensions' side: the SDK's `oxos_log` and the export
`OXOSWriteExtensionLog` (`kernel/src/extensions/extension_runtime.cpp`, spec/oxos_internal/34 §5.1).

A console without a serial port loses what the kernel reports: the debug serial port exists only in `SERIAL=1` builds and needs a modchip or debug kit wired to
COM1 (spec/oxos_internal/03), and a release kernel keeps no debug log at all (`diagnostics/serial_log.cpp`). Warnings the user should be able to read later go
to text files on the hard disk instead.

## 1. Where and how
- **Folder:** `C:\OXOS\log\` (`\Device\Harddisk0\Partition2\OXOS\log`: C: on a FATX disk, the `C` folder of an exFAT disk). The kernel creates it, with
  `C:\OXOS`, `C:\OXOS\extensions` and `C:\OXOS\cache` (`diagnostics::CreateOxosFolders`), at every cold boot when they are missing (user, 2026-11-14), and again
  before a line when they were deleted since.
- **Files:** one per subject, named by its user, e.g. `extensions.log`. Created when missing; lines are appended, never rewritten.
- **Rotation at 10 MB:** before a line would take `NAME.log` past 10 MB, every old part moves up one (`NAME_2.log` to `NAME_3.log`, ...), `NAME.log` becomes
  `NAME_1.log`, and the line starts a new `NAME.log`. So `NAME.log` is always the current part and `NAME_1.log` the newest old one (user, 2026-11-14). Old parts
  are kept until deleted: there is no limit on their number yet. Self-test builds rotate at 4 KB, so the FATX suite can rotate one twice.
- **Lines:** plain text, at most 253 characters, ending in CR LF, so they read as they are in Windows tools and over FTP.
- **When:** at `PASSIVE_LEVEL` only (it is file I/O); a call at a raised IRQL writes nothing. A disk without C:, or one that refuses the write, loses the line
  without an error: logging never stops the caller.
- **Cost:** resident code (logs are written while titles run); each line opens, extends and closes the file, so it is for rare events, not for tracing.

- **The serial port is for debug builds only (user, 2026-11-14):** a release kernel has no serial output (`XK_SERIAL` leaves only a boot mark, `DbgPrint` and
  `OutputDebugString` nothing); what a release console keeps is in these files.

## 2. The logs
| File | Written by | Lines |
|---|---|---|
| `extensions.log` | the extension run loop (spec/oxos_internal/34 §4.3) | `extension NAME delayed extension loop by N ms`: a `RunLoop` call that took over 500 ms, or one still running 500 ms after the title asked to end |
| `extensions.log` | extensions, through `oxos_log` with `OXOS_LOG_FILE` (spec/oxos_internal/34 §5.1; export 416, `OXOSWriteExtensionLog`, which writes only this log) | `[SECTION] text`, as on the serial port |
| `selftest.log` | the FATX self-test suite (self-test builds only) | 50 numbered lines, to check the rotation |

## 3. Open
- No limit on the number of old parts: they take disk space until deleted (FTP, the File Manager).
- A file of its own per extension, instead of the shared `extensions.log`.
- Time stamps: the console has no reliable clock at every boot; the boot count (`OXOS_CONTEXT::Generation` for extensions) could stand in.
