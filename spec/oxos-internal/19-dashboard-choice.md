# 19. Which dashboard starts

OXOS addition, implemented 2026-10-27. Code: `kernel/src/xe/loader.cpp` (`XepDashboards`, `XepLaunchDashboard`).

Wherever the kernel starts the dashboard (no disc, the tray open, a title that failed to load, a title asking for
the dashboard, spec/stock/05), it tries these in order and starts the first that loads:
1. `C:\evoxdash.xbe` (`\Device\Harddisk0\Partition2\evoxdash.xbe`): EvoX installed on C:, where its installer puts it.
2. `E:\evoxdash.xbe` (`\Device\Harddisk0\Partition1\evoxdash.xbe`): EvoX installed on E:.
3. `C:\xboxdash.xbe`: the Xbox dashboard, as the stock kernel starts.

A missing file fails at its open, before anything is loaded, so the next is tried. When none loads, the kernel
behaves as before: a fatal error on a cold boot (spec/stock/05), a return to the firmware otherwise. The dashboard
gets the same launch data page (reason and code) whichever it is.

Tests: `make test-dash` boots the xemu dashboard disk twice: as it is (no EvoX: `XeImageFileName` is
C:\xboxdash.xbe), and with a copy of its dashboard added as C:\evoxdash.xbe (`boot_test.py --dash --evox`:
`XeImageFileName` is C:\evoxdash.xbe).
