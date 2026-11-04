# 25 — Title compatibility: answers games expect

OXOS addition, implemented 2026-11-03.
- **Code:** `kernel/src/xdvdfs/xdvdfs.cpp` (file-system name, `XDVDFS_FCB`), `kernel/src/fatx/fatx.h` (`FATX_FCB`),
  `kernel/src/fatx/fsd.cpp` (`FatxRememberTitleFolder`, FileFsSizeInformation), `kernel/src/xe/loader.cpp` (the
  call before the title starts).
- **Tests:** boot self-tests `KiTestDvdSortKeyOk` (selftest_dvd.cpp) and `KiTestFatxTitleFolderOk`
  (selftest_fatx.cpp), on FATX and on exFAT.
- **Background:** spec/cerbios/13 §7, where Cerbios patches these games' code. Here the kernel gives the answers
  instead, so the games need no patch.

Games link their system library into their own executable, and some of its routines depend on details of the stock
kernel's file systems. Where a game's behaviour depends on what a kernel call returns, OXOS returns what the game
expects.

## 1. The physical sort key
Games call the library routine XGetFilePhysicalSortKey(handle) to learn where a file lies on its disk, for example
to order reads. A game in Cerbios's patch list (spec/cerbios/13 §7.9) uses it to find its archive on the disc. The
routine:
1. asks the file's volume for FileFsAttributeInformation (IoQueryVolumeInformation);
2. if the name is **"FATX"** (name length 4), returns the dword at **FsContext + 0x1C**, the file's first cluster. It
   returns 0 if bit 0 of the first byte at FsContext is set;
3. if the name is **"GDFX"** (length 4), returns the dword at **FsContext + 0**, the file's start sector;
4. otherwise, fails with STATUS_INVALID_PARAMETER.

OXOS therefore:
- **Disc file system:** names itself "GDFX" (FileFsAttributeInformation: attributes 0x80000 read-only, maximum name
  length 255, name length 4) and keeps `StartSector` as the first field of its per-file record.
- **FATX and exFAT:** name themselves "FATX" (unchanged) and keep `FirstCluster` at offset 0x1C of the per-file
  record. The record starts with a list link, a pointer, so bit 0 of its first byte is always clear.
- Both offsets are compile-time checks (`XK_OFFSET`).

Before this, the disc file system named itself "XDVDFS" and the FATX record had another field at 0x1C, so the routine
returned garbage on OXOS.

## 2. Sector size of D: on the hard disk
Some games ask the sector size of `D:\` and expect a DVD's 2048 bytes:
- **A disc or a disc image:** the disc file system reports 2048 already.
- **A title started from a hard-disk folder** (D: is a link to that folder):
  - Just before the title starts, the kernel opens `\??\D:\`. If that is a FATX or exFAT folder, it remembers the
    folder and keeps the handle open while the title runs.
  - FileFsSizeInformation and FileFsFullSizeInformation, asked through that folder or anything below it, report
    BytesPerSector 2048 and SectorsPerAllocationUnit scaled to match, so the cluster size and the cluster counts are
    unchanged. This needs a cluster of 2048 bytes or more, which every FATX and exFAT volume has; otherwise the
    volume's own figures are kept.
  - Every other path on the volume keeps its real sector size.
  - Each title start sets this again: a disc title or a dashboard on a disc clears it.
- **Why it is safe:** a title that aligns unbuffered reads to the reported size aligns them to 2048, which is also a
  multiple of 512.

## 3. Title patches from a file
Some problems are in a game's own code, and no kernel answer can fix them (spec/cerbios/13 §7: a race in an intro, a
CPUID wrapper, a display mode). For those the kernel writes patches into the loaded game, taken from a file, so any
title can be fixed without a new kernel.
- **Code:** `kernel/src/xe/patchfile.cpp` (the parser, `include/xk/patches.h`); `kernel/src/xe/loader.cpp`
  (`XepLoadPatches`, `XepApplyPatches`).
- **Tests:** host unit tests `tests/unit/test_patches.cpp` (the format, and the shipped file parses without an
  error); boot self-test "title patches" (boot_test.py puts a patch file for the test title on the test disk: a
  matching section changes a variable, a section for another certificate version leaves another one alone, a patch
  whose current bytes are not there leaves a third one alone, and a patch below the image is skipped).
- **Shipped file:** `kernel/title-patches/title-patches.ini`, with the Cerbios fixes (spec/cerbios/13 §7), including
  the two sector-size fixes (which the kernel also covers, §2) and the archive-position fix of spec/cerbios/13 §7.9
  (§1). Its current bytes: at 0x1C930D
  `8B F0 56 E8 00 F0 03 00` (mov esi, eax; push esi; call the sort-key routine at 0x208315, from the code around it);
  at 0x20831B the library routine's `56 8D 45 08 50 FF 35 30 C7 34 00 83 CE FF` (the same routine of another title
  of that library version, with this game's import slot for IoFileObjectType, 0x34C730). The other three are not known
  and are blind patches under a `match =` line.

### 3.1 The file
`C:\OXOS\title-patches.ini`, read through `\Device\Harddisk0\Partition2`. On a FATX disk that is C:; on an exFAT disk it
is `/C/OXOS/title-patches.ini`. Text, at most 1 MB, lines ending in LF or CR LF:

```ini
; a comment (also after a section header or a patch)
[4D4A000C]            ; every version of title 4D4A000C
name = what it fixes  ; shown in the serial log
patch = 506D7: E8 D4 64 00 > EB 08 64 00

[56550039/3]          ; only the release whose certificate version is 3 (decimal, or 0x hex)
patch = 209A8E: 558BEC83EC34EB43 > B8 00 08 00 00 C2 04 00

[434D0050]
match = 1C930D: 8B F0 56 E8     ; when these bytes are there, the blind patches below are written
patch = ECD8B: > 90 90 90 90 90 90 90 90
```

- **Sections:** `[TITLEID]` is the certificate's title ID (header +0x118 → +8), in hex. `[TITLEID/VERSION]` also
  needs the certificate version (+0xAC). Every matching section applies, in file order.
- **Patches:** `patch = ADDRESS: CURRENT_BYTES > NEW_BYTES`. The address is a virtual address of the loaded title, in
  hex (`0x` allowed). CURRENT_BYTES and NEW_BYTES are hex byte pairs, with or without spaces and tabs, 1 to 255 on
  each side: the kernel compares the CURRENT_BYTES at the address and writes the NEW_BYTES there. NEW_BYTES may be
  shorter than CURRENT_BYTES (the bytes after them are left alone), so such a line still works; the shipped file
  always gives both sides the same length, so each line shows the code before and after in full (the unit test
  checks it); NEW_BYTES is longer only where the rest of the current bytes is not known.
- **Why the current bytes:** a title ID and a version do not identify the game's code. Launchers that start a game
  from a disc image carry the game's certificate (the OXOS ISO launcher does, so dashboards show the game), and any
  header field can be copied. The code itself cannot: a patch is written only into the code it was made for. Four
  bytes or more is the rule for the shipped file (a one-byte check matches by chance too often).
- **Match lines (blind patches):** `match = ADDRESS: CURRENT_BYTES`, for a section where some patches cannot give
  their current bytes (spec/cerbios/13 §7.9: Cerbios checks one place, then writes five; three of the current bytes
  are not known). When the match line's CURRENT_BYTES are at its address, each blind patch below it in the section,
  `patch = ADDRESS: > NEW_BYTES`, is written. A blind patch without a `match =` line above it in its section is an
  error. The match bytes must lie in the same image section as the patch, so both are in memory when it is applied;
  otherwise the patch is dropped and logged. Patches with CURRENT_BYTES of their own are checked against those,
  never against the match line.
- **No other conditions.** The section's title ID (and version, if given) and the current (or match) bytes
  decide.
- **Errors:** a line that cannot be read is skipped and counted. Other keys are ignored, for later additions.

### 3.2 When patches are written
1. **When the title is loaded** (XeLoadImage, after the certificate checks and before the sections are read), the
   kernel reads the file and keeps the records of the sections that match this title and version.
   - A patch that is not entirely inside one of the title's sections is dropped and logged.
   - No file, or no matching section, means no patches.
2. **Each time a section is read from the image,** after its digest is computed, each patch that falls inside it is
   checked: if the NEW_BYTES are already there (a copy patched before), it is logged as already there; else if the
   CURRENT_BYTES (or the section's match bytes) are there, the NEW_BYTES are written; otherwise it is logged and
   not written. The patches checked against match bytes go first, because another patch of the section may change
   those bytes (the first patch of spec/cerbios/13 §7.9 is at its match address). This happens
   - for the preloaded sections, during the load and before the code sections are made read-only;
   - for the others, when the title loads them (XeLoadSection), again on every reload.
3. **The serial log** shows the counts (matching sections, patches, lines not understood), each section's `name`,
   and for every patch: written, already there, or the current (match) bytes differ.

The records live in the kernel's pool while the title runs (a few bytes per patch). They are freed when the next
title is loaded.

## 4. The video overlay at a title switch
A dashboard that plays video through the GPU's video overlay (PVIDEO) can leave it running when it starts a title:
a buffer marked in use (NV2A 0x8700 bit 4), an overlay interrupt pending (0x8100), and the overlay's addresses
never set. A test title's graphics set-up stops the overlay (0x8704 ← 1) and waits for 0x8700 to clear; in that
state the overlay never finishes and the game hangs on a black screen (about one start in three from the dashboard).
- **Code:** `kernel/src/hal/pci.cpp` `HalpIdleVideoOverlay`, called on a cold boot (HalpQuiesceBusMasters) and on the
  quick reboot that starts the next title (HalInitSystemPhase0).
- **What it does** when the overlay is switched on (PMC 0x200 bit 28): overlay interrupts off (0x8140 ← 0), both
  buffers stopped (0x8704 ← 0x11), 0x8700 ← 0, pending interrupts acknowledged (0x8100 ← 0xFFFFFFFF). Writing 0x8700
  does not clear an in-use bit; the stop does, at a vertical blank. The kernel waits up to 50 ms for that. A buffer
  still marked in use after the wait is one the overlay never finishes: the overlay is then reset (PMC bit 28 off and
  on), with 0x8088 / 0x808C (which the boot display sets, and which the reset would clear) saved and written back,
  and its interrupts are turned off and acknowledged again.
- **Log (serial build):** `pci: video overlay: buffer .. stop .. interrupt .. enable .. -> buffer .. interrupt ..`,
  with `(reset: the buffer was not released)` when the reset was needed.
- **Checked:** the test title started from the dashboard several times. With the stop alone the buffer was once still in
  use at both title switches and the title hung; with the wait and the reset it started every time (the buffer was
  released within the wait on those runs).
