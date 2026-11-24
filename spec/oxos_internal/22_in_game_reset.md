# 22 — In-game reset (IGR), replaced by in-game shortcuts (IGS)

**2026-11-23 (user): the in-game reset becomes in-game shortcuts (IGS):** an extension, `oxos_igs.xos`, with its shortcuts in
`C:\OXOS\extensions\oxos_igs.ini`, on the pads' state of §9; its own spec (a later one) links back here. §1-8 describe the in-game reset as it was built
(2026-10-29 to 2026-11-06) and stay as its record; §9 is the pads' state both use.

OXOS addition, implemented 2026-10-29, removed 2026-11-23. Code (until then): `kernel/src/in_game_reset/`: `xinput_hook.S` (`KeRaiseIrqlToDpcLevel`, `HalpIgrReturn`), `in_game_reset.cpp`
(`HalpIgrCheck`, `in_game_reset::Reboot`). Background: rmenhal's in-game reset in NKPatcher (2004, GPL-2.0; §2), and how EvoX M8 does it (spec/evox/04 §2).

Holding **LT + RT + BACK + BLACK** together on any controller, in any XDK title, reboots the console. The reboot is a cold one, a power cycle through the SMC
(§6), so:
- a netbooted console asks the PC for its image again and starts what it serves (the title launcher);
- a flashed console boots as from power-on (disc, else the dashboard, spec 19);
- a mounted disc image (spec 16) is dropped.

## 1. Where the buttons come from
Titles read their controllers with XAPI's `XInputGetState(hDevice, pState)`, which is linked into each title, so the kernel never sees the USB traffic. In the 7
titles checked (among them Halo 2 and Brute Force) the function starts the same way, with exactly one match
per XBE:

```
push ebx; push esi; xor ebx, ebx
call [KeRaiseIrqlToDpcLevel]
mov edx, [esp+0Ch]            8B 54 24 0C      (hDevice)
mov ecx, [edx+0A3h]           8B 8A A3 00 00 00   (Halo: cmp byte [edx+0A3h], 1 -> 80 BA A3 00 00 00)
...
mov eax, status; pop esi; pop ebx; ret 8
```

It returns 0 (ERROR_SUCCESS) when the state was copied. `pState` is XINPUT_STATE: packet number (4 bytes), then the gamepad: digital buttons (u16 at +4: up
0x01, down 0x02, left 0x04, right 0x08, START 0x10, BACK 0x20, left thumb 0x40, right thumb 0x80) and eight analog bytes at +6 (A, B, X, Y, BLACK, WHITE, LT,
RT).

## 2. The hook
`KeRaiseIrqlToDpcLevel` (export 129) checks its return address, as rmenhal's NKPatcher does (credit below):
- below 0x80000000 (title code);
- the dword there, byte 1 masked, is `8B ?? 24 0C` (mov reg, [esp+0Ch]);
- the dword at +6 is 0x000000A3.

On a match it moves the six stack dwords (its own return, saved esi, saved ebx, the title's return address, hDevice, pState) 12 bytes down and stores
`HalpIgrReturn` in place of the title's return address. XInputGetState's `ret 8` then lands in `HalpIgrReturn` with the original return address and both
arguments still above it. With status 0 it calls `HalpIgrCheck(pState)` and returns 0 to the title through `ret 8`. Every caller, matched or not, then gets the
normal `KfRaiseIrql(DISPATCH_LEVEL)`. No title code is changed, so the hook holds for any title built with this XAPI code, whatever its load address.

**Credit:** this hook is rmenhal's in-game reset from NKPatcher (`nkpatcher.asm`, "In-Game Reset", Copyright 2004 rmenhal, GPL-2.0, the same licence as OXOS; a
public copy is in [Rocky5/Xbox-Softmodding-Tool](https://github.com/Rocky5/Xbox-Softmodding-Tool), `App Sources/NKPatcher/Main NKP11/`). The caller test, the
signature and its constants, the six dwords moved 12 bytes down and the return redirect are the same; the button check (§3) and the reboot (§6) differ.

## 3. The combo
`HalpIgrCheck` builds a 16-bit mask: bits 0–7 the low byte of the digital buttons, bit 8 + i analog button i when it reads 0xFF (fully down). The reboot fires
when 0xD020 (LT 0x4000, RT 0x8000, BACK 0x20, BLACK 0x1000) is all set: both triggers + BACK + BLACK, one of rmenhal's NKPatcher combos (2004; also EvoX M8,
spec/evox/04 §2). NKPatcher counts BLACK as held when it is non-zero; OXOS wants 0xFF.

Unlike the full reboot to the dashboard of rmenhal's NKPatcher, nothing is written to the SMC scratch register first: they write
0x0C, and scratch bit 0x08 makes the next cold boot start the dashboard (spec/stock/12, boot flag 0x80), not the netbooted title.

`HalpIgrCheck` runs at the caller's IRQL after XInputGetState has lowered it again (passive for game threads). The reboot (§6) uses the polled SMBus, which
works at any IRQL.

## 4. Limits
- Only XDK titles: nxdk titles (the OXOS launchers, the FTP server) have their own USB code and are not covered. §9 proposes reading the buttons from the
  USB host controllers instead, which covers every title.
- A title with a different XInputGetState prologue is not recognised (no such title seen so far). The serial log says `igr: the title reads its controllers
  (XInputGetState)` the first time the hook sees a title's controller read, so a title that never prints it is not covered.
- With TRACE_IMPORTS builds (spec 01) the tracer's thunk is the caller, so the hook does not match.

## 5. Tests
- xemu: Halo from its game folder: the hook line on serial, the main menu at 75 s (the return path works).
- 1.4 console (Modxo, netboot): Halo from its folder; the combo pressed in game: `igr: LT RT BACK BLACK: reboot`, `HalReturnToFirmware(1)`, cold boot 2 s later,
  back to what netboot serves.

## 6. The reboot: a power cycle (2026-11-04)
`in_game_reset::Reboot` writes SMC register 0x02 ← 0x40 (the SMC's power cycle, documented by Xbox-Linux on xboxdevwiki's "PIC" page), retrying the SMBus write
up to five times; if the console is still running two seconds later, `HalReturnToFirmware(1)`.
- **Why not `HalReturnToFirmware(1)`** (until 2026-11-03): that is the reference reset, SMC 0x02 ← 1 and then port 0xCF9 ← 0x0E. Pressed while a test title was
  running, the next boot found 29 of the X-codes' GPU and memory settings at their power-on values (the memory interface, the DRAM controller, pads, the GPU
  clock, DRAM refresh; spec 26 §5): the RAM was sized as 128 MB with the wrong layout and the kernel stopped after the memory manager (the front LED blinking
  green). From the dashboard the same reset worked 18 times out of 18, so it is the GPU being in use when the reset comes. Port 0xCF9 alone does not reset this
  console at all (the kernel waits for ever).
- A power cycle takes the GPU's power away, so the next boot is a cold boot like any other, which the 2BL's checks cover (spec 26).
- **Debug build option** `make REBOOT_TEST=N`: N seconds into every boot (also inside a title started by a quick reboot) the kernel calls
  `in_game_reset::Reboot`, so the reboot can be tested in a loop without a controller.
- **Tests (1.4 console, Modxo, netboot, REBOOT_TEST=30):** from the dashboard, 4 power cycles, each a cold boot back into the dashboard about 3 s later; with a
  test title running (netbooted launcher, the reboot 30 s into the game), 10 power cycles: every next boot a normal cold boot (64 MB, 0x11448000; 3-9 X-code
  settings differing at 2BL start, the 2BL's usual cases), the test title loaded and running again each time.

## 7. Ordinary reboots: the SMC's reset alone (2026-11-06)
`HalReturnToFirmware(1)` and the other non-quick routines ask the SMC to reset the console (register 0x02 ← 1, polled, up to five tries, at any IRQL), wait up
to two seconds, and only then write port 0xCF9 ← 0x0E. The reference writes 0xCF9 right after the SMC request (and skips the SMC at DISPATCH_LEVEL and above).
The chipset then restarts and runs the X-codes at once, while the SMC's reset arrives later over the SMBus.
- **Measured** (FTP server's `SITE REBOOT` in a loop): with the reference order 4 of 40 warm reboots lost every X-code setting; with the SMC alone 1 of 38. That
  one reset came within 0.1 s of the request, so the remaining cause was the GPU leaving reset late. The X-codes now wait for it (spec 27); with both, 30 of 30
  and the 100-boot run of spec 27 §3.

## 8. Proposal: a quick-reboot in-game reset (2026-11-06; not implemented)
Status: design only. The power cycle of §6 stays until the tests of §8.6 pass.

### 8.1 What a quick reboot to the dashboard is
- The runtime's `XLaunchNewImage(NULL, NULL)` ("go to the dashboard"), linked into every XDK title: it makes sure a launch data page exists
  (`MmAllocateContiguousMemory(0x1000)`, `MmPersistContiguousMemory(page, 0x1000, TRUE)`), clears it, writes the launch data type and an empty launch path, and
  calls `HalReturnToFirmware(2)`. Seen in a community title-update installer whose menu's "restart" item starts a 53 KB XBE that does only this: the console is
  back in the dashboard in a moment, without the BIOS, the splash or the drives' start-up. Every title that exits to the dashboard takes the same path.
- In OXOS (`kernel/src/hardware/chipset/smbus.cpp`), `HalReturnToFirmware(2)` runs the shutdown notifications the title registered
  (`HalRegisterShutdownNotification`; its graphics and sound libraries stop their hardware), then `KiQuickReboot`: `KiQuickRebootQuiesce` masks both PICs, stops
  the IDE bus master, resets both USB host controllers into USBRESET (spec 25 §5), turns the GPU's interrupts off and the NIC's transmitter and receiver off
  (spec 25 §7); the kernel re-enters from its in-memory image with `.data` restored from its quick-reboot copy and `.persist` kept (spec/stock/02 §1, spec 18
  §4). HAL phase 0 then only reprograms the interrupt controller and stops the video overlay (spec 25 §4); the cold boot's bus-master stop (spec 10) does not
  run. Title selection then takes branch A of spec/stock/05 §2: an empty launch path starts the dashboard (spec 19), and type
  0xFFFFFFFF frees the page first (no launch data for the dashboard).
- rmenhal's NKPatcher in-game reset (to the dashboard, quick reboot) does the same: page allocated and persisted if missing, cleared, first dword
  0xFFFFFFFF, `HalReturnToFirmware(2)`. EvoX M8's in-game reset does the same reboot to the dashboard (0x8001542D with 2, spec/evox/04 §2).

### 8.2 Why the in-game reset is a power cycle today
- 2026-10-29: in the FTP server (an nxdk title), BACK as "exit to the dashboard" (a quick reboot) sometimes left the dashboard hung after it loaded; `SITE DASH`
  (the same reboot without a pad) reached it 4 of 4 times. The user decided BACK = cold reboot. Suspected: the USB host controllers' state across the quick
  reboot. On 2026-11-04 the quick reboot started putting both controllers back into USBRESET (spec 25 §5), the state a title expects; the failing case was not
  retried since.
- 2026-11-03/04: the reset (`HalReturnToFirmware(1)`) pressed while a title was drawing lost the X-codes' GPU and memory set-up at the next boot (§6). That is a
  hardware-reset problem; a quick reboot does not reset the hardware and does not run the X-codes, so it is not affected. It was fixed with the power cycle (§6)
  and the X-code checks (spec 27).

### 8.3 Proposed behaviour
| Combo (mask bits, §3) | Mask | Action |
|---|---|---|
| LT + RT + BACK + BLACK | 0xD020 | quick reboot to the dashboard (§8.4) |
| LT + RT + BACK + WHITE | 0xE020 | the power cycle of §6, for a title that leaves the hardware in a state a quick reboot cannot recover from |

Differences from today's reset, for the user:
- Faster: no BIOS, splash or drive spin-up; the dashboard is up in a few seconds.
- A netbooted console stays on the kernel it is running and goes to the dashboard: it does not ask the PC again, and a netbooted title (spec 09 §7) does not
  start again (its package was used at the cold boot).
- A mounted disc image is unmounted (§8.4 step 2), as with the power cycle.

### 8.4 The quick in-game reset, step by step
(With §9 the steps run on a kernel thread of OXOS's own instead of in the title's thread; step 3 is answered there.)
`HalpIgrCheck` runs in the title's thread that called `XInputGetState`, at that caller's IRQL (§3):
1. **IRQL.** Below DISPATCH_LEVEL (game threads read pads at passive level): go on. At DISPATCH_LEVEL or above (a title reading its pads from a DPC): use the
   power cycle instead, because the shutdown notifications and the unmount may wait.
2. **Disc images.** Clear the persisted slot table of spec 16 §5 (`storage::SavedVirtualDiscs`), so the restarted kernel re-attaches no image and the dashboard
   finds the real drive (or no disc). The live slots are not unmounted: the title's other threads may be in the middle of reads, and the quick reboot ends them
   anyway.
3. **Other threads.** Suspend every other thread of the title's process, so none of them touches the hardware or the disk while the reboot runs. (Open:
   whether `KeSuspendThread` on each is enough, or the threads must be held at raised IRQL.)
4. **Launch data.** Make sure the launch data page exists (allocate and persist one page if not), clear it and set its type to 0xFFFFFFFF with an empty path, as
   `XLaunchNewImage(NULL, NULL)` and rmenhal's NKPatcher in-game reset do.
5. **Reboot.** `HalReturnToFirmware(2)`, which runs the title's shutdown notifications and `KiQuickReboot` (§8.1).
6. **Serial log:** `igr: LT RT BACK BLACK: quick reboot to the dashboard` (and `igr: ... WHITE: power cycle`).

### 8.5 Risks
- **GPU busy.** The quick reboot does not reset the GPU. The title's graphics library idles it in its shutdown notification; if the title hangs the GPU or has
  no notification, the next title's graphics start-up resets the graphics engine and FIFO (spec 24), as after any title switch. Untested with a title killed in
  the middle of a frame.
- **Audio.** The AC'97 bus master is stopped at kernel entry (spec 10); the APU (the title's sound processor) is left to the title's sound library's shutdown
  notification and the dashboard's own start-up.
- **Disk.** A write in flight (a save, the cache partitions) is cut as with the power cycle; FATX and exFAT metadata may be left half-written. The IDE bus
  master is stopped; the drive may still be busy with a command, which the restarted kernel's first IDE access waits out.
- **Pads.** The 2026-10-29 dashboard hang (§8.2) was never explained; the USB change since is the likely fix.
- **Title not covered.** A title whose shutdown notification hangs or crashes never reaches `KiQuickReboot`; the WHITE combo (power cycle) is then the way out.

### 8.6 Tests before it replaces the power cycle
- A debug build option like `REBOOT_TEST` (§6) that runs the quick in-game reset N seconds into every title, to loop without a controller.
- On the 1.4 console (Modxo, serial log), 20 cycles each:
  - the test title that hung the hardware reset on 2026-11-03 (GPU busy at the reset);
  - a game started from a disc image (the next dashboard shows no disc; the image's slot is gone from the log);
  - the dashboard itself, and a title with music playing (audio stops, the dashboard's sound works);
  - the 2026-10-29 case: the FTP server's BACK as a quick reboot to the dashboard (an nxdk title, outside the hook, so through a test build of the FTP server),
    with a pad plugged in.
- Checks after each: the dashboard reached and its pads, sound and network working; the dashboard's free memory equal to a cold boot's (spec 18 §7); no
  `interrupt storm` or exception lines; the next title launched from the dashboard runs.
- Then the second console (spinning disk, DVD drive), and xemu for the dashboard path.

### 8.7 Open questions
- The exact launch data `XLaunchNewImage(NULL, NULL)` writes (type and flags): log it from the installer's "restart" item on the console (the serial log's
  `firmware: launch data type ...` line) and match it.
- Titles that read their pads at DISPATCH_LEVEL: none seen; §8.4 step 1 falls back to the power cycle for them.
- Whether to add a third combo that quick-reboots back into the disc's title (rmenhal's NKPatcher triggers + START + BLACK).

## 9. Buttons from the USB host controllers, for every title (2026-11-23, implemented)
Status: implemented 2026-11-23 (`kernel/src/hardware/usb/pad_reports.cpp`; the launch, `titles::SwitchTitle` in `kernel/src/titles/title_loader.cpp`),
designed from a pilot on the Conexant debug kit (2026-11-23, a throwaway build, not committed). The user asked for every title to be covered, however it was
built (XDK, nxdk or another toolkit). It replaced §1-2 (the `XInputGetState` hook, removed) as where the buttons come from, and runs §8's quick reboot from
a kernel thread.

### 9.1 Why
The hook of §2 finds `XInputGetState` in the XDK's input library, so titles built another way (nxdk dashboards and apps, homebrew; titles built against
other libraries) are not covered (§4). Every title, however it is built, reads its pads the same way in the end: through the console's two OHCI USB host
controllers, which put each finished transfer on a "done" list in memory and raise their interrupt. OXOS dispatches that interrupt before the title's own
handler runs (`KiDispatchHardwareInterrupt` calls the title's interrupt routine). There the kernel can read the pads' reports
without touching the title's code or the controller.

### 9.2 Reading the reports
At each interrupt of the first controller (IRQ 1, registers at 0xFED00000) and of the second (IRQ 9, 0xFED08000), before the title's interrupt routine:
1. `HcInterruptStatus` (+0x0C) bit 1 (WritebackDoneHead) set: the controller has written a done list. Otherwise nothing to read.
2. `HcHCCA` (+0x18) gives the physical address of the controller's communication area; its `DoneHead` (+0x84, low 4 bits are flags) is the physical address
   of the last finished transfer descriptor.
3. Follow the list (each general transfer descriptor: flags with the completion code in bits 28-31, the current buffer pointer, the next descriptor, the
   buffer end), at most 32 entries. For a descriptor finished without error, read its data: a pad's input report is 20 bytes starting `00 14`; byte 2 holds
   the digital buttons, bytes 4-11 the analog buttons A, B, X, Y, BLACK, WHITE, LT, RT (pressed past half: more than 0x80), as §3's mask.
4. Only reads: nothing is written to the controller or to the list, so the title's own handler then finds everything as the controller left it.

**Physical reads.** The descriptors and buffers are wherever the title's USB code put them, often in the title's own memory, which is not mapped at the
kernel's 0x80000000 window (only the kernel's own allocations are). The kernel reads them through one reserved page (taken with `MmMapIoSpace` when the title
starts): for each read, interrupts off, the page's PTE pointed at the wanted physical page, `invlpg`, the bytes copied, the PTE put back. In the pilot a first
version read through 0x80000000 and stopped the kernel (bug check 0x1E in the interrupt dispatch) as soon as a game (its descriptors in its own memory) read a
pad; the reserved page fixed it.

**The report's start.** A finished descriptor's buffer pointer is cleared (or, after a short transfer, points just past the data); its buffer end stays. The
pilot took the start as the end minus 19 (or the pointer minus 20) and checked the `00 14` header; for one game the computed address was wrong although the
header and the buttons matched. The real version takes the start from the endpoint and buffer the title set up (the descriptor's place in the endpoint's
list), or keeps the header check as the guard. Built (2026-11-23): the start as the pilot's, the `00 14` header as the guard.

**Telling pads apart.** A finished descriptor names no device or endpoint, so the kernel knows a pad by the physical address of its report buffer (4 slots,
the oldest reused). XDK titles keep one buffer per pad. nxdk titles fill a new buffer for every report (xemu, 2026-11-23: one pad cycled through more than
16 buffers, 0x3F64000-0x3FDC000), so their one pad moves from slot to slot. Hence:
| Rule | Why |
|---|---|
| A slot's first report counts as a change only when its buttons differ from the last report's (any slot), and those last buttons are kept across quick reboots (`XK_PERSIST`) | a pad that only moved to a new buffer makes no change event, and buttons still held when the next title starts are no change (an in-game shortcut held into the title it started does not fire again) |
| The last resort (spec 48 §5): exactly LT + RT + BACK + BLACK, held on one slot for 3 s, or over every report for 3 s | the per-slot timer started over at every new buffer, and LT + RT + BACK + BLACK never fired over an nxdk title; over every report it works with one pad (a second pad's reports without the combination start it over) |
| `OXOS_PAD_STATE::Port` is the slot | the same pad in XDK titles; in nxdk titles it changes from report to report |
| Reports with d-pad up and down, or left and right, at once are dropped | a title asking for the pad's capabilities (`XInputGetCapabilities`) gets an answer in the report's own layout and header, every supported button's bits set (`00 14 FF 00 FF ...`), on the same done list; read as buttons it held every one, and the in-game shortcuts reset a game while the user pressed A (console, 2026-11-23). Shortcuts and the last resort also match exactly their buttons, no others |
| The in-game shortcuts keep one state over all pads (spec 48 §3) | per port, a press arriving on a port that had not been seen counted as that port's first state and fired nothing (xemu, the controller test title) |

### 9.3 The reset, on a kernel thread
The interrupt only sets a flag (it runs at the controller's interrupt level). A kernel thread of OXOS's own, waiting at a high priority, does the rest:
1. **Stop the title.** Suspend every thread of the title (`KeSuspendThread`): a title thread is one whose start address is in title code (below 0x80000000),
   plus the title's main thread (started by the title launcher). Then wait 50 ms so each takes the suspend at its next safe point. In the pilot, a quick
   reboot started from a kernel thread without this let a title's own code run on during the reboot and write to memory already released (bug check 0x1E in the
   title, 10 ms into the reboot); with the threads suspended it never happened again (an nxdk dashboard with 10 threads, a game with 2).
2. **Launch data** as §8.4 step 4 (or a launch page naming another XBE to start), then `HalReturnToFirmware(2)`. The title's shutdown
   notifications run on this thread, with its threads suspended.
3. **Disc images** as §8.4 step 2.

### 9.4 Pilot results (Conexant debug kit, serial build, 2026-11-23)
| Running | Button source | Combo seen | Result |
|---|---|---|---|
| an nxdk dashboard | done list | yes (3 s after its start, the first report) | 10 threads suspended, the next XBE loaded 0.47 s after the press |
| an nxdk app (the trainer menu, the FTP server) | done list | yes | reports seen from their first frames |
| a retail game (its USB buffers in its own memory) | done list, through the reserved page | yes | 2 threads suspended, the next XBE loaded 0.47 s after the press |
| a retail game, `XInputGetState` hook (§2) instead | the hook | yes, but only once the game read its pads (after its intro) | the next XBE loaded 0.4 s after the press |

### 9.5 What changes for the in-game reset
- Every title is covered, however it was built; `xinput_hook.S` and the prologue match of §2 are no longer needed (they can stay as a fallback until §9 is
  proven on all three consoles).
- The combos of §3 and §8.3 are read from the reports the same way (the mask of §3 is built from the report's bytes).
- The reset runs on the kernel thread (§9.3), at passive level, whatever IRQL the title reads its pads at: §8.4 step 1's DPC case disappears.
- §8.6's tests apply, plus: nxdk titles (the FTP server, a dashboard), a title using both controllers (IRQ 1 and IRQ 9), and a title that reads its pads at a
  high rate (the cost of the snoop per interrupt).
