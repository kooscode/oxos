# 42. Play time: recorded on the console, sent online by the XBL dashboard (proposal)

Status: **proposal**, not implemented (user, 2026-11-15: "update it to a file ... and then when the xb-live dashboard starts, it syncs any updated play
times to online service and clears the offline playtime counter"). Nothing here is decided until the user says so.

How long each game is played, recorded on the console with no network while the game runs, kept on the console for anyone to read, and sent to an online
service the next time the XBL Dashboard starts. The extension only writes files; the dashboard, which has the network anyway, does the sending.

Why not send while the game runs: titles link their network stack (XNet) statically, with no exports, a different build per XDK version, and many games have
none; the network card belongs to the title while it runs (spec 31). Calling into a title's XDK from an extension is fragile and needs Microsoft code
patterns. A file needs none of that and works for every game, offline ones too.

## 1. Who does what
| Part | Does | Built by |
|---|---|---|
| `oxos_playtime.xos` (a kernel extension, spec 34) | keeps the totals per title in `C:\OXOS\playtime\playtime.txt` and records each session in `playtime.log` | OXOS |
| Any dashboard or app | reads `playtime.txt` to show the time played per game, with no network | anyone |
| The XBL Dashboard | at start: sends the file's records to the service, then removes what was sent | the XBL team (OXOS can give a reference implementation of the file handling) |
| The online service | stores play time per console and title; keeps one copy of each session | the XBL team |

## 2. Recording (the extension)
- **Title loaded** (`oxos_on_title_loaded`): a session starts: title ID, the title's name, start time (the console's clock, `KeQuerySystemTime`).
- **While the game runs** (`ExtensionRunLoop`): every N minutes (a setting, default 1) a progress line with the time played so far, so an in-game reset or a
  power-off loses at most N minutes. The current session is also kept in the extension's state page (spec 34 §6), which survives quick reboots.
- **Title exiting** (`oxos_on_title_exiting`): the final line.
- **The totals** in `playtime.txt` (§3.1) are updated at each progress line and at the final one.
- **Not counted:** dashboards and OXOS's own apps (the FTP server, launchers): a list of title IDs to skip, in the settings.
- **Settings** in `oxos_extensions.ini`:
```ini
[oxos_playtime]
extension = oxos_playtime.xos
save_every = 1        ; minutes between progress lines
skip = FFFF0002, ...  ; title IDs not counted (dashboards are always skipped)
```

## 3. The files

### 3.1 The totals: `playtime.txt`, kept on the console
`C:\OXOS\playtime\playtime.txt` (exFAT disk: `/C/OXOS/playtime/`): one line per title, never cleared by the sending. The console's own record of its play
time: any dashboard, app or FTP user can read it.

```
title=4D530064 name=Halo 2 seconds=86412 sessions=37 first=2026-09-02 last=2026-11-15
title=4D530004 name=Halo seconds=12030 sessions=9 first=2026-10-11 last=2026-11-14
```

- **`seconds`:** total time played; **`sessions`:** how many times it was started; **`first`** / **`last`:** the dates it was first and last played.
- **Always whole:** the extension writes the new totals to `playtime.new`, then renames it over `playtime.txt`, so a reader never sees a half-written file. It
  writes only while a game runs, never while a dashboard does.
- Deleting the file starts the totals over; the send queue (§3.2) is separate.

### 3.2 The sessions to send: `playtime.log`
`C:\OXOS\playtime\playtime.log` (exFAT disk: `/C/OXOS/playtime/`), plain text, one line per record, appended only:

```
session=0012-00000042-20261115T193012 title=4D530064 name=Halo 2 start=2026-11-15T19:30:12 seconds=1860 final=no
session=0012-00000042-20261115T193012 title=4D530064 name=Halo 2 start=2026-11-15T19:30:12 seconds=2412 final=yes
```

- **`session`:** console ID (from the EEPROM), the cold-boot count, the start time: unique per session, the same in all its lines.
- **`seconds`:** time played in that session so far; the last line of a session is its total, `final=yes` when the title ended normally.
- Text, so it can be read and checked by hand or over FTP.

## 4. Sending (the XBL Dashboard, at start)
1. If `playtime.log` exists, rename it to `playtime.sending`. From then on the extension writes a new `playtime.log`; nothing recorded later is lost or
   sent twice. (If `playtime.sending` is already there from a failed attempt, send that first.)
2. For each session, keep its last line; send them to the service.
3. When the service confirms, delete `playtime.sending`. If sending fails, leave it for the next start.

**No double counting:** a session is identified by `session`, so sending the same file again (a crash after sending, before deleting) changes nothing: the
service keeps one record per session, the one with the most seconds.

## 5. Things to settle
- The service's address and format, and how the console is identified there (the console ID, or the user's XBL account): with the XBL team.
- Whether the dashboard shows the totals per game: from `playtime.txt` on the console, or from the service across consoles.
- The clock: a console without its clock set still records durations; start times are then wrong. Send the dashboard's own time with the upload so the
  service can correct them.
- Privacy: play time leaves the console only through the dashboard; a setting to turn the extension off (`enabled = no`) already exists for every extension.

## 6. Tests
- xemu: a test title runs for a known time with the extension; `playtime.txt` has its total and `playtime.log` its session lines; a quick reboot mid-session
  keeps one session; a reboot without exit leaves a session with `final=no` and its last progress line.
- Host: a unit test of the dashboard-side reference code: rename, keep the last line per session, delete on success, keep on failure.
