# Open Xbox kernel — build on Linux with clang + lld-link.
#   make            -> build/kernel/self_tests/oxoskrnl.exe (+ map)
#   make bios       -> build/kernel/self_tests/bios.bin: the open BIOS image (our 2BL + XboxDev/cromwell X-codes;
#                      spec/stock/01 §5)
#   make test       -> boot build/kernel/self_tests/bios.bin in xemu and check all milestone conditions (boot
# self-tests) All build output goes under the repository's build/ folder (the only one git ignores for it): the kernel's in build/kernel/, one folder per kind
# of build; the apps in build/tools/ and build/tests/, the SDK in build/sdk/.
#   build/kernel/self_tests       make, make bios, make test (the kernel with its boot self-tests)
#   build/kernel/release          make run and the published BIOS (no self-tests, no debug code)
#   build/kernel/debug            make run_gdb, tools/debug_app.py, VS Code (DEBUG=1: debug info for gdb)
#   build/kernel/trace_basic      make run_trace_basic (the debug serial log)
#   build/kernel/trace_full       make run_trace_full (serial log + kernel call tracer, xemu at 128 MB)
#   build/kernel/trace_full_64mb  make run_trace_full TRACE=1 (the call tracer within the console's 64 MB)

ROOT      := ..
OUT       := $(ROOT)/build/kernel
BUILD     := $(OUT)/self_tests
RELEASE_BUILD := $(OUT)/release
# The 1.6 console's release image (MCPX16=1: its own X-codes, spec/oxos_internal/28).
RELEASE16_BUILD := $(OUT)/release_1_6
DEBUG_BUILD   := $(OUT)/debug
TRACE_BASIC_BUILD := $(OUT)/trace_basic
# IMAGE=open (default) needs no Microsoft files. IMAGE=base instead packs the kernel into your own MCPX 1.0 BIOS image (BIOS_BASE, its 2BL decrypted with
# MCPX_ROM), as before the open image existed.
IMAGE     ?= open
BIOS_BASE ?= $(firstword $(wildcard $(ROOT)/xbox_files/bios/*.bin))
# An MCPX boot ROM dump is optional: without one xemu starts at our reset code in flash. With one (MCPX_ROM=...) xemu runs the real ROM, which reaches our 2BL
# through the X-codes' overflow trick, as a console does.
MCPX_ROM  ?=
# The reference BIOS needs the real boot ROM (run_stock, CONTROLLER_BIOS=stock comparisons only).
STOCK_MCPX ?= $(ROOT)/xbox_files/mcpx/mcpx_1.0.bin
# Default hard disk: xemu-dashboard's free, MIT-licensed disk image (an open dashboard on a formatted 8 GB drive), fetched into third_party/ on first use, at a
# pinned release, and checked by SHA-256 (tools/fetch.py). The tests and the run targets only ever use a copy. HDD_IMAGE=... uses another disk.
XDASH_VERSION := v20260516-0955
XDASH_URL     := https://github.com/xemu-project/xemu-dashboard/releases/download/$(XDASH_VERSION)
XDASH_DIR     := $(abspath $(ROOT)/third_party/xemu_dashboard)
XDASH_HDD     := $(XDASH_DIR)/xemu_dashboard.qcow2
HDD_IMAGE ?= $(XDASH_HDD)
# The fetch, as a prerequisite of the targets that boot from the default disk (nothing when HDD_IMAGE is another).
HDD_FETCH  = $(filter $(XDASH_HDD),$(HDD_IMAGE))
# Boot splash: an SVG, drawn and animated at boot (tools/svg_splash.py, spec/oxos_internal/00_boot_splash.md).
SPLASH    ?= splash/oxos_splash.svg
SELFTEST  ?= 1
RETAIL_MEDIA_POLICY ?= 0
TRACE_IMPORTS ?= 0
# Source-level debugging with gdb (make run_gdb, tools/debug_app.py): DWARF debug info, and -Og so that stepping follows the source. C++ functions get no
# (MSVC-style) linkage names, which gdb cannot demangle, so gdb names them from the source. The BIOS packs the kernel as usual; oxoskrnl.elf (the same link with
# the debug info kept, converted to ELF, which gdb reads better than PE) is the symbol file for gdb.
DEBUG     ?= 0
# The kernel is built for size (-Oz): its code stays resident, in memory titles count on (spec/oxos_internal/18). Code where speed shows uses -O2 (FAST_SRCS
# below).
OPT       := $(if $(filter 1,$(DEBUG)),-Og -fno-inline-functions -g -gdwarf-4 -mllvm -dwarf-linkage-names=Abstract,-Oz -g0)
# Debug serial port (spec/oxos_internal/03_debug_serial.md): on in the self-test and trace builds, off in release builds. NETBOOT=1: the 2BL first asks the
# network for a boot image (spec/oxos_internal/09_netboot.md; tools/netboot/). NETBOOT_IP is the address the console puts in its netboot packets (it needs no
# DHCP and answers no ARP). Use its own BUILD directory (make netboot does): the 2BL is not rebuilt when only NETBOOT changes.
NETBOOT ?= 0
# MCPX_DUMP=1 (with NETBOOT=1): the 2BL copies the MCPX boot ROM at entry and sends it over the network (spec/oxos_internal/12_mcpx_dump.md; tools/mcpx_dump/).
# make mcpx_dump builds that payload image.
MCPX_DUMP ?= 0
NETBOOT_IP ?= 192.168.1.222
comma := ,
SERIAL ?= $(if $(filter 1,$(SELFTEST) $(TRACE_IMPORTS)),1,0)
# STATEDUMP=1: print the machine-state probe (tools/statedump) on the debug serial port at the end of start-up, for comparing the set-up with a reference
# kernel's. Turns SERIAL on.
STATEDUMP ?= 0
# REBOOT_TEST=N (debug): N seconds into every boot (cold, and the quick reboots that start titles) the kernel powers the console off and on
# (hardware::chipset::PowerCycle, the in-game shortcuts' coldboot), so resets can be tested in a loop without a controller.
REBOOT_TEST ?= 0
ifeq ($(STATEDUMP),1)
SERIAL := 1
endif
# GDB=1: the GDB stub built in (src/debugger/gdb_stub.cpp, shared/gdb_stub; spec/oxos_internal/51 §5), on the debug serial port. Turns SERIAL on.
# GDB_WAIT=1: a cold boot waits for gdb right after the processor set-up.
GDB ?= 0
GDB_WAIT ?= 0
ifeq ($(GDB),1)
SERIAL := 1
endif

# The LLVM tools: from the folder LLVM names (a make option or an environment variable, the same option as the SDK's oxos_extension.mk), by default LLVM
# 20.1.8's release unpacked in /opt/llvm-20.1.8 when it is there, else (or with LLVM=) from the PATH. LLVM 20.1.0-20.1.2, Ubuntu's clang-20 among them, emit
# SSE2 instructions for -march=pentium3, which the nxdk apps use (llvm issue 134607). The folder also goes first on the recipes' PATH, so what they call by
# name (ld.lld, llvm-nm, nxdk's compiler wrappers) is the same LLVM.
LLVM     ?= $(wildcard /opt/llvm-20.1.8/bin)
LLVM_DIR := $(if $(LLVM),$(abspath $(LLVM))/)
ifneq ($(LLVM),)
export PATH := $(abspath $(LLVM)):$(PATH)
endif
CXX := $(LLVM_DIR)clang++
CC  := $(LLVM_DIR)clang
LD  := $(LLVM_DIR)lld-link
# The compiler's version (20.1.8), in the nxdk stamp's name: another compiler rebuilds nxdk's libraries and the apps.
CLANG_VERSION := $(shell $(CC) --version 2>/dev/null | sed -n '1s/.*clang version \([0-9][0-9.]*\).*/\1/p')

TARGET  := --target=i386-pc-win32
# Plain Pentium code (no cmov, no multi-byte NOPs), as the reference kernels (their compiler emits only 386-class instructions). Not a fix for the 1.4 console's
# start-up freeze: that one depends on the exact binary layout with either setting (2026-10-23).
ARCH    := -march=pentium -mno-sse -mno-mmx -mno-80387
# EXTRA_FLAGS: more compiler flags for one-off experiments, in their own BUILD.
CXXFLAGS := $(EXTRA_FLAGS) $(TARGET) $(ARCH) -std=c++23 -ffreestanding -fno-exceptions -fno-rtti -fno-builtin \
            -fno-stack-protector -fno-threadsafe-statics -fno-omit-frame-pointer -nostdlib $(OPT) \
            -Wall -Wextra -Werror -Wno-unused-parameter -Isrc -DXK_BOOT_SELFTEST=$(SELFTEST) -DXK_RETAIL_MEDIA_POLICY=$(RETAIL_MEDIA_POLICY) -DXK_TRACE_IMPORTS=$(TRACE_IMPORTS) -DXK_DEBUG_SERIAL=$(SERIAL) -DXK_STATEDUMP=$(STATEDUMP) -DXK_REBOOT_TEST=$(REBOOT_TEST) -DXK_GDB=$(GDB) -DXK_GDB_WAIT=$(GDB_WAIT)
ASFLAGS := $(EXTRA_FLAGS) $(TARGET) $(ARCH) -Isrc $(if $(filter 1,$(DEBUG)),-g -gdwarf-4 -DXK_DEBUG_INFO=1)
# The version on the boot splash and the error screens (graphics/text.cpp): vX.Y.Z when built for a release (make release VERSION=X.Y.Z), else DEV (and
# XK_RELEASE_VERSION 1 or 0: a development build's disk-format countdown is short). It is a generated header, rewritten only when the text changes, so a new
# version rebuilds what uses it.
XK_VERSION_TEXT := $(if $(VERSION),v$(VERSION),DEV)
XK_VERSION_HEADER := $(BUILD)/generated/xk_version.h
CXXFLAGS += -I$(BUILD)/generated
# The kernel extensions' binary interface, shared with their SDK (spec/oxos_internal/34).
CXXFLAGS += -I$(ROOT)/sdk/include -I$(ROOT)/sdk/src
# The code shared with the apps, extensions and host tools (shared/README.md): built as the kernel's own, with its C library (OXOS_KERNEL).
CXXFLAGS += -I$(ROOT)/shared -DOXOS_KERNEL
# The boot self-tests' header, for the kernel's hooks that start them (SELFTEST=1 builds only).
CXXFLAGS += $(if $(filter 0,$(SELFTEST)),,-I$(ROOT)/tests/kernel_self_tests)
# The netboot address: the upload server's own when DHCP does not answer (network/ip.cpp).
CXXFLAGS += -DXK_NETBOOT_IP=$(subst .,$(comma),$(NETBOOT_IP))
LDFLAGS := /DRIVER /NODEFAULTLIB /ENTRY:KernelEntry /SUBSYSTEM:NATIVE /BASE:0x80010000 /FIXED \
           /ALIGN:0x20 /FILEALIGN:0x20 /MERGE:.rdata=.text /MERGE:.bss=.data /SAFESEH:NO \
           /SECTION:.xka,D /SECTION:.xkb,D \
           /Brepro /DEBUG:NONE /MAP:$(BUILD)/oxoskrnl.map

# Release builds leave out the self-tests and the import tracer: their code and buffers would stay resident and take memory titles count on (Halo 2 needs nearly
# all of it). Every kernel header (each folder of src/ holds its own): a source file is rebuilt when one changes.
KERNEL_HEADERS := $(wildcard src/*/*.h src/*/*/*.h modules/*.h $(ROOT)/shared/*.h $(ROOT)/shared/*/*.h)
# The folders of shared/ the resident kernel builds (shared/ini_file goes into the kernel modules, shared/video_player is for apps only); GDB=1 kernels also
# build the GDB stub (shared/gdb_stub, spec/oxos_internal/51 §5).
# src/runtime/c_library_extensions.cpp is not in the kernel: the SDK's library has it (tools/gen_oxos_sdk.py).
KERNEL_SHARED_DIRS := graphics $(if $(filter 1,$(GDB)),gdb_stub)
CXX_SRCS := $(filter-out $(if $(filter 0,$(TRACE_IMPORTS)),src/titles/title_tracer.cpp) src/runtime/c_library_extensions.cpp, \
                         $(wildcard src/*/*.cpp src/*/*/*.cpp $(KERNEL_SHARED_DIRS:%=$(ROOT)/shared/%/*.cpp)))
# The kernel modules (spec/oxos_internal/43): kernel/modules/ with the shared ini parsers, compiled as the kernel is but linked on their own (below), not into
# the kernel.
KERNEL_MODULES_SRCS := $(wildcard modules/*.cpp) $(wildcard $(ROOT)/shared/ini_file/*.cpp)
ASM_SRCS := $(filter-out $(if $(filter 0,$(TRACE_IMPORTS)),src/arch/trace.S),$(wildcard src/*/*.S))
# The boot self-tests: test code in tests/kernel_self_tests, compiled into SELFTEST=1 kernels only.
SELF_TESTS_DIR := $(ROOT)/tests/kernel_self_tests
SELFTEST_SRCS := $(if $(filter 0,$(SELFTEST)),,$(wildcard $(SELF_TESTS_DIR)/*.cpp))
# Objects: $(BUILD)/<folder>/<file>.obj for src/, $(BUILD)/shared/<folder>/<file>.obj for shared/, $(BUILD)/modules/<file>.obj for modules/.
KERNEL_OBJECT = $(patsubst modules/%.cpp,$(BUILD)/modules/%.obj,\
                $(patsubst $(ROOT)/shared/%.cpp,$(BUILD)/shared/%.obj,$(patsubst src/%.cpp,$(BUILD)/%.obj,$(1))))
OBJS := $(call KERNEL_OBJECT,$(CXX_SRCS)) $(ASM_SRCS:src/%.S=$(BUILD)/%.obj) \
        $(SELFTEST_SRCS:$(SELF_TESTS_DIR)/%.cpp=$(BUILD)/kernel_self_tests/%.obj)

all: $(BUILD)/oxoskrnl.exe

# Boot-only code (spec/oxos_internal/18_boot_memory.md): these files may call one another's boot-only functions.
BOOT_OBJS := splash.obj,nic.obj,ip.obj,tcp.obj,webserver.obj,webpage.obj
BOOT_OBJS := $(BOOT_OBJS),host_controller.obj,devices.obj,gamepad.obj,file_manager_screen.obj,vector_renderer.obj,ac97.obj
# ... and the boot-only functions kept code calls during a cold boot, before the pages are freed, or that a kept file defines for the boot-only code.
BOOT_ENTRIES := boot_splash::StartSplash,boot_splash::WaitForSplash,webserver::Start,webserver::Stop
BOOT_ENTRIES := $(BOOT_ENTRIES),user_interface::NoDashboardError
BOOT_ENTRIES := $(BOOT_ENTRIES),gamepad::WaitForXButton
# The disk checks of a cold boot (storage/disk_format.cpp), called by storage/disk_layout.cpp's boot-only code.
BOOT_ENTRIES := $(BOOT_ENTRIES),storage::BlankDiskCountdown,storage::IsDiskBlank,storage::DescribeDisk
# The kernel modules' cache, written at a cold boot from the boot-only copy (kernel_modules/kernel_module_loader.cpp).
BOOT_ENTRIES := $(BOOT_ENTRIES),kernel_modules::WriteKernelModulesCache
# The cache partitions' formats, read at a cold boot on an exFAT disk (filesystems/hddfs/volume_exfat.cpp); the self-tests call it too.
BOOT_ENTRIES := $(BOOT_ENTRIES),filesystems::ReadCachePartitions
# The netboot title's installation and whether there is one (titles/title_netboot.cpp): a package only ever comes with a cold boot.
BOOT_ENTRIES := $(BOOT_ENTRIES),titles::LaunchNetbootTitle,titles::HasNetbootTitle
# The cold boot's launch choice and the File Manager (titles/title_loader.cpp).
BOOT_ENTRIES := $(BOOT_ENTRIES),titles::LaunchAtColdBoot,titles::ServeFileManager
# The quick-reboot copy of the initialised .data (src/arch/entry.S).
KI_DATA_COPY_SIZE := $(shell sed -n 's/^\#define KI_DATA_COPY_SIZE \(0x[0-9A-Fa-f]*\).*/\1/p' src/arch/entry.S)

# Per-file optimization (not in DEBUG builds): -O2 for the hashes and ciphers (XBE sections, saves), memcpy and memset, the virtual-disc decompressor (it runs
# for every sector a title reads from a CCI) and the boot-only code (the splash's fade has to keep up with the clock; its pages are freed before the
# title starts, so their size costs nothing then), with the vector renderer it draws with.
FAST_SRCS  := src/crypto/crypto.cpp src/runtime/memory.cpp src/storage/virtual_disc.cpp \
              $(wildcard src/boot_splash/*.cpp) $(ROOT)/shared/graphics/vector_renderer.cpp
ifneq ($(DEBUG),1)
$(call KERNEL_OBJECT,$(FAST_SRCS)): FILE_OPT := -O2
endif

# Each object also depends on every header it includes (the compiler writes the list; -MP keeps a removed header from breaking the build).
$(BUILD)/%.obj: src/%.cpp $(KERNEL_HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(FILE_OPT) -MMD -MP -MF $(@:.obj=.d) -c $< -o $@
$(BUILD)/shared/%.obj: $(ROOT)/shared/%.cpp $(KERNEL_HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(FILE_OPT) -MMD -MP -MF $(@:.obj=.d) -c $< -o $@
$(BUILD)/modules/%.obj: modules/%.cpp $(KERNEL_HEADERS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(FILE_OPT) -MMD -MP -MF $(@:.obj=.d) -c $< -o $@

$(XK_VERSION_HEADER): FORCE
	@mkdir -p $(dir $@)
	@printf '#define XK_VERSION_TEXT "%s"\n#define XK_RELEASE_VERSION %s\n' "$(XK_VERSION_TEXT)" \
	    "$(if $(VERSION),1,0)" | cmp -s - $@ || printf '#define XK_VERSION_TEXT "%s"\n#define XK_RELEASE_VERSION %s\n' \
	    "$(XK_VERSION_TEXT)" "$(if $(VERSION),1,0)" > $@
$(BUILD)/graphics/text.obj $(BUILD)/storage/disk_layout.obj $(BUILD)/extensions/extension_runtime.obj: \
    $(XK_VERSION_HEADER)
FORCE:

# The boot self-tests (SELFTEST=1 only).
$(BUILD)/kernel_self_tests/%.obj: $(SELF_TESTS_DIR)/%.cpp $(KERNEL_HEADERS) $(wildcard $(SELF_TESTS_DIR)/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(FILE_OPT) -MMD -MP -MF $(@:.obj=.d) -c $< -o $@
-include $(OBJS:.obj=.d) $(KERNEL_MODULES_OBJS:.obj=.d)

$(BUILD)/%.obj: src/%.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/oxoskrnl.def: exports.tsv $(ROOT)/tools/gen_def.py
	@mkdir -p $(BUILD)
	python3 -I $(ROOT)/tools/gen_def.py exports.tsv $@

ifeq ($(DEBUG),1)
# One link with the debug info; the kernel is the same file with the debug sections stripped, so its code and data sit at exactly the addresses gdb reads from
# oxoskrnl.dbg.exe.
$(BUILD)/oxoskrnl.exe: $(OBJS) $(BUILD)/oxoskrnl.def
	$(LD) $(filter-out /DEBUG:NONE,$(LDFLAGS)) /DEBUG:DWARF /DEF:$(BUILD)/oxoskrnl.def \
	    /OUT:$(BUILD)/oxoskrnl.dbg.exe $(OBJS)
	$(OBJCOPY) --strip-all $(BUILD)/oxoskrnl.dbg.exe $@
	$(GNU_OBJCOPY) -O elf32-i386 $(BUILD)/oxoskrnl.dbg.exe $(BUILD)/oxoskrnl.elf
else
# The kernel as before; then oxoskrnl.elf, gdb's symbols for it (the release zip ships it, spec/oxos_internal/51): the kernel itself as ELF plus the names
# its map gives (tools/make_kernel_symbols.py), so the addresses are the kernel's by construction.
$(BUILD)/oxoskrnl.exe: $(OBJS) $(BUILD)/oxoskrnl.def
	$(LD) $(LDFLAGS) /DEF:$(BUILD)/oxoskrnl.def /OUT:$@ $(OBJS)
	GNU_OBJCOPY=$(GNU_OBJCOPY) python3 -I $(ROOT)/tools/make_kernel_symbols.py $@ $(BUILD)/oxoskrnl.map $(BUILD)/oxoskrnl.elf
endif
	python3 -I $(ROOT)/tools/check_kernel_layout.py --exe $@ --map $(BUILD)/oxoskrnl.map --boot-objs $(BOOT_OBJS) \
	    --boot-entries $(BOOT_ENTRIES) --data-copy $(KI_DATA_COPY_SIZE) $(OBJS) || { rm -f $@; exit 1; }
	python3 -I $(ROOT)/tools/check_c_library_use.py $(ROOT) || { rm -f $@; exit 1; }

# Kernel test title (tests/kernel_test_title): a minimal freestanding "game" the boot tests run from a disc to check the title loader and the kernel functions
# titles rely on. PE at 0x10000 -> XBE.
TEST_TITLE := $(BUILD)/kernel_test_title
TEST_TITLE_CXXFLAGS := $(TARGET) $(ARCH) -std=c++23 -ffreestanding -fno-exceptions -fno-rtti -fno-builtin \
                       -fno-stack-protector -nostdlib -O2 -Wall -Werror
$(TEST_TITLE).obj: $(ROOT)/tests/kernel_test_title/kernel_test_title.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(TEST_TITLE_CXXFLAGS) -c $< -o $@

$(TEST_TITLE).exe: $(TEST_TITLE).obj
	$(LD) /NODEFAULTLIB /ENTRY:XkTitleEntry /SUBSYSTEM:NATIVE /BASE:0x10000 /FIXED /Brepro /DEBUG:NONE \
	    /MAP:$(TEST_TITLE).map /OUT:$@ $<

$(TEST_TITLE).xbe: $(TEST_TITLE).exe $(ROOT)/tools/mkxbe.py
	python3 -I $(ROOT)/tools/mkxbe.py --pe $< --map $(TEST_TITLE).map --out $@ --no-preload .extra \
	    --name "OXOS kernel test title"

# --- Open BIOS image parts (spec/stock/01 §5). XboxDev/cromwell (GPL-2.0) is fetched at a pinned commit for its X-code
# table and EEPROM key states. MCPX16=1 selects Cromwell's values for the 1.6 console's MCPX.
CROMWELL_DIR    := $(abspath $(ROOT)/third_party/cromwell)
CROMWELL_COMMIT := 86f547387184d1001d377cce97d3756c8acf91cc
MCPX16          ?= 0
OBJCOPY         := $(if $(LLVM),$(LLVM_DIR)llvm-objcopy,$(firstword $(shell command -v llvm-objcopy) $(lastword $(sort $(wildcard /usr/lib/llvm-*/bin/llvm-objcopy)))))  # any LLVM version
GNU_OBJCOPY     := objcopy
BOOT_FLAGS := --target=i386-unknown-none-elf -march=pentium -mno-sse -mno-mmx -ffreestanding -fno-exceptions \
              -fno-rtti -fno-builtin -fno-pic -fno-stack-protector -nostdlib -O2 -Wall -Wextra -Werror -Isrc \
              $(if $(filter 1,$(MCPX16)),-DMCPXREVD5) -DXK_NETBOOT=$(NETBOOT) -DXK_MCPX_DUMP=$(MCPX_DUMP) -DXK_XCODE_ENTRY_SNAPSHOT=$(SERIAL) \
              -DXK_NETBOOT_IP=$(subst .,$(comma),$(NETBOOT_IP))
BOOT := $(BUILD)/boot

$(CROMWELL_DIR)/boot_rom/Xcodes.h:
	git clone -q https://github.com/XboxDev/cromwell $(CROMWELL_DIR)
	git -C $(CROMWELL_DIR) checkout -q $(CROMWELL_COMMIT)

# --- CPU microcode (spec/stock/01 §2 step 3): Intel's updates for the Xbox's Pentium III (signatures 0x686 and
# 0x68A, platform 4), loaded at start-up as the reference kernel does. The files are kept in the repo, unmodified, as Intel's licence allows
# (third_party/intel_ucode: the files, Intel's licence, where they came from); their SHA-256 is checked before each use. tools/mkucode.py picks the platform-4
# updates, unmodified.
UCODE_DIR    := $(abspath $(ROOT)/third_party/intel_ucode)
UCODE_FILES  := $(UCODE_DIR)/06-08-06 $(UCODE_DIR)/06-08-0a
$(BUILD)/ucode.inc: $(UCODE_FILES) $(UCODE_DIR)/SHA256SUMS $(ROOT)/tools/mkucode.py
	@mkdir -p $(dir $@)
	cd $(UCODE_DIR) && sha256sum --quiet -c SHA256SUMS
	python3 -I $(ROOT)/tools/mkucode.py $@ $(UCODE_FILES)
$(BUILD)/core/processor.obj: $(BUILD)/ucode.inc
$(BUILD)/core/processor.obj: CXXFLAGS += -I$(BUILD)

# Cromwell's X-codes, then write checks on the memory set-up (tools/harden_xcodes.py, spec/oxos_internal/27).
$(BOOT)/xcodes.bin: boot/xcodes.S boot/xcodes_1_6.inc boot/padcal_table.inc $(CROMWELL_DIR)/boot_rom/Xcodes.h $(ROOT)/tools/harden_xcodes.py
	@mkdir -p $(dir $@)
	$(CC) $(BOOT_FLAGS) -I$(CROMWELL_DIR)/boot_rom -Iboot -c $< -o $(BOOT)/xcodes.o
	$(OBJCOPY) -O binary -j .xcodes $(BOOT)/xcodes.o $(BOOT)/xcodes_cromwell.bin
	python3 -I $(ROOT)/tools/harden_xcodes.py $(BOOT)/xcodes_cromwell.bin $@

$(BOOT)/reset.bin: boot/reset.S
	@mkdir -p $(dir $@)
	$(CC) $(BOOT_FLAGS) -c $< -o $(BOOT)/reset.o
	$(OBJCOPY) -O binary -j .reset $(BOOT)/reset.o $@

# The 2BL: start.S and bl.cpp, and in NETBOOT=1 images netboot.cpp (the network boot).
BL_NETBOOT := $(if $(filter 1,$(NETBOOT)),$(BOOT)/netboot.o)
$(BOOT)/bl.bin: boot/start.S boot/bl.cpp boot/bl.h boot/netboot.cpp boot/bl.ld boot/inflate.h src/hardware/ethernet/nic.h
	@mkdir -p $(dir $@)
	$(CC) $(BOOT_FLAGS) -c boot/start.S -o $(BOOT)/start.o
	$(CXX) $(BOOT_FLAGS) -std=c++23 -c boot/bl.cpp -o $(BOOT)/bl.o
	$(if $(BL_NETBOOT),$(CXX) $(BOOT_FLAGS) -std=c++23 -c boot/netboot.cpp -o $(BL_NETBOOT))
	$(LLVM_DIR)ld.lld -m elf_i386 -T boot/bl.ld -nostdlib $(BOOT)/start.o $(BOOT)/bl.o $(BL_NETBOOT) -o $(BOOT)/bl.elf
	$(OBJCOPY) -O binary $(BOOT)/bl.elf $@

bios: $(BUILD)/bios.bin
# A 256 KiB image (one bank, for 256 KiB flash chips and modchip banks), with the splash in the bank's spare space.
bios_256k: $(BUILD)/bios_256k.bin

# A release, in order, stopping at the first failure: every host unit test (tools/unit_tests.sh), the release images (1 MB and 256 KB, no self-tests, no serial
# log), the dashboard test on them (make test_dash), and a check that the 256 KB image is the 1 MB image's first bank. VERSION=X.Y.Z also packs the release file
# build/kernel/release/dist/oxos_vX.Y.Z.zip (release_zip, below: the three images, the FTP server, built with nxdk, which `make nxdk` fetches, the title
# patches, the shipped extensions and the trainers, in the C:\OXOS layout; beside it symbols/, each kernel's oxoskrnl.elf for gdb, not for the console),
# puts the kernel extension SDK's release file next to it (oxos_sdk_vX.Y.Z.zip, from sdk/Makefile), and prints the SHA-256 of each. SKIP_TESTS=1 skips the
# unit tests and the dashboard test, for fast iterations; it cannot be combined with VERSION, so files made for publishing are always tested.
release:
	$(if $(and $(filter 1,$(SKIP_TESTS)),$(VERSION)),$(error SKIP_TESTS=1 cannot be used with VERSION: a published release runs every test))
	$(if $(filter 1,$(SKIP_TESTS)),@echo "release: SKIP_TESTS=1 (unit tests and test_dash skipped)",$(ROOT)/tools/unit_tests.sh)
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin $(RELEASE_BUILD)/bios_256k.bin
	$(MAKE) BUILD=$(RELEASE16_BUILD) SELFTEST=0 MCPX16=1 $(RELEASE16_BUILD)/bios_256k.bin
	$(if $(filter 1,$(SKIP_TESTS)),,$(MAKE) test_dash)
	head -c 262144 $(RELEASE_BUILD)/bios.bin | cmp - $(RELEASE_BUILD)/bios_256k.bin
	$(if $(VERSION),$(MAKE) apps extensions)
	$(if $(VERSION),$(MAKE) -C $(ROOT)/sdk VERSION=$(VERSION))
	$(if $(VERSION),$(MAKE) release_zip VERSION=$(VERSION))

# The release zip, oxos_vVERSION.zip (spec/oxos_disk_layout.md, "The release zip"): OXOS\ as it goes on the console's C:\ (C:\OXOS), with the three BIOS
# images in OXOS\bios\, OXOS's apps in OXOS\apps\ (the FTP server oxos_ftp.xbe, the boot video player oxos_boot_video.xbe), the settings
# (oxos_settings.ini with the boot video on, oxos_title_patches.ini), the OXOS boot video in OXOS\boot_videos\ (splash/oxos_boot_video.mpg), the extensions
# OXOS ships (oxos_extensions.ini listing them: the in-game shortcuts on, the trainers off), the trainer pack in OXOS\trainers\ and the folders the kernel
# makes; beside it README.txt (how to install: settings/release_readme.txt, the version filled in, Windows line ends) and the third-party notices.
# The SDK is its own zip. make release VERSION=X.Y.Z builds everything first; then the zip, the SDK's zip and their SHA-256 sums are in
# build/kernel/release/dist.
RELEASE_DIST := $(RELEASE_BUILD)/dist
RELEASE_OXOS := $(RELEASE_DIST)/zip/OXOS
release_zip:
	$(if $(VERSION),,$(error release_zip needs VERSION))
	rm -rf $(RELEASE_DIST)
	mkdir -p $(RELEASE_OXOS)/bios $(RELEASE_OXOS)/apps $(RELEASE_OXOS)/extensions $(RELEASE_OXOS)/trainers $(RELEASE_OXOS)/log $(RELEASE_OXOS)/cache
	cp $(RELEASE_BUILD)/bios.bin $(RELEASE_OXOS)/bios/bios_v$(VERSION).bin
	cp $(RELEASE_BUILD)/bios_256k.bin $(RELEASE_OXOS)/bios/bios_v$(VERSION)_256k.bin
	cp $(RELEASE16_BUILD)/bios_256k.bin $(RELEASE_OXOS)/bios/bios_1.6_v$(VERSION)_256k.bin
	cp extensions/oxos_extensions.ini settings/oxos_title_patches.ini settings/oxos_settings.ini $(RELEASE_OXOS)/
	mkdir -p $(RELEASE_OXOS)/boot_videos
	cp splash/oxos_boot_video.mpg $(RELEASE_OXOS)/boot_videos/
	set -e; for name in $(SHIPPED_EXTENSIONS); do cp $(SHIPPED_EXTENSIONS_DIR)/$$name/$$name.* \
	    $(RELEASE_OXOS)/extensions/; done
	cp $(ROOT)/third_party/xbox_trainers/trainers/*.etm $(RELEASE_OXOS)/trainers/
	cp $(OXOS_FTP_XBE) $(RELEASE_OXOS)/apps/oxos_ftp.xbe
	cp $(OXOS_BOOT_VIDEO_XBE) $(RELEASE_OXOS)/apps/oxos_boot_video.xbe
	cp $(ROOT)/THIRD_PARTY_NOTICES.md $(RELEASE_DIST)/zip/
	mkdir -p $(RELEASE_DIST)/zip/symbols/1.6
	cp $(RELEASE_BUILD)/oxoskrnl.elf $(RELEASE_DIST)/zip/symbols/
	cp $(RELEASE16_BUILD)/oxoskrnl.elf $(RELEASE_DIST)/zip/symbols/1.6/
	sed -e 's/@VERSION@/$(VERSION)/g' -e 's/$$/\r/' settings/release_readme.txt > $(RELEASE_DIST)/zip/README.txt
	cd $(RELEASE_DIST)/zip && zip -q -X -r ../oxos_v$(VERSION).zip README.txt OXOS symbols THIRD_PARTY_NOTICES.md
	cp $(abspath $(ROOT)/build/sdk/oxos_sdk_v$(VERSION).zip) $(RELEASE_DIST)/
	cd $(RELEASE_DIST) && sha256sum oxos_v$(VERSION).zip oxos_sdk_v$(VERSION).zip zip/OXOS/bios/*.bin zip/OXOS/oxos_title_patches.ini zip/OXOS/apps/*.xbe

# The image to flash once for network boots (tools/netboot/README.md): its 2BL asks tools/netboot/netboot.py for a kernel at every power-on and boots its own (a
# serial-log build) when no PC answers.
NETBOOT_BUILD := $(OUT)/netboot
netboot:
	$(MAKE) BUILD=$(NETBOOT_BUILD) NETBOOT=1 SERIAL=1 SELFTEST=0 $(NETBOOT_BUILD)/bios_256k.bin $(NETBOOT_BUILD)/bios.bin

# The MCPX ROM dumper's payload (tools/mcpx_dump/README.md): a netboot image whose 2BL also sends the boot ROM.
MCPX_DUMP_BUILD := $(OUT)/mcpx_dump
mcpx_dump:
	$(MAKE) BUILD=$(MCPX_DUMP_BUILD) NETBOOT=1 MCPX_DUMP=1 SERIAL=1 SELFTEST=0 SPLASH= $(MCPX_DUMP_BUILD)/bios_256k.bin

# The kernel modules (spec/oxos_internal/43): KERNEL_MODULES_SRCS, compiled as the kernel is (the rules above) but linked into one image of their own at
# 0xB0000000 (XK_KERNEL_MODULES_BASE) against this kernel's addresses, then appended to the kernel image.
KERNEL_MODULES_OBJS := $(call KERNEL_OBJECT,$(KERNEL_MODULES_SRCS))
KERNEL_MODULES := $(BUILD)/kernel_modules.exe
$(KERNEL_MODULES): $(KERNEL_MODULES_OBJS) $(BUILD)/oxoskrnl.exe $(ROOT)/tools/link_kernel_modules.py
	python3 -I $(ROOT)/tools/link_kernel_modules.py --kernel $(BUILD)/oxoskrnl.exe --map $(BUILD)/oxoskrnl.map \
	    --base 0xB0000000 --linker $(LD) --cc $(CC) --out $@ $(KERNEL_MODULES_OBJS)

SPLASH_ARGS = $(if $(wildcard $(SPLASH)),--splash $(SPLASH))
ifeq ($(IMAGE),base)
$(BUILD)/bios.bin: $(BUILD)/oxoskrnl.exe $(ROOT)/tools/mkbios.py $(wildcard $(SPLASH)) $(KERNEL_MODULES)
	python3 -I $(ROOT)/tools/mkbios.py --base $(BIOS_BASE) --mcpx-rom $(MCPX_ROM) \
	    --kernel $< --out $@ --flat-kernel $(BUILD)/oxoskrnl.flat.exe $(SPLASH_ARGS) --kernel-modules $(KERNEL_MODULES)
else
$(BUILD)/bios.bin: $(BUILD)/oxoskrnl.exe $(ROOT)/tools/mkbios.py $(wildcard $(SPLASH)) \
                   $(BOOT)/xcodes.bin $(BOOT)/bl.bin $(BOOT)/reset.bin $(KERNEL_MODULES)
	python3 -I $(ROOT)/tools/mkbios.py --open --xcodes $(BOOT)/xcodes.bin --loader $(BOOT)/bl.bin \
	    --reset $(BOOT)/reset.bin --cromwell $(CROMWELL_DIR) --kernel $< --out $@ \
	    --flat-kernel $(BUILD)/oxoskrnl.flat.exe $(SPLASH_ARGS) --kernel-modules $(KERNEL_MODULES)
$(BUILD)/bios_256k.bin: $(BUILD)/oxoskrnl.exe $(ROOT)/tools/mkbios.py $(wildcard $(SPLASH)) \
                        $(BOOT)/xcodes.bin $(BOOT)/bl.bin $(BOOT)/reset.bin $(KERNEL_MODULES)
	python3 -I $(ROOT)/tools/mkbios.py --open --xcodes $(BOOT)/xcodes.bin --loader $(BOOT)/bl.bin \
	    --reset $(BOOT)/reset.bin --cromwell $(CROMWELL_DIR) --kernel $< --out $@ $(SPLASH_ARGS) --size 256K \
	    --kernel-modules $(KERNEL_MODULES)
endif

$(XDASH_HDD):
	python3 -I $(ROOT)/tools/fetch.py $(XDASH_URL)/LICENSE \
	    3c4d8c95fdcd47f61e106f1e597d8c9db37a57193d0239346edcbc9f4f2334f6 $(XDASH_DIR)/LICENSE
	python3 -I $(ROOT)/tools/fetch.py $(XDASH_URL)/xbox_hdd.qcow2 \
	    00d7df7a2bc235f8801764f00b7f40e194d1e392f7a9619d6b2396c89770f6dd $@
hdd_image: $(XDASH_HDD)

test: $(BUILD)/bios.bin $(TEST_TITLE).xbe $(HDD_FETCH)
	python3 -I $(ROOT)/tests/boot_test.py --bios $< $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) --map $(BUILD)/oxoskrnl.map --hdd $(HDD_IMAGE) \
	    --title $(TEST_TITLE).xbe --title-map $(TEST_TITLE).map $(if $(filter 1,$(SERIAL)),--serial)
	$(MAKE) test_dash test_relaunch test_fatal test_udf
	$(if $(MCPX_ROM),$(MAKE) test_mcpx)

# 48-bit addressing (spec/oxos_internal/04_lba48.md): the self-tests on a 2 TB disk, a sparse copy of the test disk grown with tests/qcow2.py --grow and kept in
# xemu_state/oxos_xemu_lba48/ (a few MB on the host). The kernel writes test patterns either side of sector 0x0FFFFFFF and at the end of the disk; boot_test.py
# then finds each one at exactly LBA * 512 in the image and checks that the start of the disk is untouched. The image itself is never written (boot_test.py
# attaches a copy).
LBA48_STATE := $(ROOT)/xemu_state/oxos_xemu_lba48
LBA48_HDD   := $(LBA48_STATE)/hdd.qcow2
$(LBA48_HDD): $(HDD_FETCH)
	mkdir -p $(LBA48_STATE)
	python3 -I $(ROOT)/tests/qcow2.py --grow $(HDD_IMAGE) $@ 2T
test_lba48: $(BUILD)/bios.bin $(TEST_TITLE).xbe $(LBA48_HDD)
	python3 -I $(ROOT)/tests/boot_test.py --bios $< $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) --map $(BUILD)/oxoskrnl.map \
	    --hdd $(LBA48_HDD) --title $(TEST_TITLE).xbe --title-map $(TEST_TITLE).map

# exFAT disks (spec/oxos_internal/07_exfat_driver.md): the self-tests on a GPT disk with one exFAT volume (256 GB, 16 KB clusters, sparse), the test disk's C:
# and E: copied into /C and /E by tests/exfat.py; kept in xemu_state/oxos_xemu_exfat/. The partition starts at sector 34 with its FAT at disk sector 2048, as
# Ubuntu's Disks app lays it out (spec/oxos_internal/05 §4); OXOS's own format (partition at 2048) is booted by test_format and test_upload. The FATX suite then
# runs on E: as a folder of the volume, and tests/exfat.py checks the whole volume afterwards (checksums, chains, bitmap).
EXFAT_STATE := $(ROOT)/xemu_state/oxos_xemu_exfat
EXFAT_HDD   := $(EXFAT_STATE)/hdd.qcow2
$(EXFAT_HDD): $(HDD_FETCH)
	mkdir -p $(EXFAT_STATE)
	printf 'stale FAT tail test' > $(EXFAT_STATE)/stale.bin
	python3 -I $(ROOT)/tests/exfat.py --from-fatx $(HDD_IMAGE) $@ --size 256G --cluster 16K --part-start 34 \
	    --add /E/XkStale.stale=$(EXFAT_STATE)/stale.bin
# Then a cold boot of the disk the first run left, with the release kernel (no self-tests to format X: again): the title finds X:'s format from the first
# boot, kept in C:\OXOS\cache\oxos_cache_partitions.cache (spec/oxos_internal/05 §5).
test_exfat: $(BUILD)/bios.bin $(TEST_TITLE).xbe $(EXFAT_HDD)
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	rm -f $(EXFAT_STATE)/after_first_boot.qcow2
	python3 -I $(ROOT)/tests/boot_test.py --bios $< $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) --map $(BUILD)/oxoskrnl.map \
	    --hdd $(EXFAT_HDD) --title $(TEST_TITLE).xbe --title-map $(TEST_TITLE).map --keep-hdd $(EXFAT_STATE)/after_first_boot.qcow2
	python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin --map $(RELEASE_BUILD)/oxoskrnl.map \
	    --hdd $(EXFAT_STATE)/after_first_boot.qcow2 --title $(TEST_TITLE).xbe --title-map $(TEST_TITLE).map --kept-cache-format; \
	    status=$$?; rm -f $(EXFAT_STATE)/after_first_boot.qcow2; exit $$status

# With a real MCPX boot ROM (MCPX_ROM=...): the dashboard phase again, reached through the overflow trick.
test_mcpx: $(HDD_FETCH)
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin --mcpx-rom $(MCPX_ROM) \
	    --map $(RELEASE_BUILD)/oxoskrnl.map --hdd $(HDD_IMAGE) --no-disc --dash

# Dashboard phase: a build without self-tests, booted without a disc, so C:\xboxdash.xbe is launched.
test_dash: $(HDD_FETCH)
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) \
	    --map $(RELEASE_BUILD)/oxoskrnl.map --hdd $(HDD_IMAGE) --no-disc --dash
	# ... with EvoX installed (C:\evoxdash.xbe) but no [dashboard] in C:\OXOS\oxos_settings.ini: still the Xbox dashboard, as stock (spec/oxos_internal/19).
	python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) \
	    --map $(RELEASE_BUILD)/oxoskrnl.map --hdd $(HDD_IMAGE) --no-disc --dash --evox
	# ... with EvoX installed and listed first: EvoX.
	python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) \
	    --map $(RELEASE_BUILD)/oxoskrnl.map --hdd $(HDD_IMAGE) --no-disc --dash --evox --dash-order 'C:\evoxdash.xbe,C:\Dashboard\default.xbe'
	# ... with C:\xboxdash.xbe moved to C:\Dashboard\default.xbe and listed after a missing EvoX: started from there.
	python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) \
	    --map $(RELEASE_BUILD)/oxoskrnl.map --hdd $(HDD_IMAGE) --no-disc --dash --xbl --dash-order 'C:\evoxdash.xbe,C:\Dashboard\default.xbe'

# Quick-reboot phase: the release build runs the kernel test title from a disc with relaunch.txt; it relaunches itself as media\second.xbe through a persisted
# LaunchDataPage and HalReturnToFirmware(2). On a 64 MB and a 128 MB console: the kernel still manages all the RAM after the quick reboots (spec/stock/09 §10).
# A third run puts C:\OXOS\oxos_settings.ini [launch_redirect] on the test disk: the relaunch must start the disc's redirected\second.xbe instead
# (spec/oxos_internal/33 §6).
test_relaunch: $(TEST_TITLE).xbe $(HDD_FETCH)
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	for run in "--memory 64" "--memory 128" "--memory 64 --redirect"; do \
	    python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) \
	        --map $(RELEASE_BUILD)/oxoskrnl.map --hdd $(HDD_IMAGE) --title $(TEST_TITLE).xbe \
	        --title-map $(TEST_TITLE).map --relaunch --boot-wait 1 $$run || exit 1; \
	done

# The kernel extensions OXOS ships (kernel/extensions/NAME/, one folder each), built with the extension SDK as it ships, like anyone's extension, into
# build/kernel/extensions/NAME/; an extension's app (NAME/app/, an nxdk app) is built too and copied there as NAME.xbe, and its settings (NAME/NAME.ini) as
# NAME.ini. oxos_trainers (Universal Trainers, spec/oxos_internal/37) gets its oxos_trainers.ini generated from the trainer pack instead.
SHIPPED_EXTENSIONS := $(notdir $(patsubst %/,%,$(wildcard extensions/*/)))
SHIPPED_EXTENSIONS_DIR := $(abspath $(ROOT)/build/kernel/extensions)
extensions: $(NXDK_READY)
	$(MAKE) -C $(ROOT)/sdk
	set -e; for name in $(SHIPPED_EXTENSIONS); do mkdir -p $(SHIPPED_EXTENSIONS_DIR)/$$name; \
	    $(MAKE) -C extensions/$$name OXOS_OUT=$(SHIPPED_EXTENSIONS_DIR)/$$name; \
	    if [ -d extensions/$$name/app ]; then $(NXDK_ENV) $(MAKE) -C extensions/$$name/app -j8; \
	        cp $(SHIPPED_EXTENSIONS_DIR)/$$name/app/bin/default.xbe $(SHIPPED_EXTENSIONS_DIR)/$$name/$$name.xbe; fi; \
	    if [ -f extensions/$$name/$$name.ini ]; then cp extensions/$$name/$$name.ini $(SHIPPED_EXTENSIONS_DIR)/$$name/; fi; done
	python3 -I $(ROOT)/tools/gen_trainers_ini.py $(ROOT)/third_party/xbox_trainers $(SHIPPED_EXTENSIONS_DIR)/oxos_trainers/oxos_trainers.ini

# Kernel extensions (spec/oxos_internal/34 §9.3): the test extensions (tests/extensions), the SDK's examples and the shipped extensions, built with the SDK as
# it ships (build/sdk/oxos_sdk/), and an oxos_extensions.ini on the test disk; the relaunch's three launches must keep their state pages, run their hooks,
# events, RunLoops and Unloads (and log a slow RunLoop in C:\OXOS\log\extensions.log), and leave out the ones that fail, are refused or are not listed. The
# SDK's import definitions are brought up to date with exports.tsv first (sdk/src/oxoskrnl.def, committed with it when they change). It boots twice: the
# relaunch (media\second.xbe) as the same build of the test title, which gets no second trainer menu, then as another build (--other-build), which gets one
# (spec/oxos_internal/37 §7.5b).
TEST_EXTENSIONS_DIR := $(abspath $(ROOT)/build/tests/extensions)
test_extensions: $(TEST_TITLE).xbe $(HDD_FETCH)
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	python3 -I $(ROOT)/tools/gen_oxos_sdk.py exports.tsv src $(ROOT)/sdk/src/oxoskrnl.def $(ROOT)/sdk/include/oxos_kernel.h \
	    $(ROOT)/sdk/src/kernel_table_check.c $(ROOT)/sdk/src/oxos_c_library.cpp
	$(MAKE) -C $(ROOT)/sdk
	mkdir -p $(TEST_EXTENSIONS_DIR)
	$(MAKE) -C $(ROOT)/tests/extensions OXOS_OUT=$(TEST_EXTENSIONS_DIR)
	cp $(ROOT)/build/sdk/examples/*/*.xos $(TEST_EXTENSIONS_DIR)/
	$(MAKE) extensions
	cp $(SHIPPED_EXTENSIONS_DIR)/*/*.xos $(SHIPPED_EXTENSIONS_DIR)/*/*.xbe $(TEST_EXTENSIONS_DIR)/
	for build in "" --other-build; do \
	    python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) \
	        --map $(RELEASE_BUILD)/oxoskrnl.map --hdd $(HDD_IMAGE) --title $(TEST_TITLE).xbe \
	        --title-map $(TEST_TITLE).map --relaunch --boot-wait 1 --memory 64 --extensions $(TEST_EXTENSIONS_DIR) $$build || exit 1; \
	done

# Video encoder phase: the encoder tables must be what tools/gen_av_tables.py makes from the specs (spec/stock/18-20); then the release build on each encoder
# xemu can present (Conexant, Focus, Xcalibur): the bus probe finds it, XboxHardwareInfo reports it, and the dashboard's mode set programs it (Conexant and
# Focus registers read back as written; the Xcalibur's writes are compared with spec/stock/20); then the self-test build on each encoder, whose boot self-test
# sets 15 mode words and the encoder options and checks them.
test_encoders: $(HDD_FETCH)
	@python3 -I $(ROOT)/tools/gen_av_tables.py --check src/hardware/video/video_modes.inc || { echo "src/hardware/video/video_modes.inc is stale: run tools/gen_av_tables.py --out src/hardware/video/video_modes.inc"; exit 1; }
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	for e in conexant focus xcalibur; do python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin \
	    $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) --map $(RELEASE_BUILD)/oxoskrnl.map --hdd $(HDD_IMAGE) --no-disc \
	    --encoder $$e --boot-wait 2 || exit 1; done
	$(MAKE) $(BUILD)/bios.bin
	for e in conexant focus xcalibur; do python3 -I $(ROOT)/tests/boot_test.py --bios $(BUILD)/bios.bin \
	    $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) --map $(BUILD)/oxoskrnl.map --hdd $(HDD_IMAGE) --no-disc \
	    --encoder $$e --boot-wait 2 || exit 1; done

# Fatal-error phase: no hard disk, so the disk driver reports a fatal error (EEPROM history, code on screen).
test_fatal:
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) \
	    --map $(RELEASE_BUILD)/oxoskrnl.map --no-disc --fatal --boot-wait 2

# Formatting at boot (spec/oxos_internal/30 §4): a new blank 16 GB disk (5 s countdown: not a release build), a blank one with the partition delete's marker
# asking for 0 s, and a PC disk (MBR, NTFS; 5 s); each is formatted, then, with no dashboard on it, the upload server starts (no network link in these runs: it
# waits for one).
FORMAT_TEST = python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin \
    $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) --map $(RELEASE_BUILD)/oxoskrnl.map --no-disc --blank-hdd 16 --boot-wait 2
test_format:
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	$(FORMAT_TEST)
	$(FORMAT_TEST) --blank-marker 0
	$(FORMAT_TEST) --foreign
	$(FORMAT_TEST) --blank-marker 0 --x-at-error

# The upload server (spec/oxos_internal/31): a blank disk formatted at once (exFAT), no dashboard on it, so the kernel serves the upload page; the test uploads
# a file and the FTP server's XBE through xemu's NAT network and launches it. Then the same on two FATX disks (copies), opened with X held during the splash:
# the 8 GB dashboard disk (no F:), and that disk grown to 16 GB with an empty F: (tests/fatx.py --format-f; kept in xemu_state/oxos_xemu_fatx16/).
FATX16_STATE := $(ROOT)/xemu_state/oxos_xemu_fatx16
FATX16_HDD   := $(FATX16_STATE)/hdd.qcow2
$(FATX16_HDD): $(XDASH_HDD)
	mkdir -p $(FATX16_STATE)
	python3 -I $(ROOT)/tests/qcow2.py --grow $(XDASH_HDD) $(FATX16_STATE)/grown.qcow2 16G
	python3 -I $(ROOT)/tests/fatx.py $(FATX16_STATE)/grown.qcow2 --format-f --out $@
	rm -f $(FATX16_STATE)/grown.qcow2
UPLOAD_FATX_TEST = python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin \
    $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) --map $(RELEASE_BUILD)/oxoskrnl.map --no-disc --boot-wait 2 \
    --upload 18080 --upload-xbe $(OXOS_FTP_XBE)
test_upload: $(OXOS_FTP_XBE) $(XDASH_HDD) $(FATX16_HDD)
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	$(FORMAT_TEST) --blank-marker 0 --upload 18080 --upload-xbe $(OXOS_FTP_XBE)
	$(UPLOAD_FATX_TEST) --hdd $(XDASH_HDD)
	$(UPLOAD_FATX_TEST) --hdd $(FATX16_HDD)

# UDF phase (milestone 8): the self-test build with a UDF disc (tests/mkudf.py) instead of the XISO.
test_udf: $(BUILD)/bios.bin $(HDD_FETCH)
	python3 -I $(ROOT)/tests/boot_test.py --bios $(BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) --map $(BUILD)/oxoskrnl.map \
	    --hdd $(HDD_IMAGE) --udf --boot-wait 4

# Interactive: boot the release BIOS in xemu; optional ISO=path/to/game.iso. The HDD copy and EEPROM persist in STATE (saves and settings survive; made from
# PLAY_HDD the first time, which is never written): FRESH=1 starts it over, STATE= (empty) uses a throwaway copy. Display, by default widescreen in a 1920x1080
# window at twice the render resolution: WIDE=0 (no EEPROM widescreen flag; a flag already saved in STATE stays until FRESH=1 or the dashboard clears it),
# ASPECT=auto|native|4x3|16x9 (xemu's picture aspect; auto follows the widescreen signal each game sends), SIZE= (window size), SCALE=1..10, FULL=1 (full
# screen). HD=1: the HDTV AV pack and the EEPROM's 480p/720p/1080i flags (HD=480p,720p for a subset), so titles use their HD modes. ENCODER=focus or xcalibur:
# the video encoder xemu presents (default conexant).
# PLAY_HDD: the disk image the run targets start from, by default the stock Microsoft dashboard image when you have one (its Settings set up video modes, clock,
# language); the tests keep using HDD_IMAGE. One state directory per image, so switching images never mixes saves.
PLAY_HDD    ?= $(firstword $(wildcard $(ROOT)/xbox_files/hdd/msdash.qcow2) $(HDD_IMAGE))
# Each xemu instance keeps its state (hard-disk copy, EEPROM) in xemu_state/<bios>_<INSTANCE>: oxos_xemu (the default xemu_dashboard.qcow2), oxos_msdash
# (xbox_files/hdd/msdash.qcow2), or INSTANCE=name for any other disk (default: its file name); stock_<INSTANCE> for make run_stock.
INSTANCE    ?= $(if $(filter $(XDASH_HDD),$(abspath $(PLAY_HDD))),xemu,$(if $(findstring msdash,$(notdir $(PLAY_HDD))),msdash,$(subst -,_,$(basename $(notdir $(PLAY_HDD))))))
STATE       ?= $(ROOT)/xemu_state/oxos_$(INSTANCE)
# run_stock with another BIOS (BIOS_BASE=...): BIOS_NAME names its state, e.g. BIOS_NAME=evox -> evox_<INSTANCE>.
BIOS_NAME   ?= stock
STATE_STOCK ?= $(ROOT)/xemu_state/$(BIOS_NAME)_$(INSTANCE)
PLAY_FETCH   = $(filter $(XDASH_HDD),$(PLAY_HDD))
WIDE        ?= 1
SIZE        ?= 1920x1080
SCALE       ?= 2
ASPECT      ?= auto
RUN_OPTS     = --hdd $(PLAY_HDD) $(if $(ISO),--iso $(ISO)) $(if $(FRESH),--fresh) $(if $(filter 1,$(WIDE)),--widescreen) \
               $(if $(HD),--hd $(if $(filter 1,$(HD)),,$(HD))) $(if $(ENCODER),--encoder $(ENCODER)) \
               $(if $(MEMORY),--memory $(MEMORY)) \
               $(if $(ASPECT),--aspect $(ASPECT)) \
               $(if $(SIZE),--size $(SIZE)) $(if $(SCALE),--scale $(SCALE)) $(if $(FULL),--fullscreen)
run: $(PLAY_FETCH)
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	python3 -I $(ROOT)/tools/run.py $(RELEASE_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) $(if $(STATE),--state $(STATE)) $(RUN_OPTS)

# The same with the debug serial port (spec/oxos_internal/03_debug_serial.md), whose output is printed in this terminal. No import tracing, so titles see the
# memory of a release build (plus the serial code); also the build to flash for troubleshooting on hardware.
run_trace_basic: $(PLAY_FETCH)
	$(MAKE) BUILD=$(TRACE_BASIC_BUILD) SELFTEST=0 SERIAL=1 $(TRACE_BASIC_BUILD)/bios.bin
	python3 -I $(ROOT)/tools/run.py $(TRACE_BASIC_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) $(if $(STATE),--state $(STATE)) $(RUN_OPTS) --serial

# Source-level debugging: the same as run_trace_basic with the debug kernel (DEBUG=1) and xemu's GDB server on localhost:$(GDB_PORT), paused at power-on. Attach
# with gdb-multiarch (symbols: $(DEBUG_BUILD)/oxoskrnl.elf, plus an app's own); tools/debug_app.py does the same for an nxdk app, and tools/vscode/ sets it up
# for VS Code.
GDB_PORT    ?= 1234
run_gdb: $(PLAY_FETCH)
	$(MAKE) BUILD=$(DEBUG_BUILD) SELFTEST=0 SERIAL=1 DEBUG=1 $(DEBUG_BUILD)/bios.bin
	@echo "attach from another terminal: gdb-multiarch $(CURDIR)/$(DEBUG_BUILD)/oxoskrnl.elf -ex 'target remote localhost:$(GDB_PORT)'"
	python3 -I $(ROOT)/tools/run.py $(DEBUG_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) $(if $(STATE),--state $(STATE)) $(RUN_OPTS) \
	    --serial --gdb $(GDB_PORT)

# Import tracing: the same as run_trace_basic, plus a record of the title's last 32768 kernel calls. They are dumped by themselves when the kernel reports a
# crash and when xemu is closed (the last 30 in this terminal, all of them in $(TRACE_BUILD)/dumps/); at any other time, `make print_trace_dump` in another
# terminal prints them (tools/trace.py; LAST=N calls, ONLY=Name,Name, ALL=1 shows the common calls too). TRACE=2 (default) keeps the record above 64 MB, with
# xemu set to 128 MB, so titles get the memory of a release build (Halo 2 needs it) and the calls from before a quick reboot are kept. TRACE=1 keeps it in the
# console's 64 MB (2 MB less for the title). Each mode has its own build folder, because make does not rebuild when only the flags change; pass the same TRACE
# to print_trace_dump.
TRACE       ?= 2
TRACE_BUILD := $(OUT)/trace_full$(if $(filter 1,$(TRACE)),-64mb)
run_trace_full: $(PLAY_FETCH)
	$(MAKE) BUILD=$(TRACE_BUILD) SELFTEST=0 TRACE_IMPORTS=$(TRACE) SERIAL=1 $(TRACE_BUILD)/bios.bin
	python3 -I $(ROOT)/tools/run.py $(TRACE_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) $(if $(STATE),--state $(STATE)) $(RUN_OPTS) \
	    --serial --qmp $(TRACE_BUILD)/qmp.sock $(if $(filter 2,$(TRACE)),--memory 128) \
	    --trace-map $(TRACE_BUILD)/oxoskrnl.map --dump-dir $(TRACE_BUILD)/dumps

print_trace_dump:
	python3 -I $(ROOT)/tools/trace.py --qmp $(TRACE_BUILD)/qmp.sock --map $(TRACE_BUILD)/oxoskrnl.map \
	    --exports exports.tsv $(if $(LAST),--last $(LAST)) $(if $(ONLY),--only $(ONLY)) $(if $(filter 1,$(ALL)),--all)

# nxdk and SDL3 for the console apps (tools/*, tests/*_title except the kernel test title), fetched into third_party/ (not committed) by `make nxdk` or the
# first app build:
#   - nxdk: kooscode/nxdk, branch oxos: XboxDev nxdk fafbe41 with pbkit's graphics-context fix (pb_init on a cold boot; XboxDev/nxdk#801), lwIP sockets for
#     more than one network client (MEMP_NUM_NETCONN 32), and pdclib from kooscode/nxdk-pdclib, branch oxos: setvbuf fixes, faster fread, fwrite and
#     memcpy/memmove/memset (XboxDev/nxdk-pdclib#63-66); -O2 by default (not with DEBUG=y), for the apps and nxdk's libraries; and hal/audio's fixes
#     (XboxDev/nxdk#806): no cold reset of a running codec, and the descriptor lists kept within one page; lwIP from kooscode/lwip (branch oxos), which
#     follows lwip-tcpip/lwip master for its fixes; SDL2 from kooscode/nxdk-sdl (branch oxos); and what the XB Live dashboard added (sound shutdown and
#     resync, network driver shutdown, pbkit additions; pdclib's malloc returning freed memory to the system, malloc_trim);
#   - SDL3: kooscode/nxdk-sdl3, branch oxos: Ryzee119's nxdk-sdl3 master (it builds on that nxdk) with the XGU renderer's waits for a buffer flip in
#     RenderPresent capped at 3 vertical blanks (under xemu a flip can stall for good; Ryzee119/nxdk-sdl3#7), the XGU renderer's vertex caches invalidated
#     before every draw (a stale vertex drew wedges after render-to-texture), and the XB Live dashboard's audio, joystick and video additions.
# Every app draws and reads the controllers through SDL3 and the shared text screen in tools/sdl_screen.
APPS_BUILD       := $(abspath $(ROOT)/build)
NXDK_DIR         := $(abspath $(ROOT)/third_party/nxdk)
NXDK_URL         := https://github.com/kooscode/nxdk
NXDK_COMMIT      := 65c9816753b15036bfd08d4536cd86a9e8e5573d
NXDK_SDL3_DIR    := $(abspath $(ROOT)/third_party/nxdk-sdl3)
NXDK_SDL3_URL    := https://github.com/kooscode/nxdk-sdl3
NXDK_SDL3_COMMIT := 11aeba85bfc0e76873e64d99779f1939803ffd7c
# nxdk's wrappers call clang, lld-link and llvm-lib by name: LLVM's folder, else the folder of the system's clang (Ubuntu's /usr/lib/llvm-NN/bin, which
# also has the plain llvm-lib).
NXDK_LLVM        := $(if $(LLVM),$(abspath $(LLVM)),$(patsubst %/,%,$(dir $(realpath $(shell command -v clang)))))
NXDK_ENV         := NXDK_DIR=$(NXDK_DIR) PATH=$(NXDK_DIR)/bin:$(NXDK_LLVM):$$PATH
SDL_SCREEN       := $(addprefix $(ROOT)/tools/sdl_screen/,sdl_screen.cpp sdl_screen.h sdl_screen.mk)
# Both checkouts at their pinned commits: the stamp's name holds the two commits, the optimisation level (nxdk's default, -O2) and the compiler's
# version, so a new pin, level or compiler checks out again and rebuilds every library and app.
NXDK_READY := $(ROOT)/third_party/.nxdk_$(NXDK_COMMIT)_$(NXDK_SDL3_COMMIT)_O2_clang$(CLANG_VERSION)

$(NXDK_READY):
	set -e; checkout() { \
	    [ -d $$1/.git ] || git clone -q $$2 $$1; \
	    git -C $$1 cat-file -e $$3^{commit} 2>/dev/null || git -C $$1 fetch -q $$2 $$3; \
	    git -C $$1 checkout -q $$3; \
	    git -C $$1 submodule sync -q --recursive; \
	    git -C $$1 submodule update -q --init --recursive; \
	}; \
	checkout $(NXDK_DIR) $(NXDK_URL) $(NXDK_COMMIT); \
	checkout $(NXDK_SDL3_DIR) $(NXDK_SDL3_URL) $(NXDK_SDL3_COMMIT)
	# nxdk rebuilds a library object only when its .c file changes, not when a header does: the objects start over, so every library (and SDL3) is built from the
	# pinned sources.
	find $(NXDK_DIR)/lib $(NXDK_SDL3_DIR) -name '*.obj' -delete
	rm -f $(NXDK_DIR)/lib/*.lib
	# The apps' own objects too (in build/<folder>, next to main.exe), so every app is built again with the new nxdk and flags.
	find $(APPS_BUILD) -name main.exe -not -path '*/regression/*' | while read -r exe; do rm -f "$$(dirname "$$exe")"/*.obj "$$exe"; done
	rm -f $(ROOT)/third_party/.nxdk_*
	touch $@
nxdk: $(NXDK_READY)

# Controller input test (tests/controller_test_title): a virtual uinput gamepad presses every input and the title reports what it received.
# CONTROLLER_BIOS=stock compares with the reference BIOS.
CONTROLLER_BIOS ?= ours
CONTROLLER_DIR  := $(ROOT)/tests/controller_test_title
CONTROLLER_ISO  := $(APPS_BUILD)/tests/controller_test_title/controller_test_title.iso

$(CONTROLLER_ISO): $(NXDK_READY) $(SDL_SCREEN) $(CONTROLLER_DIR)/controller_test_title.cpp $(CONTROLLER_DIR)/Makefile
	$(NXDK_ENV) $(MAKE) -C $(CONTROLLER_DIR) -j8

# Image test title (tests/image_test_title, an nxdk title): a picture decoded on the console, a status panel and the front LED changing once a second: the
# kernel, a title and video work. make image_test builds its disc image; make run_image_test boots it in xemu with the release BIOS (RUN_OPTS as make run).
IMAGE_TEST_DIR := $(ROOT)/tests/image_test_title
IMAGE_TEST_ISO := $(APPS_BUILD)/tests/image_test_title/image_test.iso
$(IMAGE_TEST_ISO): $(NXDK_READY) $(SDL_SCREEN) $(IMAGE_TEST_DIR)/image_test.cpp $(IMAGE_TEST_DIR)/Makefile \
                   $(IMAGE_TEST_DIR)/picture.jpg
	$(NXDK_ENV) $(MAKE) -C $(IMAGE_TEST_DIR) -j8
image_test: $(IMAGE_TEST_ISO)
run_image_test: $(IMAGE_TEST_ISO) $(PLAY_FETCH)
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	python3 -I $(ROOT)/tools/run.py $(RELEASE_BUILD)/bios.bin --iso $(IMAGE_TEST_ISO) $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) $(RUN_OPTS)

test_controller: $(CONTROLLER_ISO) $(HDD_FETCH)
	$(if $(filter stock,$(CONTROLLER_BIOS)),,$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin)
	$(if $(filter stock,$(CONTROLLER_BIOS)),,$(MAKE) extensions)
	python3 -I $(ROOT)/tests/controller_test.py --bios $(if $(filter stock,$(CONTROLLER_BIOS)),$(BIOS_BASE),$(RELEASE_BUILD)/bios.bin) \
		$(if $(filter stock,$(CONTROLLER_BIOS)),--mcpx-rom $(STOCK_MCPX),$(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM))) \
		--hdd $(HDD_IMAGE) --iso $(CONTROLLER_ISO) $(if $(filter stock,$(CONTROLLER_BIOS)),,--igs $(SHIPPED_EXTENSIONS_DIR)/oxos_igs/oxos_igs.xos)

# Run an XBE from the hard disk, e.g. a homebrew app copied to F: on an exFAT disk: a disc with the XBE launcher (tools/xbe_launcher, nxdk) and launch.txt = XBE
# boots, and the launcher starts that XBE the way a dashboard does (the XBE then runs from the disk, with D: its folder). XBE='F:\apps\demo\default.xbe'
# (quoted, for the backslashes).
# The disk: xemu_state/oxos_exfat/hdd.qcow2, an exFAT copy of PLAY_HDD made the first time (mount it on the PC to copy files in; unmount it before running), or
# STATE=... for another state folder. run_trace_xbe is the same with the trace build (run_trace_full): the debug serial port in this terminal, the kernel call
# record dumped on a crash.
LAUNCHER_DIR := $(ROOT)/tools/xbe_launcher
LAUNCHER_XBE := $(APPS_BUILD)/tools/xbe_launcher/bin/default.xbe
XBE_STATE    := $(if $(filter command line,$(origin STATE)),$(STATE),$(ROOT)/xemu_state/oxos_exfat)

$(LAUNCHER_XBE): $(NXDK_READY) $(SDL_SCREEN) $(LAUNCHER_DIR)/xbe_launcher.cpp $(LAUNCHER_DIR)/Makefile
	$(NXDK_ENV) $(MAKE) -C $(LAUNCHER_DIR) -j8

xbe_disc: $(LAUNCHER_XBE) $(PLAY_FETCH)
	@test -n '$(XBE)' || { printf '%s\n' "usage: make run_xbe (or run_trace_xbe) XBE='F:\\apps\\demo\\default.xbe' [STATE=...]"; exit 1; }
	@test -f $(XBE_STATE)/hdd.qcow2 || { mkdir -p $(XBE_STATE) && \
	    python3 -I $(ROOT)/tests/exfat.py --from-fatx $(PLAY_HDD) $(XBE_STATE)/hdd.qcow2 --size 256G --cluster 16K; }
	@echo "disk: $(XBE_STATE)/hdd.qcow2"
	@python3 -I $(ROOT)/tests/exfat.py --has $(XBE_STATE)/hdd.qcow2 '$(XBE)'; r=$$?; \
	    if [ $$r = 1 ]; then printf '%s\n' "$(XBE) is not on that disk (STATE=... picks another state folder)"; \
	        echo "F: holds:"; python3 -I $(ROOT)/tests/exfat.py --ls $(XBE_STATE)/hdd.qcow2 /F | grep . || echo "  (nothing)"; exit 1; fi
	@mkdir -p $(OUT)/run_xbe
	python3 -I $(ROOT)/tests/mkxiso.py $(OUT)/run_xbe/launcher.iso --add default.xbe=$(LAUNCHER_XBE) --text 'launch.txt=$(XBE)'

run_xbe: xbe_disc
	$(MAKE) run ISO=$(OUT)/run_xbe/launcher.iso STATE=$(XBE_STATE)

run_trace_xbe: xbe_disc
	$(MAKE) run_trace_full ISO=$(OUT)/run_xbe/launcher.iso STATE=$(XBE_STATE)

# Disc images (spec/oxos_internal/16_virtual_discs.md): the ISO launcher (tools/iso_launcher, nxdk) mounts an image from the hard disk as the console's disc and
# starts its game. make test_iso: the release build boots a disc with the launcher; the image (the kernel test title, relaunch variant) is on an exFAT disk's
# F:\titles, as one .iso, a split .1.iso/.2.iso pair, a .cci and an image over 4 GB; the title runs from it, relaunches itself and ends in the dashboard. make
# run_iso ISO='F:\titles\Halo 2.iso' boots the launcher in xemu for an image on the run_xbe disk.
ISO_LAUNCHER_DIR := $(ROOT)/tools/iso_launcher
ISO_LAUNCHER_XBE := $(APPS_BUILD)/tools/iso_launcher/bin/default.xbe
ISO_KINDS ?= plain split cci splitcci large

$(ISO_LAUNCHER_XBE): $(NXDK_READY) $(SDL_SCREEN) $(ISO_LAUNCHER_DIR)/iso_launcher.cpp $(ISO_LAUNCHER_DIR)/mount_iso.h \
                     $(ISO_LAUNCHER_DIR)/Makefile
	$(NXDK_ENV) $(MAKE) -C $(ISO_LAUNCHER_DIR) -j8
iso_launcher: $(ISO_LAUNCHER_XBE)

# Title launcher (tools/title_launcher, nxdk): lists F:\games, F:\titles (and game folders) and F:\Applications and starts a disc image (mounted) or an XBE
# chosen with the controller. make title_launcher builds build/tools/title_launcher/bin/default.xbe and title_launcher.iso.
TITLE_LAUNCHER_DIR := $(ROOT)/tools/title_launcher
TITLE_LAUNCHER_XBE := $(APPS_BUILD)/tools/title_launcher/bin/default.xbe
$(TITLE_LAUNCHER_XBE): $(NXDK_READY) $(SDL_SCREEN) $(TITLE_LAUNCHER_DIR)/title_launcher.cpp \
                       $(ISO_LAUNCHER_DIR)/mount_iso.h $(TITLE_LAUNCHER_DIR)/Makefile
	$(NXDK_ENV) $(MAKE) -C $(TITLE_LAUNCHER_DIR) -j8
title_launcher: $(TITLE_LAUNCHER_XBE)

# OXOS's apps (apps/, nxdk; C:\OXOS\apps\ on the console, OXOS\apps\ in the release zip). make apps builds them all.
# The FTP server (apps/oxos_ftp): the console's disks over the network (copy, move, delete with any FTP client). make oxos_ftp builds
# build/apps/oxos_ftp/bin/default.xbe and oxos_ftp.iso; netboot.py IMAGE --title build/apps/oxos_ftp/bin --title-name ftp runs it.
OXOS_FTP_DIR := $(ROOT)/apps/oxos_ftp
OXOS_FTP_XBE := $(APPS_BUILD)/apps/oxos_ftp/bin/default.xbe
$(OXOS_FTP_XBE): $(NXDK_READY) $(SDL_SCREEN) $(OXOS_FTP_DIR)/oxos_ftp.cpp $(OXOS_FTP_DIR)/Makefile
	$(NXDK_ENV) $(MAKE) -C $(OXOS_FTP_DIR) -j8
oxos_ftp: $(OXOS_FTP_XBE)
# The boot video player (apps/oxos_boot_video) is below, after the video decoders it needs.

test_iso: $(ISO_LAUNCHER_XBE) $(TEST_TITLE).xbe $(HDD_FETCH)
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	set -e; for kind in $(ISO_KINDS); do \
	    python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) \
	        --map $(RELEASE_BUILD)/oxoskrnl.map --hdd $(HDD_IMAGE) --title $(TEST_TITLE).xbe \
	        --title-map $(TEST_TITLE).map --iso-launch $(ISO_LAUNCHER_XBE) --iso-kind $$kind --boot-wait 1; done

iso_disc: $(ISO_LAUNCHER_XBE) $(PLAY_FETCH)
	@test -n '$(ISO)' || { printf '%s\n' "usage: make run_iso ISO='F:\\titles\\Halo 2.iso' [STATE=...]"; exit 1; }
	@test -f $(XBE_STATE)/hdd.qcow2 || { mkdir -p $(XBE_STATE) && \
	    python3 -I $(ROOT)/tests/exfat.py --from-fatx $(PLAY_HDD) $(XBE_STATE)/hdd.qcow2 --size 256G --cluster 16K; }
	@mkdir -p $(OUT)/run_iso
	python3 -I $(ROOT)/tests/mkxiso.py $(OUT)/run_iso/launcher.iso --add default.xbe=$(ISO_LAUNCHER_XBE) --text 'iso.txt=$(ISO)'

run_iso: iso_disc
	$(MAKE) run ISO=$(OUT)/run_iso/launcher.iso STATE=$(XBE_STATE)

# Memory test (tests/memory_test_title, an nxdk title): how much memory a title is given, as built (XBE flag "limit to 64 MB") and with the flag cleared.
# MEMORY=128 for a 128 MB console; MEMORY_BIOS=... (with MCPX_ROM=...) runs another BIOS instead of the release build, e.g. for comparison.
MEMORY_DIR  := $(ROOT)/tests/memory_test_title
MEMORY_XBE  := $(APPS_BUILD)/tests/memory_test_title/bin/default.xbe

$(MEMORY_XBE): $(NXDK_READY) $(SDL_SCREEN) $(MEMORY_DIR)/memory_test_title.cpp $(MEMORY_DIR)/Makefile
	$(NXDK_ENV) $(MAKE) -C $(MEMORY_DIR) -j8

test_memory: $(MEMORY_XBE) $(HDD_FETCH)
	$(if $(MEMORY_BIOS),,$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin)
	python3 -I $(ROOT)/tests/memory_test.py --bios $(or $(MEMORY_BIOS),$(RELEASE_BUILD)/bios.bin) --xbe $(MEMORY_XBE) \
	    --both --memory $(or $(MEMORY),64) --hdd $(HDD_IMAGE) $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM))

# FATX disk layouts (spec/oxos_internal/45 §8, §9): large disks laid out and formatted as disk formatters do (tables with 48-bit entries,
# clusters up to 1 MB, F: or G: to the disk's end, a stale table at the end), each booted with the partition probe title (tests/partition_probe_title), which
# reports every partition's place and reads and writes a file on F: and G:.
PROBE_DIR := $(ROOT)/tests/partition_probe_title
PROBE_XBE := $(APPS_BUILD)/tests/partition_probe_title/bin/default.xbe

$(PROBE_XBE): $(NXDK_READY) $(PROBE_DIR)/partition_probe_title.cpp $(PROBE_DIR)/Makefile
	$(NXDK_ENV) $(MAKE) -C $(PROBE_DIR) -j8

test_fatx_layouts: $(PROBE_XBE) $(HDD_FETCH)
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	python3 -I $(ROOT)/tests/fatx_layout_test.py --bios $(RELEASE_BUILD)/bios.bin --xbe $(PROBE_XBE) --hdd $(HDD_IMAGE) \
	    $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) $(if $(ONLY_CASES),--only $(ONLY_CASES))

# The GDB stub (GDB=1 builds, kernel/src/debugger): tests/gdb_test.py boots a GDB_WAIT=1 kernel in xemu with the gdb test title (tests/gdb_test_title) on a
# disc and drives the stub over xemu's serial port as gdb does: the boot stop, Ctrl-C, a title breakpoint set before the title loads, threads, a step, a
# watchpoint, the title's access violation, attaching after the bug check, and the kernel's code left as it was.
GDB_BUILD      := $(OUT)/gdb_wait
GDB_TITLE_DIR  := $(ROOT)/tests/gdb_test_title
GDB_TITLE_XBE  := $(APPS_BUILD)/tests/gdb_test_title_debug/bin/default.xbe

$(GDB_TITLE_XBE): $(NXDK_READY) $(SDL_SCREEN) $(GDB_TITLE_DIR)/gdb_test_title.cpp $(GDB_TITLE_DIR)/Makefile
	$(NXDK_ENV) DEBUG=y LDFLAGS=-debug:dwarf $(MAKE) -C $(GDB_TITLE_DIR) -j8

test_gdb: $(GDB_TITLE_XBE) $(HDD_FETCH)
	$(MAKE) BUILD=$(GDB_BUILD) SELFTEST=0 DEBUG=1 GDB=1 GDB_WAIT=1 $(GDB_BUILD)/bios.bin
	python3 -I $(ROOT)/tests/gdb_test.py --bios $(GDB_BUILD)/bios.bin --map $(GDB_BUILD)/oxoskrnl.map --title $(APPS_BUILD)/tests/gdb_test_title_debug \
	    --hdd $(HDD_IMAGE) $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM))

# The GDB debugger as a kernel extension (oxos_gdb.xos, kernel/extensions/oxos_gdb, spec/oxos_internal/51 §3): the release kernel with oxos_gdb.xos enabled
# on the test disk, its title waiting for gdb at its start; tests/gdb_test.py --extension drives the same session as test_gdb from there, through
# tools/gdb_bridge.py as gdb on the Modxo rig talks (--bridge).
GDB_EXTENSION := $(SHIPPED_EXTENSIONS_DIR)/oxos_gdb/oxos_gdb.xos
test_gdb_extension: $(GDB_TITLE_XBE) $(HDD_FETCH)
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	python3 -I $(ROOT)/tools/gen_oxos_sdk.py exports.tsv src $(ROOT)/sdk/src/oxoskrnl.def $(ROOT)/sdk/include/oxos_kernel.h \
	    $(ROOT)/sdk/src/kernel_table_check.c $(ROOT)/sdk/src/oxos_c_library.cpp
	$(MAKE) -C $(ROOT)/sdk
	mkdir -p $(SHIPPED_EXTENSIONS_DIR)/oxos_gdb
	$(MAKE) -C extensions/oxos_gdb OXOS_OUT=$(SHIPPED_EXTENSIONS_DIR)/oxos_gdb
	python3 -I $(ROOT)/tests/gdb_test.py --bios $(RELEASE_BUILD)/bios.bin --map $(RELEASE_BUILD)/oxoskrnl.map --title $(APPS_BUILD)/tests/gdb_test_title_debug \
	    --hdd $(HDD_IMAGE) --extension $(GDB_EXTENSION) --bridge $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM))

# The boot video's decoders (spec/oxos_internal/38 §3.1), fetched into third_party/: libmpeg2 0.5.1 (MPEG-1 and MPEG-2 video, GPL v2 or later), its last
# release, from Debian's copy and checked against Debian's SHA-256; minimp3 (MP2 and MP3 sound, CC0) at a pinned commit.
LIBMPEG2_DIR    := $(ROOT)/third_party/libmpeg2
LIBMPEG2_URL    := https://deb.debian.org/debian/pool/main/m/mpeg2dec/mpeg2dec_0.5.1.orig.tar.gz
LIBMPEG2_SHA256 := dee22e893cb5fc2b2b6ebd60b88478ab8556cb3b93f9a0d7ce8f3b61851871d4
MINIMP3_DIR     := $(ROOT)/third_party/minimp3
MINIMP3_URL     := https://github.com/lieff/minimp3
MINIMP3_COMMIT  := 03b957e66532db2370e7a5a6ee113063528b1286
# Both at their pinned versions: the stamp's name holds them, so a new pin fetches again.
VIDEO_LIBRARIES_READY := $(ROOT)/third_party/.video_libraries_$(LIBMPEG2_SHA256)_$(MINIMP3_COMMIT)

$(VIDEO_LIBRARIES_READY):
	set -e; rm -rf $(LIBMPEG2_DIR) $(LIBMPEG2_DIR).tar.gz; \
	curl -sSLf -o $(LIBMPEG2_DIR).tar.gz $(LIBMPEG2_URL); \
	echo "$(LIBMPEG2_SHA256)  $(LIBMPEG2_DIR).tar.gz" | sha256sum -c --quiet; \
	mkdir $(LIBMPEG2_DIR); \
	tar xzf $(LIBMPEG2_DIR).tar.gz -C $(LIBMPEG2_DIR) --strip-components=1; \
	rm $(LIBMPEG2_DIR).tar.gz; \
	[ -d $(MINIMP3_DIR)/.git ] || git clone -q $(MINIMP3_URL) $(MINIMP3_DIR); \
	git -C $(MINIMP3_DIR) cat-file -e $(MINIMP3_COMMIT)^{commit} 2>/dev/null || git -C $(MINIMP3_DIR) fetch -q $(MINIMP3_URL); \
	git -C $(MINIMP3_DIR) checkout -q $(MINIMP3_COMMIT)
	rm -f $(ROOT)/third_party/.video_libraries_*
	touch $@
video_libraries: $(VIDEO_LIBRARIES_READY)

# Video decode test (tests/video_decode_title, an nxdk title): how fast the console decodes, converts and shows every .mpg file next to it, and decodes its
# sound; run on a console (results on the debug serial port and in video_decode_results.txt next to it), spec/oxos_internal/38 §5.
VIDEO_DECODE_DIR := $(ROOT)/tests/video_decode_title
VIDEO_DECODE_XBE := $(APPS_BUILD)/tests/video_decode_title/bin/default.xbe

VIDEO_PLAYER_FILES := $(wildcard $(ROOT)/shared/video_player/*.cpp $(ROOT)/shared/video_player/*.h $(ROOT)/shared/video_player/*.mk) \
                      $(ROOT)/shared/video_player/libmpeg2_config/config.h
$(VIDEO_DECODE_XBE): $(NXDK_READY) $(VIDEO_LIBRARIES_READY) $(SDL_SCREEN) $(VIDEO_DECODE_DIR)/video_decode_title.cpp $(VIDEO_DECODE_DIR)/Makefile \
                     $(VIDEO_PLAYER_FILES)
	$(NXDK_ENV) $(MAKE) -C $(VIDEO_DECODE_DIR) -j8
video_decode_title: $(VIDEO_DECODE_XBE)

# The boot video player (apps/oxos_boot_video, spec/oxos_internal/38): the kernel starts it before the first title when C:\OXOS\oxos_settings.ini asks.
OXOS_BOOT_VIDEO_DIR := $(ROOT)/apps/oxos_boot_video
OXOS_BOOT_VIDEO_XBE := $(APPS_BUILD)/apps/oxos_boot_video/bin/default.xbe
$(OXOS_BOOT_VIDEO_XBE): $(NXDK_READY) $(VIDEO_LIBRARIES_READY) $(SDL_SCREEN) $(OXOS_BOOT_VIDEO_DIR)/oxos_boot_video.cpp $(OXOS_BOOT_VIDEO_DIR)/Makefile \
                        $(VIDEO_PLAYER_FILES) $(wildcard $(ROOT)/shared/ini_file/*) $(wildcard $(ROOT)/shared/graphics/*) splash/oxos_splash.svg
	$(NXDK_ENV) $(MAKE) -C $(OXOS_BOOT_VIDEO_DIR) -j8
oxos_boot_video: $(OXOS_BOOT_VIDEO_XBE)
apps: oxos_ftp oxos_boot_video

# The OXOS test video (spec/oxos_internal/38 §6), made from OXOS's own boot splash: its picture (splash/oxos_splash.svg, drawn by tools/vsplash) fading in and
# out over 8 s with a 440 Hz tone, in the boot video format (16:9 at 848x480 with the 640x480 picture in its middle; MPEG-2 video, MP2 sound at 48 kHz). The
# boot video test plays it. Made at build time with ffmpeg; no video is kept in the repository.
OXOS_INTRO_DIR   := $(APPS_BUILD)/apps/oxos_boot_video/intro
OXOS_INTRO_VIDEO := $(APPS_BUILD)/apps/oxos_boot_video/oxos_intro.mpg
$(OXOS_INTRO_VIDEO): splash/oxos_splash.svg $(ROOT)/tools/svg_splash.py $(ROOT)/tools/vsplash/render.cpp $(ROOT)/shared/graphics/vector_renderer.cpp
	$(MAKE) -C $(ROOT)/tools/vsplash
	rm -rf $(OXOS_INTRO_DIR)
	mkdir -p $(OXOS_INTRO_DIR)
	python3 -I $(ROOT)/tools/svg_splash.py splash/oxos_splash.svg $(OXOS_INTRO_DIR)/scene.bin
	$(ROOT)/build/tools/vsplash/vsplash $(OXOS_INTRO_DIR)/scene.bin $(OXOS_INTRO_DIR)/splash
	ffmpeg -loglevel error -y -loop 1 -framerate 30 -t 8 -i $(OXOS_INTRO_DIR)/splash-end.ppm -f lavfi -i "sine=frequency=440:sample_rate=48000:duration=8" \
	    -filter_complex "[0:v]pad=848:480:104:0,fade=in:0:15,fade=out:st=7:d=1[v];\
	                     [1:a]volume=-12dB,afade=in:d=0.5,afade=out:st=7:d=1,aformat=channel_layouts=stereo[a]" \
	    -map "[v]" -map "[a]" -c:v mpeg2video -q:v 3 -c:a mp2 -ar 48000 -b:a 192k $@
	rm -rf $(OXOS_INTRO_DIR)
oxos_intro_video: $(OXOS_INTRO_VIDEO)

# Boot video test (tests/boot_video_test.py): cold boots of a serial-log kernel with the player, oxos_settings.ini and the OXOS boot video on the disk: plays,
# skipped with a button, missing file, switched off, no player.
SERIAL_BUILD := $(ROOT)/build/kernel/serial
test_boot_video: $(OXOS_BOOT_VIDEO_XBE) $(OXOS_INTRO_VIDEO) $(HDD_FETCH)
	$(MAKE) BUILD=$(SERIAL_BUILD) SELFTEST=0 SERIAL=1 $(SERIAL_BUILD)/bios.bin
	python3 -I $(ROOT)/tests/boot_video_test.py --bios $(SERIAL_BUILD)/bios.bin --player $(OXOS_BOOT_VIDEO_XBE) --video $(OXOS_INTRO_VIDEO)

# The full regression run (tests/README.md, "Regression run"): the host unit tests and every xemu phase above, one after another, with one summary in
# build/regression/. MCPX_ROM=... is used for test_mcpx only (skipped without it); ONLY=test_exfat,test_iso runs some phases.
regression:
	python3 -I $(ROOT)/tools/regression.py $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) $(if $(ONLY),--only $(ONLY))

# The same session on the reference BIOS (A/B comparison; your own BIOS and boot ROM, never committed). Its own state directory: the stock kernel signs saves
# with keys OXOS does not have, so the two cannot share saves.
# Another BIOS to compare with: BIOS_BASE=its image BIOS_NAME=name (state xemu_state/<name>_<INSTANCE>); MEMORY=128 for a 128 MB console (any run target).
run_stock: $(PLAY_FETCH)
	python3 -I $(ROOT)/tools/run.py $(BIOS_BASE) --mcpx-rom $(STOCK_MCPX) $(if $(STATE_STOCK),--state $(STATE_STOCK)) $(RUN_OPTS)

clean:
	rm -rf $(OUT)

.PHONY: all bios bios_256k release netboot mcpx_dump image_test run_image_test hdd_image test test_dash test_encoders test_mcpx test_relaunch test_extensions test_fatal test_format test_upload test_udf test_controller test_memory test_gdb test_gdb_extension regression extensions release_zip nxdk run run_xbe run_trace_xbe xbe_disc run_trace_basic run_trace_full print_trace_dump run_gdb run_stock clean iso_launcher title_launcher apps oxos_ftp oxos_boot_video oxos_intro_video test_boot_video test_iso iso_disc run_iso video_libraries video_decode_title
