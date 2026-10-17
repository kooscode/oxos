# Open Xbox kernel — build on Linux with clang + lld-link.
#   make            -> build/xboxkrnl.exe (+ map)
#   make bios       -> build/bios.bin: the open BIOS image (our 2BL + XboxDev/cromwell X-codes; spec/01 §5)
#   make test       -> boot build/bios.bin in xemu and check all milestone conditions (boot self-tests)

ROOT      := ..
BUILD     := build
# IMAGE=open (default) needs no Microsoft files. IMAGE=base instead packs the kernel into your own MCPX 1.0 BIOS
# image (BIOS_BASE, its 2BL decrypted with MCPX_ROM), as before the open image existed.
IMAGE     ?= open
BIOS_BASE ?= $(firstword $(wildcard $(ROOT)/xbox-files/bios/*.bin))
# An MCPX boot ROM dump is optional: without one xemu starts at our reset code in flash. With one (MCPX_ROM=...)
# xemu runs the real ROM, which reaches our 2BL through the X-codes' overflow trick, as a console does.
MCPX_ROM  ?=
# The reference BIOS needs the real boot ROM (run-stock, PAD_BIOS=stock comparisons only).
STOCK_MCPX ?= $(ROOT)/xbox-files/mcpx/mcpx_1.0.bin
HDD_IMAGE ?= $(ROOT)/xbox-files/hdd/xbox_hdd.qcow2
SPLASH    ?= splash/splash.png
SPLASH_QUANT ?= octree
SELFTEST  ?= 1
RETAIL_MEDIA_POLICY ?= 0
TRACE_IMPORTS ?= 0
# The CD-ROM request log shown before the dashboard (spec/04 §3a); on in every trace build.
DRIVE_LOG ?= $(if $(filter 0,$(TRACE_IMPORTS)),0,1)

CXX := clang++
CC  := clang
LD  := lld-link

TARGET  := --target=i386-pc-win32
ARCH    := -march=pentium3 -mno-sse -mno-mmx -mno-80387
CXXFLAGS := $(TARGET) $(ARCH) -std=c++20 -ffreestanding -fno-exceptions -fno-rtti -fno-builtin \
            -fno-stack-protector -fno-threadsafe-statics -fno-omit-frame-pointer -nostdlib -O2 -g0 \
            -Wall -Wextra -Werror -Wno-unused-parameter -Iinclude -DXK_BOOT_SELFTEST=$(SELFTEST) -DXK_RETAIL_MEDIA_POLICY=$(RETAIL_MEDIA_POLICY) -DXK_TRACE_IMPORTS=$(TRACE_IMPORTS) -DXK_DRIVE_LOG=$(DRIVE_LOG)
ASFLAGS := $(TARGET) $(ARCH) -Iinclude
LDFLAGS := /DRIVER /NODEFAULTLIB /ENTRY:KernelEntry /SUBSYSTEM:NATIVE /BASE:0x80010000 /FIXED \
           /ALIGN:0x20 /FILEALIGN:0x20 /MERGE:.rdata=.text /MERGE:.bss=.data /SAFESEH:NO \
           /DEBUG:NONE /MAP:$(BUILD)/xboxkrnl.map

# Release builds leave out the self-tests and the import tracer: their code and buffers would stay resident and
# take memory titles count on (Halo 2 needs nearly all of it).
CXX_SRCS := $(filter-out $(if $(filter 0,$(SELFTEST)),src/ke/selftest%.cpp) \
                         $(if $(filter 0,$(TRACE_IMPORTS)),src/xe/trace.cpp),$(wildcard src/*/*.cpp))
ASM_SRCS := $(filter-out $(if $(filter 0,$(TRACE_IMPORTS)),src/arch/trace.S),$(wildcard src/*/*.S))
OBJS := $(CXX_SRCS:src/%.cpp=$(BUILD)/%.obj) $(ASM_SRCS:src/%.S=$(BUILD)/%.obj)

all: $(BUILD)/xboxkrnl.exe

$(BUILD)/%.obj: src/%.cpp $(wildcard include/xk/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.obj: src/%.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/xboxkrnl.def: exports.lst exports.tsv $(ROOT)/tools/gen_def.py
	@mkdir -p $(BUILD)
	python3 -I $(ROOT)/tools/gen_def.py exports.tsv exports.lst $@

$(BUILD)/xboxkrnl.exe: $(OBJS) $(BUILD)/xboxkrnl.def
	$(LD) $(LDFLAGS) /DEF:$(BUILD)/xboxkrnl.def /OUT:$@ $(OBJS)

# Test title (milestone 4a): freestanding PE at 0x10000 -> XBE.
TITLE_CXXFLAGS := $(TARGET) $(ARCH) -std=c++20 -ffreestanding -fno-exceptions -fno-rtti -fno-builtin \
                  -fno-stack-protector -nostdlib -O2 -Wall -Werror
$(BUILD)/title/title.obj: $(ROOT)/tests/title/title.cpp
	@mkdir -p $(dir $@)
	$(CXX) $(TITLE_CXXFLAGS) -c $< -o $@

$(BUILD)/title.exe: $(BUILD)/title/title.obj
	$(LD) /NODEFAULTLIB /ENTRY:XkTitleEntry /SUBSYSTEM:NATIVE /BASE:0x10000 /FIXED /DEBUG:NONE \
	    /MAP:$(BUILD)/title.map /OUT:$@ $<

$(BUILD)/title.xbe: $(BUILD)/title.exe $(ROOT)/tools/mkxbe.py
	python3 -I $(ROOT)/tools/mkxbe.py --pe $< --map $(BUILD)/title.map --out $@ --no-preload .extra

# --- Open BIOS image parts (spec/01 §5). XboxDev/cromwell (GPL-2.0) is fetched at a pinned commit for its X-code
# table and EEPROM key states. MCPX16=1 selects Cromwell's values for the 1.6 console's MCPX.
CROMWELL_DIR    := $(abspath $(ROOT)/third_party/cromwell)
CROMWELL_COMMIT := 86f547387184d1001d377cce97d3756c8acf91cc
MCPX16          ?= 0
OBJCOPY         := $(firstword $(shell command -v llvm-objcopy) /usr/lib/llvm-18/bin/llvm-objcopy)
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

test: $(BUILD)/bios.bin $(BUILD)/title.xbe
	python3 -I $(ROOT)/tests/boot_test.py --bios $< $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) --map $(BUILD)/xboxkrnl.map --hdd $(HDD_IMAGE) \
	    --title $(BUILD)/title.xbe --title-map $(BUILD)/title.map
	$(MAKE) test-dash test-relaunch test-fatal test-udf
	$(if $(MCPX_ROM),$(MAKE) test-mcpx)

# With a real MCPX boot ROM (MCPX_ROM=...): the dashboard phase again, reached through the overflow trick.
test-mcpx: $(BUILD)/title.xbe
	$(MAKE) BUILD=build-release SELFTEST=0 build-release/bios.bin
	python3 -I $(ROOT)/tests/boot_test.py --bios build-release/bios.bin --mcpx-rom $(MCPX_ROM) \
	    --map build-release/xboxkrnl.map --hdd $(HDD_IMAGE) --no-disc --dash

# Dashboard phase: a build without self-tests, booted without a disc, so C:\xboxdash.xbe is launched.
test-dash: $(BUILD)/title.xbe
	$(MAKE) BUILD=build-release SELFTEST=0 build-release/bios.bin
	python3 -I $(ROOT)/tests/boot_test.py --bios build-release/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) \
	    --map build-release/xboxkrnl.map --hdd $(HDD_IMAGE) --no-disc --dash

# Quick-reboot phase: the release build runs the title from a disc with relaunch.txt; it relaunches itself as
# media\second.xbe through a persisted LaunchDataPage and HalReturnToFirmware(2).
test-relaunch: $(BUILD)/title.xbe
	$(MAKE) BUILD=build-release SELFTEST=0 build-release/bios.bin
	python3 -I $(ROOT)/tests/boot_test.py --bios build-release/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) \
	    --map build-release/xboxkrnl.map --hdd $(HDD_IMAGE) --title $(BUILD)/title.xbe --title-map $(BUILD)/title.map \
	    --relaunch --boot-wait 1

# Fatal-error phase: no hard disk, so the disk driver reports a fatal error (EEPROM history, code on screen).
test-fatal:
	$(MAKE) BUILD=build-release SELFTEST=0 build-release/bios.bin
	python3 -I $(ROOT)/tests/boot_test.py --bios build-release/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) \
	    --map build-release/xboxkrnl.map --no-disc --fatal --boot-wait 2

# UDF phase (milestone 8): the self-test build with a UDF disc (tests/mkudf.py) instead of the XISO.
test-udf: $(BUILD)/bios.bin
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
PLAY_HDD    ?= $(firstword $(wildcard $(ROOT)/xbox-files/hdd/xbox_hdd_msdash.qcow2) $(HDD_IMAGE))
PLAY_NAME    = $(basename $(notdir $(PLAY_HDD)))
STATE       ?= $(ROOT)/xbox-files/play/oxos-$(PLAY_NAME)
STATE_STOCK ?= $(ROOT)/xbox-files/play/stock-$(PLAY_NAME)
WIDE        ?= 1
SIZE        ?= 1920x1080
SCALE       ?= 2
ASPECT      ?= auto
RUN_OPTS     = --hdd $(PLAY_HDD) $(if $(ISO),--iso $(ISO)) $(if $(FRESH),--fresh) $(if $(filter 1,$(WIDE)),--widescreen) \
               $(if $(ASPECT),--aspect $(ASPECT)) \
               $(if $(SIZE),--size $(SIZE)) $(if $(SCALE),--scale $(SCALE)) $(if $(FULL),--fullscreen)
run:
	$(MAKE) BUILD=build-release SELFTEST=0 build-release/bios.bin
	python3 -I $(ROOT)/tools/run.py build-release/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) $(if $(STATE),--state $(STATE)) $(RUN_OPTS)

# The same with the CD-ROM request log on screen before the dashboard. No import tracing, so titles see the memory
# of a release build; also the build to flash for troubleshooting disc boots on hardware.
run-trace:
	$(MAKE) BUILD=build-drivelog SELFTEST=0 DRIVE_LOG=1 build-drivelog/bios.bin
	python3 -I $(ROOT)/tools/run.py build-drivelog/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) $(if $(STATE),--state $(STATE)) $(RUN_OPTS)

# Controller input test (tests/padtest, an nxdk title): a virtual uinput gamepad presses every input and the title
# reports what it received. nxdk is fetched into third_party/ (not committed). PAD_BIOS=stock compares with the
# reference BIOS.
NXDK_DIR    := $(abspath $(ROOT)/third_party/nxdk)
NXDK_COMMIT := 14d5ee97e73347c973f1f57b68b79ec08c9e77f2
NXDK_ENV    := NXDK_DIR=$(NXDK_DIR) PATH=$(NXDK_DIR)/bin:/usr/lib/llvm-18/bin:$$PATH
PAD_BIOS    ?= ours

$(NXDK_DIR)/Makefile:
	git clone -q https://github.com/XboxDev/nxdk $(NXDK_DIR)
	git -C $(NXDK_DIR) checkout -q $(NXDK_COMMIT)
	git -C $(NXDK_DIR) submodule update -q --init --recursive

padtest: $(NXDK_DIR)/Makefile
	$(NXDK_ENV) $(MAKE) -C $(ROOT)/tests/padtest -j8

test-pad: padtest
	$(if $(filter stock,$(PAD_BIOS)),,$(MAKE) BUILD=build-release SELFTEST=0 build-release/bios.bin)
	python3 -I $(ROOT)/tests/pad_test.py --bios $(if $(filter stock,$(PAD_BIOS)),$(BIOS_BASE),build-release/bios.bin) \
		$(if $(filter stock,$(PAD_BIOS)),--mcpx-rom $(STOCK_MCPX),$(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM))) \
		--hdd $(HDD_IMAGE) --iso $(ROOT)/tests/padtest/padtest.iso

# The same session on the reference BIOS (A/B comparison; your own BIOS and boot ROM, never committed). Its own
# state directory: the stock kernel signs saves with keys OXOS does not have, so the two cannot share saves.
run-stock:
	python3 -I $(ROOT)/tools/run.py $(BIOS_BASE) --mcpx-rom $(STOCK_MCPX) $(if $(STATE_STOCK),--state $(STATE_STOCK)) $(RUN_OPTS)

clean:
	rm -rf $(BUILD) build-release

.PHONY: all bios test test-dash test-mcpx test-relaunch test-fatal test-udf test-pad padtest run run-trace run-stock clean
