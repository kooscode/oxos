# Xbox game trainers (.etm)

Game trainers for OXOS's Universal Trainers (spec/oxos_internal/37) and for XBMC4Gamers, which OXOS also supports (spec/oxos_internal/23_trainer_hook.md).
Most come from the Internet Archive's "Xbox Original ETM Trainers" collection (https://archive.org/details/ETMTrainers, 2017); 127 more from two other
trainer packs that circulate in the Xbox scene. Here they are de-duplicated and renamed, with their contents unmodified.

| Path | |
|---|---|
| `trainers/` | 1,084 trainers for 551 title IDs: all of them, several for many games (other regions, game versions, cheat counts) |
| `index.csv` | every file: the game, its title IDs, the builds it fits, which titles it is the default trainer of, its cheat count, the buttons of its in-game-key cheats, region, the trainer's own name and author, MD5, original file names |
| `SHA256SUMS` | checksums of every trainer |

The release zip puts all of them in `C:\OXOS\trainers\`, and `tools/gen_trainers_ini.py` writes `oxos_trainers.ini` from `index.csv`: one section per title
ID with its default trainer, the title's other trainers in comments above it.

## index.csv
- **title_ids:** the title IDs in the trainer's header that are real ones (two capital letters and a number). The header's list often has no terminating 0, so
  the words after the last ID are other data (the cheat switches): those are left out.
- **builds:** the game build timestamps (XBE header +0x114) the trainer checks before it patches anything, in hex; `any` when it has no check (it patches any
  build of its game); `hidden` when it checks but the constant could not be read. A trainer only works on the build it names.
- **default_for:** the title IDs whose ini section names this trainer: the main trainer the first version of this pack chose for the game (the most cheats,
  non-beta), else the trainer with the most cheats.
- **keys:** `N = BUTTON+BUTTON, BUTTON+BUTTON; ...`: the controller buttons that switch cheat N during the game ("in-game keys", IGK), read from the trainer's
  code (below). RIGHT STICK and LEFT STICK mean the stick pressed in.

## Using them
- **OXOS:** they come with the release in `C:\OXOS\trainers\`; the trainer menu comes up before a game that has one.
- **XBMC4Gamers:** copy `trainers/` to `E:\Apps\XBMC4Gamers\system\trainers` (XBMC4Gamers' `Q:\system\trainers`), or to the folder set as the trainer path
  in XBMC's program settings; restart XBMC; in the game's context menu (White button), open the trainer settings, enable the trainer and its cheats, then
  launch the game.

A trainer patches one build of a game. If one does nothing, its `builds` in `index.csv` is not the build you have: try another trainer for the game. Each
file name says its region and number of cheats (`p13` = 13 cheats).

## How the set was made
- The Internet Archive collection has 2,986 .etm files, of which 1,001 are unique by MD5. Three were left out: a RAR archive named .ETM, a file in another
  format, and a trainer with an empty title ID.
- Two more trainer packs were merged in (2026-11-16): their files that are not already here by MD5, 127. Their `.xbtf` files (XBMC's other trainer format) are
  ETMs in an obfuscated wrapper (XBMC4Xbox, `CTrainer::Load`): unwrapped, 837 of 872 were byte for byte ETMs already here, 27 were new and are included as
  `.etm`. Two `.xbtf` files that need XBMC's helper routines (offset 0x1A), and three ETMs with a broken header or no title ID, were left out.
- Trainers that do the same are kept once (45 files): the same title IDs, the same builds, the same number of cheats and the same code, differing only in the
  saved on/off defaults, a state byte before the code, or the cheat names.
- Each trainer is grouped by the title ID in its header. XBMC matches on the same ID; the original file names were unreliable (one name was used by 9
  different games).
- File names come from the game's entry in the title-ID database (https://mobcat.zip/XboxIDs/), or the name this pack already uses for the game, plus
  region and cheat count; FATX-safe and at most 42 characters. The first version of this pack (2026-10-31) kept one trainer per game in `trainers/` and the
  rest in `variants/`; those files keep their names.
- **Builds** are read from each trainer's code: a compare with the timestamp at 0x10114 (`cmp dword [0x10114], imm32` and its variants), in plain code, in a
  body the trainer decodes itself at its entry (decoded first), or XORed with a checksum of the trainer's own code.
- **Keys** are read from each trainer's code: the trainers test the low byte of the controller's digital buttons (`and reg, MASK; cmp reg, MASK`; 0x80 the
  right stick pressed in, 0x40 the left, 0x20 BACK, 0x10 START, 0x01 to 0x08 the D-pad), often a stick test around D-pad tests, and then check a cheat's
  switch or flip a flag the cheat's code checks. They are listed only for cheats whose names mark them as in-game keys ("IGK", "IGKey", "key"); for a
  cheat whose name gives its buttons ("Max Health (Rt+U)": Rt is the right thumbstick), only where the code agrees. Checked against the cheats whose names
  give their buttons: 195 agreed, 10 differed (half of them only in how the name writes BACK, `<|`); where they differ, nothing is listed. Cheats whose
  buttons could not be read keep only their name.

## Licence
These files are not OXOS's and are not covered by OXOS's licence (GPL-2.0).
- They were written by members of the Xbox scene: their authors are named in each trainer (for example Evox-T and
  xboxtrainers.net), and its title is listed in `index.csv`.
- They carry no licence terms. They are redistributed unmodified (the `.xbtf` ones unwrapped to the ETM inside), for use with games you own.
- An author who wants a trainer removed can open an issue, and it will be removed.
