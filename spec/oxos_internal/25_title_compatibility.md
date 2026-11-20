# 25 — Title compatibility: answers games expect

OXOS addition, implemented 2026-11-03.
- **Code:** `kernel/src/filesystems/xdvdfs.cpp` (file-system name, `XDVDFS_FCB`), `kernel/src/filesystems/hddfs/hddfs.h` (`HDDFS_FCB`),
  `kernel/src/filesystems/hddfs/dispatch.cpp` (`filesystems::HDDFSRememberTitleFolder`, FileFsSizeInformation), `kernel/src/titles/title_loader.cpp` (the call
  before the title starts).
- **Tests:** boot self-tests `KiTestDvdSortKeyOk` (tests/kernel_self_tests/dvd_drive.cpp) and `KiTestFATXTitleFolderOk` (tests/kernel_self_tests/fatx.cpp), on
  FATX and on exFAT.

Games link their system library into their own executable, and some of its routines depend on details of the stock kernel's file systems. Where a game's
behaviour depends on what a kernel call returns, OXOS returns what the game expects.

## 1. The physical sort key
Games call the library routine XGetFilePhysicalSortKey(handle) to learn where a file lies on its disk, for example to order reads. A title (434D0050) uses it to
find its archive on the disc. The routine:
1. asks the file's volume for FileFsAttributeInformation (IoQueryVolumeInformation);
2. if the name is **"FATX"** (name length 4), returns the dword at **FsContext + 0x1C**, the file's first cluster. It returns 0 if bit 0 of the first byte at
   FsContext is set;
3. if the name is **"GDFX"** (length 4), returns the dword at **FsContext + 0**, the file's start sector;
4. otherwise, fails with STATUS_INVALID_PARAMETER.

OXOS therefore:
- **Disc file system:** names itself "GDFX" (FileFsAttributeInformation: attributes 0x80000 read-only, maximum name length 255, name length 4) and keeps
  `StartSector` as the first field of its per-file record.
- **FATX and exFAT:** name themselves "FATX" (unchanged) and keep `FirstCluster` at offset 0x1C of the per-file record. The record starts with a list link, a
  pointer, so bit 0 of its first byte is always clear.
- Both offsets are compile-time checks (`XK_OFFSET`).

Before this, the disc file system named itself "XDVDFS" and the FATX record had another field at 0x1C, so the routine returned garbage on OXOS.

## 2. Sector size of D: on the hard disk
Some games ask the sector size of `D:\` and expect a DVD's 2048 bytes:
- **A disc or a disc image:** the disc file system reports 2048 already.
- **A title started from a hard-disk folder** (D: is a link to that folder):
  - Just before the title starts, the kernel opens `\??\D:\`. If that is a FATX or exFAT folder, it remembers the folder and keeps the handle open while the
    title runs.
  - FileFsSizeInformation and FileFsFullSizeInformation, asked through that folder or anything below it, report BytesPerSector 2048 and SectorsPerAllocationUnit
    scaled to match, so the cluster size and the cluster counts are unchanged. This needs a cluster of 2048 bytes or more, which every FATX and exFAT volume
    has; otherwise the volume's own figures are kept.
  - Every other path on the volume keeps its real sector size.
  - Each title start sets this again: a disc title or a dashboard on a disc clears it.
- **Why it is safe:** a title that aligns unbuffered reads to the reported size aligns them to 2048, which is also a multiple of 512.

## 3. Title patches from a file
Patching a game's code in memory is decades old: on PCs it goes back to DOS, where memory-resident (TSR) patchers and trainers changed games while they ran, and
on the Xbox, region and video-mode fixes and the "HDD fixes" that let a disc game run from a hard disk have done it. Most problems OXOS meets are better fixed
in the kernel, by giving the game the answer it expects (§1, §2). Some are in the game's own code, and no kernel answer can fix them. For those the kernel has a
universal title patcher: as a title's sections are loaded, it writes byte patches into them, taken from a text file. Any title can be fixed without a new
kernel, and a patch is written only into the exact code it was made for (§3.1).

The patcher knows nothing about particular titles. Which titles need patches, and what the patches are, is worked out separately, title by title; the
results are written into the file, and the patcher applies them.

- **Code:** the title patcher (`kernel/modules/title_patcher.cpp`): it reads the file at each title load and builds the title's records with the parser
  (`shared/ini_file/title_patcher_ini.cpp`, `shared/ini_file/title_patcher_ini.h`) on the shared ini line reader (`shared/ini_file/ini_file.cpp`); the
  resident kernel keeps the records and writes them into each section as it is read (`kernel/src/titles/title_loader.cpp`: `titles::LoadPatches` builds
  them, `titles::ApplyPatches` writes them).
- **Tests:** host unit tests `tests/unit/test_title_patcher_ini.cpp` (the format, and the shipped file parses without an error); boot self-test "title patches"
  (boot_test.py puts a patch file for the test title on the test disk: a matching section changes a variable, a section for another certificate version leaves
  another one alone, a patch whose current bytes are not there leaves a third one alone, and a patch below the image is skipped).
- **Shipped file:** `kernel/settings/oxos_title_patches.ini`, which the release copies to `C:\OXOS\`: each entry in the section of the certificate version
  it was made for, with the retail bytes before it.
- **OXOS fixes in the shipped file** (marked "OXOS fix" in it): title 434D0050's Xbox Live sign-in picks a security gateway at random from the list the
  service returns, `random % count`, with no check for an empty list (0x213B46, `div ecx`): with no gateways returned it stops with a divide error (bug check
  0x1E, 0xC0000094). The patch at 0x213AD6 retargets the `jbe` that is taken only for a count of 0 (`76 4B` > `76 94`) to the routine's own failure path (state
  3, as when the lookup fails), so sign-in fails and the title carries on. Checked on the Modxo console: the crash reproduced with the trace kernel, and with
  the patch written sign-in no longer stops the title.

### 3.1 The file
`C:\OXOS\oxos_title_patches.ini`, read through `\Device\Harddisk0\Partition2`. On a FATX disk that is C:; on an exFAT disk it is
`/C/OXOS/oxos_title_patches.ini`. Text, at most 1 MB, lines ending in LF or CR LF:

```ini
; a comment (also after a section header or a patch)
[56550013]            ; every version of title 56550013
name = what it fixes  ; shown in the serial log
patch = 1FC80: 55 8B EC 83 > C3 8B EC 83

[56550039/3]          ; only the release whose certificate version is 3 (decimal, or 0x hex)
patch = 209A8E: 558BEC83EC34FF75 > B8 00 08 00 00 C2 04 00

[12345678]            ; an example: when the match bytes are there, the blind patch below is written
match = 1000: 55 8B EC 83
patch = 1010: > 90 90
```

- **Sections:** `[TITLEID]` is the certificate's title ID (header +0x118 → +8), in hex. `[TITLEID/VERSION]` also needs the certificate version (+0xAC). Every
  matching section applies, in file order.
- **Patches:** `patch = ADDRESS: CURRENT_BYTES > NEW_BYTES`. The address is a virtual address of the loaded title, in hex (`0x` allowed). CURRENT_BYTES and
  NEW_BYTES are hex byte pairs, with or without spaces and tabs, 1 to 255 on each side: the kernel compares the CURRENT_BYTES at the address and writes the
  NEW_BYTES there. NEW_BYTES may be shorter than CURRENT_BYTES (the bytes after them are left alone), so such a line still works; the shipped file always gives
  both sides the same length, so each line shows the code before and after in full (the unit test checks it); NEW_BYTES is longer only where the rest of the
  current bytes is not known.
- **Why the current bytes:** a title ID and a version do not identify the game's code. Launchers that start a game from a disc image carry the game's
  certificate (the OXOS ISO launcher does, so dashboards show the game), and any header field can be copied. The code itself cannot: a patch is written only
  into the code it was made for. Four bytes or more is the rule for the shipped file (a one-byte check matches by chance too often).
- **Match lines (blind patches):** `match = ADDRESS: CURRENT_BYTES`, for a section where some patches cannot give their current bytes. When the match line's
  CURRENT_BYTES are at its address, each blind patch below it in the section, `patch = ADDRESS: > NEW_BYTES`, is written. A blind patch without a `match =` line
  above it in its section is an error. The match bytes must lie in the same image section as the patch, so both are in memory when it is applied; otherwise the
  patch is dropped and logged. Patches with CURRENT_BYTES of their own are checked against those, never against the match line.
- **No other conditions.** The section's title ID (and version, if given) and the current (or match) bytes decide.
- **Errors:** a line that cannot be read is skipped and counted. Other keys are ignored, for later additions.

### 3.2 When patches are written
1. **When the title is loaded** (titles::LoadImage, after the certificate checks and before the sections are read), the kernel reads the file and keeps the
   records of the sections that match this title and version.
   - A patch that is not entirely inside one of the title's sections is dropped and logged.
   - No file, or no matching section, means no patches.
2. **Each time a section is read from the image,** after its digest is computed, each patch that falls inside it is checked: if the NEW_BYTES are already there
   (a copy patched before), it is logged as already there; else if the CURRENT_BYTES (or the section's match bytes) are there, the NEW_BYTES are written;
   otherwise it is logged and not written. The patches checked against match bytes go first, because another patch of the section may change those bytes (a
   patch may be at its own match address). This happens
   - for the preloaded sections, during the load and before the code sections are made read-only;
   - for the others, when the title loads them (XeLoadSection), again on every reload.
3. **The serial log** shows the counts (matching sections, patches, lines not understood), each section's `name`, and for every patch: written, already there,
   or the current (match) bytes differ.

The records live in the kernel's pool while the title runs (a few bytes per patch). They are freed when the next title is loaded.

## 4. The video overlay at a title switch
A dashboard that plays video through the GPU's video overlay (PVIDEO) can leave it running when it starts a title: a buffer marked in use (NV2A 0x8700 bit 4),
an overlay interrupt pending (0x8100), and the overlay's addresses never set. A test title's graphics set-up stops the overlay (0x8704 ← 1) and waits for 0x8700
to clear; in that state the overlay never finishes and the game hangs on a black screen (about one start in three from the dashboard).
- **Code:** `kernel/src/hardware/chipset/chipset.cpp` `hardware::IdleVideoOverlay`, called on a cold boot (hardware::QuiesceBusMasters) and on the quick reboot
  that starts the next title (hardware::InitHalPhase0).
- **What it does** when the overlay is switched on (PMC 0x200 bit 28): overlay interrupts off (0x8140 ← 0), both buffers stopped (0x8704 ← 0x11), 0x8700 ← 0,
  pending interrupts acknowledged (0x8100 ← 0xFFFFFFFF). Writing 0x8700 does not clear an in-use bit; the stop does, at a vertical blank. The kernel waits up to
  50 ms for that. A buffer still marked in use after the wait is one the overlay never finishes: the overlay is then reset (PMC bit 28 off and on), with 0x8088
  / 0x808C (which the boot display sets, and which the reset would clear) saved and written back, and its interrupts are turned off and acknowledged again.
- **Log (serial build):** `pci: video overlay: buffer .. stop .. interrupt .. enable .. -> buffer .. interrupt ..`, with `(reset: the buffer was not released)`
  when the reset was needed.
- **Checked:** the test title started from the dashboard several times. With the stop alone the buffer was once still in use at both title switches and the
  title hung; with the wait and the reset it started every time (the buffer was released within the wait on those runs).

## 5. USB host controllers at a title switch
Before the quick reboot that starts the next title, the kernel resets both OHCI host controllers (HcCommandStatus bit 0, HostControllerReset), so that neither
keeps writing into memory the next title is given. A controller leaves that reset in the USBSUSPEND state (OHCI 1.0a §7.1.2), not USBRESET, the state it has
after a cold boot. A title's USB library (XPP), started by XInitDevices, reads HcControl's functional state first: for USBRESET or USBOPERATIONAL it carries on
at once; for USBSUSPEND (or USBRESUME) it sets USBRESUME and sleeps 20 ms (KeDelayExecutionThread). A test title builds its input object on first use, and
records it only after the constructor, which calls XInitDevices, has returned. During the 20 ms sleep its loader thread (just woken by a disc read, priority 9
against the main thread's 8) asked for the input object as well and called XInitDevices again. The second set-up found `\Device\MU_0` taken (IoCreateDevice,
STATUS_OBJECT_NAME_COLLISION), carried on with memory-unit devices it never created, and its USB interrupt code then read through a garbage device pointer (bug
check 0x1E, an access violation at the start of the title, about 0.35 s in).
- **Code:** `kernel/src/hardware/chipset/smbus.cpp` `KiQuickRebootQuiesce` (and the same step in `kernel/src/hardware/chipset/chipset.cpp`
  `hardware::QuiesceBusMasters`): after the reset completes (bit 0 clear, within 10 µs), HcControl ← 0: USBRESET, every list off. A title then finds the
  controllers as after a cold boot and starts its USB library without the sleep.
- **Checked:** the Modxo console, the test title started directly over netboot and from the dashboard: before, it stopped at start every time, and the trace
  showed both set-ups (`\Device\MU_0` created twice, by two threads); with the change it starts and plays.

## 6. C++ exceptions thrown from a catch block
A test title throws a C++ exception (0xE06D7363) while it creates an online match, and a second one from inside the catch block of the first. The title's C++
runtime finds the first one's catch, unwinds to its frame from inside its handler (saving FS:[0] before RtlUnwind and linking it back on top after it, with Next
= FS:[0]), then runs the catch. On the stock kernel the saved registration is the dispatcher's guard frame around the handler call (spec/stock/14, "Dispatch").
The kernel had no guard frame, so the saved registration was the title's own frame, which was then linked to itself: the second exception's dispatch called that
frame's handler for ever, and the title stayed on its loading screen.
- **Code:** `kernel/src/core/exceptions.cpp` `KiExecuteHandler` (handlers run under the exception guard or the unwind guard), `core::DispatchException` (nested
  flag and frame), `KiUnwind` (collided unwind).
- **Found with:** a serial log of each exception (code, address, thread, handler) and of the registration chain at each dispatch and unwind (debug code, not
  kept): after the first unwind the chain read `D0090C10 → D0090C90 → D0091528 → D0091528 → …`.
- **Checked:** the Modxo console: before, the test title stopped on its loading screen at that point every time (twice); with the guard frames it creates the
  match and carries on. 158 host unit tests pass.

## 7. The network receiver at a title switch
Before the quick reboot that starts the next title, the kernel turns the NIC's receiver off as well as its transmitter. A quick reboot does not run the
cold boot's bus-master stop (spec 10): HAL phase 0 only reprograms the interrupt controller and stops the video overlay (§4), so this step is the only one
between two titles. A receiver left on keeps taking every frame the network sends (broadcasts, ARP, the end of an FTP session) and writes it, with its
descriptor, into the receive ring of the title that just ended. That memory is free for the next title, and the writes go on until the next title starts
the network itself, if it ever does.
- **Seen with:** a title started by the OXOS FTP server (`SITE LAUNCH`). About 3 s in, its background copy of the disc's compressed archives to its cache
  drive stops with a zlib error ("incorrect data check" or "invalid block type") in the first archive it unpacks. Every disc read had delivered the image's
  bytes (a sum per read, compared with the image on the PC); the bytes changed afterwards, in the unpacker's memory. The title records the failure and, at the
  front end (skipping the intro, or its end), formats its cache drive and shows its "dirty disc" screen. xemu, where nothing sends the console frames, never
  showed it.
- **Code:** `kernel/src/hardware/chipset/smbus.cpp` `KiQuickRebootQuiesce`: NvTxControl (0x084) ← 0, NvRxControl (0x094) ← 0, then up to 100000 reads of
  NvRxStatus (0x098) until bit 0 (a frame being received) is clear. The rings and TxRxControl are left alone: a full reset there left the NIC in a state a
  title's network start-up waited on forever (XBMC4Gamers started by EvoX's dashboard, 2026-10-29). The next title that uses the network programs its own
  rings and turns the receiver on again.
- **Checked:** the Modxo console (dev kit, exFAT disk), that title started by the FTP server: before, the dirty-disc screen on every run; with the receiver off,
  the copy carries on and the front end comes up.
