# Spec 04d — UDF file system (xboxkrnl 1.0.4627.1)

Clean-room behavioural spec. Evidence: batch_decompile of 0x8002c0dc (mount), 0x8002ba64 (volume recognition),
0x8002bb81 (anchor), 0x8002bd6b (VDS), 0x8002bbee (PVD), 0x8002bc3b (partition), 0x8002bcb0 (LVD), 0x8002bec9 (file
set), 0x8002c49b (tag check); the recognition ID table at 0x80012538. Structure offsets are ECMA-167 / OSTA UDF.

## 1. Mount (CD-ROM devices: UDF is tried first, then XDVDFS)
1. Drive geometry (IOCTL 0x2404C) gives the sector count.
2. **Volume recognition** from sector 16 to the end: each sector's 5-character identifier must be one of BEA01 (1),
   TEA01 (2), CDROM (3), CD001 (4), CDW01 (5), CDW02 (6), NSR01 (7), NSR02 (8), BOOT2 (9), else unrecognized.
   - Structure type 0: BEA01 → only once, version 1; **NSR02** → success if a BEA01 came first and version is 1;
     TEA01 → unrecognized; other identifiers are skipped.
   - Structure type ≠ 0 (ISO 9660 descriptors): allowed only before BEA01, with type 1, 2 or 0xFF and identifier
     CDROM or CD001.
   (NSR03, i.e. UDF 2.x, is therefore not mounted.)
3. **Tag check** (everywhere): descriptor version 2, the expected tag identifier, tag location = the block number
   (partition-relative inside the partition). Checksum and CRC are not verified.
4. **AVDP** at sector 256 → the main volume descriptor sequence extent.
5. **VDS** (length non-zero, multiple of 2048): for each descriptor type the one with the highest sequence number
   counts:
   - PVD: volume sequence number < 2, character set list = max = 1, both charspecs = OSTA CS0
   - Partition Descriptor: contents "+NSR02"; gives the partition start and length, and its number
   - LVD: block size 2048, domain "*OSTA UDF Compliant" with revision 0x100–0x150, OSTA CS0, map table fits the
     block, < 2 maps, map table length > 5, map 0 = type 1, length 6, volume sequence < 2; its partition number and
     the FSD extent (logical volume contents use)
   - Terminating descriptor: success if all three were good and the partition numbers agree.
6. **File set**: the FSD extent (recorded, non-zero, block multiple) is scanned in the partition; descriptors must
   be FSDs (the highest file set number gives the root directory ICB, a "next extent" is followed) and the scan
   must end at a terminating descriptor inside the extent.
7. The root directory is opened (its File Entry read).

## 2. Our implementation (milestone 8, `src/udf/udf.cpp`)
- Read-only, like XDVDFS: create/open (no write access, no creation), read (cached and non-cached), query
  information, directory enumeration (FileDirectoryInformation, wildcards, on-disc order), volume queries ("UDF").
- File Entries (tag 261): short_ad, long_ad (single partition) and embedded data; unrecorded extents read as zeros;
  at most 32 extents and 4 GB per file; extended allocation descriptor lists are rejected.
- FIDs (tag 257): deleted and parent entries are not listed; OSTA CS0 names: 8-bit as is, 16-bit characters above
  0xFF become '_'. Names compare case-insensitively.
- Times come from the File Entry's modification time; attributes are read-only (+ directory).
- Not covered by the RE pass (our choices): directory size limit 1 MB, EntryOffset = FID offset as FileIndex.
- Test: `tests/mkudf.py` builds a UDF 1.02 image; `make test-udf` boots with it and checks a fragmented long_ad file,
  short_ad, embedded data, a 16-bit name, enumeration order and errors.

## Read (reference dispatch 0x8002ce74)
As XDVDFS (spec 04c §2): dismounted → STATUS_VOLUME_DISMOUNTED; directory → STATUS_INVALID_DEVICE_REQUEST; length 0 →
success; at or past the end → STATUS_END_OF_FILE; the length is clipped to the file, and non-cached reads go to the
device rounded up to whole 2048-byte sectors with the clipped length reported, so their length need not be whole
sectors. Ours checks only the offset's alignment for non-cached reads, after the end-of-file test. Scatter/gather
reads are not supported on UDF (the reference allows them; deviation).
