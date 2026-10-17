# Spec 16 — Volume dismount, Fsc cache control, Ethernet PHY

Evidence: batch_decompile `ev_b6f63e48…` (FscGetCacheSize, FscInvalidateIdleBlocks, FscSetCacheSize,
IoDismountVolume, IoDismountVolumeByName, PhyGetLinkState, PhyInitialize) and `ev_45147092…` (MII read/write, link
state, link wait). **Status: implemented (milestone 5d-1).**

## Dismount
- **IoDismountVolume(device):**
  - nothing mounted → STATUS_VOLUME_DISMOUNTED (0xC000026E)
  - the volume driver has no DriverDismountVolume → STATUS_INVALID_DEVICE_REQUEST
  - otherwise reference the volume device, call DriverDismountVolume(volume), dereference it
- IoDismountVolumeByName opens the device object by name, then dismounts.
- **Ours:** FATX, XDVDFS and RAW mark the volume dismounted and detach it from the target (MountedOrSelfDevice =
  NULL, so the next open mounts again), then delete the volume device. Its DriverDeleteDevice frees the volume
  state and drops the target reference when the last file object goes.

## Fsc (file system cache; milestone 13)
Evidence: `ev_f30ee26f…` (0x8001ceca FscGetCacheSize, 0x8001d09e FscInvalidateIdleBlocks, 0x8001d2b2 FscSetCacheSize),
`ev_3cf013b7…` (0x8001cd34 init, 0x8001cd7b list rebuild, 0x8001cdd6 grow, 0x8001ced0 lookup, 0x8001cf28 element
from PFN, 0x8001cf5b cached-or-direct decision, 0x8001cfc9 invalidate range, 0x8001d117 shrink), `ev_5ddf102a…`
(0x8001d318 take idle, 0x8001d379 set writable, 0x8001d3b2 map empty, 0x8001d452 unmap, 0x8001d4ba discard,
0x8001d52c fill, 0x8001d6a8 map, 0x8001d8c2 write, 0x8001d9b9 direct read, 0x8001daba cached read), objdump of the
boot call (0x8001e96a) and the XDVDFS caller (0x8002b4e0–0x8002b52a). Names are ours (inferred from behaviour).

**Reference behaviour.**
- **Structure (observed):** element array in pool (tag 'FscA'), 0x14 bytes each: {block number = byte offset >> 12,
  cache extension (the volume's {device, partition length, sector size}; 0 = empty), buffer VA | low byte = map count
  | 0x100 read in progress | 0x200 read waiters | 0x400 being discarded, LIST_ENTRY}. One list (head 0x8003AB90):
  empty elements first, then least to most recently used; lookups walk from the most recent end and stop at the first
  empty element; a hit moves to the tail. Count at 0x8003AB98, array at 0x8003AB9C.
- **Pages (observed):** system-PTE pages of usage 8 (file cache, counted in MmQueryStatistics.CachePagesCommitted and
  the per-usage counts, i.e. against the title's budget), mapped **read-only**; a writer's map makes the PTE writable
  until unmap. The page database entry holds the element index in bits 17–27 (used to find the element from the VA
  and to move the page). At boot the kernel reserves 8 system PTEs for a fill window and calls FscSetCacheSize(16):
  16 pages from the start; the dashboard raises it to 256 (measured).
- **Eviction (observed):** a miss takes the first element in the list with map count 0 (empty ones first, then least
  recently used); if every buffer is mapped the thread waits (event 0x800360a4) until one is unmapped. A buffer
  being read is marked; other threads asking for it wait for the read (event 0x80036094). FscSetCacheSize: > 0x800
  invalid; growing allocates the new pages and a bigger array; shrinking marks idle buffers for discard (waiting for
  mapped ones), frees their pages and compacts the array. FscInvalidateIdleBlocks empties every unmapped buffer;
  dismount empties a volume's unmapped buffers.
- **Reads (observed):** a miss reads the whole 4 KB block, or up to the partition end for the last block. Cached
  read (XDVDFS, UDF) of a file: if the file is opened for random access, or the offset or buffer is not aligned to
  the device's alignment requirement, everything is copied out of cache buffers. Otherwise reads of less than a page,
  or without a whole page after the start is rounded up, are copied from the cache; larger ones read the unaligned
  head through the cache, the whole pages directly into the caller's buffer, and the short tail through the cache.
  When the caller's length covers the file's end rounded up to a sector (0x8002b4e0), the direct part is rounded up
  to whole sectors (the device fills the buffer to the sector's end). Non-cached opens bypass the cache (spec/04c).
- FATX maps FAT and directory blocks (map, modify, write the covering sectors from the buffer, unmap); file data in
  whole sectors goes straight to the device.

**Ours.** `src/io/fsc.cpp` has the same structure, page accounting (16 cache pages at boot, usage 8, read-only
mapping, element index in the page database, relocatable), eviction order, size changes, read-or-direct decision
and sector rounding; FATX, XDVDFS (file data and directory entries) and UDF (file data) use it. Differences: a
passive-level lock (KEVENT) instead of DISPATCH_LEVEL; a buffer turns read-only when its last map goes (the
reference: at every unmap); invalidation also takes mapped buffers out of lookups, because our FATX keeps up to 12
blocks mapped while it holds its lock (the reference unmaps at once); UDF descriptors (file entries) are still read
directly. Until milestone 13 only FATX had a cache (its own, allocated on first use, counted as system memory) and
XDVDFS read every directory entry and partial sector from the drive.

## PHY (NIC MMIO 0xFEF00000)
- **MII registers:** control +0x190 (`register | 0x20` for PHY 1; 0x400 = write; 0x8000 = busy), data +0x194. Busy
  waits are 50 µs steps up to 8 ms.
- **Link state:** 1 link up (status register bit 2), 2 = 100 Mbit, 4 = 10 Mbit (advertised & partner 0x180 /
  0x60), 8 = full duplex, 0x10 = half duplex (0x140 / 0xA0).
- **PhyInitialize(forceReset, …):**
  1. A call already in progress → 0x800700AA.
  2. If already initialized and not forced: refresh the link state.
  3. Otherwise reset (control 0x8000) and wait for it to clear (1000 × 500 µs).
  4. Wait for auto-negotiation to complete (status 0x20, 6000 × 500 µs).
  5. With auto-negotiation: wait for link and compute the state. Without it: force 100/10 Mbit from the status
     abilities.
  6. Failure → 0x801F0001.
  The reference also clears bit 8 of vendor register 0x18 except on one hardware revision (not done).
- **PhyGetLinkState(verify):** refresh when unknown or asked to verify, then return the state.

## FSCTL_DISMOUNT_VOLUME (0x90020) (0x8002859c FATX, 0x8002aef5 RAW/XDVDFS; milestone 10)
- FATX: FILE_SYSTEM_CONTROL with 0x90020 runs the volume dismount; 0x9411C and 0x98120 go to another routine
  (0x800284bc, not reproduced); anything else STATUS_INVALID_DEVICE_REQUEST. Information = 0.
- RAW / XDVDFS (and our UDF): 0x90020 calls the driver's DriverDismountVolume; anything else invalid.
- Why it matters: XAPI formats a utility drive (X/Y/Z) by writing new FATX metadata through the volume handle and
  then dismounting; without the dismount the old mount's free-space view stays and the title sees DISK_FULL
  (Halo failed to size its map cache files and stalled on a black screen).
