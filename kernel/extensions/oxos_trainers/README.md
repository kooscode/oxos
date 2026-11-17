# oxos_trainers: Universal Trainers

A kernel extension that runs a game's trainer when the game starts, from any dashboard, a disc image or an original disc: no trainer-aware dashboard needed.
Before a game that has a trainer, its menu (`oxos_trainers.xbe`, `app/`) shows the trainer and each cheat with a switch; A starts the game.
Design: [`spec/oxos_internal/37_universal_trainers.md`](../../../spec/oxos_internal/37_universal_trainers.md).

## Install
| File | On the console |
|---|---|
| `oxos_trainers.xos` (`make -C kernel extensions` builds it into `build/kernel/extensions/oxos_trainers/`) | `C:\OXOS\extensions\oxos_trainers.xos` |
| `oxos_trainers.xbe`, the menu (built next to it, from `app/`) | `C:\OXOS\extensions\oxos_trainers.xbe` |
| `oxos_trainers.ini` (generated next to it from the trainer pack) | `C:\OXOS\extensions\oxos_trainers.ini` |
| the trainers (`third_party/xbox_trainers/trainers/*.etm`) | `C:\OXOS\trainers\` |

and in `C:\OXOS\oxos_extensions.ini` (the release lists it with `enabled = no`: change it to `yes`):

```ini
[oxos_trainers]
extension = oxos_trainers.xos
enabled = yes
```

## Turning a trainer on
Start the game from a dashboard, a launcher or the File Manager: the menu comes first. Switch the trainer and the cheats you want on (D-pad, A), then Start Game.
The menu writes them to `oxos_trainers.ini`, which can also be edited by hand: it has a section for every title the pack has a trainer for, named by the title
ID, every trainer off.

```ini
; Halo 2: Halo 2 (NTSC/PAL) +13, Trainer By acidflash & dootdoo - Evox-T
[4D530064]
title = Halo 2
trainer = Halo 2.etm
trainer_enabled = yes
1. Infinite Ammo = yes
2. Rapid Fire = no
...
10. Never Die = yes
```

- `trainer_enabled = yes` runs the trainer; each cheat is `N. name = yes` or `no`. The number N is what counts: it is the cheat's number in the trainer, and the
  name is only for you.
- The log line in `C:\OXOS\log\extensions.log` says what happened: `[oxos_trainers] Halo 2: trainer Halo 2.etm applied, 2 of 13 cheats on.`
- A trainer patches one version of a game. If it does nothing, or the game crashes, turn it off again (or try that game's trainer in the pack's `variants/`).

## What it does, and does not do
- In the title-loading event of a launch through the launch data page, it reads the XBE's title ID from the file; for a game with a section and a trainer file
  it starts the menu in its place (`oxos_title_redirect`), and lets the menu's own launch of the game through. Other titles start with no delay.
- In the title-loaded event, before the game's first instruction, it reads only the running title's section (`oxos_ini_section`: the rest of the 200 KB file
  is never in memory), loads the trainer into memory that stays for the game, sets its cheat switches, and calls it as a dashboard's stub does (spec 23 §1):
  entered with `ebp` and `edx` at the trainer, the CPU's write protection off, so it can patch the game.
- **XBMC4Gamers:** a game started from XBMC4Gamers with one of its own trainers installed (its hook in the kernel) gets XBMC4Gamers' trainer, not this one.
- **ISO launchers and homebrew:** an nxdk program (its XBE's one library entry is `CXBE`) never gets a trainer or the menu, also when it carries a game's
  title ID, as OXOS's ISO launcher does: the menu and the trainer come with the game the launcher starts.
- **A disc the console starts by itself** (in the drive at power-on, or loaded with xemu's "Load Disc"): the menu comes first too. With no launch data page,
  the menu takes the game to be the disc, and starts it with a page the kernel frees as it starts the disc, so the game gets no launch data, as before.
- Trainers that need a dashboard's helper routines (none in the pack) are not run.
