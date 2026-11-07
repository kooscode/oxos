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
- **Sense handling (reference 0x8002530b; REQUEST SENSE after a failed command, at most 4 retries per request,
  retry counter 0x8003ad3d, limit 0x8003ad3e):**
  - NO SENSE: retried (ILI set → STATUS_INVALID_BLOCK_LENGTH 0xC0000173).
  - RECOVERED ERROR: success (ILI → 0xC0000173).
  - NOT READY: ASC 0x04 (becoming ready) → STATUS_DEVICE_NOT_READY, **retried after 100 ms** (a timer DPC restarts
    the request); ASC 0x30 → STATUS_UNRECOGNIZED_MEDIA; ASC 0x3A → STATUS_NO_MEDIA_IN_DEVICE, not retried.
  - MEDIUM ERROR: ASC 0x30 → UNRECOGNIZED_MEDIA; otherwise STATUS_DEVICE_DATA_ERROR (0xC000009C).
  - ILLEGAL REQUEST: ASC 0x21 → STATUS_NONEXISTENT_SECTOR; 0x30 → UNRECOGNIZED_MEDIA; 0x6F/0x80 →
    STATUS_TOO_MANY_SECRETS (copy protection); otherwise INVALID_DEVICE_REQUEST.
  - **UNIT ATTENTION (the disc changed): retried at once**; STATUS_IO_DEVICE_ERROR once the retries are used up.
    Callers never see it while a retry is left.
  - Other keys: STATUS_IO_DEVICE_ERROR.
- Ours: UNIT ATTENTION and NOT READY/0x04 retried as above (polled packet commands stall 100 ms; DMA reads use a
  timer DPC); the other keys map as before without the finer ASC distinctions.
- **Disc change in xemu:** after a disc is loaded, xemu's drive answers the first command with NOT READY/0x3A
  ("removed") and the next with UNIT ATTENTION; only then do commands work. The reference's XDVDFS mount sends the
  authentication MODE SENSE first and ignores its result, so that command takes the "removed" answer and the
  geometry request's UNIT ATTENTION is retried. Ours sends the MODE SENSE to the drive too, even when it emulates
  the security page (the drive's answer is dropped). Without that, the geometry request got "no media", the mount
  fell back to RAW, and the MS dashboard reported an inserted disc as unrecognised until a reset.
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
- Read (reference dispatch 0x8002b430): dismounted volume → STATUS_VOLUME_DISMOUNTED; directory →
  STATUS_INVALID_DEVICE_REQUEST; length 0 → success with 0 bytes; offset at or past the end of the file →
  STATUS_END_OF_FILE; otherwise the length is clipped to the end of the file. Non-cached reads go to the device
  with the transfer rounded up to whole sectors and the clipped length reported: **the length need not be a
  multiple of 2048** (Halo 2 reads its map files' tails with lengths such as 0x27600 and shows "disc dirty or
  damaged" when that fails). Ours checks only the offset's sector alignment for non-cached reads, and only after
  the end-of-file test: after a non-cached read of a file's tail the position is the unaligned file size, and the
  next read must return STATUS_END_OF_FILE (Brute Force copies its .tgz archives to Z: until it sees it; an invalid
  parameter there ended its copy threads and left it on "Loading" for ever).
- **Mounting (reference 0x800183a3):** CD_ROM devices try **XDVDFS first** (0x8002aca0), then UDF (0x8002c0dc, spec
  04d) only when XDVDFS returns STATUS_UNRECOGNIZED_VOLUME; any other failure, with a whole-device open, mounts
  RAW. (Earlier versions of this spec had the order reversed.)
- XDVDFS mount: authenticate (IOCTL 0x24080, result ignored), drive geometry (0x2404C; failure fails the mount),
  then the volume descriptor at sector 32. No TEST UNIT READY.

## 3. RAW file system
- Mounted when a whole device is opened and no file system recognises it: always for the RAW_MOUNT_ONLY whole disk
  (Partition0). Only the empty name opens, with disposition OPEN/OPEN_IF and per-volume share access. Reads, writes,
  flush and device controls go to the device unchanged.

## Object manager fix found here
- A trailing backslash after a device name reaches the parse procedure as `\` (root directory); without it the name
  is empty (volume open).
