# 48. In-game shortcuts (IGS): an extension

Status: **implemented 2026-11-23**. OXOS's own design. Code: `kernel/extensions/oxos_igs/` (`oxos_igs.cpp`, the ini's reading and the firing rule in
`shortcuts.h`, the shipped `oxos_igs.ini`); the kernel's side is spec 22 §9 (`kernel/src/hardware/usb/pad_reports.cpp`) and spec 34 §7.3. Tests:
`tests/unit/test_in_game_shortcuts.cpp`, `test_toasts.cpp`. It replaces the in-game reset (spec/oxos_internal/22, IGR): the user, 2026-11-23: "lets
call IGR IGS (In Game Shortcuts)", an extension `oxos_igs.xos` configured in `oxos_igs.ini` like the trainers' extension, the default buttons as the in-game
reset's, a shortcut for the FTP server on BLACK + WHITE (commented out), and actions for a cold boot, a warm boot and a direct launch.

User, 2026-11-23: "IGR becomes IGS instead"; the shortcuts are an extension with an ini file like the trainers', the default buttons as today's in-game
reset, a commented-out shortcut for the FTP server on BLACK + WHITE, and actions for a cold boot, a warm boot and a direct launch.

## 1. What it is
- `C:\OXOS\extensions\oxos_igs.xos`, listed and enabled in the shipped `oxos_extensions.ini` (spec 34 §3.1), so it runs over every title, dashboards
  included. Its settings are `C:\OXOS\extensions\oxos_igs.ini`, next to it (spec 34 §10's names).
- It gets the pads' state from the kernel (spec 22 §9, through spec 34's pad events), so it covers every title however it was built.
- A shortcut is a set of buttons held together and an action. A shortcut fires when exactly its buttons are held, no others (user, 2026-11-23: "we only
  match EXACT buttons"), once: its buttons must all be released before it, or any other, can fire again.

## 2. `oxos_igs.ini`
```ini
; OXOS in-game shortcuts (spec/oxos_internal/48). One section per shortcut; the section's name is any word, shown in the log.
;   buttons = names joined by +, held together (at least two): a b x y black white ltrigger rtrigger padup paddown padleft padright start back
;             lthumb rthumb (the sticks pressed). Analog buttons count as held past half way.
;   action  = coldboot   power the console off and on (through the SMC): the BIOS, netboot and the disc start again
;             warmboot   the console's reset (through the SMC)
;             direct     stop the title and start target at once (a quick reboot); without target, the dashboard (oxos_settings.ini [dashboard])
;   target  = the XBE for direct (C:\..., E:\..., F:\..., G:\..., X:\ to Z:\, or a \Device\ path)
;   toast_message = text shown on screen first (optional, up to 42 characters)
;   toast_timeout = seconds the message stays up: 1 to 30 (default 2)
;   launch_delay  = seconds from the message showing (without one: from the press) to the action: 0 to 5 (default 1); 0 acts at once, without the
;                   message

[dashboard]
buttons = ltrigger + rtrigger + back + black
action = coldboot
toast_message = IN GAME RESET
toast_timeout = 2
launch_delay = 1

;[ftp]
;buttons = black + white
;action = direct
;target = C:\OXOS\apps\oxos_ftp.xbe
;toast_message = Starting the FTP server
;toast_timeout = 2
;launch_delay = 1
```
- The `[dashboard]` shortcut is today's in-game reset: the same buttons and the same power cycle (spec 22 §6). `direct` without a target is the quick reboot to
  the dashboard of spec 22 §8, once its tests (spec 22 §8.6) pass.
- Errors (an unknown button name, fewer than two buttons, an unknown action, `direct` with a target that does not exist or on a drive OXOS does not know,
  a `toast_message` over 42 characters, a `toast_timeout` or `launch_delay` out of range or not a whole number) are logged and the shortcut is skipped;
  the others still work. No file, or no valid shortcut: the extension does not stay loaded (the kernel's last resort, §5, still works). At most 16
  shortcuts.
- The user, 2026-11-23: the shipped `[dashboard]` toast reads "IN GAME RESET".

## 3. What the extension does
1. `ExtensionStart`: lists its sections (`oxos_ini_sections`, spec 34 §5.2), reads each (`oxos_ini_section`), checks them, keeps the valid ones.
2. Registers for pad changes (spec 34's `oxos_on_pad_changed`); the callback runs on the run-loop thread, at passive level, for each change of any pad's
   buttons.
3. On a change of any pad: the shortcut whose buttons are exactly the ones held fires; it is then blocked until none of its buttons is held, and until
   then no shortcut fires (letting go of one button of a combination does not fire a shorter one). A combination still held from the title before does not fire again in the next one:
   the kernel keeps the last buttons across the quick reboot and sends no change for them (spec 22 §9.2). While a shortcut waits for its toast (4.), no
   other fires. One state over all pads, not one per `Port`: nxdk titles move one pad from port to port (spec 22 §9.2).
   - `coldboot` and `warmboot`: `oxos_console_reset(OXOS_RESET_COLD)` / `(OXOS_RESET_WARM)`;
   - `direct`: `oxos_title_launch(target)`, the target's drive letter turned into its `\Device\` path first (or `oxos_title_launch(nullptr)` for the
     dashboard): the kernel suspends the title's threads, writes the launch data page and quick-reboots (spec 22 §9.3). A target that is the running
     title (`oxos_title_path()`) does nothing: no restart of the FTP server from the FTP server.
4. **With a `toast_message`:** before acting, it queues the toast (spec 36, `oxos_toast_enqueue`, bottom centre, `toast_timeout` seconds) and acts
   `launch_delay` seconds after the toast's "shown" event (user, 2026-11-23: "how long to wait before launch (i.e. time to read toast)", in whole seconds,
   0 to 5, 0 at once, default 1), so the user can read what is about to happen; the toast hiding first does not hasten it. The action never depends on
   the toast: when the toast cannot be queued (toasts off, the queue full) or is dropped, or no "shown" arrives within one second, it acts at once. Without a
   `toast_message`, `launch_delay` counts from the press; `launch_delay = 0` acts at once and shows no toast.
5. Logs each firing (`[oxos_igs] [dashboard] back + black + ltrigger + rtrigger: coldboot`, with `logging_enabled = yes`).

## 4. What the kernel provides (spec 34)
- The pads' state from the USB host controllers' done lists (spec 22 §9.2), and change events to extensions.
- `oxos_title_launch(Path)`: spec 22 §9.3's steps; `oxos_console_reset(Kind)`: spec 22 §6's power cycle and §7's reset.

## 5. The kernel's last resort
IGS is an extension: in safe boot (B held, spec 34 §3.3), without the kernel modules' cache, or with a broken `oxos_extensions.ini`, it is not loaded. The
kernel keeps one combination of its own on the same pad state: **LT + RT + BACK + BLACK held for 3 seconds** powers the console off and on (spec 22 §6). With
IGS loaded, its `[dashboard]` shortcut fires as soon as the buttons are held, so the last resort only acts when IGS did not.

## 6. What goes
- The `XInputGetState` hook (spec 22 §1-2, `xinput_hook.S`, `HalpIgrCheck`) and the combination in the kernel (spec 22 §3): removed 2026-11-23 (the user:
  "we will rip out IGR"), replaced by spec 22 §9's pad state and §5.
- Spec 22 §8.3's two combinations: the ini's shortcuts take their place.

## 7. Tests
- Host unit tests: the ini's parsing (button names, actions, errors), the firing rule (all held, once until released, first match), on recorded pad states.
- xemu (`test_controller`, the virtual pad): a shortcut with `direct` to a test XBE fires and the XBE starts; a `coldboot` shortcut reboots; the last resort
  after 3 seconds with IGS off.
- Consoles: the three encoders; an XDK game, an nxdk dashboard, an nxdk app; both controller ports.
