# Cerbios spec 07: virtual disc images (ISO, CCI, HDD and memory-unit images)

Studied in Cerbios 3.1.0 Beta: the kernel's virtual image driver (device control, read path, mount and unmount) and
the extension's device creation at boot. Behaviour and formats only.

## 1. Devices
- At every boot the extension creates the object directory `\Device\Virtual0` and the **control device
  `\Device\Virtual0\Image0`** (slot 0), with the links `\Device\CdRom1` and `\??\CdRom1:` pointing to it.
- Up to **two images** can be mounted, in **slots 1 and 2**; a mounted image appears as `\Device\Virtual0\Image<n>`.
- The slot table lives in kernel data that is cleared only on a cold boot, so a mount **survives quick reboots**:
  at each boot the extension reopens the image files of every slot that is in use and not marked for unmount, and
  recreates the device and links (§3). A cold boot (power cycle) forgets all mounts.

## 2. Device control codes
Sent with NtDeviceIoControlFile to the control device or to an image device. Two equivalent code families:

| Code | Alias | Action |
|---|---|---|
| 0x0CE52B00 | 0x1EE7CD00 | none, returns success (presence check) |
| 0x0CE52B01 | 0x1EE7CD01 | **mount** (§2.1): on the control device into the first free slot, on an image device into that slot |
| 0x0CE52B02 | 0x1EE7CD02 | **unmount**: on the control device all slots, on an image device its slot |

Image devices also answer the usual queries: for DVD images the CD-ROM codes 0x24000, 0x24038, 0x2404C, 0x24080,
0x24800 (media checks and geometry); for disk images 0x70000 (geometry), 0x70014 and 0x74004 (partition info). Other
codes fail with STATUS_INVALID_DEVICE_REQUEST.

### 2.1 Mount request (input buffer)
| Offset | Size | Field |
|---|---|---|
| 0x00 | 1 | number of image files (1 or 2: an image may be split in two parts) |
| 0x01 | 1 | image type (below) |
| 0x04 + 8 × i | 8 | file i path, counted ANSI string {u16 length, u16 maximum, pointer} (i = 0, 1) |
| 0x44 | 8 | link name to create for the image, counted ANSI string (≤ 0x106 characters) |

Image types:

| Type | Meaning | Sector size | Device type |
|---|---|---|---|
| `D` | DVD image (plain ISO) | 2048 | CD-ROM |
| `d` | DVD image, **CCI** compressed (§4) | 2048 | CD-ROM |
| 0 | DVD image (plain), but the link name is forced to `\Device\CdRom0` (the existing one is deleted), replacing the drive's device name; stored as `D` | 2048 | CD-ROM |
| `M` | memory-unit image | 4096 | disk |
| other (stored as `H`) | hard-disk image | 512 | disk |

- Mounting fails with STATUS_INVALID_DEVICE_REQUEST (0xC0000010) when no slot is free, the chosen slot is in use,
  a file cannot be opened or (CCI) its header is invalid.
- For DVD types (`D`, `d`, 0) the existing `\??\CdRom0:` link is deleted first; once the image device exists,
  `\??\CdRom0:` is linked to it, so titles and the dashboard read the image as the DVD drive. A "virtual DVD
  mounted" flag is set; the SMC tray logic and the DVD device creation honour it (spec 05).
- Each part's size comes from the file's size information; the parts are concatenated in order (part 0's sectors
  first). The image size is the sum.
- **Full disc dumps:** for a plain image whose first part is larger than 0x18310800 bytes, the volume descriptor is
  checked at byte offset **0x18310000** (start of the game partition at 0x18300000 plus sector 32): if both the
  first and the last 20 bytes of that 2048-byte sector are `MICROSOFT*XBOX*MEDIA`, the image is marked as a full
  dump and **every read is offset by 0x18300000 bytes**, so the device presents only the game partition.

### 2.2 Unmount
The slot is marked "unmount pending"; for DVD types the "virtual DVD mounted" flag is cleared and `\??\CdRom0:` is
deleted; the image's own link is deleted. The device stays until the next boot, which skips pending slots.

## 3. Boot-time re-creation
For each slot in use: both file handles are reopened (shared read), then the device `\Device\Virtual0\Image<n>` is
created (CD-ROM type for `D`/`d`, disk otherwise; sector size by type), the stored link name is created, and for
`D`/`d` also `\??\CdRom0:`. Slots not in use are cleared.

## 4. CCI format (read path)
- **Header** (at offset 0 of each part, 32 bytes, little-endian):

  | Offset | Size | Field |
  |---|---|---|
  | 0x00 | 4 | magic `CCIM` |
  | 0x04 | 4 | header size, must be 0x20 |
  | 0x08 | 8 | uncompressed size in bytes |
  | 0x10 | 8 | offset of the block index |
  | 0x18 | 4 | block size, must be 0x800 |
  | 0x1C | 1 | version, must be 1 |
  | 0x1D | 1 | index alignment, must be 2 |
- **Index:** one u32 per 2048-byte block, plus one more at the end. Entry × 4 (32-bit arithmetic, so bits 30–31
  drop out) = the block's file offset; the code also extracts bit 31 but does not use it. The block's stored length
  is (next entry × 4) − (this entry × 4).
- **Block:** if the stored length is exactly 0x800, the block is stored raw. Otherwise its first byte is a padding
  count p, and the following (length − 1 − p) bytes are an **LZ4 block** (token: high nibble literal length, low
  nibble match length − 4, both extended by 255-runs; 2-byte little-endian match offset) that decodes to 2048 bytes.
- Reads are served two blocks at a time through a shared buffer under a lock; plain images are read with direct
  sub-requests to the image files.

## For OXOS (notes, not Cerbios behaviour)
- The mount request carries user pointers (paths); a clean design copies and validates them under the caller's
  mode.
- Keeping the slot table in memory that survives a quick reboot is what lets a dashboard mount, then relaunch.

## Open
- Replies to CD-ROM queries for images, now traced: READ_TOC and GET_LAST_SESSION return 12 constant bytes
  (`00 12 01 01 00 14 01 00 00 00 02 00`, one data track); GET_DRIVE_GEOMETRY returns the image size in sectors,
  media type 0xB, 1 track, 1 sector, 2048 bytes; CHECK_VERIFY and AUTHENTICATE_DISK succeed. Everything else
  fails, including DVD_READ_STRUCTURE and SCSI pass-through. Reads past the end, and writes, fail with
  STATUS_INVALID_PARAMETER; reads on an unmounted slot fail with STATUS_NO_MEDIA_IN_DEVICE.
- What writers put in bit 31 of a CCI index entry (the read path ignores it).
