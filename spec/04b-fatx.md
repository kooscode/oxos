# Spec 04b — FATX file system (stock behaviour)

On-disk format (publicly documented; confirmed against the test image with `tests/fatx.py`) plus the
driver behaviour taken from the reference FATX driver. Evidence: batch_decompile `ev_484d7415…` (FatxMountVolume,
RAW mount), `ev_0590f4f8…` (volume device creation, boot-sector processing), `ev_c176be65…` (FatxFsdCreate), and the
FATX driver object at 0x80012470. **Status: implemented (milestone 3c-2).**

## 1. On-disk format
- **Superblock** (partition offset 0, 4 KB reserved): 'FATX', serial number, sectors per cluster, root directory
  cluster (1). Layout and cluster arithmetic: spec/04 §6. Data cluster N (N ≥ 1) sits at data start + (N−1) × cluster size.
- **FAT:**
  - entries are 16-bit (FATX16, fewer than 0xFFF0 clusters) or 32-bit
  - 0 = free; ≥ 0xFFF8 (sign-extended for 16-bit entries) = end of chain; 0xFFF7 = bad
- **Directory entry** (64 bytes):
  - FileNameLength — 0x00 or 0xFF ends the directory; 0xE5 marks a deleted entry
  - Attributes (READONLY 1, HIDDEN 2, SYSTEM 4, DIRECTORY 0x10, ARCHIVE 0x20)
  - Name[42], with unused bytes 0xFF
  - FirstCluster, FileSize (0 for directories)
  - Creation/LastWrite/LastAccess timestamps
- **Directories** are cluster chains of entries, at most 4096 per directory. A new directory gets one cluster filled
  with 0xFF.
- **Timestamps:** high word = date (day, month << 5, (year − 2000) << 9), low word = time (seconds / 2,
  minute << 5, hour << 11).
- **Names:**
  - 1–42 characters from 0x20–0x7E
  - none of `"*/:<>?\|`; "." and ".." are invalid
  - case-insensitive compare, case preserved
  - whole path ≤ 250 characters

## 2. Driver behaviour
- Volume device: FILE_DEVICE_DISK_FILE_SYSTEM (8), extension = volume control block. StackSize = target + 1;
  alignment = max(own, target); sector size from the drive geometry. It holds a reference on the partition device.
  Free clusters are counted at mount.
- **Create:**
  - dismounted volume → STATUS_VOLUME_DISMOUNTED
  - FILE_OPEN_BY_FILE_ID → STATUS_NOT_IMPLEMENTED
  - allocation size above 4 GB → STATUS_INVALID_PARAMETER
- **Opening the volume itself** (empty name, no related file):
  - only FILE_OPEN / FILE_OPEN_IF are allowed (otherwise ACCESS_DENIED)
  - FILE_DIRECTORY_FILE → NOT_A_DIRECTORY; DELETE_ON_CLOSE → CANNOT_DELETE
  - shared volume share-access; Information = FILE_OPENED
- **Path walk:**
  - an invalid element → OBJECT_NAME_INVALID
  - a missing or non-directory intermediate → OBJECT_PATH_NOT_FOUND
  - a missing final element:
    - DELETE_PENDING if the parent is being deleted
    - NAME_NOT_FOUND for OPEN / OVERWRITE
    - otherwise create the entry (a directory if FILE_DIRECTORY_FILE) → FILE_CREATED
  - an existing final element:
    - file + FILE_CREATE → NAME_COLLISION
    - file + trailing `\` or FILE_DIRECTORY_FILE → NOT_A_DIRECTORY
    - directory with anything but OPEN/OPEN_IF → NAME_COLLISION
    - directory + NON_DIRECTORY_FILE → FILE_IS_A_DIRECTORY
- **Access and sharing on open:**
  - read-only attribute + write or delete access → ACCESS_DENIED
  - share access is checked per file; SUPERSEDE / OVERWRITE(_IF) truncate → FILE_OVERWRITTEN (or FILE_SUPERSEDED)
  - DELETE_ON_CLOSE marks a file delete-pending
- **Read / write:**
  - a read at or past EOF → END_OF_FILE; reads stop at EOF
  - writes extend the cluster chain; FILE_WRITE_TO_END_OF_FILE appends; above 4 GB → DISK_FULL
  - unbuffered handles need sector-aligned offsets and lengths
  - volume handles pass reads and writes to the partition
- **Information:**
  - Query: Basic, Standard, Internal, Ea, Position, Name, NetworkOpen, All
  - Set: Basic, Disposition (directories must be empty → DIRECTORY_NOT_EMPTY; read-only → CANNOT_DELETE),
    Position, EndOfFile, Allocation, Rename (ReplaceIfExists; no directory targets)
- **Directory query:**
  - FileDirectoryInformation and FileNamesInformation, ANSI names
  - mask with `*` and `?` (default `*`), RestartScan
  - first empty result → NO_SUCH_FILE; afterwards → NO_MORE_FILES
- **Volume information:** Volume (serial number), Size / FullSize (clusters, sectors per cluster, bytes per sector),
  Device, Attribute ("FATX", 42-character components).
- **Delete:** at the last cleanup of a delete-pending file, the entry is marked 0xE5 and the chain is freed.

## Implementation notes
- Our driver uses a 32 × 4 KB write-through block cache for the FAT and directories. Sector-aligned data runs go
  straight to the disk. One lock per volume.
- Not done yet: scatter/gather I/O on files, the Fsc* cache exports, dismount, and the RAW file system.
