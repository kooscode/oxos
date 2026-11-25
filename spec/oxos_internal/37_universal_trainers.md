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
   the extension listed with `enabled = yes` in `C:\OXOS\oxos_extensions.ini` (spec 34 §3). The release ships all of it, with `enabled = no` (user,
   2026-11-16: a screen before most games, also where no trainer fits the game's version, is in the way for players who do not use trainers).
2. A game that has a trainer is started from a dashboard, a launcher or the File Manager: the **trainer menu** comes first (§4a). It shows the game's title
   image and name, the trainer, and a switch for the trainer and for each cheat. **Start Game** is selected, so A starts the game as set; the D-pad moves, A
   switches. A game without a trainer starts directly, with no screen and no wait.
3. The game starts with its trainer when it is switched on. `C:\OXOS\log\extensions.log` says so: `[oxos_trainers] Halo 2: trainer Halo 2.etm applied, 2
   of 13 cheats on.`, when the extension's section of `oxos_extensions.ini` has `logging_enabled = yes` (spec 34 §5.1); the release ships `no` (user,
   2026-11-16: a line at every game kept filling the log). Only when XBMC4Gamers launches the game with its own trainer installed does OXOS leave it to
   XBMC4Gamers (spec 23 §5).
4. A disc the console starts by itself (in the drive at power-on, or loaded with xemu's "Load Disc") gets the menu too (§4a, step 2).
5. The settings can also be edited in `oxos_trainers.ini` (§2).

## 2. The settings file
`C:\OXOS\extensions\oxos_trainers.ini`, the extension's own (spec 34 §3.1), generated from the trainer pack by `tools/gen_trainers_ini.py`
(`make -C kernel extensions`) from the pack's `index.csv`: a section for each title ID the pack has a trainer for (551 sections from 1,084 trainers), sorted by
the game's name, every trainer off:
```ini
; Halo 2: Halo 2 (NTSC/PAL) +13, Trainer By acidflash & dootdoo - Evox-T
; other trainer: Halo 2 (p1).etm (+1, build 4158CC50)
; other trainer: Halo 2 (p13).etm (+13, build 42408C4E)
...
[4D530064]
title = Halo 2
trainer = Halo 2.etm
trainer_enabled = no
1. Infinite Ammo = no
2. Rapid Fire = no
...
9. Cloaked (IGKey) = no
9 keys = BACK
...
12. Jump 2x as high (IGKey) = no
12 keys = RIGHT STICK + D-PAD DOWN
13. Suicide (IGKey) = no
13 keys = RIGHT STICK + D-PAD UP
```
- **The section** is the title ID (8 hex digits): the one thing the kernel knows of every running title. Its trainer is the title's default in the pack's
  index (`default_for`): the pack's main trainer for the game, else the one with the most cheats. The title's other trainers are listed in comments above it,
  with their cheat count and the build they fit; `trainer =` can name one of them instead.
- **`N. name = yes/no`:** N is the cheat's number in the trainer, and is what the extension reads; the name is only for the reader. So names that repeat (13
  in the pack), are empty (36: written `(no name)`) or hold `=` or other characters (`=` becomes `-`, non-ASCII `?`) cannot confuse it. The default is the
  trainer's own switch. A cheat without a line keeps that default.
- **`N keys = BUTTON + BUTTON, ...`:** the controller buttons that switch cheat N during the game, pressed together (`RIGHT STICK`, `LEFT STICK`: the stick
  pressed in; `D-PAD UP`, `BACK`, `START`, ...); combinations separated by commas each do something of their own (FIFA's goals: up and down for each team).
  The menu shows them (§4a); the extension ignores these lines (it reads only `N.`). They come from the pack's index, which read them from each trainer's code
  (`third_party/xbox_trainers/README.md`): 346 cheats of 131 trainers. They belong to the trainer `trainer =` names; the menu writes them anew with another.
- **`trainer N = FILE | build TIMESTAMP ... | keys N: BUTTON + BUTTON; ...`:** a title with several trainers lists them all (Halo 2: 8), its default first,
  then the most cheats first: the file, the game builds it is made for (the XBE timestamps it checks; `any`: no check; `hidden`, `unknown`: not known) and its
  key cheats. The menu switches between them (§4a) and adds the ones copied in later (§7.3). The extension reads only `trainer =`.
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
| 4. Load | `C:\OXOS\trainers\FILE` (at most 64 KB) into a 64 KB buffer in the extension's own image, in the kernel's code window (spec/oxos_internal/34 §8.2: movable pages code can run from), kept for the whole game (a trainer's hooks jump into its code); the next launch loads the image anew |
| 5. Check | format 1, every header offset inside the file, the title ID among the trainer's three, no dashboard helper routines needed (offset 0x1A is 0 in every pack trainer); otherwise logged and not run |
| 6. Switches | one byte per cheat at the trainer's switch table: 1 for `yes`, 0 for `no` |
| 7. Run | as XBMC4Gamers' stub does (§4): `edx` and `ebp` at the trainer, CR0.WP cleared, `call` its entry, CR0 and every register restored |

**The trainer file (`.etm`, studied 2026-11-15 and 2026-11-16 on the trainers of `third_party/xbox_trainers`):** a header with the file size (0x00), format 1
(0x04), the offset of the cheat switches (0x0A, one byte per cheat, 1 = on), of the text table (0x0E: trainer name, author, then one name per cheat, ending in
0), of up to three title IDs (0x12; the list often has no terminating 0, so the dwords after the last ID are other data, such as the switches: only 3 of the
pack's trainers list a second real title ID), the entry point (0x16), and the entry of trainers that need the dashboard's helper routines (0x1A, 0 in all). Of
the pack's 1,084 (2026-11-16), 1,073 check the game's XBE build timestamp (0x10114) before patching, so a trainer for another version of the game patches
nothing; 11 do not check.

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
| 1. Which game | the title-loading event, of a launch through the launch data page or of the disc | read the XBE's title ID and build (its timestamp) from its file (the image header and certificate, about 0.2 KB) before it is loaded; nothing more for an nxdk program (`CXBE`: the ISO launcher, homebrew), a title without a section or trainer file, or with XBMC4Gamers' hook installed |
| 2. Redirect | `oxos_title_redirect` (spec 34 §7.2) | `C:\OXOS\extensions\oxos_trainers.xbe` in the game's place, when it exists; the state page remembers the game and this launch. Also for the disc the console starts by itself, with no launch data page (power-on, xemu's "Load Disc"; 2026-11-15, v0.4.10b): the menu then takes the game to be `\Device\CdRom0\default.xbe`, and starts it with a page of type 0xFFFFFFFF, which the kernel frees as it starts the disc, so the game gets no launch data, as before |
| 3a. New trainers | the app, first | the scan (§7.3): trainer files not yet in the ini are added to it, before anything else is read |
| 3. The menu | the app | reads the game's path from the launch data page (unchanged by the redirect), the game's XBE for its name and title image (`$$XTIMAGE`, else `E:\UDATA\<title ID>\TitleImage.xbx`; XPR0, DXT1/3/5 or 32-bit), its section and trainer |
| 4. Start Game | the app | writes the section's changed values (only the values; a missing line is added at the section's end; for another of the title's trainers: `trainer =`, `trainer_enabled` and the cheat and keys lines replaced by that trainer's, the `trainer N` lines kept), puts back the launch data page as it was given (the kernel's "tried" flag cleared) and quick-reboots, as a dashboard starts a title |
| 5. Through | the title-loading event | the launch right after the menu's, of the game it was started for, is not redirected; the title-loaded event runs the trainer as the section now says |
| 6. The same build again | the title-loading event | the launch right after that title's, of the same title ID and build, gets no second menu; another build gets the menu (§7.5b) |

Measured on the console (2026-11-15, serial log): the decision in step 1 takes about 50 ms for a game with a section, in a 200 KB ini.

**The look:** the OXOS letters over "UNIVERSAL TRAINERS" (`app/oxos_trainers_logo.svg`, drawn by the kernel's vector renderer,
`shared/graphics/vector_renderer.cpp`, from the scene `tools/svg_splash.py` makes at build time); the X turns from metal to green as in the boot splash. A
grid and sparks behind, the title image in a turning reticle, a "+N" badge (a trainer with N cheats), the cheats with switches, a KEY tag on in-game-key
cheats. **Several trainers** (`trainer N` lines): beside the game's name `< 2 OF 8 >`, green when the trainer is made for the running game's build (its XBE
timestamp, read with the title image; or any build, or not known), amber when it is made for another; LEFT and RIGHT (D-pad or left stick) switch, the card,
cheats and keys follow, and each trainer keeps the switches set for it while the menu runs (one shown first starts with its file's defaults). An amber one gets
a red band across its greyed-out cheats, rising to the right, "[ VERSION NOT COMPATIBLE ]" along it in white (letters stepped along the band: the debug font
does not turn); it cannot be switched on: its TRAINER switch shows off, A does nothing on its rows, and Start Game saves the section with
`trainer_enabled = no` (also when it was saved on). The build is read from the trainer's own check, so on another build it would patch nothing, or patch the
wrong places and crash the game. The hints at the bottom: A (what it does on the selected row: ON/OFF, START GAME; none on the rows of a trainer that cannot
be switched on), START GAME (START, while a row other than Start Game is selected), LEFT/RIGHT TRAINER, BACK EXIT (to the dashboard: no game, nothing
saved), and SAVED AT START at the right while something changed. Under the cheats, for a trainer with in-game keys, the keys strip: the selected cheat's
buttons as amber button caps ("IN GAME [RIGHT STICK] + [D-PAD DOWN]", combinations side by side, scrolling when they do not fit), or what the KEY tag means; the
list then shows six cheats at a time instead of seven.

## 5. Tests
- xemu (`make test_extensions`, part of `make regression`): `boot_test.py` writes a trainer of its own for the test title (two cheats, both off in the file)
  whose code writes 0xC0DE0001 to the title's `XkTitleTrainerMark` when cheat 1 is on (0xC0DE0000 when off), the menu, and a section with the trainer off and
  cheat 1 on, with a keys line for cheat 1 (the extension must not read it as cheat 1's switch). The test title's relaunch brings up the menu
  (`XeImageFileName`), where a virtual pad presses down, A (the trainer on) and START. Checked: the title's second run got the launch data page as its first run
  wrote it; the ini on the disk now says `trainer_enabled = yes` and nothing else changed; `extension_test.xos` reads the mark in the title-exiting event:
  0xC0DE0001; the log says "the trainer menu first" and "1 of 2 cheats on" once.
- Console (2026-11-15, Modxo console, serial build): Halo 2 from the FTP server's launch, Infinite Ammo and Never Die on: "trainer Halo 2.etm applied, 2 of 13
  cheats on", the game plays and both cheats work (user, 2026-11-15: "works perfect!!!"). With the menu: Halo 2's ISO launcher started from the FTP server is
  not redirected (nxdk), the game it starts is: the menu shows Halo 2's title image, the trainer and its 13 cheats as the ini has them.
- xemu (`make test_extensions`, 2026-11-16): the test title's section lists two trainers of the test's own (marks 0xC0DE0001 and 0xC0DE0002) and the disk
  holds a third (0xC0DE0003) the ini does not know: at the cold boot's menu the scan adds it as `trainer 3`; at the relaunch's the pad presses RIGHT (the
  second trainer), switches it and its cheat 1 on and starts; the section then reads exactly as expected (`trainer = xktest_two.etm`, its cheat lines, the
  keys line gone, the three trainer lines), and the game gets mark 0xC0DE0002.
- xemu (`make test_extensions`, 2026-11-18): the relaunch test boots twice, its relaunch as the same build (no second menu; the trainer is chosen at the
  cold boot's menu and runs at both runs) and as another build (`--other-build`: the menu again, where it is chosen), §7.5b.
- Console (2026-11-16, Modxo console, all 1,084 trainers in `C:\OXOS\trainers\`): Halo 2 from `F:\Games` brings up the menu with `< 1 OF 8 >`.
- xemu screenshots (2026-11-16, `tools/xemu_screenshot.py`, Halo 2's ISO as the disc): trainer 2 (the v1.1 +13) `< 2 OF 8 >` amber with the band; Cloaked shows
  "IN GAME [BACK]", Jump 2x as high "[RIGHT STICK] + [D-PAD DOWN]", Suicide "[RIGHT STICK] + [D-PAD UP]"; the KEY tags on the three.

## 6. Later
- **All games' settings** from OXOS Setup (spec 39): the menu for any game with a trainer, without starting it.
- **Run-once** (spec 34 §4.3a): unload the extension (7 KB) once the trainer is installed; only the trainer stays.
- **Several trainers for one title, wrong game version, new trainer files:** §7.
- **Memory:** the trainer's block (Halo 2's: 1.3 KB, one 4 KB page) comes out of the title's memory; measure it against the stock gap (spec 18 §7).
- **The other trainer format** XBMC supports (`.xbtf`, xboxtrainers.net) is an ETM in an obfuscated wrapper; the pack's are converted to `.etm`
  (`third_party/xbox_trainers/README.md`). The two that need XBMC's helper routines (0x1A non-zero) cannot run as ETMs and are not in the pack.

## 7. Trainers found and chosen automatically (proposal, 2026-11-15; partly built 2026-11-16)
Status: **built 2026-11-16:** the title's trainers in its section (`trainer N` lines, §2, in place of §7.2's index), the menu's scan for new trainers (§7.3, as
built), the player's choice between a title's trainers (§7.4a, as built), title updates (§7.5); **built 2026-11-18:** no second menu when a title
starts the same build again (§7.5b). **Still a proposal:** the extension choosing the trainer that fits the build (§7.4), games with no trainer for their
build (§7.5a). (User, 2026-11-15: "spec it - dont build it yet.. i want as easy as possible way to auto
detect - when our trainer xbe runs, it should scan all the trainers and update the ini file for any new ones, that we we are guarenteed its always up to date").

Today a trainer is found through the ini alone: the game's title ID names a section, the section's `trainer =` names one file. A new `.etm` in
`C:\OXOS\trainers\` is not used until someone adds a section for it, and a title with several trainers (regions, title updates, cheat counts: the pack has
more than one for 273 titles) always gets the one the section names, even when it was made for another build of the game.

### 7.1 What decides which trainer fits
Two things, and nothing else (region and PAL/NTSC only matter when a regional release is a different build or has its own title ID):
- **The title ID:** the trainer lists up to three (`.etm` 0x12); the game's certificate has one.
- **The build:** 1,073 of the pack's 1,084 trainers compare the game XBE's build timestamp (image header +0x114, at 0x10114 in memory) with the one they were
  made for, and patch nothing when it differs. A title update is a new build (a new timestamp, the same title ID), so each update needs its own trainer. The
  timestamp sits in the trainer's code (`cmp dword [0x10114], imm32`), in plain code or, in some trainers, in a body XOR-encoded with the trainer's first title
  ID (the decoder is the trainer's entry; Splinter Cell Chaos Theory's +12 is one), or XORed with a checksum of the trainer's own code (38 trainers, 15 of them
  with the constant out of a simple reading's reach). The other 11 have no check: they fit any build of their title ID, and patch the wrong places on a build
  they were not made for.

### 7.2 The index: what each trainer file fits
Not built: the sections' `trainer N` lines (§2) hold the same, per title. The proposal was a section `[trainers]` at the top of `oxos_trainers.ini`, one line
per file in `C:\OXOS\trainers\`:

```ini
[trainers]
Halo 2.etm = 4D530064 4158CC50
Halo 2 (p13).etm = 4D530064 01010001 42408C4E
Halo - Combat Evolved.etm = 4D530004 00033800 any
```
The value is the title IDs the trainer lists (as the file has them: some trainers fill an unused slot with other bytes, such as `01010001` above, which
match no game), then the build timestamp it checks for, or `any` when it has no check (or the check cannot be read). The examples are the pack's: Halo 2's
+13 is for the disc's build, the `p13` variant for the build of its Xbox Live title update.
`tools/gen_trainers_ini.py` writes it for the pack, and the menu keeps it up to date on the console (§7.3).

### 7.3 The menu keeps the ini up to date (built 2026-11-16)
Every time `oxos_trainers.xbe` starts (before a game, or on its own), first of all, before the game's section is read, so a trainer just copied in for this
game can be chosen at once:
1. List `C:\OXOS\trainers\` (the names of its `.etm` files: one directory read).
2. Read the ini and collect the trainer file names it knows: every `trainer =` and `trainer N =`, and the `[unusable trainers]` section's. Only the section
   lines and these lines are taken apart; the other lines only move their section's end.
3. Read only the files the ini does not know. A trainer is added to the section of each title ID it lists, as its next `trainer N = FILE | build ...` (when
   the section has no trainer lines yet, its own trainer becomes `trainer 1`); a title with no section gets one (`title` from the trainer's name, `trainer =`
   the file, `trainer_enabled = no`, every cheat as in the file). A file OXOS cannot use (not a trainer, no title ID, needs a dashboard's helper routines) gets
   `FILE = why` in `[unusable trainers]`, so it is not read again. The build is read from plain code only (`cmp` with the timestamp at 0x10114); a trainer whose
   check is encoded gets `unknown` (the pack's own trainers have theirs from the index).
4. Remove the lines of the files the ini names that are gone from the folder (user, 2026-11-17: delete a trainer file and the menu forgets it; only those
   lines change, no other trainer is read). Their `trainer N` and `[unusable trainers]` lines go. A section whose `trainer =` file is gone gets the first of
   its other trainers that can be read, switched off, with that trainer's cheats (as the menu writes a choice; the old cheat choices belonged to the deleted
   trainer); a section left without any trainer is removed, with the blank line and comment lines right above it, so the menu no longer comes up for that
   game (a menu already started for it starts the game at once). Nothing is removed when the folder cannot be listed to its end. A file copied back later
   is new again (step 3).
5. Write the ini once, when something changed (an empty or missing ini gets a first line saying what it is). A new trainer for a section without a
   `trainer =` becomes its trainer.
6. Write how long each part took to `C:\OXOS\log\oxos_trainers_scan.log`, with `logging_enabled = yes` (step 3 of §1).

Started on its own (from a dashboard, with no game), the menu then shows "TRAINER REFRESH COMPLETE" (or "TRAINERS UP TO DATE"), how many trainers it added,
how many files are no usable trainers, how many deleted trainers it removed and how many there are, and A leaves to the dashboard: the way from an empty ini to every trainer in it (in xemu: an
empty ini and the 1,084 trainers, 551 sections built in 1.3 s; 14 s on the console). Starting any game that has a section does the same for the trainers
that came or went since.

With more than 50 new trainers the menu opens its screen first and shows "TRAINER REFRESH IN PROGRESS", "NEW TRAINERS: 838 OF 1076" and a progress bar while it
reads them, redrawn about ten times a second, instead of a black screen (user, 2026-11-16).

Measured on the console (2026-11-16, Modxo console, 1,084 trainers, the 245 KB ini):

| Case | Time |
|---|---|
| nothing new: every normal start | 152 ms: listing 39 ms, the ini read and its 1,360 names compared 113 ms |
| 5 new trainers | 204 ms: the 5 read and added 8 ms, the ini written 44 ms |
| an ini without trainers: all 1,084 new (never shipped) | 12.7 s once; 14.5 s with the refresh screen (Halo 2's section only, 1,076 new; recorded on the console); the next start 237 ms |

(The first version took 205 ms for the ini; reading only the lines that matter brought it to 113 ms.) A trainer for a title with no section yet gets its
section the next time the menu runs: the extension only starts the menu for a title that has a section (§4a step 1).

### 7.4 The extension picks the trainer that fits the build
In the title-loading event the extension already reads the XBE's header; it reads the build timestamp from it too, then:
1. The title's section and the `[trainers]` lines for its title ID.
2. The section's `trainer =`, if it fits this build (its timestamp, or `any`): that one (the user's choice is kept).
3. Else a trainer whose timestamp is this build's; it becomes the section's `trainer =` (written by the menu, which also writes that trainer's cheat lines,
   every cheat off: cheat numbers belong to one trainer).
4. Else one with `any`.
5. Else no trainer fits this copy: no menu, and the title starts directly (today it would get a menu whose cheats do nothing).

The extension never scans the folder itself: a launch with no trainer stays as fast as today.

### 7.4a Several trainers for the same build: the player picks one
Built 2026-11-16 as the player's choice between all the title's trainers (§4a: LEFT and RIGHT, those for another build crossed out); the proposal below
(only the trainers fitting the build, the default by cheat count) waits for §7.4.
Not every variant is another build: Halo 2 has five trainers for the disc's build (`Halo 2.etm` +13, `Halo 2 (p4).etm`, `Halo 2 (p2).etm`, `Halo 2 (p2) 2.etm`,
`Halo 2 (p1).etm`), with different cheats (and the +13's Infinite Ammo also keeps the player alive, 2026-11-15). When more than one trainer fits the running
build:
- **The default** is the one with the most cheats; on a tie, the title's default trainer in the pack's index (`default_for`) before the others, then by
  file name.
- **The menu shows a selector** under Start Game: `TRAINER 1 OF 5` with the trainer's name and author; LEFT and RIGHT switch to the next fitting trainer, and
  the cheat list below changes with it.
- **The choice is remembered** per title: the section's `trainer =` names it, and it is kept as long as it fits the running build (§7.4 step 2).
- **Cheat settings belong to one trainer** (cheat 1 of one trainer is not cheat 1 of another): switching to another trainer writes that trainer's cheat lines,
  every cheat off, and switching back starts from off again. Keeping each trainer's settings is not worth the bigger ini.
- With one fitting trainer, the selector is not shown.

### 7.5 Title updates: the menu for the update, not the disc (built 2026-11-16)
How a game finds its update, read from Halo 2's XBE (build 4158CC50; the XDK's start-up code the game carries, so every XDK game does the same, whether
it runs from a disc, an image of one, xemu's Load Disc or a folder on the hard disk):
1. At start-up, before the game's own code, unless the title is the dashboard (FFFE0000): the running XBE's path (`XeImageFileName`) must lie in the folder
   D: names (the target of `\??\D:`: `\Device\CdRom0` for a disc; the game's folder for one on the hard disk), unless the launch data says the title was
   restarted from there (then the path it records is used).
2. Its title data folder T: (`E:\TDATA\<title ID>\`) holds `$u\contentmeta.xbx`, a signed description of the update: the code checks a 20-byte hash and
   that this title ID is among the ones it lists, and reads the update XBE's name.
3. It reads that XBE's header (`T:\$u\` plus the name of the running XBE's file, `default.xbe`) and compares its certificate with the running one.
4. All good: it launches `T:\$u\default.xbe` in its own place, the usual title launch (the path becomes
   `\Device\Harddisk0\Partition1\TDATA\<title ID>\$u\default.xbe`), recording where it came from in the launch data, so the update does not look for
   an update itself.

So the kernel sees two launches of the same title ID: the disc's XBE, which only checks and restarts, then the update's, the build that really runs.
OXOS shows the menu and runs the trainer only for the second:
- The title-loading event, for an XBE that is not in `E:\TDATA\<title ID>\$u\` itself: when that folder holds `contentmeta.xbx` and an XBE of the running
  XBE's name, no menu; the log says "the title update in E:\TDATA\4d530064\$u starts next: the trainer menu comes with it". The title-loaded event skips
  the trainer the same way.
- The update's launch gets the menu as any other; the menu reads the update XBE's build, so the trainer made for the update is the green one (Halo 2's
  v1.1 update: the `p13` trainer, 42408C4E).
- OXOS only checks that the two files are there. When the game finds the update unusable (a broken signature, another version), it runs the disc's build
  without a menu; the log line says why the menu did not come.

Checked in xemu (2026-11-16) with Halo 2's ISO as the disc:
- Halo 2's update 5 (build 4600A62B, 2007), installed by the installer on the Halo 2 Multiplayer Map Pack disc after Halo 2 had run once: no menu at the
  disc's launch, the log line, then the menu at the update's (`...\TDATA\4d530064\$u\default.xbe`; no trainer is made for 4600A62B, so all eight show
  not compatible), and after START the update runs (its title screen says version 1.5). The installer copies the disc's files and only signs
  `contentmeta.xbx` (its first 20 bytes, zero on the disc).
- The same files copied unsigned: no menu, the log line, and Halo 2 ignoring the update and starting the disc's build (the case above).

A console, and a game folder on the hard disk as the disc, are still to be tried.

### 7.5a A game whose build has no trainer: the player can see why
With §7.4 a title that has trainers, but none for the build on this console, starts with no menu, which is right; but the player has no way to know why. So:
- **The extension logs it** at that launch, one line in `C:\OXOS\log\extensions.log` (no other disk write in the title-loading event):
  `[oxos_trainers] 55530041: trainers for this title, none for its build 421A8501.` (The release has the extension's log off, `logging_enabled = no`:
  this needs a place of its own.)
- **The menu, started on its own,** reads those lines and shows them under the scan's result (§7.3): "No trainer for the build on this console:" and one line
  per title (its name from the ini section, the build), the latest first, each title once. A title that has since got a fitting trainer (a new `.etm`) is
  left out.

### 7.5b A title that starts the same build again: no second menu (built 2026-11-18)
A title may start a new launch of itself straight after it starts: it restarts itself once at start-up (after preparing its cache drive, for example), or
goes on to another XBE of its own. Each launch went through the title-loading event as a new one, so the player got the menu a second time, for the same
title, right after pressing Start Game. But a launch of the same title ID is not always the same trainer's: a title update, or an XBE of the title built
apart, is another build, and a trainer is made for one build (§7.1). The menu tells builds apart (its green and amber trainers, §4a); the title-loaded event
only checks the title ID (§3 step 5). So the rule follows the build:

| The launch | What the extension does |
|---|---|
| The title the menu started (§4a step 5) | remembers its launch number; the title ID and build were remembered with the menu's launch (step 2) |
| The very next launch: the same title ID and the same build (XBE timestamp, read as in step 1) | no menu; logged: `<title ID>: the title launches the same build again (<build>): no menu (<path>).`; the title-loaded event runs the trainer the section names, as at the launch before; its launch number becomes the one remembered, so a title that restarts itself again goes through too |
| The very next launch: another build, or another title ID | the usual decision (§4a step 1): the menu, which shows the trainers made for that build |
| Any later launch (another title came between) | the usual decision |

Title updates (§7.5) are not affected: the disc's XBE that only starts its update gets no menu and is not remembered, so the update's launch gets the menu
as before. When OXOS does not see the update coming (§7.5's check finds no update XBE of the disc XBE's name), the menu comes for the disc's build and again
for the update's, which is another build.

The state page holds the title's path, title ID, build and launch numbers (`MENU_STATE` in `oxos_trainers.cpp`).

Checked in xemu (`make test_extensions`, 2026-11-18), two boots of the relaunch test (§5):
- The test title relaunching itself as `media\second.xbe`, the same XBE: the menu at the cold boot only, where the pad chooses the second trainer and
  switches it and its cheat 1 on; the log line above for the relaunch; the trainer applied at both runs, mark 0xC0DE0002 at the last exit.
- `media\second.xbe` with its timestamp changed (`boot_test.py --other-build`): the menu at the cold boot (START) and again at the relaunch, where the pad
  chooses; no "same build" line; the trainer applied once.

### 7.6 Tests
- Host: the index line of every pack trainer (title IDs, build timestamp or `any`, plain and XOR-encoded checks) against `third_party/xbox_trainers/index.csv`;
  the choice of §7.4 for a title with several builds.
- xemu (`make test_extensions`): a second test trainer for another build of the test title is ignored, the one for its build is used; a new `.etm` copied to
  the disk gets its index line and section on the menu's next run; the test title restarting itself shows no second menu (built, §7.5b); two trainers
  for the test title's build show the selector, RIGHT then Start Game runs the second one and the ini names it; with only trainers for another build, no
  menu and the log line.
- Console: Halo 2 (the +13 and its variants for other builds), a game with a title update.

