# Spec 05 — Title selection, LaunchDataPage, XBE loader, fatal errors

Clean-room behavioural spec (no decompiled code). Evidence: batch_decompile `ev_e510f757…`
(0x8002f2f4, 0x800551ef, 0x8001e9f5, 0x8002f291, 0x8002f236, 0x80055e75) and `ev_0bb25bf3…`
(0x8002edc9, 0x80055523). XBE field names follow the public XBE format.
Names marked (inferred) are not symbols in the binary.

## 1. Pre-launch checks, cold boot (`0x800551ef`, inferred name XeCheckFirstBoot)
Returns a reason bit mask (0 = all good):
- bit 0: the RTC is invalid, or the system time is outside **[2001-01-01, 2101-01-01)**.
  "RTC valid" (0x800156a8) = a **CMOS signature**: with interrupts off, for indexes 0x10–0x6F and then 0x80–0xFF,
  write the index to port **0x72** and read port **0x71**; the bytes must alternate **0x55, 0xAA, 0x55, …**
  (restarting at 0x55 for the second range). `NtSetSystemTime` (ord 228) writes this pattern (0x800156e8) after
  setting the RTC, so the check means "the clock has been set at least once".
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
  dashboard. On failure, cold boot only: SMC LED sequence (SMC reg 0x08 = 0xE1, then reg 0x07 = 1, custom LED mode), then **fatal error 13**, or **13 + LDP type**
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
10. (Ours, milestone 9: region and media checks are enforced only with `RETAIL_MEDIA_POLICY=1`; by default they
   behave like a modded BIOS — the Complex 4627 2BL patches the stock kernel to OR the certificate's media types
   with 0x00FFFFFF and relax the region — so unauthenticated discs (backups, XISOs in emulators) start.)
   **Media**: get the launch device's media flags (0x8002ec30). Certificate AllowedMediaTypes (+0x9C) & flags
   must be non-zero, otherwise **0xC0050002**. For media flag 2 (game disc) the title ID is pinned: the
   first title ID launched from the disc is remembered (0x8003B344), and a different one → 0xC0050002.
11. Cold boot: show the title logo bitmap (LogoBitmapAddress +0x170, size +0x174) using 0x800565f3.
12. Load every section with flag PRELOAD (0x2) (0x8002ea65).
13. **Resolve kernel imports** (0x8002ecee): walk the thunk table, replacing each `0x80000000 | ordinal`
    entry with the kernel export address. Details to confirm in the next pass.
14. For PRELOAD sections that are not WRITABLE (bit 0 clear): post-load step 0x8002e9ad (probably
    digest check or protection). Then 0x8002ed7c.
15. **If GameRegion lacks bit 31: zero `XboxEEPROMKey`** (16 bytes), so retail titles never see it.
16. (Before this, 0x80014cc9 arms the SMC events, spec 12 §4; an eject handled during boot fails a disc title with
   0xC000003A.) **SMC "reset on eject" (SMBus 0x20, command 0x19)**: write 0 for normal titles, 1 for the dashboard
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
- 0x80055933 is used for the disk errors: SMC LED sequence 0xE1 (as in §3), then the reporter. Confirmed.
- Reporter details (confirmed): the EEPROM image is setting 0xFFFF (256 bytes); the stored error code is byte
  **+0xFA**, the history word **+0xFC**. A nonzero +0xFA is cleared (and nothing is recorded). Before the EEPROM
  update it stops the boot animation (0x800565f3(0, 0), 0x80055ee4). The digits: two (units on the right), each a
  seven-segment glyph drawn as lines by the GPU code; then 0x80021b78(0) and `int 3`.

### Our implementation (milestone 5f)
- `src/ex/fatal.cpp` `IdexFatalError(code)`: LED sequence, the cert boot key wiped, the EEPROM history as above,
  then (with an AV pack) a 640x480 X8R8G8B8 framebuffer with the two digits in **our own** seven-segment layout,
  shown through `AvSetDisplayMode`; the thread then sleeps forever. No GPU drawing, no animation to stop yet.
- Test: `make test-fatal` boots without a hard disk → code 9; checks the drawn digits and the EEPROM history bit.

## 7. Details confirmed for milestone 4a
Evidence: batch_decompile `ev_38839e94…` (thunk resolver 0x8002ecee, section load 0x8002ea65, post-load protect
0x8002e9ad, certificate keys 0x8002ed7c), `ev_6e2b3c15…` (media flags 0x8002ec30, key derivation 0x8002ed4b),
`ev_cfe66569…` (section digest 0x8002f4a4, section commit/decommit 0x8002e96c / 0x8002ea00).
- **Kernel thunks:**
  - every entry is taken as an ordinal (low 16 bits) and resolved through the kernel's own PE export directory
    (ordinal − Base)
  - an ordinal outside the table, or with RVA 0, → STATUS_ORDINAL_NOT_FOUND (0xC0000138)
- **Section header** (0x38): Flags (1 WRITABLE, 2 PRELOAD, 4 EXECUTABLE, 0x10 HEAD_PAGE_RO, 0x20 TAIL_PAGE_RO),
  VirtualAddress, VirtualSize, PointerToRawData, SizeOfRawData, SectionName, SectionReferenceCount, pointers to the
  head/tail shared-page USHORT counters, SHA-1 digest[20].
- **Section load:**
  - commit VA..VA+VirtualSize and bump the head and tail page counters
  - read the raw data and zero-fill the rest
  - digest = SHA1(u32 SizeOfRawData ‖ data); a mismatch → 0xC0000190 and the section is unloaded
  - ReferenceCount = 1. On a boot-time mismatch, retail titles reboot to the firmware.
- **Section unload:** decrement both counters, and decommit the range minus any head/tail page still shared.
- **After preload:** non-writable sections become PAGE_EXECUTE_READ, excluding partial head/tail pages unless the
  HEAD/TAIL_RO flag is set.
- **Media flags:** the file's volume device type is CD_ROM → (title-disc flag ? 2 : IOCTL_DVD_READ_STRUCTURE
  0x335140: STATUS_UNRECOGNIZED_MEDIA → 0xC; layer type 1 → 0x14/0x24 by layer count; else 0x44/0x84); otherwise 1.
- **Keys:** XboxLANKey, XboxSignatureKey and XboxAlternateSignatureKeys[16] = first 16 bytes of
  HMAC-SHA1(certificate key from the 2BL, certificate +0xB0 / +0xC0 / +0xD0 + 16i).

## 8. Our implementation (milestone 4a)
- `src/xe/loader.cpp`:
  - XeLoadImage, XeLoadSection/XeUnloadSection, thunk resolution, media and region checks, key derivation
  - the launch order: `\Device\CdRom0\default.xbe`, then the dashboard
- **Policy differences:**
  - The header signature is not verified, and the non-secure-HDD fatal error 5 is not raised.
  - Section digest mismatches are counted (`XeDigestMismatches`) but not fatal: homebrew XBEs (nxdk/cxbe)
    carry no valid digests. The reference reboots with fatal-error firmware return.
  - Retail XOR keys (entry 0xA8FC57AB, thunk 0x5B6D40B6) are tried first, then the debug keys
    (0x94859D4B / 0xEFB1F152); the reference BIOS kernel used for RE carries the debug pair.
  - The console region is all regions until the EEPROM is read.
  - No logo or SMC tray state yet (LaunchDataPage launches: §9).
- Path symbolic links (`\??\D:` → a directory inside a device) reparse the link target plus the rest of the name.
- **Test title:** `tests/title/title.cpp` → `tools/mkxbe.py` → `default.xbe` on the generated test disc.

## 9. Our implementation (milestone 5d-2): LaunchDataPage launches
- Title thread: if `LaunchDataPage` is set (only possible after a quick reboot), path A of §2:
  - Flags bit 0 already set → free the page, fall back to the dashboard (loop guard); otherwise set it.
  - Type 0xFFFFFFFF → the page is freed before the launch; an empty path → the dashboard.
  - `dir;name.xbe` → `\??\D:` links to `dir`, the image `dir\name.xbe` is loaded (link removed on failure).
- With a page present but no usable path the disc is not tried; the dashboard is.
- **Test:** with `relaunch.txt` on the test disc, the title allocates and persists a page, stores its results
  in LaunchData, requests `\Device\CdRom0\media;second.xbe` and calls `HalReturnToFirmware(2)`; the second run
  checks XeImageFileName, the D: link and the page contents, then frees it.

## 10. Our implementation (milestone 5e): launch policy
- `XepCheckFirstBoot` (§1, cold boot only; result in `XeLaunchReason`), `XepLaunchTitle` (§2 A/B including the
  tray-state comparison and the error → LDP code mapping), `XepLaunchDashboardWithReason` / `XepLaunchDashboard`
  (§3, LED + fatal error 13/13+type/20, then `HalReturnToFirmware(4)`), `XepPrepareToEnterTitle` (§5, LDP only).
- Details confirmed in the reference: a nonzero reason still tries the disc first (and passes the reason to the loader);
  only a failure goes to LDP(type 3, reason). Path A failures skip the tray checks and go straight to the code mapping.
  The `;` split uses the first `;`.
- The title-disc flag (0x8003B338) is set by DVD authentication, which is not implemented: it stays 0, so
  media flag 2 and fatal error 20 never occur.
- Not yet: the boot-flags paths (0x04/0x08/0x40/0x80), the AV saved-data move of §5, the logo, the fatal-error
  reporter's EEPROM history and on-screen digits (ours bug-checks with the code).
- Tests: the relaunch phase ends with a launch of a missing image → dashboard with LDP {1, 1, 1}; the dashboard
  phase checks the no-disc reason path; the misc suite round-trips the CMOS signature.

## Open questions
- 0x8002ebba (signature), 0x8002ecee (thunk resolve), 0x8002ec30 (media flags), 0x8002ea65/0x8002e9ad
  (section load / post-step), 0x800565f3 (logo), 0x8002ed7c.
- Where 0x8003B158 (game region) is loaded from the EEPROM; the meaning of EEPROM settings 0x08 / 0x30.
- INIT graphics library and boot animation: needed for parity of the boot visuals, not for title compatibility.
