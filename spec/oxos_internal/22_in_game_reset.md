# 22 — In-game reset (IGR)

OXOS addition, implemented 2026-10-29. Code: `kernel/src/arch/in_game_reset.S` (`KeRaiseIrqlToDpcLevel`, `HalpIgrReturn`),
`kernel/src/hardware/in_game_reset.cpp` (`HalpIgrCheck`, `HalpIgrReboot`). Background: how Cerbios and EvoX do it (spec/cerbios/08, spec/evox/04 §2).

Holding **LT + RT + BACK + BLACK** together on any controller, in any XDK title, reboots the console. The reboot is a
cold one, a power cycle through the SMC (§6), so:
- a netbooted console asks the PC for its image again and starts what it serves (the title launcher);
- a flashed console boots as from power-on (disc, else the dashboard, spec 19);
- a mounted disc image (spec 16) is dropped.

## 1. Where the buttons come from
Titles read their controllers with XAPI's `XInputGetState(hDevice, pState)`, which is linked into each title, so the
kernel never sees the USB traffic. In the 7 titles checked (Halo, Halo 2, Brute Force, Demo Disc, Enter the Matrix,
Half-Life 2, Prince of Persia) the function starts the same way, with exactly one match per XBE:

```
push ebx; push esi; xor ebx, ebx
call [KeRaiseIrqlToDpcLevel]
mov edx, [esp+0Ch]            8B 54 24 0C      (hDevice)
mov ecx, [edx+0A3h]           8B 8A A3 00 00 00   (Halo: cmp byte [edx+0A3h], 1 -> 80 BA A3 00 00 00)
...
mov eax, status; pop esi; pop ebx; ret 8
```

It returns 0 (ERROR_SUCCESS) when the state was copied. `pState` is XINPUT_STATE: packet number (4 bytes), then the
gamepad: digital buttons (u16 at +4: up 0x01, down 0x02, left 0x04, right 0x08, START 0x10, BACK 0x20, left thumb
0x40, right thumb 0x80) and eight analog bytes at +6 (A, B, X, Y, BLACK, WHITE, LT, RT).

## 2. The hook
`KeRaiseIrqlToDpcLevel` (export 129) checks its return address, as Cerbios's does (spec/cerbios/08 §1):
- below 0x80000000 (title code);
- the dword there, byte 1 masked, is `8B ?? 24 0C` (mov reg, [esp+0Ch]);
- the dword at +6 is 0x000000A3.

On a match it moves the six stack dwords (its own return, saved esi, saved ebx, the title's return address, hDevice,
pState) 12 bytes down and stores `HalpIgrReturn` in place of the title's return address. XInputGetState's `ret 8` then
lands in `HalpIgrReturn` with the original return address and both arguments still above it. With status 0 it calls
`HalpIgrCheck(pState)` and returns 0 to the title through `ret 8`. Every caller, matched or not, then gets the normal
`KfRaiseIrql(DISPATCH_LEVEL)`. No title code is changed, so the hook holds for any title built with this XAPI code,
whatever its load address.

## 3. The combo
`HalpIgrCheck` builds Cerbios's 16-bit mask (spec/cerbios/08 §2): bits 0–7 the low byte of the digital buttons,
bit 8 + i analog button i when it reads 0xFF (fully down). The reboot fires when 0xD020 (LT 0x4000, RT 0x8000,
BACK 0x20, BLACK 0x1000) is all set. That is Cerbios's IGRFULL default.

Unlike Cerbios's IGRFULL, nothing is written to the SMC scratch register first: Cerbios writes 0x0C, and scratch bit
0x08 makes the next cold boot start the dashboard (spec/stock/12, boot flag 0x80), not the netbooted title.

`HalpIgrCheck` runs at the caller's IRQL after XInputGetState has lowered it again (passive for game threads). The
reboot (§6) uses the polled SMBus, which works at any IRQL.

## 4. Limits
- Only XDK titles: nxdk titles (the OXOS launchers, the FTP server) have their own USB code and are not covered.
- A title with a different XInputGetState prologue is not recognised (no such title seen so far). The serial log
  says `igr: the title reads its controllers (XInputGetState)` the first time the hook sees a title's controller
  read, so a title that never prints it is not covered.
- With TRACE_IMPORTS builds (spec 01) the tracer's thunk is the caller, so the hook does not match.

## 5. Tests
- xemu: Halo from its game folder: the hook line on serial, the main menu at 75 s (the return path works).
- 1.4 console (Modxo, netboot): Halo from its folder; the combo pressed in game: `igr: LT RT BACK BLACK: reboot`,
  `HalReturnToFirmware(1)`, cold boot 2 s later, back to what netboot serves.

## 6. The reboot: a power cycle (2026-11-04)
`HalpIgrReboot` writes SMC register 0x02 ← 0x40 (power cycle, Cerbios's IGRCYCLE action, spec/cerbios/08), retrying
the SMBus write up to five times; if the console is still running two seconds later, `HalReturnToFirmware(1)`.
- **Why not `HalReturnToFirmware(1)`** (until 2026-11-03): that is the reference reset, SMC 0x02 ← 1 and then port
  0xCF9 ← 0x0E. Pressed while a test title was running, the next boot found 29 of the X-codes' GPU and memory
  settings at their power-on values (the memory interface, the DRAM controller, pads, the GPU clock, DRAM refresh;
  spec 26 §5): the RAM was sized as 128 MB with the wrong layout and the kernel stopped after the memory manager (the
  front LED blinking green). From the dashboard the same reset worked 18 times out of 18, so it is the GPU being in use
  when the reset comes. Port 0xCF9 alone does not reset this console at all (the kernel waits for ever).
- A power cycle takes the GPU's power away, so the next boot is a cold boot like any other, which the 2BL's checks
  cover (spec 26).
- **Debug build option** `make REBOOT_TEST=N`: N seconds into every boot (also inside a title started by a quick
  reboot) the kernel calls `HalpIgrReboot`, so the reboot can be tested in a loop without a controller.
- **Tests (1.4 console, Modxo, netboot, REBOOT_TEST=30):** from the dashboard, 4 power cycles, each a cold boot back
  into the dashboard about 3 s later; with a test title running (netbooted launcher, the reboot 30 s into the
  game), 10 power cycles: every next boot a normal cold boot (64 MB, 0x11448000; 3-9 X-code settings differing at 2BL
  start, the 2BL's usual cases), the test title loaded and running again each time.

## 7. Ordinary reboots: the SMC's reset alone (2026-11-06)
`HalReturnToFirmware(1)` and the other non-quick routines ask the SMC to reset the console (register 0x02 ← 1, polled,
up to five tries, at any IRQL), wait up to two seconds, and only then write port 0xCF9 ← 0x0E. The reference writes
0xCF9 right after the SMC request (and skips the SMC at DISPATCH_LEVEL and above). The chipset then restarts and runs
the X-codes at once, while the SMC's reset arrives later over the SMBus.
- **Measured** (FTP server's `SITE REBOOT` in a loop): with the reference order 4 of 40 warm reboots lost every X-code
  setting; with the SMC alone 1 of 38. That one reset came within 0.1 s of the request, so the remaining cause was the
  GPU leaving reset late. The X-codes now wait for it (spec 27); with both, 30 of 30 and the 100-boot run of spec 27
  §3.

## 8. Proposal: a quick-reboot in-game reset (2026-11-06; not implemented)
Status: design only. The power cycle of §6 stays until the tests of §8.6 pass.

### 8.1 What a quick reboot to the dashboard is
- The runtime's `XLaunchNewImage(NULL, NULL)` ("go to the dashboard"), linked into every XDK title: it makes sure a
  launch data page exists (`MmAllocateContiguousMemory(0x1000)`, `MmPersistContiguousMemory(page, 0x1000, TRUE)`),
  clears it, writes the launch data type and an empty launch path, and calls `HalReturnToFirmware(2)`. Seen in a
  community title-update installer whose menu's "restart" item starts a 53 KB XBE that does only this: the console
  is back in the dashboard in a moment, without the BIOS, the splash or the drives' start-up. Every title that exits
  to the dashboard takes the same path.
- In OXOS (`kernel/src/hardware/smbus.cpp`), `HalReturnToFirmware(2)` runs the shutdown notifications the title
  registered (`HalRegisterShutdownNotification`; its graphics and sound libraries stop their hardware), then
  `KiQuickReboot`: `KiQuickRebootQuiesce` masks both PICs, stops the IDE bus master, resets both USB host controllers
  into USBRESET (spec 25 §5), turns the GPU's interrupts off and the NIC's transmitter off; the kernel re-enters from
  its in-memory image with `.data` restored from its quick-reboot copy and `.persist` kept (spec/stock/02 §1,
  spec 18 §4). HAL phase 0
  stops every bus master again (spec 10). Title selection then takes branch A of spec/stock/05 §2: an empty launch
  path starts the dashboard (spec 19), and type 0xFFFFFFFF frees the page first (no launch data for the dashboard).
- Cerbios's IGRDASH combo (LT RT START BACK) does the same: page cleared, first dword 0xFFFFFFFF, quick reboot, after
  suspending every other thread of the title's process at raised IRQL (spec/cerbios/08 §4). Its IGRGAME combo is a
  quick reboot that restarts the disc's title instead.

### 8.2 Why the in-game reset is a power cycle today
- 2026-10-29: in the FTP server (an nxdk title), BACK as "exit to the dashboard" (a quick reboot) sometimes left the
  dashboard hung after it loaded; `SITE DASH` (the same reboot without a pad) reached it 4 of 4 times. The user
  decided BACK = cold reboot. Suspected: the USB host controllers' state across the quick reboot. On 2026-11-04 the
  quick reboot started putting both controllers back into USBRESET (spec 25 §5), the state a title expects; the
  failing case was not retried since.
- 2026-11-03/04: the reset (`HalReturnToFirmware(1)`) pressed while a title was drawing lost the X-codes' GPU and memory
  set-up at the next boot (§6). That is a hardware-reset problem; a quick reboot does not reset the hardware and does
  not run the X-codes, so it is not affected. It was fixed with the power cycle (§6) and the X-code checks (spec 27).

### 8.3 Proposed behaviour
| Combo (Cerbios mask bits, §3) | Mask | Action |
|---|---|---|
| LT + RT + BACK + BLACK | 0xD020 | quick reboot to the dashboard (§8.4) |
| LT + RT + BACK + WHITE | 0xE020 | the power cycle of §6, for a title that leaves the hardware in a state a quick reboot cannot recover from |

Differences from today's reset, for the user:
- Faster: no BIOS, splash, jingle or drive spin-up; the dashboard is up in a few seconds.
- A netbooted console stays on the kernel it is running and goes to the dashboard: it does not ask the PC again,
  and a netbooted title (spec 09 §7) does not start again (its package was used at the cold boot).
- A mounted disc image is unmounted (§8.4 step 2), as with the power cycle.

### 8.4 The quick in-game reset, step by step
`HalpIgrCheck` runs in the title's thread that called `XInputGetState`, at that caller's IRQL (§3):
1. **IRQL.** Below DISPATCH_LEVEL (game threads read pads at passive level): go on. At DISPATCH_LEVEL or above
   (a title reading its pads from a DPC): use the power cycle instead, because the shutdown notifications and the
   unmount may wait.
2. **Disc images.** Clear the persisted slot table of spec 16 §5 (`VdSaved`), so the restarted kernel re-attaches no
   image and the dashboard finds the real drive (or no disc). The live slots are not unmounted: the title's other
   threads may be in the middle of reads, and the quick reboot ends them anyway.
3. **Other threads.** As Cerbios does: suspend every other thread of the title's process, so none of them touches
   the hardware or the disk while the reboot runs. (Open: whether `KeSuspendThread` on each is enough, or the
   threads must be held at raised IRQL as Cerbios does.)
4. **Launch data.** Make sure the launch data page exists (allocate and persist one page if not), clear it and set
   its type to 0xFFFFFFFF with an empty path, as `XLaunchNewImage(NULL, NULL)` and Cerbios's IGRDASH do.
5. **Reboot.** `HalReturnToFirmware(2)`, which runs the title's shutdown notifications and `KiQuickReboot` (§8.1).
6. **Serial log:** `igr: LT RT BACK BLACK: quick reboot to the dashboard` (and `igr: ... WHITE: power cycle`).

### 8.5 Risks
- **GPU busy.** The quick reboot does not reset the GPU. The title's graphics library idles it in its shutdown
  notification; if the title hangs the GPU or has no notification, the next title's graphics start-up resets the
  graphics engine and FIFO (spec 24), as after any title switch. Untested with a title killed in the middle of a
  frame.
- **Audio.** The AC'97 bus master is stopped at kernel entry (spec 10); the APU (the title's sound processor) is
  left to the title's sound library's shutdown notification and the dashboard's own start-up.
- **Disk.** A write in flight (a save, the cache partitions) is cut as with the power cycle; FATX and exFAT metadata
  may be left half-written. The IDE bus master is stopped; the drive may still be busy with a command, which the
  restarted kernel's first IDE access waits out.
- **Pads.** The 2026-10-29 dashboard hang (§8.2) was never explained; the USB change since is the likely fix.
- **Title not covered.** A title whose shutdown notification hangs or crashes never reaches `KiQuickReboot`; the
  WHITE combo (power cycle) is then the way out.

### 8.6 Tests before it replaces the power cycle
- A debug build option like `REBOOT_TEST` (§6) that runs the quick in-game reset N seconds into every title, to loop
  without a controller.
- On the 1.4 console (Modxo, serial log), 20 cycles each:
  - the test title that hung the hardware reset on 2026-11-03 (GPU busy at the reset);
  - a game started from a disc image (the next dashboard shows no disc; the image's slot is gone from the log);
  - the dashboard itself, and a title with music playing (audio stops, the dashboard's sound works);
  - the 2026-10-29 case: the FTP server's BACK as a quick reboot to the dashboard (an nxdk title, outside the hook, so
    through a test build of the FTP server), with a pad plugged in.
- Checks after each: the dashboard reached and its pads, sound and network working; the dashboard's free memory equal
  to a cold boot's (spec 18 §7); no `interrupt storm` or exception lines; the next title launched from the dashboard
  runs.
- Then the second console (spinning disk, DVD drive), and xemu for the dashboard path.

### 8.7 Open questions
- The exact launch data `XLaunchNewImage(NULL, NULL)` writes (type and flags): log it from the installer's
  "restart" item on the console (the serial log's `firmware: launch data type ...` line) and match it.
- Titles that read their pads at DISPATCH_LEVEL: none seen; §8.4 step 1 falls back to the power cycle for them.
- Whether to add Cerbios's IGRGAME (quick reboot back into the disc's title) as a third combo.
