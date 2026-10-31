# oxos-isonator

Makes game folders that dashboards list and start as games, from images of your own discs or from extracted game
folders. It runs on any platform with Python 3.8 or later and uses only the standard library.

```
Halo 2/
    Halo 2.iso       the disc image: an XISO, or Halo 2.1.iso + Halo 2.2.iso with --split
    default.xbe      the launcher
```

The launcher is the OXOS [ISO launcher](../iso-launcher) with the game's identity copied in: title ID, title name,
alternate title IDs, region, ratings, disc number, version, and the title image (`$$XTIMAGE`). Dashboards such as
XBMC4Gamers, EvoX and the OXOS title launcher therefore show the game itself, with its name, artwork and save data.
Starting it mounts the image in its folder as the console's disc through the virtual disc device that OXOS and Cerbios
share (`\Device\CdRom1`, [`spec/oxos-internal/16-virtual-discs.md`](../../spec/oxos-internal/16-virtual-discs.md),
[`spec/cerbios/07-virtual-discs.md`](../../spec/cerbios/07-virtual-discs.md)). It then starts the game the way a disc
in the drive starts. The same folder works on OXOS and on Cerbios.

## Usage
```
make -C kernel iso-launcher                      # once: builds tools/iso-launcher/bin/default.xbe, the template

# An extracted game (default.xbe at its top): build an XISO in OUT/<name>/ and its launcher
tools/oxos-isonator/oxos-isonator.py "extracted/Halo 2" -o games

# Images you already have: only the launcher, next to each image
tools/oxos-isonator/oxos-isonator.py "games/Halo 2/Halo 2.iso"
tools/oxos-isonator/oxos-isonator.py games/                       # every game folder (or every image) in games/
tools/oxos-isonator/oxos-isonator.py xiso/ --own-folder           # a folder of loose images: one folder each

tools/oxos-isonator/oxos-isonator.py --info xiso/                 # what each image holds
```

| Option | What it does |
|---|---|
| `-o DIR` | where games built from folders go (default: the current folder) |
| `--split` | cut an image over 4 GB into `NAME.1.iso` + `NAME.2.iso`, for FATX disks (Cerbios, stock partitions). Without it an image is one file: exFAT (OXOS) has no file size limit. An existing image is split in place: the tail is copied to part 2 and the file is shortened to part 1, so no second copy of the whole image is made. |
| `--own-folder` | move each image (and its second part) into a folder named after it. The launcher starts the image in its own folder, so every game needs a folder of its own. |
| `--name NAME` | the folder and image name for a game built from a folder (default: the source folder's name) |
| `--title TEXT` | the title name dashboards show (default: the game's own) |
| `--verify` | read a built image back and compare every file with the source |
| `--force` | replace a `default.xbe` that is not a launcher made by this tool |
| `--launcher XBE` | another ISO launcher build as the template |
| `--info` | only print each image's kind, size, title and whether it has a title image |

## Details
- **Images read:** XISO, and full disc dumps (the game partition at 0x18300000, recognised as Cerbios and OXOS do it),
  whole or split. CCI images are read by the consoles, not by this tool; `tools/mkcci` makes them.
- **Images written:** XDVDFS with the volume descriptor at sector 32. Each directory is a balanced binary tree of
  entries in case-insensitive name order, with no entry crossing a sector and 0xFF as padding. Files are written after
  the directories, each starting on a sector. File attributes are as on retail discs (0x80 files, 0x10 folders).
  Only the files themselves are stored, so an image built from a folder is the smallest XISO of that game.
- **Folders read:** a folder whose `default.xbe` (any letter case) is the game itself is built as a game, even when
  it holds `.iso` data files of its own; a folder with no `default.xbe`, or only a launcher, is a folder of images.
- **Names:** XDVDFS names are 8-bit characters, up to 255 bytes. On FATX the folder and image names must be 42
  characters or fewer.
- **The launcher template** must be a plain ISO launcher with no title image. A `default.xbe` already in a game folder
  is replaced only when it is an OXOS ISO launcher (any build, so running the tool again updates the launchers), so a
  game's own `default.xbe` is never overwritten without `--force`.
- **On start** the launcher mounts the image, shows the OXOS boot splash for a second (a quick fade in and out, as a
  dashboard does when it starts a title), then starts the game. It shows text only when something is wrong (no
  image, a failed mount).
- Repackinator (Team Resurgent) does the same job for Cerbios with its own launcher. This tool is a separate
  implementation (OXOS is GPL-2.0, Repackinator GPL-3.0), and its launcher is the OXOS one.
