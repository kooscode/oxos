# Xbox game trainers (.etm)

Game trainers for XBMC4Gamers, which OXOS supports (spec/oxos-internal/23-trainer-hook.md). They come from the
Internet Archive's "Xbox Original ETM Trainers" collection (https://archive.org/details/ETMTrainers, 2017). Here
they are de-duplicated and renamed, with their contents unmodified.

| Path | |
|---|---|
| `trainers/` | 505 trainers, one per game: copy these to the console |
| `variants/` | 493 other versions of trainers for the same games (other regions, game versions, cheat counts) |
| `index.csv` | every file: game title ID, number of cheats, region, the trainer's own title, MD5, original file names |
| `SHA256SUMS` | checksums of every trainer |

## Using them
1. Copy `trainers/` to `E:\Apps\XBMC4Gamers\system\trainers` (XBMC4Gamers' `Q:\system\trainers`), or to the
   folder set as the trainer path in XBMC's program settings.
2. Restart XBMC. Games with a trainer get a trainer icon.
3. In the game's context menu (White button), open the trainer settings, enable the trainer and its cheats, then
   launch the game. The LED flashes as the trainer loads.

A trainer patches one version of a game. If the chosen one crashes or does nothing, try that game's other version
from `variants/`. Each file name says its region and number of cheats (`p13` = 13 cheats).

## How the set was made
- The collection has 2,986 .etm files, of which 1,001 are unique by MD5.
- Each trainer is grouped by the title ID in its header. XBMC matches on the same ID; the file names were unreliable
  (one name was used by 9 different games).
- The trainer kept per game has the most cheats, preferring non-beta builds.
- It is named after the game's title ID entry in the title-ID database (https://mobcat.zip/XboxIDs/).
- File names are FATX-safe and at most 42 characters.
- Three files were left out:
  - a RAR archive named .ETM;
  - a file in another format;
  - a trainer with an empty title ID.

## Licence
These files are not OXOS's and are not covered by OXOS's licence (GPL-2.0).
- They were written by members of the Xbox scene: their authors are named in each trainer (for example Evox-T and
  xboxtrainers.net), and its title is listed in `index.csv`.
- They carry no licence terms. They are redistributed unmodified, as the Internet Archive publishes them, for use with
  games you own.
- An author who wants a trainer removed can open an issue, and it will be removed.
