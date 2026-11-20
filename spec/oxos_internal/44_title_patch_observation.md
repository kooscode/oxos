# 44. Title patches by observation: what another BIOS changes in a title's memory

OXOS research method, 2026-11-19. The patches OXOS ships for single titles (`kernel/settings/oxos_title_patches.ini`, spec 25 §3) are now fully derived from the
titles themselves: each title was run under a control BIOS and under Cerbios, its code in memory was dumped at the same moment under both, and the two were
compared. Each difference was then explained by reading the title's own code around it. **Nothing** was taken from Cerbios's image: no code, no patch tables, no
bytes, and none of it was disassembled for this work.

Spec 25 §3 built OXOS's universal title patcher: it writes byte patches from a file into a title's sections as they load, and knows nothing about particular
titles. This spec is where the patcher becomes useful: it works out, for a first set of titles, which patches they need, and its results (§7) are the entries
of the shipped file (§9). Later studies can add more titles to the same file the same way.

## 1. Which titles, and why
Cerbios's release notes (`Readme.nfo` shipped with Cerbios Hybrid 3.1.0 Beta) clearly list the affected titles it patches in clear text, under the heading
"Titles Fixed With (ApplyTitlePatches = True)".

This is the reason why only these named Titles were a target for memory dump observations; no other title was searched.

| Title ID | Cerbios notes | Notes version |
|---|---|---|
| 4D530031 | "Added 480p Video Mode" | 3.0.0 Beta |
| 4541038B | "Fixed 720p Issue" | 3.0.0 Beta |
| 45410042 | "Fixed HDD Rip Issue" | 3.0.0 Beta |
| 434D0050 | "Fixed HDD Rip Issue" | 3.0.0 Beta |
| 48450001 | "Fixed HDD Rip Issue" | 3.0.0 Beta |
| 56550013 | "Fixed Booting Issue" | 3.0.0 Beta |
| 41560010 | "Fixed Booting Issue" | 3.0.0 Beta |
| 53430007 | "Added Force NTSC" | 3.0.0 Beta |
| 56550039 | "Fixed HDD Rip Issue" | 3.1.0 Beta |
| 4D4A000C | "Fixed … Loading Screen When Using Overclocked Or CPU-Upgraded Consoles" | 3.1.0 Beta |
| 56550041 | "Fixed Menu + UI When Xbox Set To Widescreen" | 3.1.0 Beta |

## 2. What was run
| Item | What |
|---|---|
| Emulator | xemu, 64 MB, MCPX 1.0 boot ROM (dumped from a console, spec oxos_internal/12), started with `tools/run.py --gdb` |
| Control BIOS | "Complex 4627": Microsoft's retail 4627 kernel with its image checks removed, so it loads any XBE |
| Observed BIOS | Cerbios Hybrid 3.1.0 Beta, 256 KB, its default settings (title patches on) |
| Hard disk | the Microsoft dashboard disk (FATX); for the hard-disk runs, each title's `default.xbe` copied to `E:\obs\TITLEID\` |
| Titles | each title's own disc image (XISO) in the DVD drive, or its `default.xbe` on the hard disk |

**The titles' code had to be the retail code**, so that a difference could only come from the BIOS:
- Each XBE section carries a SHA-1 digest. Where all of a title's sections matched their digests, the code is the signed retail code.
- Where an image's code had been changed at a few places, those bytes were put back to the original and the digests were then checked again: 434D0050
  (five places), 56550039 (two places) and 4541038B (one byte). All three then matched every digest.
- Three images (41560010, 45410042, 48450001) carry a change in `.text` that no known patch explains. They were run as they are; the control runs show what
  they hold, and none of their changes lies at a place either BIOS wrote.
- The certificate fields that scene copies edit (allowed regions and media) are not covered by the digests; §6 says how they were varied.

## 3. The runs
Each title was run in three ways, each under both BIOSes:

| Run | How the title starts | Console settings |
|---|---|---|
| Disc | its disc image in the DVD drive; the BIOS starts it | xemu's defaults (composite, 4:3) |
| Disc, HD | the same | the EEPROM's 480p, 720p and 1080i flags and widescreen set, HDTV AV pack (`tools/run.py --hd --widescreen`) |
| Hard disk | OXOS's XBE launcher (`tools/xbe_launcher`) on a disc starts `E:\obs\TITLEID\default.xbe` (launch data page, quick reboot) | xemu's defaults |

That is 66 runs for the 11 titles, and 28 more for the certificate regions (§6). **Total of 94 Title runs in Xemu and Title memory-dump observations**, leading
to clean room patches for all the titles OXOS patches.


## 4. Capturing the title's memory
xemu's GDB server was driven from gdb in each run (no breakpoint or change in either BIOS):
1. **At the title's first instruction.** A hardware breakpoint at the XBE's entry point (header +0x128, decoded with the retail key 0xA8FC57AB, or the debug
   key when the retail one gives an address outside the image). The launcher and the dashboard run at the same addresses before the title, so the breakpoint
   only stops when the title's own first four bytes are there. At that stop, every section the XBE marks as preloaded is dumped from memory (its virtual
   address, for its raw size).
2. **40 seconds later.** gdb stops the title again and dumps every section in memory then, including those the title loads itself.
3. xemu is closed; the next run starts from a fresh copy of the disk and EEPROM.

## 5. Comparing
1. **The control against the file:** in every control run, every preloaded section in memory equals the XBE file, except the kernel import table (the loader
   writes the kernel's addresses there). So the control kernel changes nothing in a title's code, and the XBE file is what the title runs.
2. **Cerbios against the control:** the two dumps of the same kind of run, byte by byte, the import table left out (the two kernels' addresses differ). What
   remains is what Cerbios changed in the title. Neighbouring changed bytes (less than 3 apart) form one change.
3. **Later in the run:** the dumps 40 seconds in show the same Cerbios changes. They also differ from the file in sound, video and controller library code
   (`DSOUND`, `XPP`, `BINKYUY2`, ...), equally under both BIOSes: the titles' own run-time changes, not the BIOS's.
4. **What each change does:** the title's code around each change was disassembled (the title's own XBE, never the BIOS), with the kernel functions it calls
   named from its import table. §7 says what was found.

## 6. Conditions observed
- **Started from the hard disk:** 434D0050's change at 0xF6EB7 appears only in the hard-disk run. Its other four changes appear in all three.
- **HD and widescreen:** 4541038B's change appears only in the disc HD run.
- **Certificate regions:** the scene images of nine titles allow every region (certificate +0xA0 = 7). Each was also run from the hard disk with the region set
  to 1, 2 and 4, under Cerbios. Only 45410042 depends on it: its change appears with region 1 (North America) and in no other run. The other eight show the
  same changes with every region. The control BIOS, run with 45410042 at region 1, again left the code as the file.
- Every other change appears in all three kinds of run.

## 7. What was found
Addresses are the title's virtual addresses; "before" is the retail code (the file and the control run), "after" what the Cerbios run held. The certificate
version of the release run is given with each title ID.

**434D0050 (1): five changes observed, the fifth only from the hard disk.**
| Place | Before > after | What it does |
|---|---|---|
| 0x1C930D | `8B F0 56 E8 00 F0 03 00` > `B8 7C D0 07 00 90 90 90` | uses the archive's position on the original disc, 0x7D07C |
| 0x20831B | `56 8D 45 08 50 FF 35 30 C7 34 00 83 CE FF` > `BE 56 34 12 00 8B C6 5E C9 C2 04 00 90 90` | the sort-key routine returns 0x123456 at once |
| 0xECD8B | `C7 44 24 20 00 00 48 00` > 8 × `90` | no error value after the `D:\` check |
| 0x1C9CC3 | `C7 05 EC 6A 3A 00 00 00 00 00` > 10 × `90` | the global 0x3A6AEC is no longer cleared |
| 0xF6EB7 | `14` > `05` | 20-digit file names instead of 80-digit ones |

- **0x1C930D:** the title opens `d:\default.xbe`, takes its physical sort key (its position on the disc) and works out from it where its archive starts (it
  logs "default.big is at %u"). On a rebuilt image or a hard disk the files are not where they were on the disc.
- **0x20831B:** the start of the title's XGetFilePhysicalSortKey, which asks the file's volume for its attributes.
- **0xECD8B:** after a check on `D:\` fails, the title stores 0x480000 in a local value.
- **0x1C9CC3:** when a value read from the disc differs from a stored one, the title clears a global.
- **0xF6EB7:** the routine that turns a name into the title's numeric file names takes 20 characters, 4 digits each. With 5 it makes 20-digit names: the
  names of the five data files once they are renamed to fit FATX's 42-character limit.

**4541038B (3): one change observed, only with HD and widescreen.** 0x1F7AFC `75` > `EB`: after creating a graphics object the title checks the result, and on
failure releases it and goes to its error path (0x1F7EE0). The change always takes the success branch.

**4D530031 (5): four changes observed, two pairs.**
- 0x15DF0F and 0x15DF69, `8B 35` > `FF 35`: two places that set up the render target call the field set-up routine (0xC6E30: 640 × 240 or 288 lines, sets
  bit 0x80 of the target's flags) with the object at 0x38DA6C in a register. The change pushes the object instead.
- 0x15DF17 `8F F6` > `8D F6` and 0x15DF71 `8E F6` > `8C F6`: the same two calls move 0x200 bytes back, to the frame set-up routine (0xC6C30: 640 × 480 or
  576 lines, clears bit 0x80), which takes the object on the stack. The title draws full frames instead of fields.

**56550013 (1): one change observed.** 0x1FC80 `55` > `C3`: the routine that plays a movie (it builds the file name with the extension `xmv`). It returns at
once, so the movies are skipped.

**41560010 (2): seven changes in one routine observed.** The title's CPUID routine (0x140C30) saves only EBP, and CPUID overwrites EBX, which a C caller expects
kept.
- 0x140C30 `55` > `60` and 0x140C6C `5D` > `61`: `pushad` and `popad` instead of saving EBP alone.
- 0x140C38 `08` > `24`; 0x140C49, 0x140C51, 0x140C5A, 0x140C63 `0C` > `28`: the parameter offsets move by 0x1C to match the larger save.

**56550041 (2): two changes observed.** 0x3D980 and 0x15788D, `40` > `48`: two camera set-ups (near 0.1, far 10.0) take a value from a virtual method of an
object at table offset 0x40; the change calls the method at 0x48 instead.

**56550039 (3): one change observed.** 0x209A8E `55 8B EC 83 EC 34 FF 75` > `B8 00 08 00 00 C2 04 00`: the routine opens a path (RtlInitAnsiString, NtOpenFile)
and returns its volume's sector size (NtQueryVolumeInformationFile, FileFsSizeInformation). The change returns 2048, a DVD's sector size, at once; from a hard
disk the title would otherwise see 512.

**48450001 (2): one change observed.** 0xA2DA0 `81` > `C3`: the title's "DisplayBadDisc" (it logs "DisplayBadDisc called: %s" and draws the dirty-disc screen)
returns at once.

**45410042 (3): one change, only with certificate region 1.** 0x5BA3F `75 0B` > `90 90`: the title asks for the sector size of `D:\` (the routine at
0x6210A, through the volume's size information); for 2048 it uses its DVD layout (mode 2), otherwise mode 0. The change always uses the DVD layout. From a
hard disk the title would otherwise see 512.

**53430007 (2): one change observed.** 0xBC550 `83` > `C3`: while a state value (0x39A5D4) is not 3, the routine draws string 0 of `strings/XBOX_Strings.txt` on
the screen. It returns at once. Which message string 0 is was not looked up; the release notes call this fix "Force NTSC".

## 8. What showed no change, and why OXOS does not patch it
A title patch is shipped only where a run showed the change in the title's memory. These showed none, so OXOS has nothing observed to ship for them and does
not patch them:

| Title ID | Runs | Why no change was seen |
|---|---|---|
| 4D4A000C (1) | disc, disc HD, hard disk; regions 1, 2, 4 | the fix is for overclocked or CPU-upgraded consoles; xemu runs at the stock 733 MHz |
| Other releases | | one release of each title was run; another release of 56550041, 56550039 or 45410042 that needs the fix elsewhere was not |

## 9. What OXOS ships
`kernel/settings/oxos_title_patches.ini` holds the changes of §7, each with the retail bytes before it, so a patch is written only into the code it was found
in (spec 25 §3). Two choices:
- **434D0050 0xF6EB7 is not shipped.** It only works when the five data files carry 20-digit names; an image or an exFAT folder keeps the original
  names, and the title would then look for files that are not there. OXOS's patcher has no conditions (spec 25 §3.1), so the line is left out.
- **4541038B 0x1F7AFC is shipped without its condition.** It changes only the failure branch of a check; where the object is created, both branches agree.

## 10. Reproducing
For a title: its disc image and its `default.xbe` (checked against the section digests, §2), the two BIOS images, an MCPX 1.0 ROM, xemu and gdb. For each
run of §3: `tools/run.py BIOS --mcpx-rom MCPX --memory 64 --gdb PORT --iso IMAGE [--hd --widescreen] [--hdd DISK]`, then gdb with the stop and dumps of §4,
then the comparison of §5. Each run takes about 90 seconds.
