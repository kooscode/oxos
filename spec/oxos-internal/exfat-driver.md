# exFAT driver

Status: phases 1-3 of the plan work (GPT/MBR, read-write exFAT, folder partitions with bounded sizes; `make test-exfat`
passes); cache-partition formatting through the volume handle (storage spec §5) is next. The storage design (layout, virtual partitions, cache formatting, size reporting) is
`exfat-storage-spec.md`; this document is the driver: the on-disk format it reads and writes, and how it fits the
kernel. The format facts come from Microsoft's published exFAT specification (2019) and the UEFI specification
(GPT); nothing here comes from another implementation's code.

## 1. On-disk format (the parts the driver uses)
All values little-endian. A *sector* is 2^BytesPerSectorShift bytes, a *cluster* 2^(BytesPerSectorShift +
SectorsPerClusterShift) bytes.

### 1.1 Boot region
Sectors 0–11 of the volume are the main boot region, 12–23 its backup:

| Sector | Content |
|---|---|
| 0 | boot sector (below) |
| 1–8 | extended boot sectors: each ends with the dword 0xAA550000 |
| 9 | OEM parameters (zero) |
| 10 | reserved (zero) |
| 11 | boot checksum: the 32-bit checksum of sectors 0–10, repeated to fill the sector |

Boot sector fields: 0 `EB 76 90`; 3 `EXFAT   `; 11–63 zero; 64 PartitionOffset (u64, sectors); 72 VolumeLength
(u64, sectors); 80 FatOffset (u32, sectors); 84 FatLength (u32, sectors); 88 ClusterHeapOffset (u32, sectors); 92
ClusterCount (u32); 96 FirstClusterOfRootDirectory (u32); 100 VolumeSerialNumber (u32); 104 FileSystemRevision
(u16, 0x0100); 106 VolumeFlags (u16: bit 0 active FAT, bit 1 volume dirty, bit 2 media failure); 108
BytesPerSectorShift (u8, 9–12); 109 SectorsPerClusterShift (u8; the two shifts add up to at most 25); 110
NumberOfFats (u8, 1 or 2); 111 DriveSelect (0x80); 112 PercentInUse (u8, 0xFF = unknown); 510 0xAA55.

**Boot checksum:** over every byte of sectors 0–10 except bytes 106, 107 (VolumeFlags) and 112 (PercentInUse):
`sum = (sum >> 1 | sum << 31) + byte` (32-bit rotate right, then add).

### 1.2 FAT and cluster heap
- The FAT holds one u32 per cluster: entries 0 and 1 are 0xFFFFFFF8 and 0xFFFFFFFF; for cluster c (2 …
  ClusterCount + 1) the entry is the next cluster of its chain, 0xFFFFFFFF at the end, 0xFFFFFFF7 for a bad cluster.
- Cluster c starts at sector ClusterHeapOffset + (c − 2) × sectors per cluster.
- The FAT is only meaningful for chains: a file marked **NoFatChain** occupies consecutive clusters and its FAT
  entries are not read (and need not be written). Free space is tracked by the allocation bitmap, not the FAT.

### 1.3 Directory entries (32 bytes)
Byte 0 is the entry type: bit 7 in use, bit 6 secondary, bit 5 benign, bits 0–4 the type code. 0x00 ends the
directory (no in-use entries follow); a type with bit 7 clear is a deleted entry.

| Type | Entry | Fields |
|---|---|---|
| 0x81 | allocation bitmap (root only) | 1 BitmapFlags; 20 FirstCluster; 24 DataLength (u64) |
| 0x82 | up-case table (root only) | 4 TableChecksum; 20 FirstCluster; 24 DataLength |
| 0x83 | volume label (root only) | 1 CharacterCount; 2 label (11 UTF-16) |
| 0x85 | file (primary of a file's entry set) | 1 SecondaryCount; 2 SetChecksum (u16); 4 FileAttributes (u16); 8 CreateTimestamp; 12 LastModifiedTimestamp; 16 LastAccessedTimestamp; 20 Create10msIncrement; 21 LastModified10msIncrement; 22–24 UTC offsets |
| 0xC0 | stream extension (first secondary) | 1 GeneralSecondaryFlags (bit 0 allocation possible, bit 1 NoFatChain); 3 NameLength; 4 NameHash (u16); 8 ValidDataLength (u64); 20 FirstCluster; 24 DataLength (u64) |
| 0xC1 | file name (following secondaries) | 1 GeneralSecondaryFlags; 2 up to 15 UTF-16 characters |

- A file's **entry set** is the file entry, the stream extension and ceil(NameLength / 15) name entries, in
  consecutive slots of its directory. Other benign secondaries may follow and are kept.
- **SetChecksum:** over all bytes of the set except bytes 2–3 of the file entry: `sum = (sum >> 1 | sum << 15) +
  byte` (16-bit).
- **NameHash:** over the up-cased name, each UTF-16 character as its low byte then its high byte, with the same
  16-bit formula.
- **Directories** are files with attribute 0x10; their DataLength is a multiple of the cluster size and equals
  ValidDataLength. The root directory has no entry set: its size comes from its FAT chain.
- **Attributes:** 0x01 read-only, 0x02 hidden, 0x04 system, 0x10 directory, 0x20 archive (the FATX values).
- **Timestamps:** bits 0–4 seconds / 2, 5–10 minute, 11–15 hour, 16–20 day, 21–24 month, 25–31 year − 1980; the
  10 ms increment adds 0–199 × 10 ms; a UTC offset byte with bit 7 clear means "local time".

### 1.4 Allocation bitmap and up-case table
- **Bitmap:** bit (c − 2) of the bitmap (byte (c − 2) / 8, bit (c − 2) % 8) is set when cluster c is in use.
- **Up-case table:** u16 values, the up-case of character i at position i; the value 0xFFFF followed by a count n
  means "the next n characters map to themselves". TableChecksum is the 32-bit boot-checksum formula over its bytes.

### 1.5 Partition tables
- **GPT:** sector 0 a protective MBR (an entry of type 0xEE, 0x55AA); sector 1 the header: 0 `EFI PART`; 8
  revision; 12 header size (92); 16 header CRC32 (computed with this field zeroed); 24 its own LBA; 32 the backup
  header's LBA; 40 first usable LBA; 48 last usable LBA; 56 disk GUID; 72 entry array LBA; 80 entry count; 84 entry
  size (128); 88 entry array CRC32. Entry: 0 type GUID; 16 unique GUID; 32 first LBA; 40 last LBA; 48 attributes;
  56 UTF-16 name. The basic-data type GUID is EBD0A0A2-B9E5-4433-87C0-68B6B72699C7 (stored as A2 A0 D0 EB E5 B9 33
  44 87 C0 68 B6 B7 26 99 C7). CRC32 is the standard reflected CRC-32 (polynomial 0xEDB88320).
- **MBR:** four 16-byte entries at byte 446: 4 type; 8 first LBA (u32); 12 sector count (u32); 0x55AA at 510.

## 2. Driver design
- **One dispatcher.** The FATX request dispatcher (create, cleanup, close, read/write, information, directories,
  volume queries, dismount) serves both formats. It already defines the behaviour titles rely on; the on-disk
  details sit behind the volume and directory layers, which have a FATX and an exFAT implementation chosen per
  volume.
- **Neutral entries.** The layers exchange a 64-byte entry (name up to 42 characters, attributes, first cluster,
  size, FATX-style timestamps) so the dispatcher stays unchanged; exFAT-only state (entry-set length, NoFatChain)
  lives in the FCB.
- **Names:** an exFAT name is visible to titles when it is 1–42 characters, all in Latin-1 and valid by the FATX
  rules; others are skipped when enumerating and are not found by lookups (they stay on the disk untouched).
  Comparisons up-case through the volume's up-case table for characters below 0x100.
- **Sizes:** files up to 4 GB − 1 for now (the dispatcher's FATX limit); larger files on the disk are skipped.
- **Clusters:** the dispatcher numbers clusters as the format does; the layer maps a cluster to its byte offset.
  Allocation prefers consecutive clusters; a NoFatChain file that cannot grow in place gets its FAT chain written
  and the flag cleared.
- **Shared state:** all virtual partitions of an exFAT disk share one exFAT state (geometry, up-case table, free
  count, allocation hint) and one block cache key (the exFAT partition), under the FATX layer's lock.

## 3. Tests
- `tests/exfat.py` builds GPT disks with an exFAT volume from host folders (the writer) and checks a volume
  completely (the checker: boot checksums, entry-set checksums, name hashes, FAT chains, the bitmap against every
  allocation, free count).
- `make test-exfat` boots the self-test BIOS on such a disk and checks what the kernel reads and writes, then runs
  the checker on the disk afterwards.
