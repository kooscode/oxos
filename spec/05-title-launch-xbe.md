# Spec 05 — Title selection, LaunchDataPage, XBE loader, fatal errors

Clean-room behavioural spec (no decompiled code). Evidence: batch_decompile `ev_e510f757…`
(0x8002f2f4, 0x800551ef, 0x8001e9f5, 0x8002f291, 0x8002f236, 0x80055e75) and `ev_0bb25bf3…`
(0x8002edc9, 0x80055523). XBE field names follow the public XBE format.
Names marked (inferred) are not symbols in the binary.

## 1. Pre-launch checks, cold boot (`0x800551ef`, inferred name XeCheckFirstBoot)
Returns a reason bit mask (0 = all good):
- bit 0: the RTC is invalid, or the system time is outside **[2001-01-01, 2101-01-01)**.
- Read EEPROM user settings (`ExQueryNonVolatileSetting(0xFF, …, 0x60 bytes)`):
  - read failure → bits 1|2
  - the dword at settings offset 0x30 is 0 → bit 2
  - the byte at settings offset 0x08 is 0 → bit 1
  (Likely "language/time zone not configured" → the dashboard's first-run set-up. Inferred.)
A non-zero result is passed as the launch reason (§2), which ends at the dashboard with that reason.

## 2. Title selection (`XeLaunchTitle`, inferred, 0x8002f2f4, argument = reason)
**LaunchDataPage (LDP)** = 4 KB persisted page, pointer exported as ord 164 `LaunchDataPage`.
Header: `+0x000 dwLaunchDataType`, `+0x004 dwTitleId`, `+0x008 szLaunchPath[520]`, `+0x210 dwFlags`.
Dashboard-reason fields used by the kernel: `+0x400 dword`, `+0x408 dword`.

A. reason == 0 **and an LDP exists** (a title asked to launch another title):
  1. If dwFlags bit 0 is **already** set → free the LDP and go to B. This guards against launch loops.
  2. Set dwFlags bit 0; copy the first 360 bytes of szLaunchPath.
  3. If dwLaunchDataType == 0xFFFFFFFF → free the LDP (one-shot).
  4. Empty path → **dashboard** (§3).
  5. Path form `\Device\…\dir;name.xbe`: replace `;` with `\`, and create the symbolic link
     **`\??\D:` → the directory part**. This is the title's D: drive.
  6. Load the XBE (§4). Success → return (the phase-1 thread then enters the title).
     Failure → delete the D: link and map the error as in B.4.
B. Otherwise:
  1. Read the tray state (`HalReadSMCTrayState`), then try **`\Device\CdRom0\default.xbe`**.
  2. Success → return.
  3. If reason ≠ 0 → dashboard with LDP(type 3, reason).
  4. Read the tray state again. Tray state ≠ 0x60 (no media) → dashboard. Tray state changed → treat as not ready.
     These errors → plain dashboard: 0xC0000010, 0xC0000013, 0xC0000015, 0xC0000034, 0xC000003A,
     0xC00000A3, 0xC000014F.
     Any other error → dashboard with LDP(type 1, code), where code =
     **3 for 0xC0050001 (region mismatch)**, **5 for 0xC0050002 (media type not allowed)**, else 1.

## 3. Dashboard launch
- `XeLaunchDashboardWithReason(type, code)` (inferred, 0x8002f291): allocate the LDP if needed
  (`MmAllocateContiguousMemory(4096)`), `MmPersistContiguousMemory(…, TRUE)`, zero it, set dwLaunchDataType = 1,
  `+0x400 = type`, `+0x408 = code`, then launch the dashboard.
- `XeLaunchDashboard` (inferred, 0x8002f236): load **`\Device\Harddisk0\Partition2\xboxdash.xbe`** as
  dashboard. On failure, cold boot only: an SMC write (0xE1), then **fatal error 13**, or **13 + LDP type**
  if the LDP type is 1, or **20** if the title-disc flag is set. In every failure case it then calls
  `HalReturnToFirmware(4)` (fatal-error reboot).
- Boot-flags 0x40 path (0x80055e75): poll SMC register 3 (tray state) every 250 ms until it is
  not 0 or 0x50. If it is 0x60 → dismount CdRom0 and run `XeLaunchTitle(0)`; otherwise → dashboard.

## 4. XBE loader (`XeLoadImage(path, isDashboard, reason)`, inferred, 0x8002edc9)
1. Store a copy of the path in `XeImageFileName` (ord 326; pool tag 'XeFn'). Open the file.
2. Read the first 4 KB (pool tag 'XeHi'). **Validate**: bytes read ≥ 0x178; magic **'XBEH'**;
   SizeOfHeaders (+0x108) ≥ 0x179; SizeOfImage (+0x10C) ≥ SizeOfHeaders; **BaseAddress (+0x104) == 0x10000**.
   Otherwise → STATUS_INVALID_IMAGE_FORMAT (0xC000007B).
3. `NtAllocateVirtualMemory` at 0x10000: reserve SizeOfImage, commit SizeOfHeaders. Copy the 4 KB to 0x10000,
   then read the rest of the headers to 0x11000.
4. Check that the Certificate (+0x118, size 0x1D0) and the section headers (+0x120, count +0x11C,
   0x38 bytes each) both lie inside the headers.
5. **Header signature verification** (0x8002ebba; not yet decompiled). It uses `XePublicKeyData`.
   For an open kernel this is a policy choice: titles do not depend on it.
6. **Non-secure HDD rule**: if the disk-OK flag is 0, the console isn't a manufacturing/devkit unit
   (bit 31 of 0x8003B158 clear), and the certificate's AllowedMediaTypes doesn't have 0x40000000
   (non-secure hard disk) → **fatal error 5**.
7. **NonKernelImportDirectory (+0x15C) must be 0**, otherwise the image is rejected.
8. **Decode retail XOR keys**: EntryPoint (+0x128) ^= **0x94859D4B**;
   KernelImageThunkAddress (+0x158) ^= **0xEFB1F152**.
9. **Region**: certificate GameRegion (+0xA0) & the console's game region (**0x8003B158**, from the EEPROM;
   bit 31 = manufacturing) must overlap, otherwise **0xC0050001**.
10. **Media**: get the launch device's media flags (0x8002ec30). Certificate AllowedMediaTypes (+0x9C) & flags
   must be non-zero, otherwise **0xC0050002**. For media flag 2 (game disc) the title ID is pinned: the
   first title ID launched from the disc is remembered (0x8003B344), and a different one → 0xC0050002.
11. Cold boot: show the title logo bitmap (LogoBitmapAddress +0x170, size +0x174) using 0x800565f3.
12. Load every section with flag PRELOAD (0x2) (0x8002ea65).
13. **Resolve kernel imports** (0x8002ecee): walk the thunk table, replacing each `0x80000000 | ordinal`
    entry with the kernel export address. Details to confirm in the next pass.
14. For PRELOAD sections that are not WRITABLE (bit 0 clear): post-load step 0x8002e9ad (probably
    digest check or protection). Then 0x8002ed7c.
15. **If GameRegion lacks bit 31: zero `XboxEEPROMKey`** (16 bytes), so retail titles never see it.
16. **SMC "reset on eject" (SMBus 0x20, command 0x19)**: write 0 for normal titles, 1 for the dashboard
    or titles allowed media 0x80000000 (and for boot-flags 4 / devkit). Boot-flags bit 3 records the
    current state, and the write is skipped on quick reboot if the state is unchanged. The write is
    retried until it succeeds.
On error: release the image memory and free `XeImageFileName`.

## 5. Just before entering the title (0x8001e9f5)
- If an LDP exists: move it to a new persisted 4 KB contiguous page **below the kernel's physical
  address**, and free the old one.
- If the AV saved-data (persisted framebuffer, `AvGetSavedDataAddress`) isn't at the expected physical address
  (0x8003B2D0) → re-allocate it there and move it (0x8002fd1b). This keeps the splash image on screen across launches.
Then phase-1 calls the entry point at 0x10128 (spec 03).

## 6. Fatal errors
- **Reporter (0x80055523, code)**. It zeroes the second boot key, then reads the full EEPROM (0xFFFF, 256 bytes):
  - if no error is recorded yet: on manufacturing units store the code, and for code > 4 set bit
    (code − 5) in the 16-bit error-history word
  - otherwise clear the stored error
  It writes the EEPROM back; manufacturing units also get **SMC 0x02 = 0x40** (power-cycle). If the
  video set-up succeeds, it **draws the error number** with the INIT-section graphics code
  (seven-segment digit table at 0x8006F008/0x8006F000). Then it terminates the thread (`int 3`).
- Known codes: **5** HDD not secured for the title, **7/8/9** HDD timeout / IDENTIFY failed /
  too small or invalid, **10/11/12** DVD equivalents, **13** dashboard failed to load (+LDP type),
  **20** dashboard failed with a title disc present, **21** boot-flags 0x20 path.
- 0x80055933 is used for the disk errors. It is probably the same reporter path (unverified).

## Open questions
- 0x8002ebba (signature), 0x8002ecee (thunk resolve), 0x8002ec30 (media flags), 0x8002ea65/0x8002e9ad
  (section load / post-step), 0x800565f3 (logo), 0x8002ed7c.
- Where 0x8003B158 (game region) is loaded from the EEPROM; the meaning of EEPROM settings 0x08 / 0x30.
- INIT graphics library and boot animation: needed for parity of the boot visuals, not for title compatibility.
