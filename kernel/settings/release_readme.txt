OXOS @VERSION@ - Open Xbox Operating System

INSTALL
1. Flash the BIOS image for your console from OXOS\bios\ to your modchip:
     1.0 - 1.4 : bios_v@VERSION@_256k.bin (256 KB bank) or bios_v@VERSION@.bin (1 MB bank)
     1.6       : bios_1.6_v@VERSION@_256k.bin (only this one works on a 1.6)
2. Copy the whole OXOS folder, as it is, with everything in it, to the console's
   C: drive, so that it becomes C:\OXOS (on an exFAT disk: /C/OXOS).
     - Easiest: hold X while the console boots, open the web address it shows
       in a browser, go to C: and upload the entire OXOS folder at once.
     - Or: start OXOS\apps\oxos_ftp.xbe and copy the folder with any FTP client.

UPDATING
Copy the OXOS folder again, but keep your own .ini files if you changed them
(C:\OXOS\oxos_settings.ini, C:\OXOS\oxos_extensions.ini,
C:\OXOS\extensions\oxos_trainers.ini, C:\OXOS\extensions\oxos_igs.ini),
and add the sections that are new in this release's copies to yours.

IN-GAME SHORTCUTS
LT + RT + BACK + BLACK in any title shows "IN GAME RESET" and resets the console.
More shortcuts (start any XBE, e.g. the FTP server on BLACK + WHITE):
C:\OXOS\extensions\oxos_igs.ini. Without the extension (safe mode, B held at
power on), hold LT + RT + BACK + BLACK for 3 seconds.

SOUND
If a title leaves the sound chip frozen, "Audio Codec Frozen - Restarting"
shows and the console restarts into the same launch, with sound.

More: https://github.com/kooscode/oxos
