# 09. Network boot (NETBOOT=1)

OXOS addition, not reference behaviour. A development aid: a console flashed once with a netboot image boots kernels
served by a PC (`tools/netboot/netboot.py`, user guide in `tools/netboot/README.md`), so testing a build on hardware
needs a power-cycle instead of a reflash. Implemented 2026-10-22.

## 1. Where it runs
- In the open image's 2BL (`kernel/boot/netboot.h`, included by `boot/bl.cpp` when built with `NETBOOT=1`), after
  the SMC challenge and the chipset set-up (`BlSetUpChipset`, which enables the NIC: spec/stock/21), before the
  kernel is unpacked.
- Paging off, interrupts off, polled; physical addresses throughout.
- Time comes from the MCPX ACPI PM timer (I/O 0x8008, 3.375 MHz). If it does not advance over about 1 ms of port
  cycles, netboot is skipped.
- Built by `make netboot` (`BUILD=build/netboot NETBOOT=1 NETLOG=1 SELFTEST=0`): the 2BL with netboot and a
  network-log kernel as the flash fallback.

## 2. NIC use
- PCI device 4: BAR0 = 0xFEF00000, BAR1 = 0xE001 (what the kernel's `HalpInitPci` assigns), command |= I/O, memory,
  bus master.
- Set-up as the kernel's network log (spec/oxos-internal/03 §7). The differences:
  - a receive ring of 32 descriptors;
  - packet filter 0x7F0020 (always + own address; broadcasts are received);
  - receiver started (0x094 = 1);
  - MAC registers in nxdk byte order (A = bytes 0–3, B = bytes 4–5).
- MAC address 02:58:42:4F:58:31; IP address `NETLOG_IP` (only as the packets' source).
- Receive descriptors: {buffer, length, flags}. Handed to the NIC with length 2048 and flags bit 15. A received frame
  has bit 15 clear and bit 0 (valid) set; error bits 7–12 drop it. The descriptor is given back and TxRxControl bit 1
  written (nxdk's nvnetdrv, MIT).
- Link: up to 4 s for MII status bits 2 and 5 (link, auto-negotiation done). Speed and duplex come from the
  advertised & partner abilities (MII registers 4 & 5).
- Before the kernel starts, the NIC is stopped:
  - receiver and transmitter off;
  - TxRxControl reset;
  - ring addresses 0, adapter control 0, interrupts masked and acknowledged.

  So nothing DMAs into memory the kernel reuses. The kernel's own drivers set it up again.

## 3. Memory (physical)
| Range | Use |
|---|---|
| 0x00300000 | 32 receive descriptors, then 2 transmit descriptors |
| 0x00301000–0x00310FFF | receive buffers (32 × 2 KiB) |
| 0x00320000–0x00320FFF | transmit frames (2 × 2 KiB) |
| 0x00800000–0x00BFFFFF | the downloaded image (one 256 KiB bank, up to 1 MiB with a large kernel, up to 4 MiB with a title, §7) |

All of it is below the 2BL (0x400000) or above the unpacked kernel. The kernel treats all of it as free memory.

## 4. Protocol
- UDP port 5141 both ways. The console sends broadcasts (to 255.255.255.255) from `NETLOG_IP`. The PC replies with
  broadcasts, since the console answers no ARP.
- Every message starts with a 20-byte little-endian header: magic `'OXNB'` (0x424E584F), op (byte), version (byte,
  1), reserved (16 bits), session, offset, length (32 bits each).
- The session is a random value per power-on (PM timer). Replies for another session are ignored.

| Op | From | Offset / Length | Data |
|---|---|---|---|
| 1 HELLO | console | 0 / 0 | the console's MAC address (6 bytes) |
| 2 OFFER | PC | CRC-32 of the image / its size (0x40000 to 0x100000, a multiple of 1 KiB) | none |
| 3 READ | console | byte offset / 1024 | none |
| 4 DATA | PC | the same offset / 1024 | the 1024 bytes |
| 5 BOOT | console | 0 = booting the download, 1 = rejected, booting flash / 0 | none |
| 6 MCPX | console (`MCPX_DUMP=1` images only) | 0 / 512 | the MCPX boot ROM, sent three times before the first HELLO (spec 12) |

1. HELLO every 250 ms, 8 times, until an OFFER arrives.
2. READ the 256 chunks in order. Each request is repeated after 100 ms without a matching DATA, 30 times at most.
   A chunk that never arrives means a flash boot.
3. Check the CRC-32 (IEEE, as zlib) of the whole image and the payload magic `'OXKP'` at 0x6000. Then send BOOT.
4. Unpack the kernel and copy the key block from the downloaded image, as from flash (spec/stock/01 §5). The 2BL
   in flash stays in charge; the downloaded image's 2BL, X-codes and reset code are not used.

## 5. What the kernel sees
- The same entry, page tables and key block as a flash boot. It cannot tell the difference, with one exception: its
  flash bank holds another kernel.
- **Boot splash:** the kernel reads the splash through the flash bank's payload header (spec/oxos-internal/00), so a
  netbooted kernel shows the splash in flash.
- **Quick reboot:** `KernelEntry` copies the initialised part of `.data` (DOS-header fields 0x2C bytes from 0x34)
  into a 4 KiB buffer in `.persist` (`KiDataInitCopy`) before anything writes `.data`. `KiQuickReboot` restores from
  that copy (`KiDataInitSource`), and from the flash copy (field 0x30) only if the copy did not fit. This applies to
  every build, so quick reboots no longer depend on the flash holding the same kernel.

## 6. LED (spec/oxos-internal/03 §6)
- 0xFF (solid orange) while netboot waits for the link and the network.
- Then 0xAA (blinking orange) again for the unpacking.

## Images larger than one bank (2026-10-23)
A kernel whose packed stream does not fit in 256 KiB (a Microsoft-derived kernel used as a reference, about 0x3B000
bytes packed) is served as one larger image: the same layout as a bank (payload header at 0x6000, key block, kernel
stream), the stream simply running past 0x40000. The PC offers the file's size (a multiple of 1 KiB, at most 1 MiB;
`netboot.py` sends the first bank only when the kernel stream ends inside it, so ordinary 1 MiB `bios.bin` images
still go out as 256 KiB). Loaders flashed before this change accept only 0x40000 and boot from flash otherwise.

## 7. A title delivered with the kernel (2026-10-25)
For hardware-in-the-loop tests: `netboot.py --title DIR [--title-name NAME]` (default name `imgtest`) also sends a
title, and the kernel installs it on the hard disk and runs it instead of the disc or the dashboard. There is no
RAM disc: the title runs from the disk, as any title launched from it.
- **Image:** the image as usual (the first bank, or the whole large image), padded to a 4 KiB boundary, then the
  package. At bank offset 0x5FE0: 'OXTI' (0x4954584F), the package's offset in the image, its size. The download
  limit is 4 MiB (loaders flashed before accept at most 1 MiB).
- **Package:** {'OXTP', version 1, count, name[16]}, then per file {name[64], offset, size} (offsets from the
  package's start, each file 16-byte aligned), then the files; padded to a multiple of 1 KiB. DIR's `default.xbe`
  is stored as NAME.xbe; the other files keep their names.
- **Kernel entry** (cold boots only, `xe/nettitle.cpp`): with 'OXKP' at bank 0x6000 and the marker present, note
  the package's physical range and clear the marker, so a quick reboot does not find it again.
- **Memory manager start-up:** the package's pages are held (mapped, counted as contiguous memory), not put on the
  free lists, and skipped by the page tag test.
- **Title launch** (path B of spec/stock/05 §2, before the disc): create `E:\NAME\`, write each file, give the
  pages back, link D: to the folder, load `E:\NAME\NAME.xbe`. This happens whatever the dashboard would be asked to
  do: a console without a clock backup always has a reason (the clock is not set). If writing or loading fails,
  the usual disc / dashboard selection follows. Each step is on the log ("netboot title: ...").
