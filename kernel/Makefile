# Open Xbox kernel — build on Linux with clang + lld-link.
#   make            -> build/xboxkrnl.exe (+ map)
#   make bios       -> build/bios.bin (kernel packed into BIOS_BASE with the stock 2BL)
#   make test       -> boot build/bios.bin in xemu and check all milestone conditions (boot self-tests)

ROOT      := ..
BUILD     := build
BIOS_BASE ?= $(ROOT)/xbox-files/bios/Complex_4627.bin
MCPX_ROM  ?= $(ROOT)/xbox-files/mcpx/mcpx_1.0.bin
HDD_IMAGE ?= $(ROOT)/xbox-files/hdd/xbox_hdd.qcow2
SELFTEST  ?= 1
RETAIL_MEDIA_POLICY ?= 0
TRACE_IMPORTS ?= 0

CXX := clang++
CC  := clang
LD  := lld-link

TARGET  := --target=i386-pc-win32
ARCH    := -march=pentium3 -mno-sse -mno-mmx -mno-80387
CXXFLAGS := $(TARGET) $(ARCH) -std=c++20 -ffreestanding -fno-exceptions -fno-rtti -fno-builtin \
            -fno-stack-protector -fno-threadsafe-statics -fno-omit-frame-pointer -nostdlib -O2 -g0 \
            -Wall -Wextra -Werror -Wno-unused-parameter -Iinclude -DXK_BOOT_SELFTEST=$(SELFTEST) -DXK_RETAIL_MEDIA_POLICY=$(RETAIL_MEDIA_POLICY) -DXK_TRACE_IMPORTS=$(TRACE_IMPORTS)
ASFLAGS := $(TARGET) $(ARCH) -Iinclude
LDFLAGS := /DRIVER /NODEFAULTLIB /ENTRY:KernelEntry /SUBSYSTEM:NATIVE /BASE:0x80010000 /FIXED \
           /ALIGN:0x20 /FILEALIGN:0x20 /MERGE:.rdata=.text /MERGE:.bss=.data /SAFESEH:NO \
           /DEBUG:NONE /MAP:$(BUILD)/xboxkrnl.map

CXX_SRCS := $(wildcard src/*/*.cpp)
ASM_SRCS := $(wildcard src/*/*.S)
OBJS := $(CXX_SRCS:src/%.cpp=$(BUILD)/%.obj) $(ASM_SRCS:src/%.S=$(BUILD)/%.obj)

all: $(BUILD)/xboxkrnl.exe

$(BUILD)/%.obj: src/%.cpp $(wildcard include/xk/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

$(BUILD)/%.obj: src/%.S
	@mkdir -p $(dir $@)
	$(CC) $(ASFLAGS) -c $< -o $@

$(BUILD)/xboxkrnl.def: exports.lst $(ROOT)/xboxkrnl-4627-exports.tsv $(ROOT)/tools/gen_def.py
	@mkdir -p $(BUILD)
	python3 -I $(ROOT)/tools/gen_def.py $(ROOT)/xboxkrnl-4627-exports.tsv exports.lst $@

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

bios: $(BUILD)/bios.bin

$(BUILD)/bios.bin: $(BUILD)/xboxkrnl.exe $(ROOT)/tools/mkbios.py
	python3 -I $(ROOT)/tools/mkbios.py --base $(BIOS_BASE) --mcpx-rom $(MCPX_ROM) \
	    --kernel $< --out $@ --flat-kernel $(BUILD)/xboxkrnl.flat.exe

test: $(BUILD)/bios.bin $(BUILD)/title.xbe
	python3 -I $(ROOT)/tests/boot_test.py --bios $< --mcpx-rom $(MCPX_ROM) --map $(BUILD)/xboxkrnl.map --hdd $(HDD_IMAGE) \
	    --title $(BUILD)/title.xbe --title-map $(BUILD)/title.map
	$(MAKE) test-dash test-relaunch test-fatal test-udf

# Dashboard phase: a build without self-tests, booted without a disc, so C:\xboxdash.xbe is launched.
test-dash: $(BUILD)/title.xbe
	$(MAKE) BUILD=build-release SELFTEST=0 build-release/bios.bin
	python3 -I $(ROOT)/tests/boot_test.py --bios build-release/bios.bin --mcpx-rom $(MCPX_ROM) \
	    --map build-release/xboxkrnl.map --hdd $(HDD_IMAGE) --no-disc --dash

# Quick-reboot phase: the release build runs the title from a disc with relaunch.txt; it relaunches itself as
# media\second.xbe through a persisted LaunchDataPage and HalReturnToFirmware(2).
test-relaunch: $(BUILD)/title.xbe
	$(MAKE) BUILD=build-release SELFTEST=0 build-release/bios.bin
	python3 -I $(ROOT)/tests/boot_test.py --bios build-release/bios.bin --mcpx-rom $(MCPX_ROM) \
	    --map build-release/xboxkrnl.map --hdd $(HDD_IMAGE) --title $(BUILD)/title.xbe --title-map $(BUILD)/title.map \
	    --relaunch --boot-wait 1

# Fatal-error phase: no hard disk, so the disk driver reports a fatal error (EEPROM history, code on screen).
test-fatal:
	$(MAKE) BUILD=build-release SELFTEST=0 build-release/bios.bin
	python3 -I $(ROOT)/tests/boot_test.py --bios build-release/bios.bin --mcpx-rom $(MCPX_ROM) \
	    --map build-release/xboxkrnl.map --no-disc --fatal --boot-wait 2

# UDF phase (milestone 8): the self-test build with a UDF disc (tests/mkudf.py) instead of the XISO.
test-udf: $(BUILD)/bios.bin
	python3 -I $(ROOT)/tests/boot_test.py --bios $(BUILD)/bios.bin --mcpx-rom $(MCPX_ROM) --map $(BUILD)/xboxkrnl.map \
	    --hdd $(HDD_IMAGE) --udf --boot-wait 4

# Interactive: boot the release BIOS in xemu with a copy of the HDD; optional ISO=path/to/game.iso.
run:
	$(MAKE) BUILD=build-release SELFTEST=0 build-release/bios.bin
	python3 -I $(ROOT)/tools/run.py build-release/bios.bin --mcpx-rom $(MCPX_ROM) --hdd $(HDD_IMAGE) $(if $(ISO),--iso $(ISO))

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
		--mcpx-rom $(MCPX_ROM) --hdd $(HDD_IMAGE) --iso $(ROOT)/tests/padtest/padtest.iso

# The same session on the reference BIOS (A/B comparison; the BIOS file is the user's own, never committed).
run-stock:
	python3 -I $(ROOT)/tools/run.py $(ROOT)/xbox-files/bios/Complex_4627.bin --mcpx-rom $(MCPX_ROM) --hdd $(HDD_IMAGE) $(if $(ISO),--iso $(ISO))

clean:
	rm -rf $(BUILD) build-release

.PHONY: all bios test test-dash test-relaunch test-fatal test-udf test-pad padtest run run-stock clean
