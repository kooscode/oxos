# OXOS features: what OXOS adds to a stock Xbox kernel

One page for everything OXOS has that the retail Xbox kernel does not, and everything planned next, each with its spec. The kernel itself is a complete,
clean-room retail kernel (every function and variable, specs in [`stock/`](stock/)); this page lists only what goes beyond it.

**Keeping it current:** a feature gets a row here when it is proposed or decided, with its spec (`oxos_internal/NN_name.md`, numbered in the order written).
When it ships, its row moves from "Planned" to "Implemented" and its ⬜ becomes ✅. Development aids are listed at the end.

## Implemented

### Boot and hardware
| Feature | What it does | Spec |
|---|---|---|
| ✅ Open BIOS image | OXOS's own 2BL, reset code and X-code interpreter: no Microsoft code and no boot-ROM dump in the image | [stock 01 §5](stock/01_boot.md), [27 §5](oxos_internal/27_xcode_write_checks.md) |
| ✅ Every console revision, 1.0 to 1.6 | the Conexant, Focus and Xcalibur video encoders; a separate 1.6 image | [02](oxos_internal/02_display_modes.md), [28](oxos_internal/28_1_6_console.md), [stock 17–20](stock/17_av_display_mode.md) |
| ✅ 128 MB consoles | the upper 64 MB for titles without the "limit to 64 MB" flag (homebrew, Chihiro titles), as the development-kit kernel; the retail kernel uses only 64 MB | [stock 09 §10](stock/09_memory_manager.md) |
| ✅ Display modes | progressive, HD (720p/1080i) and VGA timings, widescreen | [02](oxos_internal/02_display_modes.md) |
| ✅ Boot splash and jingle | OXOS's animated vector logo and its own music on a cold boot | [00](oxos_internal/00_boot_splash.md), [17](oxos_internal/17_boot_jingle.md) |
| ✅ Boot without a DVD drive | consoles with the drive removed or disconnected boot normally | [08](oxos_internal/08_no_dvd_drive.md) |
| ✅ Reliable cold boots | DRAM refresh, GPU clock, memory-interface and X-code write checks; bus masters stopped at entry; graphics engine brought up | [10](oxos_internal/10_dma_quiesce.md), [20](oxos_internal/20_dram_refresh.md), [24](oxos_internal/24_graphics_engine_init.md), [26](oxos_internal/26_gpu_clock.md), [27](oxos_internal/27_xcode_write_checks.md) |
| ✅ Error screens that explain themselves | what happened and what to try, instead of a bare number; a crash shows its code and parameters | [29](oxos_internal/29_error_screens.md) |

### Disks
| Feature | What it does | Spec |
|---|---|---|
| ✅ exFAT disks | one PC-readable volume of any size, Xbox partitions as folders, names up to 255 characters, files of 4 GB and more; fast allocation (v0.4.8); cache partitions keep their format across a power-off | [05](oxos_internal/05_exfat_storage_spec.md), [06](oxos_internal/06_exfat_storage_plan.md), [07](oxos_internal/07_exfat_driver.md) |
| ✅ Large disks (LBA48) | disks beyond 137 GB; FATX disks with extra F:/G: partitions | [04](oxos_internal/04_lba48.md) |
| ✅ Blank disk to dashboard | a blank or foreign disk is formatted as exFAT at boot; delete all partitions | [30](oxos_internal/30_disk_format.md) |

### Titles
| Feature | What it does | Spec |
|---|---|---|
| ✅ Titles from disc images | titles run from disc images (XISO, full dumps, CCI) and folders on the hard disk | [16](oxos_internal/16_virtual_discs.md) |
| ✅ Dashboard choice | starts EvoX, the Xbox dashboard or the XBL Dashboard, whichever is installed | [19](oxos_internal/19_dashboard_choice.md) |
| ✅ Title compatibility and title patches | answers games expect; per-title patches from `oxos_title_patches.ini` | [25](oxos_internal/25_title_compatibility.md) |
| ✅ Launch redirects | `oxos_launch_redirect.ini` starts another XBE in place of one a title launches | [33](oxos_internal/33_launch_redirect.md) |
| ✅ Trainers from dashboards | the title-start call dashboards patch for trainers (XBMC4Gamers); the trainer pack | [23](oxos_internal/23_trainer_hook.md), [trainer pack](../third_party/xbox_trainers/README.md) |
| ✅ In-game reset | LT + RT + BACK + BLACK reboots from any title | [22](oxos_internal/22_in_game_reset.md) |

### On the console
| Feature | What it does | Spec |
|---|---|---|
| ✅ OXOS File Manager | hold X at boot: upload files and launch XBEs from a browser, also on a blank disk | [31](oxos_internal/31_upload_server.md), [32](oxos_internal/32_file_manager_button.md) |
| ✅ Kernel extensions and their SDK | `.xos` files that hook kernel functions and title events, run while a title runs; safe mode (hold B) | [34](oxos_internal/34_kernel_extensions.md), [SDK](../sdk/README.md) |
| ✅ Log files | `C:\OXOS\log\`, rotated at 10 MB, also for extensions | [35](oxos_internal/35_log_files.md) |


### Developer Support
| Feature | What it does | Spec |
|---|---|---|
| ✅ Netboot | a console boots kernels and titles served by a PC, without reflashing | [09](oxos_internal/09_netboot.md) |
| ✅ Debug serial port | the kernel's log on COM1 (serial builds) | [03](oxos_internal/03_debug_serial.md) |
| ✅ Kernel call tracer | records a title's last kernel calls, to trace a hang or crash | [01](oxos_internal/01_kernel_call_tracer.md) |
| ✅ Host unit tests and the regression run | the kernel's core tested on a PC; `make regression`: unit tests and every xemu phase | [11](oxos_internal/11_host_unit_tests.md), [tests](../tests/README.md) |
| ✅ Title memory watched | boot-only code freed before the first title; `make test_memory` and `make regression` hold titles to 15 960 free pages on 64 MB | [18](oxos_internal/18_boot_memory.md) |
| ✅ MCPX boot-ROM dumper | dumps a console's MCPX boot ROM over the network | [12](oxos_internal/12_mcpx_dump.md) |


## Planned
| Feature | What it will do | Status | Spec |
|---|---|---|---|
| ⬜ Built-in trainers | a trainer menu before any game that has a trainer: trainer and each cheat on/off, remembered per game; no dashboard needed (extension `oxos_trainer.xos`) | proposal (2026-11-15) | [37](oxos_internal/37_built_in_trainers.md) |
| ⬜ Boot video | a video of the user's choice before the dashboard or first title, on cold boots and/or quick reboots (extension `oxos_boot_video.xos`) | proposal (2026-11-15) | [38](oxos_internal/38_boot_video.md) |
| ⬜ OXOS Setup | hold A at boot: one menu for every OXOS setting (dashboard order, extensions, trainers, boot video, video settings, disk, system info) | proposal (2026-11-15) | [39](oxos_internal/39_oxos_setup.md) |
| ⬜ Screenshots | a button combination (or a command from a PC) saves the picture on screen in `C:\OXOS\screenshots\`, from any title (extension `oxos_screenshots.xos`) | proposal (2026-11-15) | [40](oxos_internal/40_screenshots.md) |
| ⬜ Homebrew development on a real console | build, deploy, run, log and debug nxdk apps on a real console from Linux, macOS or Windows: one `oxos` tool, gdb or lldb over the GDB remote protocol, VS Code | proposal (2026-11-15) | [41](oxos_internal/41_remote_debugging.md) |
| ⬜ In-game reset by quick reboot | the in-game reset returns to the dashboard by a quick reboot, as titles do, instead of a power cycle | proposal (2026-11-06) | [22 §8](oxos_internal/22_in_game_reset.md) |
| ⬜ Toasts | short messages over a running title, from the kernel and from extensions | proposal (2026-11-14) | [36](oxos_internal/36_toasts.md) |
| ⬜ OXOS features as extensions | Move `Title patches`, `Netboot` and `Kernel call tracer` features to Kernel Extensions so resident kernel gets smaller - winning back title memory, today the kernel has about 0.06 % less memory available than stock ([18 §7](oxos_internal/18_boot_memory.md)) | decided (2026-11-13), not built | [34 §10](oxos_internal/34_kernel_extensions.md) |
| ⬜ Extensions: the rest | `oxos_resolve_path`; the error screen names a crashed extension | open items | [34](oxos_internal/34_kernel_extensions.md) |
| ⬜ One image for every revision | a run-time 1.6 check, so 1.0–1.6 share one image | planned | [28 §4.5](oxos_internal/28_1_6_console.md) |
| ⬜ exFAT memory units | exFAT on memory units, the last phase of the exFAT plan | planned | [06](oxos_internal/06_exfat_storage_plan.md) |

**Ideas, not planned yet:** features studied in Cerbios that OXOS may add (a second hard disk, fan, LED, LCD and clock control, video options): see
[`cerbios/00_overview.md`](cerbios/00_overview.md).

