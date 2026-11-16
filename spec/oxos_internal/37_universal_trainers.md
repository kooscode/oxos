# 37. Universal Trainers: a game's trainer from any dashboard or disc

Status: **implemented** (2026-11-15): the extension `oxos_trainers.xos` and its menu `oxos_trainers.xbe` (`kernel/extensions/oxos_trainers/`, `app/`), with
the settings in `C:\OXOS\extensions\oxos_trainers.ini`. User, 2026-11-15: "a trainer selection xbe that we redirect to for any game
that we have a trainer for ... so no dashboard specific trainers required, we handle it at kernel level"; then "trainers as a full fledged feature implemented
using extensions ... for now i will manually edit the .ini file per title, but we will build a trainer settings app later."; then "the extension will
launch the trainer xbe first with title info ... have a "Start Game" button thats the default ... only redirect to this trainer launcher if trainers extension
is enabled AND the game is listed and have a trainer available, else launch game directly without any in between screens or delays."

Before, a trainer ran only when a dashboard installed it: XBMC4Gamers patches the kernel's title-start call (spec 23) and loads the trainer itself. With
Universal Trainers, OXOS does it for every launch, from any dashboard, the File Manager, a disc image or an original disc.

## 1. What the user sees
1. Install (once): `oxos_trainers.xos`, `oxos_trainers.xbe` and `oxos_trainers.ini` in `C:\OXOS\extensions\`, the trainers in `C:\OXOS\trainers\`, and
   the extension listed with `enabled = yes` in `C:\OXOS\oxos_extensions.ini` (spec 34 §3).
2. A game that has a trainer is started from a dashboard, a launcher or the File Manager: the **trainer menu** comes first (§4a). It shows the game's title
   image and name, the trainer, and a switch for the trainer and for each cheat. **Start Game** is selected, so A starts the game as set; the D-pad moves, A
   switches. A game without a trainer starts directly, with no screen and no wait.
3. The game starts with its trainer when it is switched on. `C:\OXOS\log\extensions.log` says so: `[oxos_trainers] Halo 2: trainer Halo 2.etm applied, 2
   of 13 cheats on.` Only when XBMC4Gamers launches the game with its own trainer installed does OXOS leave it to XBMC4Gamers (spec 23 §5).
4. The settings can also be edited in `oxos_trainers.ini` (§2). A game that is the cold boot's title (a disc in the drive at power-on) gets no menu (§4a) but
   its trainer as the ini says.

## 2. The settings file
`C:\OXOS\extensions\oxos_trainers.ini`, the extension's own (spec 34 §3.1), generated from the trainer pack by `tools/gen_trainers_ini.py`
(`make -C kernel extensions`): a section for each title ID the pack has a trainer for (651 sections from 505 trainers), sorted by the game's name, every
trainer off:
```ini
; Halo 2: Halo 2 (NTSC/PAL) +13, Trainer By acidflash & dootdoo - Evox-T
[4D530064]
title = Halo 2
trainer = Halo 2.etm
trainer_enabled = no
1. Infinite Ammo = no
2. Rapid Fire = no
...
13. Suicide (IGKey) = no
```
- **The section** is the title ID (8 hex digits): the one thing the kernel knows of every running title. A trainer listing several title IDs (regions) gets a
  section for each ID no other trainer is the main one for.
- **`N. name = yes/no`:** N is the cheat's number in the trainer, and is what the extension reads; the name is only for the reader. So names that repeat (13
  in the pack), are empty (36: written `(no name)`) or hold `=` or other characters (`=` becomes `-`, non-ASCII `?`) cannot confuse it. The default is the
  trainer's own switch. A cheat without a line keeps that default.
- `title` and the comment above the section (trainer name, author) are for the reader; the extension ignores them.
- Only the running title's section is read (`oxos_ini_section`, spec 34 §3.1): the 200 KB file is never in memory.

## 3. How it works
In the title-loading event (spec 34 §7.2), for a launch through the launch data page: the menu (§4a) in the game's place, when the game has a trainer.
In the title-loaded event, before the game's first instruction:

| Step | What |
|---|---|
| 1. The section | the title ID's section; nothing more unless `trainer_enabled = yes` and `trainer =` names a file |
| 2. Not for nxdk programs | an XBE whose one library entry is `CXBE` (nxdk's cxbe) is homebrew or OXOS's ISO launcher, which carries the game's title ID (spec 16) but is not the game: no trainer. The trainer runs in the game the launcher starts |
| 3. XBMC4Gamers | its hook installed in the kernel (spec 23 §5): its stub runs its own trainer, so OXOS logs that and does nothing |
| 4. Load | `C:\OXOS\trainers\FILE` (at most 64 KB) into contiguous memory taken in the title's launch, kept for the whole game (a trainer's hooks jump into its code), freed by the next launch |
| 5. Check | format 1, every header offset inside the file, the title ID among the trainer's three, no dashboard helper routines needed (offset 0x1A is 0 in every pack trainer); otherwise logged and not run |
| 6. Switches | one byte per cheat at the trainer's switch table: 1 for `yes`, 0 for `no` |
| 7. Run | as XBMC4Gamers' stub does (§4): `edx` and `ebp` at the trainer, CR0.WP cleared, `call` its entry, CR0 and every register restored |

**The trainer file (`.etm`, studied 2026-11-15 on the 998 trainers of `third_party/xbox_trainers`):** a header with the file size (0x00), format 1 (0x04),
the offset of the cheat switches (0x0A, one byte per cheat, 1 = on), of the text table (0x0E: trainer name, author, then one name per cheat, ending in 0), of
up to three title IDs (0x12; 446 trainers list more than one, mostly regions), the entry point (0x16), and the entry of trainers that need the dashboard's
helper routines (0x1A, 0 in all). 966 of the 998 check the game's XBE build timestamp (0x10114) before patching, so a trainer for another version of the game
patches nothing; 32 do not check.

## 4. The calling contract (XBMC4Gamers' stub, read from its default.xbe)
XBMC4Gamers allocates 0x3C00 bytes of persistent contiguous memory, stores its address at 0x8000000C, copies a 0xA7-byte stub and the trainer (switches
already set) into it, and patches the kernel's title-start call to `call [0x8000000C]` (spec 23 §1). At the title's start the stub saves every register,
compares the running title ID (`[[0x10118] + 8]`) with the trainer's three, and on a match sets `ebp` = `edx` = the trainer, clears CR0.WP, calls the entry
(0x1A if non-zero, else 0x16), restores CR0, and continues into the title's entry point. OXOS does the same from the title-loaded event, which runs at the
same moment: the title loaded, its first instruction not yet run.

## 4a. The trainer menu (`oxos_trainers.xbe`)
An nxdk + SDL3 app (`kernel/extensions/oxos_trainers/app/`), started by `oxos_trainers.xos` in place of a game:

| Step | Where | What |
|---|---|---|
| 1. Which game | the title-loading event, a launch through the launch data page | read the XBE's title ID from its file (the image header and certificate, about 0.2 KB) before it is loaded; nothing more for an nxdk program (`CXBE`: the ISO launcher, homebrew), a title without a section or trainer file, or with XBMC4Gamers' hook installed |
| 2. Redirect | `oxos_title_redirect` (spec 34 §7.2) | `C:\OXOS\extensions\oxos_trainers.xbe` in the game's place, when it exists; the state page remembers the game and this launch |
| 3. The menu | the app | reads the game's path from the launch data page (unchanged by the redirect), the game's XBE for its name and title image (`$$XTIMAGE`, else `E:\UDATA\<title ID>\TitleImage.xbx`; XPR0, DXT1/3/5 or 32-bit), its section and trainer |
| 4. Start Game | the app | writes the section's changed values (only the values; a missing line is added at the section's end), puts back the launch data page as it was given (the kernel's "tried" flag cleared) and quick-reboots, as a dashboard starts a title |
| 5. Through | the title-loading event | the launch right after the menu's, of the game it was started for, is not redirected; the title-loaded event runs the trainer as the section now says |

Measured on the console (2026-11-15, serial log): the decision in step 1 takes about 50 ms for a game with a section, in a 200 KB ini.

**The look:** the OXOS letters over "TRAINERS" (`app/oxos_trainers_logo.svg`, drawn by the kernel's vector renderer, `kernel/src/graphics/vector_renderer.cpp`,
from the scene `tools/svg_splash.py` makes at build time); the X turns from metal to green as in the boot splash. A grid and sparks behind, the title image in
a turning reticle, a "+N" badge (a trainer with N cheats), the cheats with switches, a KEY tag on in-game-key cheats.

## 5. Tests
- xemu (`make test_extensions`, part of `make regression`): `boot_test.py` writes a trainer of its own for the test title (two cheats, both off in the file)
  whose code writes 0xC0DE0001 to the title's `XkTitleTrainerMark` when cheat 1 is on (0xC0DE0000 when off), the menu, and a section with the trainer off and
  cheat 1 on. The test title's relaunch brings up the menu (`XeImageFileName`), where a virtual pad presses down, A (the trainer on) and START. Checked: the
  title's second run got the launch data page as its first run wrote it; the ini on the disk now says `trainer_enabled = yes` and nothing else changed;
  `extension_test.xos` reads the mark in the title-exiting event: 0xC0DE0001; the log says "the trainer menu first" and "1 of 2 cheats on" once.
- Console (2026-11-15, Modxo console, serial build): Halo 2 from the FTP server's launch, Infinite Ammo and Never Die on: "trainer Halo 2.etm applied, 2 of 13
  cheats on", the game plays and both cheats work (user, 2026-11-15: "works perfect!!!"). With the menu: Halo 2's ISO launcher started from the FTP server is
  not redirected (nxdk), the game it starts is: the menu shows Halo 2's title image, the trainer and its 13 cheats as the ini has them.

## 6. Later
- **All games' settings** from OXOS Setup (spec 39): the menu for any game with a trainer, without starting it.
- **The cold boot's title:** a menu for a disc in the drive at power-on (the kernel would start the menu, with a launch data page made for it).
- **Run-once** (spec 34 §4.3a): unload the extension (7 KB) once the trainer is installed; only the trainer stays.
- **Several trainers for one title:** the pack's `variants/` (other regions, versions, cheat counts); today the section's `trainer =` names one.
- **Wrong game version:** for the 32 trainers without their own check, record the XBE timestamp each was made for and refuse a mismatch.
- **Memory:** the trainer's block (Halo 2's: 1.3 KB, one 4 KB page) comes out of the title's memory; measure it against the stock gap (spec 18 §7).
- **The other trainer format** XBMC supports (`.xbtf`, xboxtrainers.net): later, if wanted.
