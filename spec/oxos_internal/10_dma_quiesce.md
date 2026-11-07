# 10. Stopping bus masters left running at kernel entry

OXOS addition. Implemented 2026-10-22 (`HalpQuiesceBusMasters` in `kernel/src/hardware/chipset.cpp`, the first step of
`HalpInitPci`, HAL phase 0).

## 1. Why
- A console with a modchip usually runs the modchip's menu (XeniumOS) before it starts a flash bank. If that start
  is not a full hardware reset, devices the menu used can still be running when the kernel starts. They keep
  reading and writing RAM through their own DMA lists, in memory the kernel then reuses.
- Devices at risk: the USB host controllers (gamepads), the NIC, the audio DMA channels, the IDE bus master and the
  GPU's FIFO.
- On a 1.4 console OXOS hung at changing places (right after the DVD probe, in the dashboard's USB or network
  start-up) depending on the build, with interrupts dead. The RAMDAC colour table also kept the menu's contents.
  xemu starts every device reset.
- `HalpInitPci` itself enables bus mastering on every device (command bits 0-2), without resetting the devices, as
  the reference does on a cold boot from its own 2BL.

## 2. What it does, in order
1. For bus 0 devices 2, 3 (USB), 4 (NIC), 5 (APU), 6 (AC'97), 9 (IDE): log the command register and BAR0, then
   clear the command register's bus-master bit (bit 2).
2. Map 0xFEC00000-0xFEFFFFFF for a moment: page-directory entry 0x3FB through the self-map at 0xC0300000, a 4 MB
   page, uncached. The entry is restored afterwards, with `invlpg` each time.
3. Each USB host controller (OHCI) whose BAR0 is in that window and whose memory decoding is on:
   1. log HcControl and HcHCCA;
   2. HcInterruptDisable = 0xC000007F;
   3. HcControl = 0 (USB reset state, every list off);
   4. HcCommandStatus = 1 (host controller reset), waiting up to 100 µs for it to clear.
4. The NIC, under the same conditions:
   1. log its ring addresses and transmit / receive control;
   2. transmitter (0x84) and receiver (0x94) off, interrupts masked (0x004);
   3. TxRxControl (0x144) 0x14, then 0x04 after 10 µs;
   4. ring addresses (0x100, 0x104) 0.
5. AC'97 (I/O BAR1, when I/O decoding is on): each bus-master channel control register (0x0B, 0x1B, 0x2B, 0x7B)
   written 0 (stop), then 2 (reset the channel's registers).
6. IDE bus master (I/O BAR4): command register 0 (stop).
7. NV2A:
   1. log the PCI command register, PMC 0x200, PFIFO 0x2500 and 0x3220;
   2. PMC 0x200 bits 8 (PFIFO) and 12 (PGRAPH) cleared, then the original value written back, which resets both
      engines.
   3. Frame-buffer tiles and Z compression off (added 2026-11-01): for each tile *i* (0-7) PFB 0x100240 + 16·*i*
      (tile), + 4 (limit) and + 8 (pitch) ← 0 and PFB 0x100300 + 4·*i* (Z compression) ← 0; PFB 0x100324 (Z-compression
      tag offset) ← 0.
      - They power up holding junk. On the 1.4 console a cold boot reads 0x100300 = 0x100304 = 0x04000000 and
        0x100324 = 0x10. The stock kernel's graphics device clears them before any title (spec/stock/22 §7), and
        Direct3D titles set their own. OXOS has no boot animation, so nothing did.
      - nxdk's pbkit (`pb_init`) copies the tile and Z-compression registers into the graphics engine as it finds
        them, so they must hold sane values.
      - **Not sufficient on its own (open).** Apps built with nxdk's pbkit (its samples, XBL Dashboard) still stop at
        pbkit's GPU error screen on a cold boot on hardware: class 0x97 method 0x1D6C (semaphore offset) is rejected
        with "FD invalid operation" in `pb_init`. xemu is not affected.
      - What was measured:
        - The same app works after a Direct3D title has run (quick reboots only).
        - pbkit's own engine reset (PMC 0x200 bit 12 off and on) changes none of the 448 readable PGRAPH registers,
          so the graphics engine keeps its power-on junk.
        - Setting the engine's copies of the memory configuration (PGRAPH 0x4009A0-0x4009A8, RDI select 0xEA
          0x00-0x0C) and every readable register that differs from the after-Direct3D state was not enough.
      - The rest: the graphics engine's live context is power-on junk, which nxdk's pbkit saves over its own context
        image. Spec 24 (HalpInitGraphicsEngine, right after this step) loads a default context at a cold boot.

## 3. Not covered
- The APU's own engines are not reset: no register-level description is available. Its bus-master bit is
  cleared, but the rest of `HalpInitPci` sets it again.
