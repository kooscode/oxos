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

CXX := clang++
CC  := clang
LD  := lld-link

TARGET  := --target=i386-pc-win32
ARCH    := -march=pentium3 -mno-sse -mno-mmx -mno-80387
CXXFLAGS := $(TARGET) $(ARCH) -std=c++20 -ffreestanding -fno-exceptions -fno-rtti -fno-builtin \
            -fno-stack-protector -fno-threadsafe-statics -nostdlib -O2 -g0 \
            -Wall -Wextra -Werror -Wno-unused-parameter -Iinclude -DXK_BOOT_SELFTEST=$(SELFTEST)
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

bios: $(BUILD)/bios.bin

$(BUILD)/bios.bin: $(BUILD)/xboxkrnl.exe $(ROOT)/tools/mkbios.py
	python3 -I $(ROOT)/tools/mkbios.py --base $(BIOS_BASE) --mcpx-rom $(MCPX_ROM) \
	    --kernel $< --out $@ --flat-kernel $(BUILD)/xboxkrnl.flat.exe

test: $(BUILD)/bios.bin
	python3 -I $(ROOT)/tests/boot_test.py --bios $< --mcpx-rom $(MCPX_ROM) --map $(BUILD)/xboxkrnl.map --hdd $(HDD_IMAGE)

clean:
	rm -rf $(BUILD)

.PHONY: all bios test clean
