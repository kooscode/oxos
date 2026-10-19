# Open Xbox kernel — build on Linux with clang + lld-link.
#   make            -> build/selftest/xboxkrnl.exe (+ map)
#   make bios       -> build/selftest/bios.bin: the open BIOS image (our 2BL + XboxDev/cromwell X-codes; spec/stock/01 §5)
#   make test       -> boot build/selftest/bios.bin in xemu and check all milestone conditions (boot self-tests)
# All build output goes under build/, one folder per kind of build:
#   build/selftest         make, make bios, make test (the kernel with its boot self-tests)
#   build/release          make run and the published BIOS (no self-tests, no debug code)
#   build/debug            make run-gdb, tools/debug-app.py, VS Code (DEBUG=1: debug info for gdb)
#   build/trace-basic      make run-trace-basic (the debug serial log)
#   build/trace-full       make run-trace-full (serial log + kernel call tracer, xemu at 128 MB)
#   build/trace-full-64mb  make run-trace-full TRACE=1 (the call tracer within the console's 64 MB)

ROOT      := ..
OUT       := build
BUILD     := $(OUT)/selftest
RELEASE_BUILD := $(OUT)/release
DEBUG_BUILD   := $(OUT)/debug
TRACE_BASIC_BUILD := $(OUT)/trace-basic
# IMAGE=open (default) needs no Microsoft files. IMAGE=base instead packs the kernel into your own MCPX 1.0 BIOS
# image (BIOS_BASE, its 2BL decrypted with MCPX_ROM), as before the open image existed.
IMAGE     ?= open
BIOS_BASE ?= $(firstword $(wildcard $(ROOT)/xbox-files/bios/*.bin))
# An MCPX boot ROM dump is optional: without one xemu starts at our reset code in flash. With one (MCPX_ROM=...)
# xemu runs the real ROM, which reaches our 2BL through the X-codes' overflow trick, as a console does.
MCPX_ROM  ?=
# The reference BIOS needs the real boot ROM (run-stock, CONTROLLER_BIOS=stock comparisons only).
STOCK_MCPX ?= $(ROOT)/xbox-files/mcpx/mcpx_1.0.bin
# Default hard disk: xemu-dashboard's free, MIT-licensed disk image (an open dashboard on a formatted 8 GB drive),
# fetched into third_party/ on first use, at a pinned release, and checked by SHA-256 (tools/fetch.py). The tests
# and the run targets only ever use a copy. HDD_IMAGE=... uses another disk.
XDASH_VERSION := v20260516-0955
XDASH_URL     := https://github.com/xemu-project/xemu-dashboard/releases/download/$(XDASH_VERSION)
XDASH_DIR     := $(abspath $(ROOT)/third_party/xemu-dashboard)
XDASH_HDD     := $(XDASH_DIR)/xemu-dashboard.qcow2
HDD_IMAGE ?= $(XDASH_HDD)
# The fetch, as a prerequisite of the targets that boot from the default disk (nothing when HDD_IMAGE is another).
HDD_FETCH  = $(filter $(XDASH_HDD),$(HDD_IMAGE))
# Boot splash: a 640x480 256-colour PNG is used as it is; any other picture (SPLASH=my-picture.jpg) is converted
# with Pillow, reduced to 256 colours with SPLASH_QUANT (octree or median).
SPLASH    ?= splash/splash.png
SPLASH_QUANT ?= octree
SELFTEST  ?= 1
RETAIL_MEDIA_POLICY ?= 0
TRACE_IMPORTS ?= 0
# Source-level debugging with gdb (make run-gdb, tools/debug-app.py): DWARF debug info, and -Og so that stepping
# follows the source. C++ functions get no (MSVC-style) linkage names, which gdb cannot demangle, so gdb names
# them from the source. The BIOS packs the kernel as usual; xboxkrnl.elf (the same link with the debug info kept,
# converted to ELF, which gdb reads better than PE) is the symbol file for gdb.
DEBUG     ?= 0
OPT       := $(if $(filter 1,$(DEBUG)),-Og -fno-inline-functions -g -gdwarf-4 -mllvm -dwarf-linkage-names=Abstract,-O2 -g0)
# Debug serial port (spec/stock/18): on in the self-test and trace builds, off in release builds.
SERIAL ?= $(if $(filter 1,$(SELFTEST) $(TRACE_IMPORTS)),1,0)

CXX := clang++
CC  := clang
LD  := lld-link

TARGET  := --target=i386-pc-win32
ARCH    := -march=pentium3 -mno-sse -mno-mmx -mno-80387
CXXFLAGS := $(TARGET) $(ARCH) -std=c++20 -ffreestanding -fno-exceptions -fno-rtti -fno-builtin \
            -fno-stack-protector -fno-threadsafe-statics -fno-omit-frame-pointer -nostdlib $(OPT) \
            -Wall -Wextra -Werror -Wno-unused-parameter -Iinclude -DXK_BOOT_SELFTEST=$(SELFTEST) -DXK_RETAIL_MEDIA_POLICY=$(RETAIL_MEDIA_POLICY) -DXK_TRACE_IMPORTS=$(TRACE_IMPORTS) -DXK_DEBUG_SERIAL=$(SERIAL)
ASFLAGS := $(TARGET) $(ARCH) -Iinclude $(if $(filter 1,$(DEBUG)),-g -gdwarf-4)
LDFLAGS := /DRIVER /NODEFAULTLIB /ENTRY:KernelEntry /SUBSYSTEM:NATIVE /BASE:0x80010000 /FIXED \
           /ALIGN:0x20 /FILEALIGN:0x20 /MERGE:.rdata=.text /MERGE:.bss=.data /SAFESEH:NO \
           /Brepro /DEBUG:NONE /MAP:$(BUILD)/xboxkrnl.map

# Release builds leave out the self-tests and the import tracer: their code and buffers would stay resident and
# take memory titles count on (Halo 2 needs nearly all of it).
CXX_SRCS := $(filter-out $(if $(filter 0,$(SELFTEST)),src/ke/selftest%.cpp) \
                         $(if $(filter 0,$(TRACE_IMPORTS)),src/xe/trace.cpp),$(wildcard src/*/*.cpp))
ASM_SRCS := $(filter-out $(if $(filter 0,$(TRACE_IMPORTS)),src/arch/trace.S),$(wildcard src/*/*.S))
OBJS := $(CXX_SRCS:src/%.cpp=$(BUILD)/%.obj) $(ASM_SRCS:src/%.S=$(BUILD)/%.obj)

all: $(BUILD)/xboxkrnl.exe

# Each object also depends on every header it includes (the compiler writes the list; -MP keeps a removed header
# from breaking the build), including headers next to the sources such as src/fatx/fatx.h.
$(BUILD)/%.obj: src/%.cpp $(wildcard include/xk/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -MMD -MP -MF $(@:.obj=.d) -c $< -o $@
-include $(OBJS:.obj=.d)

$(BUILD)/%.obj: src/%.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/xboxkrnl.def: exports.tsv $(ROOT)/tools/gen_def.py
	@mkdir -p $(BUILD)
	python3 -I $(ROOT)/tools/gen_def.py exports.tsv $@

ifeq ($(DEBUG),1)
# One link with the debug info; the kernel is the same file with the debug sections stripped, so its code and data
# sit at exactly the addresses gdb reads from xboxkrnl.dbg.exe.
$(BUILD)/xboxkrnl.exe: $(OBJS) $(BUILD)/xboxkrnl.def
	$(LD) $(filter-out /DEBUG:NONE,$(LDFLAGS)) /DEBUG:DWARF /DEF:$(BUILD)/xboxkrnl.def \
	    /OUT:$(BUILD)/xboxkrnl.dbg.exe $(OBJS)
	$(OBJCOPY) --strip-all $(BUILD)/xboxkrnl.dbg.exe $@
	$(GNU_OBJCOPY) -O elf32-i386 $(BUILD)/xboxkrnl.dbg.exe $(BUILD)/xboxkrnl.elf
else
$(BUILD)/xboxkrnl.exe: $(OBJS) $(BUILD)/xboxkrnl.def
	$(LD) $(LDFLAGS) /DEF:$(BUILD)/xboxkrnl.def /OUT:$@ $(OBJS)
endif

# Kernel test title (tests/kernel-test-title): a minimal freestanding "game" the boot tests run from a disc to check
# the title loader and the kernel functions titles rely on. PE at 0x10000 -> XBE.
TEST_TITLE := $(BUILD)/kernel-test-title
TEST_TITLE_CXXFLAGS := $(TARGET) $(ARCH) -std=c++20 -ffreestanding -fno-exceptions -fno-rtti -fno-builtin \
                       -fno-stack-protector -nostdlib -O2 -Wall -Werror
$(TEST_TITLE).obj: $(ROOT)/tests/kernel-test-title/kernel_test_title.cpp
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
OBJCOPY         := $(firstword $(shell command -v llvm-objcopy) /usr/lib/llvm-18/bin/llvm-objcopy)
GNU_OBJCOPY     := objcopy
BOOT_FLAGS := --target=i386-unknown-none-elf -march=pentium3 -mno-sse -mno-mmx -ffreestanding -fno-exceptions \
              -fno-rtti -fno-builtin -fno-pic -fno-stack-protector -nostdlib -O2 -Wall -Wextra -Werror -Iinclude \
              $(if $(filter 1,$(MCPX16)),-DMCPXREVD5)
BOOT := $(BUILD)/boot

$(CROMWELL_DIR)/boot_rom/Xcodes.h:
	git clone -q https://github.com/XboxDev/cromwell $(CROMWELL_DIR)
	git -C $(CROMWELL_DIR) checkout -q $(CROMWELL_COMMIT)

$(BOOT)/xcodes.bin: boot/xcodes.S $(CROMWELL_DIR)/boot_rom/Xcodes.h
	@mkdir -p $(dir $@)
	$(CC) $(BOOT_FLAGS) -I$(CROMWELL_DIR)/boot_rom -c $< -o $(BOOT)/xcodes.o
	$(OBJCOPY) -O binary -j .xcodes $(BOOT)/xcodes.o $@

$(BOOT)/reset.bin: boot/reset.S
	@mkdir -p $(dir $@)
	$(CC) $(BOOT_FLAGS) -c $< -o $(BOOT)/reset.o
	$(OBJCOPY) -O binary -j .reset $(BOOT)/reset.o $@

$(BOOT)/bl.bin: boot/start.S boot/bl.cpp boot/bl.ld include/xk/inflate.h
	@mkdir -p $(dir $@)
	$(CC) $(BOOT_FLAGS) -c boot/start.S -o $(BOOT)/start.o
	$(CXX) $(BOOT_FLAGS) -std=c++20 -c boot/bl.cpp -o $(BOOT)/bl.o
	ld.lld -m elf_i386 -T boot/bl.ld -nostdlib $(BOOT)/start.o $(BOOT)/bl.o -o $(BOOT)/bl.elf
	$(OBJCOPY) -O binary $(BOOT)/bl.elf $@

bios: $(BUILD)/bios.bin

SPLASH_ARGS = $(if $(wildcard $(SPLASH)),--splash $(SPLASH) --splash-quantize $(SPLASH_QUANT))
ifeq ($(IMAGE),base)
$(BUILD)/bios.bin: $(BUILD)/xboxkrnl.exe $(ROOT)/tools/mkbios.py $(wildcard $(SPLASH))
	python3 -I $(ROOT)/tools/mkbios.py --base $(BIOS_BASE) --mcpx-rom $(MCPX_ROM) \
	    --kernel $< --out $@ --flat-kernel $(BUILD)/xboxkrnl.flat.exe $(SPLASH_ARGS)
else
$(BUILD)/bios.bin: $(BUILD)/xboxkrnl.exe $(ROOT)/tools/mkbios.py $(wildcard $(SPLASH)) \
                   $(BOOT)/xcodes.bin $(BOOT)/bl.bin $(BOOT)/reset.bin
	python3 -I $(ROOT)/tools/mkbios.py --open --xcodes $(BOOT)/xcodes.bin --loader $(BOOT)/bl.bin \
	    --reset $(BOOT)/reset.bin --cromwell $(CROMWELL_DIR) --kernel $< --out $@ \
	    --flat-kernel $(BUILD)/xboxkrnl.flat.exe $(SPLASH_ARGS)
endif

$(XDASH_HDD):
	python3 -I $(ROOT)/tools/fetch.py $(XDASH_URL)/LICENSE \
	    3c4d8c95fdcd47f61e106f1e597d8c9db37a57193d0239346edcbc9f4f2334f6 $(XDASH_DIR)/LICENSE
	python3 -I $(ROOT)/tools/fetch.py $(XDASH_URL)/xbox_hdd.qcow2 \
	    00d7df7a2bc235f8801764f00b7f40e194d1e392f7a9619d6b2396c89770f6dd $@
hdd-image: $(XDASH_HDD)

test: $(BUILD)/bios.bin $(TEST_TITLE).xbe $(HDD_FETCH)
	python3 -I $(ROOT)/tests/boot_test.py --bios $< $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) --map $(BUILD)/xboxkrnl.map --hdd $(HDD_IMAGE) \
	    --title $(TEST_TITLE).xbe --title-map $(TEST_TITLE).map $(if $(filter 1,$(SERIAL)),--serial)
	$(MAKE) test-dash test-relaunch test-fatal test-udf
	$(if $(MCPX_ROM),$(MAKE) test-mcpx)

# 48-bit addressing (spec/oxos-internal/lba48.md): the self-tests on a 2 TB disk, a sparse copy of the test disk
# grown with tests/qcow2.py --grow and kept in xemu-state/oxos-xemu-lba48/ (a few MB on the host). The kernel writes
# test patterns either side of sector 0x0FFFFFFF and at the end of the disk; boot_test.py then finds each one at
# exactly LBA * 512 in the image and checks that the start of the disk is untouched. The image itself is never
# written (boot_test.py attaches a copy).
LBA48_STATE := $(ROOT)/xemu-state/oxos-xemu-lba48
LBA48_HDD   := $(LBA48_STATE)/hdd.qcow2
$(LBA48_HDD): $(HDD_FETCH)
	mkdir -p $(LBA48_STATE)
	python3 -I $(ROOT)/tests/qcow2.py --grow $(HDD_IMAGE) $@ 2T
test-lba48: $(BUILD)/bios.bin $(TEST_TITLE).xbe $(LBA48_HDD)
	python3 -I $(ROOT)/tests/boot_test.py --bios $< $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) --map $(BUILD)/xboxkrnl.map \
	    --hdd $(LBA48_HDD) --title $(TEST_TITLE).xbe --title-map $(TEST_TITLE).map

# exFAT disks (spec/oxos-internal/exfat-driver.md): the self-tests on a GPT disk with one exFAT volume (256 GB,
# 16 KB clusters, sparse), the test disk's C: and E: copied into /C and /E by tests/exfat.py; kept in
# xemu-state/oxos-xemu-exfat/. The FATX suite then runs on E: as a folder of the volume, and tests/exfat.py checks the
# whole volume afterwards (checksums, chains, bitmap).
EXFAT_STATE := $(ROOT)/xemu-state/oxos-xemu-exfat
EXFAT_HDD   := $(EXFAT_STATE)/hdd.qcow2
$(EXFAT_HDD): $(HDD_FETCH)
	mkdir -p $(EXFAT_STATE)
	python3 -I $(ROOT)/tests/exfat.py --from-fatx $(HDD_IMAGE) $@ --size 256G --cluster 16K
test-exfat: $(BUILD)/bios.bin $(TEST_TITLE).xbe $(EXFAT_HDD)
	python3 -I $(ROOT)/tests/boot_test.py --bios $< $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) --map $(BUILD)/xboxkrnl.map \
	    --hdd $(EXFAT_HDD) --title $(TEST_TITLE).xbe --title-map $(TEST_TITLE).map

# With a real MCPX boot ROM (MCPX_ROM=...): the dashboard phase again, reached through the overflow trick.
test-mcpx: $(HDD_FETCH)
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin --mcpx-rom $(MCPX_ROM) \
	    --map $(RELEASE_BUILD)/xboxkrnl.map --hdd $(HDD_IMAGE) --no-disc --dash

# Dashboard phase: a build without self-tests, booted without a disc, so C:\xboxdash.xbe is launched.
test-dash: $(HDD_FETCH)
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) \
	    --map $(RELEASE_BUILD)/xboxkrnl.map --hdd $(HDD_IMAGE) --no-disc --dash

# Quick-reboot phase: the release build runs the kernel test title from a disc with relaunch.txt; it relaunches itself as
# media\second.xbe through a persisted LaunchDataPage and HalReturnToFirmware(2).
test-relaunch: $(TEST_TITLE).xbe $(HDD_FETCH)
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) \
	    --map $(RELEASE_BUILD)/xboxkrnl.map --hdd $(HDD_IMAGE) --title $(TEST_TITLE).xbe --title-map $(TEST_TITLE).map \
	    --relaunch --boot-wait 1

# Fatal-error phase: no hard disk, so the disk driver reports a fatal error (EEPROM history, code on screen).
test-fatal:
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) \
	    --map $(RELEASE_BUILD)/xboxkrnl.map --no-disc --fatal --boot-wait 2

# UDF phase (milestone 8): the self-test build with a UDF disc (tests/mkudf.py) instead of the XISO.
test-udf: $(BUILD)/bios.bin $(HDD_FETCH)
	python3 -I $(ROOT)/tests/boot_test.py --bios $(BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) --map $(BUILD)/xboxkrnl.map \
	    --hdd $(HDD_IMAGE) --udf --boot-wait 4

# Interactive: boot the release BIOS in xemu; optional ISO=path/to/game.iso.
# The HDD copy and EEPROM persist in STATE (saves and settings survive; made from PLAY_HDD the first time, which is
# never written): FRESH=1 starts it over, STATE= (empty) uses a throwaway copy. Display, by default widescreen in a
# 1920x1080 window at twice the render resolution: WIDE=0 (no EEPROM widescreen flag; a flag already saved in STATE
# stays until FRESH=1 or the dashboard clears it), ASPECT=auto|native|4x3|16x9 (xemu's picture aspect; auto follows the
# widescreen signal each game sends), SIZE= (window size), SCALE=1..10, FULL=1 (full screen).
# PLAY_HDD: the disk image the run targets start from, by default the stock Microsoft dashboard image when you have
# one (its Settings set up video modes, clock, language); the tests keep using HDD_IMAGE. One state directory per
# image, so switching images never mixes saves.
PLAY_HDD    ?= $(firstword $(wildcard $(ROOT)/xbox-files/hdd/msdash.qcow2) $(HDD_IMAGE))
# Each xemu instance keeps its state (hard-disk copy, EEPROM) in xemu-state/<bios>-<INSTANCE>: oxos-xemu (the
# default xemu-dashboard.qcow2), oxos-msdash (xbox-files/hdd/msdash.qcow2), or INSTANCE=name for any other disk
# (default: its file name); stock-<INSTANCE> for make run-stock.
INSTANCE    ?= $(if $(filter $(XDASH_HDD),$(abspath $(PLAY_HDD))),xemu,$(if $(findstring msdash,$(notdir $(PLAY_HDD))),msdash,$(subst _,-,$(basename $(notdir $(PLAY_HDD))))))
STATE       ?= $(ROOT)/xemu-state/oxos-$(INSTANCE)
# run-stock with another BIOS (BIOS_BASE=...): BIOS_NAME names its state, e.g. BIOS_NAME=cerbios -> cerbios-<INSTANCE>.
BIOS_NAME   ?= stock
STATE_STOCK ?= $(ROOT)/xemu-state/$(BIOS_NAME)-$(INSTANCE)
PLAY_FETCH   = $(filter $(XDASH_HDD),$(PLAY_HDD))
WIDE        ?= 1
SIZE        ?= 1920x1080
SCALE       ?= 2
ASPECT      ?= auto
RUN_OPTS     = --hdd $(PLAY_HDD) $(if $(ISO),--iso $(ISO)) $(if $(FRESH),--fresh) $(if $(filter 1,$(WIDE)),--widescreen) \
               $(if $(MEMORY),--memory $(MEMORY)) \
               $(if $(ASPECT),--aspect $(ASPECT)) \
               $(if $(SIZE),--size $(SIZE)) $(if $(SCALE),--scale $(SCALE)) $(if $(FULL),--fullscreen)
run: $(PLAY_FETCH)
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	python3 -I $(ROOT)/tools/run.py $(RELEASE_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) $(if $(STATE),--state $(STATE)) $(RUN_OPTS)

# The same with the debug serial port (spec/stock/18), whose output is printed in this terminal. No import tracing, so
# titles see the memory of a release build (plus the serial code); also the build to flash for troubleshooting on
# hardware.
run-trace-basic: $(PLAY_FETCH)
	$(MAKE) BUILD=$(TRACE_BASIC_BUILD) SELFTEST=0 SERIAL=1 $(TRACE_BASIC_BUILD)/bios.bin
	python3 -I $(ROOT)/tools/run.py $(TRACE_BASIC_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) $(if $(STATE),--state $(STATE)) $(RUN_OPTS) --serial

# Source-level debugging: the same as run-trace-basic with the debug kernel (DEBUG=1) and xemu's GDB server on
# localhost:$(GDB_PORT), paused at power-on. Attach with gdb-multiarch (symbols: $(DEBUG_BUILD)/xboxkrnl.elf, plus an
# app's own); tools/debug-app.py does the same for an nxdk app, and tools/vscode/ sets it up for VS Code.
GDB_PORT    ?= 1234
run-gdb: $(PLAY_FETCH)
	$(MAKE) BUILD=$(DEBUG_BUILD) SELFTEST=0 SERIAL=1 DEBUG=1 $(DEBUG_BUILD)/bios.bin
	@echo "attach from another terminal: gdb-multiarch $(CURDIR)/$(DEBUG_BUILD)/xboxkrnl.elf -ex 'target remote localhost:$(GDB_PORT)'"
	python3 -I $(ROOT)/tools/run.py $(DEBUG_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) $(if $(STATE),--state $(STATE)) $(RUN_OPTS) \
	    --serial --gdb $(GDB_PORT)

# Import tracing: the same as run-trace-basic, plus a record of the title's last 32768 kernel calls. They are dumped by
# themselves when the kernel reports a crash and when xemu is closed (the last 30 in this terminal, all of them in
# $(TRACE_BUILD)/dumps/); at any other time, `make print-trace-dump` in another terminal prints them (tools/trace.py; LAST=N calls,
# ONLY=Name,Name, ALL=1 shows the common calls too). TRACE=2 (default) keeps the record above 64 MB, with xemu set
# to 128 MB, so titles get the memory of a release build (Halo 2 needs it) and the calls from before a quick reboot
# are kept. TRACE=1 keeps it in the console's 64 MB (2 MB less for the title). Each mode has its own build folder,
# because make does not rebuild when only the flags change; pass the same TRACE to print-trace-dump.
TRACE       ?= 2
TRACE_BUILD := $(OUT)/trace-full$(if $(filter 1,$(TRACE)),-64mb)
run-trace-full: $(PLAY_FETCH)
	$(MAKE) BUILD=$(TRACE_BUILD) SELFTEST=0 TRACE_IMPORTS=$(TRACE) SERIAL=1 $(TRACE_BUILD)/bios.bin
	python3 -I $(ROOT)/tools/run.py $(TRACE_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) $(if $(STATE),--state $(STATE)) $(RUN_OPTS) \
	    --serial --qmp $(TRACE_BUILD)/qmp.sock $(if $(filter 2,$(TRACE)),--memory 128) \
	    --trace-map $(TRACE_BUILD)/xboxkrnl.map --dump-dir $(TRACE_BUILD)/dumps

print-trace-dump:
	python3 -I $(ROOT)/tools/trace.py --qmp $(TRACE_BUILD)/qmp.sock --map $(TRACE_BUILD)/xboxkrnl.map \
	    --exports exports.tsv $(if $(LAST),--last $(LAST)) $(if $(ONLY),--only $(ONLY)) $(if $(filter 1,$(ALL)),--all)

# Controller input test (tests/controller-test-title, an nxdk title): a virtual uinput gamepad presses every input
# and the title reports what it received. The title is built on demand (its disc image below) with nxdk, which
# `make nxdk` (or the first make test-controller) fetches into third_party/ (not committed). CONTROLLER_BIOS=stock
# compares with the reference BIOS.
NXDK_DIR    := $(abspath $(ROOT)/third_party/nxdk)
NXDK_COMMIT := 14d5ee97e73347c973f1f57b68b79ec08c9e77f2
NXDK_ENV    := NXDK_DIR=$(NXDK_DIR) PATH=$(NXDK_DIR)/bin:/usr/lib/llvm-18/bin:$$PATH
CONTROLLER_BIOS ?= ours
CONTROLLER_DIR  := $(ROOT)/tests/controller-test-title
CONTROLLER_ISO  := $(CONTROLLER_DIR)/controller-test-title.iso

$(NXDK_DIR)/Makefile:
	git clone -q https://github.com/XboxDev/nxdk $(NXDK_DIR)
	git -C $(NXDK_DIR) checkout -q $(NXDK_COMMIT)
	git -C $(NXDK_DIR) submodule update -q --init --recursive
nxdk: $(NXDK_DIR)/Makefile

$(CONTROLLER_ISO): $(NXDK_DIR)/Makefile $(CONTROLLER_DIR)/controller_test_title.c $(CONTROLLER_DIR)/Makefile
	$(NXDK_ENV) $(MAKE) -C $(CONTROLLER_DIR) -j8

test-controller: $(CONTROLLER_ISO) $(HDD_FETCH)
	$(if $(filter stock,$(CONTROLLER_BIOS)),,$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin)
	python3 -I $(ROOT)/tests/controller_test.py --bios $(if $(filter stock,$(CONTROLLER_BIOS)),$(BIOS_BASE),$(RELEASE_BUILD)/bios.bin) \
		$(if $(filter stock,$(CONTROLLER_BIOS)),--mcpx-rom $(STOCK_MCPX),$(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM))) \
		--hdd $(HDD_IMAGE) --iso $(CONTROLLER_ISO)

# Run an XBE from the hard disk, e.g. a homebrew app copied to F: on an exFAT disk: a disc with the XBE launcher
# (tools/xbe-launcher, nxdk) and launch.txt = XBE boots, and the launcher starts that XBE the way a dashboard does (the
# XBE then runs from the disk, with D: its folder). XBE='F:\apps\demo\default.xbe' (quoted, for the backslashes).
# The disk: xemu-state/oxos-exfat/hdd.qcow2, an exFAT copy of PLAY_HDD made the first time (mount it on the PC to copy
# files in; unmount it before running), or STATE=... for another state folder.
LAUNCHER_DIR := $(ROOT)/tools/xbe-launcher
LAUNCHER_XBE := $(LAUNCHER_DIR)/bin/default.xbe
XBE_STATE    := $(if $(filter command line,$(origin STATE)),$(STATE),$(ROOT)/xemu-state/oxos-exfat)

$(LAUNCHER_XBE): $(NXDK_DIR)/Makefile $(LAUNCHER_DIR)/xbe_launcher.c $(LAUNCHER_DIR)/Makefile
	$(NXDK_ENV) $(MAKE) -C $(LAUNCHER_DIR) -j8

run-xbe: $(LAUNCHER_XBE) $(PLAY_FETCH)
	@test -n '$(XBE)' || { printf '%s\n' "usage: make run-xbe XBE='F:\\apps\\demo\\default.xbe' [STATE=...]"; exit 1; }
	@test -f $(XBE_STATE)/hdd.qcow2 || { mkdir -p $(XBE_STATE) && \
	    python3 -I $(ROOT)/tests/exfat.py --from-fatx $(PLAY_HDD) $(XBE_STATE)/hdd.qcow2 --size 256G --cluster 16K; }
	@echo "disk: $(XBE_STATE)/hdd.qcow2"
	@python3 -I $(ROOT)/tests/exfat.py --has $(XBE_STATE)/hdd.qcow2 '$(XBE)'; r=$$?; \
	    if [ $$r = 1 ]; then printf '%s\n' "$(XBE) is not on that disk (STATE=... picks another state folder)"; \
	        echo "F: holds:"; python3 -I $(ROOT)/tests/exfat.py --ls $(XBE_STATE)/hdd.qcow2 /F | grep . || echo "  (nothing)"; exit 1; fi
	@mkdir -p $(OUT)/run-xbe
	python3 -I $(ROOT)/tests/mkxiso.py $(OUT)/run-xbe/launcher.iso --add default.xbe=$(LAUNCHER_XBE) --text 'launch.txt=$(XBE)'
	$(MAKE) run ISO=$(OUT)/run-xbe/launcher.iso STATE=$(XBE_STATE)

# Memory test (tests/memory-test-title, an nxdk title): how much memory a title is given, as built (XBE flag
# "limit to 64 MB") and with the flag cleared. MEMORY=128 for a 128 MB console; MEMORY_BIOS=... (with MCPX_ROM=...)
# runs another BIOS instead of the release build, e.g. for comparison.
MEMORY_DIR  := $(ROOT)/tests/memory-test-title
MEMORY_XBE  := $(MEMORY_DIR)/bin/default.xbe

$(MEMORY_XBE): $(NXDK_DIR)/Makefile $(MEMORY_DIR)/memory_test_title.c $(MEMORY_DIR)/Makefile
	$(NXDK_ENV) $(MAKE) -C $(MEMORY_DIR) -j8

test-memory: $(MEMORY_XBE) $(HDD_FETCH)
	$(if $(MEMORY_BIOS),,$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin)
	python3 -I $(ROOT)/tests/memory_test.py --bios $(or $(MEMORY_BIOS),$(RELEASE_BUILD)/bios.bin) --xbe $(MEMORY_XBE) \
	    --both --memory $(or $(MEMORY),64) --hdd $(HDD_IMAGE) $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM))

# The same session on the reference BIOS (A/B comparison; your own BIOS and boot ROM, never committed). Its own
# state directory: the stock kernel signs saves with keys OXOS does not have, so the two cannot share saves.
# Another BIOS to compare with: BIOS_BASE=its image BIOS_NAME=name (state xemu-state/<name>-<INSTANCE>); MEMORY=128
# for a 128 MB console (any run target).
run-stock: $(PLAY_FETCH)
	python3 -I $(ROOT)/tools/run.py $(BIOS_BASE) --mcpx-rom $(STOCK_MCPX) $(if $(STATE_STOCK),--state $(STATE_STOCK)) $(RUN_OPTS)

clean:
	rm -rf $(OUT)

.PHONY: all bios hdd-image test test-dash test-mcpx test-relaunch test-fatal test-udf test-controller test-memory nxdk run run-xbe run-trace-basic run-trace-full print-trace-dump run-gdb run-stock clean
