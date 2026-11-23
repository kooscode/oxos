# oxos_igs: in-game shortcuts

A kernel extension: buttons held together on a controller, in any title, reset the console or start an XBE at once. It replaces the in-game reset:
LT + RT + BACK + BLACK goes back to the dashboard, as before. Works the same in every title, whatever it was built with (the pads are read from the USB
controllers by the kernel).
Design: [`spec/oxos_internal/48_in_game_shortcuts.md`](../../../spec/oxos_internal/48_in_game_shortcuts.md).

## Install
| File | On the console |
|---|---|
| `oxos_igs.xos` (`make -C kernel extensions` builds it into `build/kernel/extensions/oxos_igs/`) | `C:\OXOS\extensions\oxos_igs.xos` |
| `oxos_igs.ini` (copied next to it from this folder) | `C:\OXOS\extensions\oxos_igs.ini` |

and in `C:\OXOS\oxos_extensions.ini` (the release lists it with `enabled = yes`):

```ini
[oxos_igs]
extension = oxos_igs.xos
enabled = yes
```

## Shortcuts
One `[section]` per shortcut in `oxos_igs.ini`; the file explains each key. As shipped:

| Shortcut | Buttons | Does |
|---|---|---|
| `[dashboard]` | LT + RT + BACK + BLACK | powers the console off and on (`action = coldboot`) |
| `[ftp]` (commented out) | BLACK + WHITE | starts the FTP server at once (`action = direct`, `target = C:\OXOS\apps\oxos_ftp.xbe`) |

- `action = direct` without a `target` goes to the dashboard by a quick reboot; `warmboot` resets the console.
- `toast_message` shows a message first, for `toast_timeout` seconds (default 2); the action follows `launch_delay` seconds after it shows (0 to 5,
  default 1; 0: at once, without the message). When the message cannot show within a second, the action comes at once.
- A shortcut fires when exactly its buttons are held, no others, and once; release its buttons to use it again (nothing fires until then). Buttons
  still held when a title starts do not fire it.
- A `direct` shortcut whose target is already running does nothing.
- A broken section (an unknown button, one button only, an unknown action, a target that does not exist) is left out and logged
  (`logging_enabled = yes` under `[oxos_igs]`: `C:\OXOS\log\extensions.log`); the others still work.
- Without the extension (safe boot with B held, or switched off), the kernel still powers the console off and on when LT + RT + BACK + BLACK are held
  for 3 seconds.
