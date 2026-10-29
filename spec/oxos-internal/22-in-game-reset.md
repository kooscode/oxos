# 22 — In-game reset (IGR)

OXOS addition, implemented 2026-10-29. Code: `kernel/src/arch/igr.S` (`KeRaiseIrqlToDpcLevel`, `HalpIgrReturn`),
`kernel/src/hal/igr.cpp` (`HalpIgrCheck`). Background: how Cerbios and EvoX do it (spec/cerbios/08, spec/evox/04 §2).

Holding **LT + RT + BACK + BLACK** together on any controller, in any XDK title, reboots the console. The reboot is a
cold one (`HalReturnToFirmware(HalRebootRoutine)`, the SMC reset), so:
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

`HalpIgrCheck` runs at the caller's IRQL after XInputGetState has lowered it again (passive for game threads), so the
reboot goes through the SMC.

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
