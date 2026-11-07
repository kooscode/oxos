# Cerbios spec 10: video options

Studied in Cerbios 3.1.0 Beta: the kernel's display-mode set, encoder-option and EEPROM-query code. Behaviour and
formats only.

## 1. AV pack codes
The SMC's AV pack value (HalBootSMCVideoMode) is mapped to an internal code: SMC 0 → 3 (SCART), 1 → 4 (HDTV),
2 → 5 (VGA), 3 → 2 (RF), 4 → 6 (S-Video), 6 → 1 (composite), anything else (no pack) → 0. The options below use the
internal code.

## 2. FORCE480P (record +0xDA)
- Also switched on at boot when FORCEVGA is set or the pack is VGA (spec 01 §3).
- When a title sets a display mode, the requested mode is replaced if **all** of these hold: the pack is HDTV or
  VGA; the title is not on the kernel's exclusion list (§5); the factory AV region is not PAL
  (standard byte 3 or 4); the requested mode is not already an HD mode (bit 31 clear); and the user's video flags
  allow 480p (EEPROM video flags bit 0x80000). The mode then becomes 480p: **0x88070701** if the requested mode is 0
  or has any of the bits 0x100100, else **0x88080801**.

## 3. FORCEVGA (record +0xDB)
- With FORCEVGA set, or with a VGA pack, an HD display mode is programmed with the VGA variant of the encoder set-up
  (the HD-only registers are cleared afterwards; the public notes call the output RGsB, sync on green).
- EEPROM query of the factory **AV region** (setting index 0x103): with FORCEVGA set or a VGA pack the kernel
  reports **0x00400100** (NTSC-M, 60 Hz) instead of the stored value, so titles pick 60 Hz modes.

## 4. FORCEFFILTER (record +0xDC, 0–6)
When a title sets the TV encoder's flicker-filter option (AvSendTVEncoderOption option 0x0B), the value is replaced
by FORCEFFILTER, unless FORCEFFILTER is **6**, which keeps the title's own value.

## 5. Per-title video fixes (always on)
- EEPROM AV region query (0x103): for a PAL-region title (game region 4) on a fixed list of PAL-only title IDs, the
  kernel reports **0x00800300** (PAL-I, 50 Hz). The list (names from the title-ID database titleIDs.db): 51 titles with the
  0x434D ("CM") prefix, all football games (Club Football, Club Football 2005,
  LMA/BDFL/Football Manager Campionato/Mánager de Liga/Roger Lemerre 2003–2006, England International Football 2004,
  Manchester United Manager 2005, Sensible Soccer 2006; 51 IDs in ranges 0x434D0012–19, 1B–23, 2F–33, 35–45, 49–4D
  plus 0x434D0004, 0C, 0F, 28, 46, 54, 5B), and 0x4553000A TimeSplitters 2, 0x48450001 Rugby League, 0x4C530002
  SCAR: Squadra Corse Alfa Romeo, 0x5454009B The Warriors.
- The **FORCE480P exclusion list** is the same 51 0x434D football IDs plus 0x48450001 Rugby League (all PAL-only
  releases).
- EEPROM video-flags query (setting index 8): for 0x42420002 (Dance:UK) and 0x42570001 (Robot Wars: Extreme
  Destruction), when the AV standard is not 3, bit 0x400000 is set, and if bit 0x80000 is clear, bit 0x200000 is set;
  for 0x53450010 (NCAA College Football 2K3) bit 0x10000 is cleared.

## 6. Region-free
EEPROM query of the factory **game region** (setting index 0x104) returns the **running title's own region**
(reduced to 1, 2, 4 or 0x80000000; spec 09 §3), so every title sees its own region.

## For OXOS (notes, not Cerbios behaviour)
- These are policy layers over the retail AV code: implement them as filters on the mode value and the EEPROM
  query results, not inside the encoder tables.
- The lists are compatibility data: confirm each title by testing rather than relying on these IDs alone.

## Open
- A second display-mode override flag in the kernel (separate from FORCE480P) whose setter was not found.
