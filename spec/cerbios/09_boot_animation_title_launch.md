# Cerbios spec 09: paths, boot animation, title selection and the XBE loader

Studied in Cerbios 3.1.0 Beta: the extension's path translator, launch selection, dashboard fallback and XBE loader. Behaviour and formats only.

## 1. Path translation (DASHPATH, BOOTANIMPATH, LCD and screenshot paths)
A path is rewritten in place: each rule below is tried in turn, and a rule whose prefix matches the start of the path replaces that prefix:
1. `<L>:\` for L in C E F G X Y Z → `\Device\Harddisk0\Partition<n>\` (C 2, E 1, F 6, G 7, X 3, Y 4, Z 5).
2. `HDD<d>-<L>:\` (d = 0 or 1; L in C E F G H I J K L M N X Y Z) → `\Device\Harddisk<d>\Partition<n>\` (C 2, E 1, F 6, G 7, H 8, I 9, J 10 … N 14, X 3, Y 4, Z
   5).
3. `D:\` → `\Device\CdRom0\`; `DVD-ROM:\` → `\Device\CdRom0\`.

Anything else (for example a full `\Device\...` path) is used unchanged.

## 2. What starts at boot (phase 1, spec 06 step 17)
1. **Boot animation** — cold boot only, and only if boot flag 0x10 (SMC scratch bit 0x04) is clear: load BOOTANIMPATH (translated; skipped if empty), else the
   built-in `\Device\Harddisk0\Partition1\Cerbios\BootAnims\Xbox\bootanim.xbe`. On success a "boot animation running" flag is set and that XBE runs. The
   animation then starts the next stage itself (by rebooting into the launch below).
2. **Launch data** — otherwise, if a launch-data page exists:
   - a page already marked consumed (bit 0 of dword 0x84) is freed; a page whose first dword is 0xFFFFFFFF (dashboard request, spec 08) is freed;
   - else the page is marked consumed and its launch path (360 bytes at dword 2) is used. An empty path means "start the dashboard". The path has the form
     `<directory>;<file>`: the `;` becomes `\`, and while the title loads `\??\D:` is linked to the directory part (removed again if the load fails).
3. **The disc** — with no launch data: read the SMC tray state, then load `\Device\CdRom0\default.xbe`.
   - On failure, if the tray does not report media present (state 0x60), or the error is one of "not found / no media / unrecognised / bad path / wrong volume"
     kinds, start the dashboard.
   - Other failures write a launch-data page of type 1 with a reason code (3 for STATUS 0xC0050001, 5 for 0xC0050002, else 1) and start the dashboard, which can
     show the disc error.
4. **The dashboard** (also the fallback of every case above):
   - DEBUG set: the path read from `\Device\Harddisk0\Partition1\dashboard.xbx` (a 360-byte file holding `<directory>;<file>`).
   - otherwise, in order until one loads: DASHPATH (translated), `E:\Cerbios\Recovery\default.xbe`
     (`\Device\Harddisk0\Partition1\Cerbios\Recovery\default.xbe`), `\Device\Harddisk0\Partition2\xboxdash.xbe`. In **safe mode the recovery XBE comes first**,
     then DASHPATH, then xboxdash.xbe.
   - If none loads: (cold boot) front LED pattern 0xE1 and **fatal error 13** (or 13 + the reason in the launch data at dword 0x100, when a type-1 page is
     present), then HalReturnToFirmware 4.

### 2.1 The XMV boot animation (`BootAnims\XMV Player\BootAnim.xbe`, "BootAnim V1.2")
Studied 2026-11-17 in the Cerbios 3.1.0 Beta release (the XBE flattened to its memory image at 0x10000; REA/Ghidra evidence `ev_97620c16…` strings,
`ev_06aa95fc…` decompilation of 0x11160, 0x1ecd5 and 0x1fd7d, `ev_4d85fc31…` the callers of HalReturnToFirmware; the rest from its disassembly).
- **What it is built with:** an XDK title, its library list XAPILIB, D3D8, D3DX8, XGRAPHC, XBOXKRNL, **XMV**, LIBCP, LIBC and DSOUND, all version 1.0.5933.
  The video is played by the XDK's own XMV library (section `XMV`, 0x43220), which is why it plays `.xmv` files: no decoder of its own.
- **Files:** it links `BOOTANIM:` to its own folder (`\??\BOOTANIM:`; "Cannot create Symbolic Link" logged if it fails) and reads `BOOTANIM:\bootanim.ini`:
  `[Calibration] Enabled` (true = the screen is blue while it waits) and `Ticks`, `[Jukebox] Enabled`. With the jukebox off it plays
  `BOOTANIM:\DefaultAnim.xmv`; on, a random `.xmv` from `BOOTANIM:\Jukebox\` ("randomIndex is %i"; "No XMVs exist in Jukebox" when there is none).
- **The wait before the video:** always `Ticks` × 250 ms (0x1ddf3: `Ticks` × 0xFA passed to a sleep, logged "Waiting %i ms to Play"), then 50 ms more, then
  playback. The calibration setting only colours the screen during that wait; it is there for TVs that take time to show the new picture.
- **Playback and skipping** (0x11840): the video plays on a thread of its own (start 0x112b0); the main thread waits on it 16 ms at a time and reads the four
  controllers each time: a controller whose input field (offset 0x50 of its 0xE0-byte record) is set stops the video (0x118d0–0x118f9). At the end a "played"
  flag is set (+0x188, 0x11916).
- **After the video** (0x1df10–0x1df28): with the flag set, if a controller holds a certain button (0x11160: any of the four records connected with its
  button byte at offset 7 set) it starts `D:\Cerbios\Recovery\default.xbe` with `D:` linked to `\Device\Harddisk0\Partition1` ("Recovery XBE not found …
  falling back to firmware" if absent); otherwise it calls **HalReturnToFirmware(2), a quick reboot, without writing launch data**. Cerbios then takes the
  path of §2 step 3: the disc, else the dashboard. (The boot animation runs on cold boots only, so the quick reboot does not start it again.)
- **Errors:** a file it cannot play logs "Failed to play XMV @ %s. Make sure the XMV is in the correct format" and shows an error screen (0x126e0).

## 3. The XBE loader
Given a path (and whether it is a system XBE: dashboard or boot animation):
- **XOnline redirect:** if XONLINEDASHREDIR is set and the path is `\Device\Harddisk0\Partition2\XODash\xonlinedash.xbe`, `…\XODash\xonlinedash_original.xbe` is
  loaded instead.
- XeImageFileName is set to the path; a "started from the DVD" flag is set when it begins with `\Device\CdRom0\`.
- **Header checks:** the first 4 KB must hold `XBEH`, size of headers ≥ 0x179, size of image ≥ size of headers, base address 0x10000; the certificate and
  section headers must lie inside the headers. Otherwise STATUS_INVALID_IMAGE_FORMAT. **No signature or hash is verified.**
- The image range at 0x10000 is reserved (size of image) and committed for the headers; the headers are read there.
- Title ID (certificate +0x08) and game region (+0xA0) are kept; the region is reduced to one of 1, 2, 4 or 0x80000000 (anything else counts as 1). Spec 10 §6
  reports it as the console's region.
- The entry point and kernel thunk address are decoded with the retail XOR pair, or the debug pair when the stored thunk value has bit 31 set (the standard XBE
  keys).
- **Media and region unlocking:** the certificate's allowed-media field (+0x9C) gets its low 7 bits set (every media type allowed); if the certificate is larger
  than 0x1DB bytes, bit 0 of its field at +0x1D8 is cleared.
- Preload sections are loaded (and, with ADVCPUSUPPORT, patched: spec 12 §5); kernel imports are bound by ordinal (an unknown ordinal fails with
  STATUS_ORDINAL_NOT_FOUND); other imports are matched by module name.
- Library versions: the D3D8 (or D3D8LTCG) entry gives the XDK build and QFE used by the graphics hooks (spec 13 §5).
- Upper-memory unlock from the XBE init flags (spec 02 §4).
- The per-title LAN key, signature key and 16 alternate signature keys are derived from the certificate (HMAC with the kernel's key) into the exported key
  variables, as retail does.
- Reset-on-eject state is set for the title (spec 13 §6).

## For OXOS (notes, not Cerbios behaviour)
- The dashboard chain (configured path, recovery, stock path) is the part users rely on; the boot animation is optional.

## Open
- The loader's handling of a non-system XBE when the caller passes its third argument (a disc-only rule; returns STATUS 0xC000003A for some regions).
