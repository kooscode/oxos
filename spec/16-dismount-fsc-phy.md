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

## Fsc
- FscGetCacheSize returns the cache size in pages.
- FscSetCacheSize(pages): more than 0x800 → STATUS_INVALID_PARAMETER; otherwise grow or shrink.
- FscInvalidateIdleBlocks moves idle buffers to the free list.
- Ours: our file systems keep their own small write-through caches, so the size is only recorded.

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
