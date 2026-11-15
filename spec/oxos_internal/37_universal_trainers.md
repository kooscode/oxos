# 37. Universal Trainers: a trainer menu before any game, from any dashboard or disc (proposal)

Status: **proposal**, not implemented (user, 2026-11-15: "a trainer selection xbe that we redirect to for any game that we have a trainer for ... so no
dashboard specific trainers required, we handle it at kernel level"). Nothing here is decided until the user says so.

Today a trainer runs only when a dashboard installs it: XBMC4Gamers patches the kernel's title-start call (spec 23) and loads the trainer itself. With built-in
trainers, OXOS does it for every launch, from any dashboard, a disc, a disc image or another title: when a title with a trainer starts, a small trainer menu
comes first; the user switches the trainer and each of its cheats on or off, and the game starts with them.

## 1. What the user sees
1. A title is launched: from any dashboard, the File Manager, a launcher, a disc image (the ISO launcher, or an image mounted as the disc in xemu) or an
   original DVD. Only when XBMC4Gamers launches it with its own trainer installed does OXOS leave it to XBMC4Gamers (spec 23 §5).
2. If `C:\OXOS\trainers\` has a trainer for that title, the **trainer menu** starts instead: the game's name, the trainer's name and author, and its cheats as
   a list with on/off switches (A toggles, the D-pad moves).
3. **Start** launches the game with the trainer and the chosen cheats; **B** launches it without the trainer. The choice is remembered per title.
4. The game starts as if launched directly (same path, same launch data), with the trainer installed.

Skipping the menu: a title the user marked "don't ask" starts straight away with its remembered choice; holding a button during the launch (which one is open)
opens the menu anyway.

## 2. How it works
| Step | Where | What |
|---|---|---|
| 1. Which title | the kernel's launch (`titles::LaunchTitle`, spec/stock/05) | read the XBE's certificate (title ID, version) before loading it; look for a trainer for that title ID |
| 2. Redirect | as launch redirects do (spec 33) | start `C:\OXOS\oxos_trainer_menu.xbe` instead, passing the game's path, title ID and the original launch data in the launch data page |
| 3. The menu | an nxdk + SDL3 app (`tools/sdl_screen`, like the other OXOS apps) | reads the trainer's names and cheats, shows them, writes the choice to `C:\OXOS\oxos_trainers.ini`, launches the game with a marker in the launch data so step 2 does not redirect again |
| 4. Install | the title-loaded event (spec 34 §7.2), before the game's first instruction | load the `.etm`, set its cheat switches, run it as XBMC's stub does (spec 23 §1), keep it resident for the whole game |

**The trainer file (`.etm`, studied 2026-11-15 on the 998 trainers of `third_party/xbox_trainers`):** a header with the file size (0x00), format 1 (0x04),
the offset of the cheat switches (0x0A, one byte per cheat, 1 = on), of the text table (0x0E: trainer name, author, then one name per cheat), of up to three
title IDs (0x12; 446 trainers list more than one, mostly regions) and the entry point (0x16). 966 of the 998 check the game's XBE build timestamp (0x10114)
before patching, so a trainer for another version of the game patches nothing; 32 do not check. The menu needs only the header and the text table.

**Choices file (`C:\OXOS\oxos_trainers.ini`),** editable by hand or over FTP as well as from the menu and from OXOS Setup (spec 39):
```ini
[4D530064]          ; Halo 2
trainer = Halo 2.etm
enabled = yes
ask = yes           ; no: start with these settings, without the menu
Infinite Ammo = on
Never Die = on
Rapid Fire = off
```

## 3. Where the code lives
Universal Trainers run only for users who want them, so by spec 34 §10 they belong in an extension, `oxos_trainer.xos`, rather than the kernel: its
title-loading event finds the trainer and redirects, its title-loaded event installs it. It needs from the extension interface: replacing the launch's XBE path
(the title-loading event, as `oxos_nettitle.xos` will, spec 34 §10) and memory that stays while the title runs. It is a run-once extension (spec 34 §4.3a): it
installs the trainer in the title-loaded event and is unloaded before the game starts. Only the trainer stays resident, in its own block (a trainer's hooks
jump into its code, so the trainer itself cannot be freed while the game runs; Halo 2's is 1.3 KB, one 4 KB page). Safe mode (B at boot) turns it off with the
other extensions.

## 4. Things to settle before building it
- **The trainer's calling contract:** which registers and memory XBMC's stub sets up before calling a trainer, and whether some trainers call XBMC's helper
  routines (physical 0x10 / 0x14, spec 23 §1). Read from XBMC4Gamers' stub, then tested on several trainers on the console.
- **Memory:** the trainer stays resident during the game (XBMC reserves 0x3C00 bytes). It must come out of the title's memory without making the gap to the
  stock kernel larger (OXOS gives games 4–36 KB less than stock today, under 0.06 % of the 64 MB, spec 18 §7), or be accounted as accepted.
- **Two installers:** when a dashboard such as XBMC4Gamers has also installed its trainer hook (spec 23), only one may run: OXOS steps aside, or the menu says
  so.
- **Wrong game version:** for the 32 trainers without their own check, record the XBE timestamp each was made for and refuse a mismatch.
- **Several trainers for one title:** the trainer pack's `variants/` (other regions, versions, cheat counts): the menu lists them all and remembers the pick.
- **Cheats bound to in-game buttons** ("IGKey", 15 in the pack) work while playing; the menu marks them so.
- **The other trainer format** XBMC supports (`.xbtf`, xboxtrainers.net): later, if wanted.

## 5. Tests
- Host: unit tests of the `.etm` reader on the trainer pack (names, cheat counts, title IDs) and of the choices file.
- xemu: a test title with a test trainer (own, written for the test: patches a value the title reports) through the menu, with and without the trainer.
- Console: the Halo 2 trainer, as checked through XBMC4Gamers on 2026-10-31 (spec 23).
