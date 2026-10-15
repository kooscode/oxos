# Spec 04c — DVD drive, XDVDFS, RAW file system

Evidence: batch_decompile `ev_db951e21…` (DVD init 0x80055d55), `ev_1ba14b9d…` (CdRom0 creation 0x80026457), the
CD-ROM driver object at 0x800123d8, its READ dispatch (0x80025d9a) and the DEVICE_CONTROL code list (objdump at
0x80026289). XDVDFS on-disk format: public documentation, checked against images built by `tests/mkxiso.py`.
**Status: implemented (milestone 3c-3).**

## 1. DVD drive (ATAPI, IDE device 1)
- Init: select the slave (0xB0) and wait for BSY to clear (600 × 50 ms, otherwise fatal error 10). SET FEATURES transfer
  mode PIO 4 then UDMA 2 (fatal 12). IDENTIFY PACKET DEVICE (fatal 11).
- `\Device\CdRom0`: type CD_ROM (2), no extension, flags |= 0x44, sector size 0x800, alignment 1, plus the link
  `\??\CdRom0:`. Driver: StartIo, CREATE/CLOSE succeed, READ, DEVICE_CONTROL; everything else is invalid.
- READ:
  - the buffer must be 2-byte aligned, and the offset and length 2048-aligned
  - the request must lie inside the media length (READ CAPACITY), otherwise STATUS_INVALID_PARAMETER
  - LBA = offset >> 11; queued on the shared IDE channel
- IOCTLs queued to the channel:
  - SCSI pass-through direct 0x4D014
  - READ_TOC 0x24000, 0x24038, 0x2403E
  - GET_DRIVE_GEOMETRY 0x2404C
  - 0x24080 (special handler), 0x24084, CHECK_VERIFY 0x24800 (TEST UNIT READY)
  - IDE pass-through 0x4D028 and the DVD-specific codes
  - others → STATUS_INVALID_DEVICE_REQUEST
- Cold boot, unless boot flag 0x40 is set: CHECK_VERIFY every 200 ms while the drive reports NOT_READY. On
  IO_DEVICE_ERROR, retry at most 5 times.
- Sense mapping (ours): NOT READY + ASC 0x3A → STATUS_NO_MEDIA_IN_DEVICE; NOT READY otherwise →
  STATUS_DEVICE_NOT_READY; UNIT ATTENTION → STATUS_VERIFY_REQUIRED; ILLEGAL REQUEST → INVALID_DEVICE_REQUEST.
- Our transfers: packet commands are polled PIO; READ(10) uses bus-master DMA in 128 KB chunks.

## 2. XDVDFS
- Volume descriptor at sector 32: "MICROSOFT*XBOX*MEDIA" at +0 and +0x7EC, root directory sector +20 and size +24,
  creation time +28.
- Directory = a binary search tree of entries: {u16 left, u16 right (dword offsets, 0 = none), u32 start sector,
  u32 size, u8 attributes, u8 name length, name}, 4-aligned, never crossing a sector; the rest of a sector is 0xFF.
  Ordered by case-insensitive (upper-case) byte compare, prefix first.
- Read-only:
  - a create disposition other than OPEN/OPEN_IF, or write/delete access → STATUS_MEDIA_WRITE_PROTECTED (our
    choice for OPEN_IF of a missing name too)
  - a missing name → NAME_NOT_FOUND; a missing directory on the path → PATH_NOT_FOUND
- Files are contiguous from their start sector. Enumeration is in tree order. Every time is the volume time.
- Mounting: CD_ROM devices try XDVDFS (the reference then tries a second file system — UDF — which is not done yet).

## 3. RAW file system
- Mounted when a whole device is opened and no file system recognises it: always for the RAW_MOUNT_ONLY whole disk
  (Partition0). Only the empty name opens, with disposition OPEN/OPEN_IF and per-volume share access. Reads, writes,
  flush and device controls go to the device unchanged.

## Object manager fix found here
- A trailing backslash after a device name reaches the parse procedure as `\` (root directory); without it the name
  is empty (volume open).
