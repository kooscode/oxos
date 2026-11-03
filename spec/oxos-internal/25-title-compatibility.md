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
