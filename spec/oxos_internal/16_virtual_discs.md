# 16. Virtual discs: titles from disc images on the hard disk

Implemented 2026-10-27. An image of an Xbox disc (made from the user's own disc) on the hard disk stands in for the DVD drive, so a title starts and runs from
it as from the disc. The device interface is rmenhal's virtual DVD drive from NKPatcher (`cdromimagedrv.asm`, 2004, GPL-2.0): the control codes
0x1EE7CD00/01/02, `\Device\CdRom1`, split images and the drive replies of §3.1. The extended mount request is the one Team Resurgent's Hermes (`src/external.h`,
GPL-3.0) sends; OXOS takes that layout too, so tools written for either work here, and the OXOS ISO launcher works on other BIOSes that offer this device. Code:
`kernel/src/storage/virtual_disc.cpp`, `tools/iso_launcher/`.

## 1. Images
- **XISO** (the game partition only) and **full disc dumps**. The game partition starts at 0x18300000 in a dump (Microsoft's disc layout, spec/stock/04c;
  extract-xiso reads it the same way). A dump is recognised when its first part is larger than 0x18310800 bytes and the 2048-byte sector at 0x18310000 starts
  and ends with `MICROSOFT*XBOX*MEDIA`. Every read is then offset by 0x18300000. OXOS also reports the game partition's size, not the file's.
- **CCI** (LZ4-compressed blocks, made with Team Resurgent's Repackinator; format: Team Resurgent's libCCI `FORMAT.md`, GPL-2.0). A split CCI is two whole CCI
  files, each with its own header and index for its run of sectors; part 0 holds the first ones. `tools/mkcci` converts an `.iso` into one or two parts and
  checks them against the input.
- **One or two parts**, read as one image (part 0's sectors first). A FATX disk needs split images (files stop at 4 GB); on exFAT an image may be one file of
  any size (§4).

## 2. Devices and requests
rmenhal's NKPatcher interface, with Hermes's request layout; the slots are OXOS's:
- The control device is `\Device\Virtual0\Image0`, with the links `\Device\CdRom1` and `\??\CdRom1:`. It is created after the drive
  (`storage::InitVirtualDiscs`, from `storage::InitIdeChannel`). It is not a mountable device type, so an open reaches the driver directly.
- Images go in slots 1 and 2, as `\Device\Virtual0\Image1` and `Image2` (CD-ROM devices, 2048-byte sectors).
- The device controls are 0x1EE7CD00/01/02 (NKPatcher's; also 0x0CE52B01/02, as Hermes sends them, and 0x0CE52B00): present, mount and unmount. A mount request
  has Hermes's layout (0x4C bytes: part count, type, eight counted-string paths, a link name), which extends NKPatcher's (a dword part count, then the eight
  paths at +4). The strings are copied, bounded to 0x106 characters, after an address check.
- A mount first unmounts its target (as NKPatcher's attach does): every slot through the control device, its own slot through an image device.
- Types `D` (plain) and `d` (CCI), as Hermes sends them, and 0 (plain: NKPatcher's request, whose dword part count leaves the type byte 0) are accepted.
  Other types fail with STATUS_INVALID_DEVICE_REQUEST.
- A link name other than the drive's (`\Device\CdRom0`, `\??\CdRom0:`) is created to point at the image's device.

## 3. The image is the console's disc (OXOS design)
OXOS keeps `\Device\CdRom0` as the drive's device. While a DVD-type image is mounted, the drive's read and device-control routines hand every request to the
image (`storage::ServeVirtualDvd`). So:
- D:, `\??\CdRom0:`, the loader's `\Device\CdRom0\default.xbe` and every title's own drive requests reach the image with no renamed objects. The drive's volume
  is dismounted at mount and unmount, so the next open mounts XDVDFS on the image (or on the drive again).
- With two DVD images, the newer one is the disc. Unmounting it brings back the other one, else the drive.
- The loader treats a mounted image as an authenticated Xbox game disc (XBE media type 2). Mounting and unmounting clear the disc's pinned title
  (spec/stock/05).

### 3.1 Replies to drive requests
| Request | Reply |
|---|---|
| READ (whole sectors inside the image) | the image's bytes; anything else STATUS_INVALID_PARAMETER |
| 0x24000 READ_TOC, 0x24038 GET_LAST_SESSION | NKPatcher's 12 bytes: one data track (`00 12 01 01 00 14 01 00 00 00 02 00`; GET_LAST_SESSION the same) |
| 0x2404C GET_DRIVE_GEOMETRY | image sectors, RemovableMedia, 1, 1, 2048 |
| 0x24800 CHECK_VERIFY, 0x24080 AUTHENTICATE_DISK | success |
| 0x4D014 SCSI pass-through | the Xbox security commands (the drive emulation's own test, `storage::IsDvdSecurityCommand`: MODE SENSE / SELECT of page 0x3E, READ DVD STRUCTURE at 0xFF02FDFF layer 0xFE) get the drive emulation's replies (spec/stock/04 §3a): the authenticated mode page 0x3E, an accepted MODE SELECT, a security sector built for the running title and signed for the open key. TEST UNIT READY succeeds; other commands fail. OXOS answers so that titles which check the disc themselves still start. |
| others (e.g. 0x335140 DVD_READ_STRUCTURE) | STATUS_INVALID_DEVICE_REQUEST, as NKPatcher |

An unmounted image device answers STATUS_NO_MEDIA_IN_DEVICE.

## 4. Reading
- Parts are opened with NtCreateFile (read, shared read, synchronous) and read with NtReadFile in the caller's thread. FATX reads whole sectors straight from
  the disk, so there is no second cache under XDVDFS's.
- CCI reads go one block at a time under a per-slot lock: two index entries (from a cached window of 1024 entries per part), then the stored block. A block
  stored as 2048 bytes is raw. Otherwise its first byte is a padding count p, and the next (length − 1 − p) bytes are an LZ4 block that must decode to exactly
  2048 bytes.
- **exFAT files of 4 GB or more** (spec/oxos_internal/07 §2) are ordinary files: read and written with 64-bit offsets, so an image can be copied to the console
  in one piece (FTP, the File Manager). Size queries and directory listings report the full 64-bit size.

## 5. Quick reboots
The slot table (type, part paths, link name) is in `.persist`. A mount survives quick reboots (launches, a title's own restarts) and ends at a cold boot. On a
console with a DVD drive an eject also ends it: opening the tray while a title runs resets the console through the SMC, and that is a cold boot. At each boot
the title-launch thread re-attaches every saved slot before choosing a title (`storage::RestoreVirtualDiscMounts`). A slot whose files no longer open is
dropped.

Unmounting stops new requests to the slot and waits for running ones to finish. It then closes the files, deletes the image's link and frees the slot. The image
device stays (a later mount reuses it).

## 6. Mounting from a title: `MountISO`
`tools/iso_launcher/mount_iso.h` is a header-only helper for nxdk titles such as dashboards. `MountISO(path)` takes a drive path (C E F G X Y Z) or a `\Device\`
path to an `.iso` or `.cci` image and builds the request itself: drive letter to partition, the second part of a `name.1.iso` split image, CCI from the
extension. The caller then launches `\Device\CdRom0\default.xbe`. `UnmountISO()` makes `\Device\CdRom0` the drive again. Parts are pieces of one disc image
(FATX files stop at 4 GB), not the discs of a multi-disc title.

## 7. The ISO launcher (`tools/iso_launcher`)
An nxdk title, built with `make -C kernel iso_launcher`.
1. **Which image.** The first line of `D:\iso.txt` names it: an image, a folder holding one, or a `\Device\` path. Drive letters follow the title launcher (C E
   X Y Z, F G). Without `iso.txt`, the launcher uses the image in its own folder, so a copy of the launcher placed next to an image starts that image. `none`
   unmounts the image and starts the dashboard.
2. **A folder.** The launcher takes the best image in it: `.iso` before `.cci`. Second parts (`.2.iso`) never count as images on their own.
3. **Two parts.** For `name.1.iso` (or `.1.cci`), `name.2.iso` is added when it exists.
4. **Mount.** Type 0 for plain images (an NKPatcher-style request). Type `d` with the link `\Device\CdRom0`
   for CCI.
5. **Launch.** The launcher leaves the screen alone (no video mode change of its own, so the title's is the only one; it sets a mode only to show an error). It
   launches `\Device\CdRom0\default.xbe` through the launch data page and a quick reboot. The page names the launcher as the caller with nxdk's default title ID
   0xFFFF0002 (launch data type 0, no data), not the certificate's: a launcher made by oxos_isonator carries the game's certificate, and a game that finds its
   own title ID there with no data takes the launch as a return from its own relaunch (Prince of Persia: The Sands of Time then starts the dashboard;
   2026-10-29, console and xemu).

When something is missing, the launcher shows text instead and says why (the screen holds no text otherwise).

### 7a. Game folders (`tools/oxos_isonator`)
A host tool (Python, standard library) that makes a dashboard game folder per image: the image (an XISO it builds from an extracted game folder, or an existing
image) and a `default.xbe` next to it. That `default.xbe` is the ISO launcher with the game's certificate fields (title ID, title name, alternate title IDs,
region, ratings, disc number, version) and its title image section (`$$XTIMAGE`) copied in, so dashboards list the game itself. Started, it takes the image in
its own folder (step 1). Images are one file unless `--split` (two parts under 4 GB, for FATX). Checked in xemu on OXOS (exFAT, F:) and on another BIOS
with this device (FATX, E:): the launcher mounts the image and the game starts. For a path that does not exist, it lists the nearest folder above it that does.

## 8. Tests
- `make test_iso` boots the release build in xemu with a disc that holds the launcher. The image, of the kernel test title with relaunch.txt, sits in
  `F:\titles` on an exFAT disk, in five forms: one `.iso`; a `.1.iso` + `.2.iso` pair; a `.cci` named through its folder; a `.1.cci` + `.2.cci` pair; and a >4
  GB image whose `media\a.txt` lies beyond 4 GB. The title must start from `\Device\CdRom0\default.xbe`, pass its checks, relaunch itself twice through quick
  reboots (the mount survives), and end in the dashboard. The mount must succeed with no failed image reads.
- `tests/mkcci.py` writes CCI images (a greedy LZ4 compressor), and `tests/mkxiso.py` places `High` files beyond 4 GB in a sparse image.
- Checked by hand: in xemu, Halo 2 (a 4.7 GB XISO on an exFAT disk) reaches its title screen through the launcher. On the 1.4 console (500 GB exFAT SSD, the
  launcher delivered by netboot) the same image mounts and Halo 2 reaches its title screen, with no failed reads.

## Open
- Hard-disk and memory-unit images (types `M`, `H`).
- A menu in the launcher for choosing an image with the controller.
- On exFAT an image's name may have up to 255 characters, all in Latin-1 (spec/oxos_internal/07 §2); on FATX at most 42.
