# Cerbios spec 08: in-game reset (IGR) and button combos

Studied in Cerbios 3.1.0 Beta: the kernel's controller-state hook and the combo handler. Behaviour and formats only.

## 1. Where the buttons come from
The kernel intercepts the completion of the controller state request that titles make (the XInput "get state" path)
and passes the returned state and the controller's port number to the combo handler. The state is the usual
XINPUT_STATE layout: packet number (4 bytes), then the gamepad: digital buttons (u16 at +4) and eight analog button
bytes (+6 … +13: A, B, X, Y, BLACK, WHITE, LT, RT).

## 2. Building the 16-bit button mask
- Bits 0–7: the low byte of the digital buttons (D-pad up, down, left, right, START, BACK, left stick, right stick).
- Bits 8–15: analog button i (A, B, X, Y, BLACK, WHITE, LT, RT) counts as held only when its value is **0xFF**
  (fully pressed).
- This matches the ini encoding (spec 01 §2: key code k sets bit k XOR 8).

## 3. Port filter
IGRMASTERPORT (record +0xF8, 0–4): 0 = any controller; 1–4 = only the controller on port (value − 1).

## 4. Combos
Each combo (record field, 16-bit mask; 0 disables it) fires when **all** its bits are set in the mask. Checked in
this order on every state report:

| Combo | Field | Default | Action |
|---|---|---|---|
| IGRDASH | +0xF9 | 0xC030 (LT RT START BACK) | make sure a launch-data page exists (allocate and persist one page if needed), clear it, set its first dword to 0xFFFFFFFF (launch the dashboard), then **quick reboot** (HalReturnToFirmware 2) |
| IGRGAME | +0xFB | 0xD010 | **quick reboot** without new launch data: the used launch data is discarded at boot, so the boot starts the disc in the drive, or the dashboard if there is none (spec 09 §2) |
| IGRFULL | +0xFD | 0xD020 | write 0x0C to SMC register 0x1B (scratch), then **reboot** (HalReturnToFirmware 1) |
| IGRSHUTDOWN | +0xFF | 0xC021 | SMC register 0x02 = 0x80 (power off) |
| IGRCYCLE | +0x101 | 0xD001 | SMC register 0x02 = 0x40 (power cycle) |
| IGRSCREEN | +0x103 | 0x00C0 (LS RS) | if ENABLESCREENSHOTS is set, request a screenshot (taken at the next frame, spec 13 §5) |

- Only the first of IGRDASH / IGRGAME / IGRFULL that matches is acted on (they end in a reboot). Before any of the
  reboot or power actions the kernel **suspends every other thread of the process** (at raised IRQL, all threads
  but the current one). After the SMC power command it spins forever, waiting for the power to go.
- IGRSHUTDOWN and IGRCYCLE are checked after the reset combos; IGRSCREEN last.

## For OXOS (notes, not Cerbios behaviour)
- A hook at the input-state completion keeps IGR working for every title without patching the title.

## Open
- What the scratch value 0x0C asks of the next boot (bit 0x04 skips the boot animation, spec 06 §1; 0x08 is not
  yet traced).
- The exact request the hook recognises (it checks a type byte of 0xA3 in the request before chaining its
  completion).
