# Shared code

Source code that more than one part of OXOS builds in: the kernel, the console apps ([`../apps/`](../apps/)), the shipped kernel extensions and their apps
([`../kernel/extensions/`](../kernel/extensions/)), the host tools ([`../tools/`](../tools/)) and the host unit tests ([`../tests/unit/`](../tests/unit/)).
Only code goes here, never an app or an extension; each part adds `-I shared` (with `OXOS_KERNEL` defined in the kernel's builds) and builds the files it uses.

| Folder | What it is | Built into |
|---|---|---|
| [`ini_file/`](ini_file/) | The ini reader (`ini_file.cpp`) and the parsers of OXOS's ini files: `oxos_settings.ini` (`settings_ini.cpp`), `oxos_extensions.ini` and an extension's own settings (`extensions_ini.cpp`), `oxos_launch_redirect.ini` (`launch_redirect_ini.cpp`), `oxos_title_patches.ini` (`title_patcher_ini.cpp`) | the kernel (the extension loader for `extensions_ini.cpp`), `oxos_boot_video.xbe`, the unit tests |
| [`graphics/`](graphics/) | The vector renderer of the boot splash and the trainer menu's logo (`vector_renderer.cpp`) | the kernel (boot-only), `oxos_trainers.xbe`, `tools/vsplash`, the unit tests |
| [`video_player/`](video_player/) | The video player: `.mpg` files (MPEG-1/2 video with libmpeg2, MP2/MP3 sound with minimp3) full screen with sound (`video_player.cpp`), the `.mpg` reader (`mpeg_program_stream.cpp`) and the AC'97 output (`sound_output.cpp`); `video_player.mk` builds it into an app (spec/oxos_internal/38 §4) | **apps only** (nxdk): `oxos_boot_video.xbe`, later `oxos_settings.xbe`; the reader also `tests/video_decode_title` |
| [`c_library.h`](c_library.h) | The C library the code here calls: the kernel's own in the kernel (`kernel/src/runtime/c_library.h`), the real one elsewhere | everything here |

The rules (docs/cpp_style.md §2):
- **It builds everywhere** (except `video_player/`, which needs the FPU, MMX and SSE, nxdk and the C library, and is built into apps only; the kernel builds the
  folders listed in `kernel/Makefile`'s `KERNEL_SHARED_DIRS`): no kernel calls, no memory allocation (the caller passes the buffers), standard types
  (`uint32_t`, `size_t`, `bool`) rather than the kernel's (`ULONG`), and the C library only through `c_library.h`, by the standard names (`memchr`,
  `strncasecmp`, `strlcpy`).
- **Names:** a namespace per folder under `oxos::` (`oxos::ini_file`, `oxos::graphics`); types and constants without the kernel's `XK_` / `XE_` prefixes.
- **The kernel's size:** the kernel builds these files exactly as its own (`-Oz`, boot-only code under `OXOS_KERNEL`), so moving code here costs it nothing.
