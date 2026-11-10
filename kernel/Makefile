# Open Xbox kernel — build on Linux with clang + lld-link.
#   make            -> build/selftest/xboxkrnl.exe (+ map)
#   make bios       -> build/selftest/bios.bin: the open BIOS image (our 2BL + XboxDev/cromwell X-codes; spec/stock/01 §5)
#   make test       -> boot build/selftest/bios.bin in xemu and check all milestone conditions (boot self-tests)
# All build output goes under build/, one folder per kind of build:
#   build/selftest         make, make bios, make test (the kernel with its boot self-tests)
#   build/release          make run and the published BIOS (no self-tests, no debug code)
#   build/debug            make run_gdb, tools/debug_app.py, VS Code (DEBUG=1: debug info for gdb)
#   build/trace_basic      make run_trace_basic (the debug serial log)
#   build/trace_full       make run_trace_full (serial log + kernel call tracer, xemu at 128 MB)
#   build/trace_full_64mb  make run_trace_full TRACE=1 (the call tracer within the console's 64 MB)

ROOT      := ..
OUT       := build
BUILD     := $(OUT)/selftest
RELEASE_BUILD := $(OUT)/release
# The 1.6 console's release image (MCPX16=1: its own X-codes, spec/oxos_internal/28).
RELEASE16_BUILD := $(OUT)/release_1_6
DEBUG_BUILD   := $(OUT)/debug
TRACE_BASIC_BUILD := $(OUT)/trace_basic
# IMAGE=open (default) needs no Microsoft files. IMAGE=base instead packs the kernel into your own MCPX 1.0 BIOS
# image (BIOS_BASE, its 2BL decrypted with MCPX_ROM), as before the open image existed.
IMAGE     ?= open
BIOS_BASE ?= $(firstword $(wildcard $(ROOT)/xbox_files/bios/*.bin))
# An MCPX boot ROM dump is optional: without one xemu starts at our reset code in flash. With one (MCPX_ROM=...)
# xemu runs the real ROM, which reaches our 2BL through the X-codes' overflow trick, as a console does.
MCPX_ROM  ?=
# The reference BIOS needs the real boot ROM (run_stock, CONTROLLER_BIOS=stock comparisons only).
STOCK_MCPX ?= $(ROOT)/xbox_files/mcpx/mcpx_1.0.bin
# Default hard disk: xemu-dashboard's free, MIT-licensed disk image (an open dashboard on a formatted 8 GB drive),
# fetched into third_party/ on first use, at a pinned release, and checked by SHA-256 (tools/fetch.py). The tests
# and the run targets only ever use a copy. HDD_IMAGE=... uses another disk.
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
# Source-level debugging with gdb (make run_gdb, tools/debug_app.py): DWARF debug info, and -Og so that stepping
# follows the source. C++ functions get no (MSVC-style) linkage names, which gdb cannot demangle, so gdb names
# them from the source. The BIOS packs the kernel as usual; xboxkrnl.elf (the same link with the debug info kept,
# converted to ELF, which gdb reads better than PE) is the symbol file for gdb.
DEBUG     ?= 0
# The kernel is built for size (-Oz): its code stays resident, in memory titles count on (spec/oxos_internal/18). Code
# where speed shows uses -O2 (FAST_SRCS below).
OPT       := $(if $(filter 1,$(DEBUG)),-Og -fno-inline-functions -g -gdwarf-4 -mllvm -dwarf-linkage-names=Abstract,-Oz -g0)
# Debug serial port (spec/oxos_internal/03_debug_serial.md): on in the self-test and trace builds, off in release builds.
# NETBOOT=1: the 2BL first asks the network for a boot image (spec/oxos_internal/09_netboot.md; tools/netboot/).
# NETBOOT_IP is the address the console puts in its netboot packets (it needs no DHCP and answers no ARP).
# Use its own BUILD directory (make netboot does): the 2BL is not rebuilt when only NETBOOT changes.
NETBOOT ?= 0
# MCPX_DUMP=1 (with NETBOOT=1): the 2BL copies the MCPX boot ROM at entry and sends it over the network
# (spec/oxos_internal/12_mcpx_dump.md; tools/mcpx_dump/). make mcpx_dump builds that payload image.
MCPX_DUMP ?= 0
NETBOOT_IP ?= 192.168.1.222
comma := ,
SERIAL ?= $(if $(filter 1,$(SELFTEST) $(TRACE_IMPORTS)),1,0)
# STATEDUMP=1: print the machine-state probe (tools/statedump) on the debug serial port at the end of start-up, for
# comparing the set-up with a reference kernel's. Turns SERIAL on.
STATEDUMP ?= 0
# REBOOT_TEST=N (debug): N seconds into every boot (cold, and the quick reboots that start titles) the kernel reboots the
# console the way the in-game reset does (HalpIgrReboot), so resets can be tested in a loop without a controller.
REBOOT_TEST ?= 0
ifeq ($(STATEDUMP),1)
SERIAL := 1
endif

CXX := clang++
CC  := clang
LD  := lld-link

TARGET  := --target=i386-pc-win32
# Plain Pentium code (no cmov, no multi-byte NOPs), as the reference kernels (their compiler emits only 386-class
# instructions). Not a fix for the 1.4 console's start-up freeze: that one depends on the exact binary layout with
# either setting (2026-10-23).
ARCH    := -march=pentium -mno-sse -mno-mmx -mno-80387
# EXTRA_FLAGS: more compiler flags for one-off experiments, in their own BUILD.
CXXFLAGS := $(EXTRA_FLAGS) $(TARGET) $(ARCH) -std=c++20 -ffreestanding -fno-exceptions -fno-rtti -fno-builtin \
            -fno-stack-protector -fno-threadsafe-statics -fno-omit-frame-pointer -nostdlib $(OPT) \
            -Wall -Wextra -Werror -Wno-unused-parameter -Iinclude -DXK_BOOT_SELFTEST=$(SELFTEST) -DXK_RETAIL_MEDIA_POLICY=$(RETAIL_MEDIA_POLICY) -DXK_TRACE_IMPORTS=$(TRACE_IMPORTS) -DXK_DEBUG_SERIAL=$(SERIAL) -DXK_STATEDUMP=$(STATEDUMP) -DXK_REBOOT_TEST=$(REBOOT_TEST)
ASFLAGS := $(EXTRA_FLAGS) $(TARGET) $(ARCH) -Iinclude $(if $(filter 1,$(DEBUG)),-g -gdwarf-4)
# The version on the boot splash and the error screens (bootscreen/text.cpp): vX.Y.Z when built for a release (make release VERSION=X.Y.Z),
# else DEV (and XK_RELEASE_VERSION 1 or 0: a development build's disk-format countdown is short). It is a generated
# header, rewritten only when the text changes, so a new version rebuilds what uses it.
XK_VERSION_TEXT := $(if $(VERSION),v$(VERSION),DEV)
XK_VERSION_HEADER := $(BUILD)/generated/xk_version.h
CXXFLAGS += -I$(BUILD)/generated
# The netboot address: the upload server's own when DHCP does not answer (network/ip.cpp).
CXXFLAGS += -DXK_NETBOOT_IP=$(subst .,$(comma),$(NETBOOT_IP))
LDFLAGS := /DRIVER /NODEFAULTLIB /ENTRY:KernelEntry /SUBSYSTEM:NATIVE /BASE:0x80010000 /FIXED \
           /ALIGN:0x20 /FILEALIGN:0x20 /MERGE:.rdata=.text /MERGE:.bss=.data /SAFESEH:NO \
           /SECTION:.xka,D /SECTION:.xkb,D \
           /Brepro /DEBUG:NONE /MAP:$(BUILD)/xboxkrnl.map

# Release builds leave out the self-tests and the import tracer: their code and buffers would stay resident and
# take memory titles count on (Halo 2 needs nearly all of it).
CXX_SRCS := $(filter-out $(if $(filter 0,$(TRACE_IMPORTS)),src/titles/trace.cpp), \
                         $(wildcard src/*/*.cpp src/*/*/*.cpp))
ASM_SRCS := $(filter-out $(if $(filter 0,$(TRACE_IMPORTS)),src/arch/trace.S),$(wildcard src/*/*.S))
SELFTEST_SRCS := $(if $(filter 0,$(SELFTEST)),,$(wildcard selftest/*.cpp))
OBJS := $(CXX_SRCS:src/%.cpp=$(BUILD)/%.obj) $(ASM_SRCS:src/%.S=$(BUILD)/%.obj) \
        $(SELFTEST_SRCS:%.cpp=$(BUILD)/%.obj)

all: $(BUILD)/xboxkrnl.exe

# Boot-only code (spec/oxos_internal/18_boot_memory.md): these files may call one another's boot-only functions.
BOOT_OBJS := splash.obj,jingle.obj,bootsound.obj,vsplash.obj,nic.obj,ip.obj,tcp.obj,server.obj,page.obj
BOOT_OBJS := $(BOOT_OBJS),usb_host.obj,pads.obj
# ... and the boot-only functions kept code calls during a cold boot, before the pages are freed, or that a kept file
# defines for the boot-only code (the File Manager's screen, in core/fatal.cpp with the other screens).
BOOT_ENTRIES := HalpStartSplash,HalpWaitForSplash,UploadServe,UploadStop
BOOT_ENTRIES := $(BOOT_ENTRIES),XkShowFileManagerScreen,XkCloseFileManagerScreen,XkNoDashboardError
BOOT_ENTRIES := $(BOOT_ENTRIES),GamepadWaitForX
# The quick-reboot copy of the initialised .data (src/arch/entry.S).
KI_DATA_COPY_SIZE := $(shell sed -n 's/^\#define KI_DATA_COPY_SIZE \(0x[0-9A-Fa-f]*\).*/\1/p' src/arch/entry.S)

# Per-file optimization (not in DEBUG builds): -O2 for the hashes and ciphers (XBE sections, saves), memcpy and memset,
# the virtual-disc decompressor (it runs for every sector a title reads from a CCI) and the boot-only code (the splash
# and the jingle have to keep up with the clock; their pages are freed before the title starts, so their size costs
# nothing then). bootscreen/text.cpp is resident (the error screens use it), so it stays -Oz.
FAST_SRCS  := src/crypto/crypto.cpp src/runtime/memory.cpp src/storage/virtual_disc.cpp \
              $(filter-out src/bootscreen/text.cpp,$(wildcard src/bootscreen/*.cpp))
ifneq ($(DEBUG),1)
$(patsubst src/%.cpp,$(BUILD)/%.obj,$(FAST_SRCS)): FILE_OPT := -O2
endif

# Each object also depends on every header it includes (the compiler writes the list; -MP keeps a removed header
# from breaking the build).
$(BUILD)/%.obj: src/%.cpp $(wildcard include/xk/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(FILE_OPT) -MMD -MP -MF $(@:.obj=.d) -c $< -o $@

$(XK_VERSION_HEADER): FORCE
	@mkdir -p $(dir $@)
	@printf '#define XK_VERSION_TEXT "%s"\n#define XK_RELEASE_VERSION %s\n' "$(XK_VERSION_TEXT)" \
	    "$(if $(VERSION),1,0)" | cmp -s - $@ || printf '#define XK_VERSION_TEXT "%s"\n#define XK_RELEASE_VERSION %s\n' \
	    "$(XK_VERSION_TEXT)" "$(if $(VERSION),1,0)" > $@
$(BUILD)/bootscreen/text.obj $(BUILD)/storage/disk_layout.obj: $(XK_VERSION_HEADER)
FORCE:

# The boot self-tests (SELFTEST=1 only).
$(BUILD)/selftest/%.obj: selftest/%.cpp $(wildcard include/xk/*.h)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) $(FILE_OPT) -MMD -MP -MF $(@:.obj=.d) -c $< -o $@
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
	python3 -I $(ROOT)/tools/check_kernel_layout.py --exe $@ --map $(BUILD)/xboxkrnl.map --boot-objs $(BOOT_OBJS) \
	    --boot-entries $(BOOT_ENTRIES) --data-copy $(KI_DATA_COPY_SIZE) $(OBJS) || { rm -f $@; exit 1; }

# Kernel test title (tests/kernel_test_title): a minimal freestanding "game" the boot tests run from a disc to check
# the title loader and the kernel functions titles rely on. PE at 0x10000 -> XBE.
TEST_TITLE := $(BUILD)/kernel_test_title
TEST_TITLE_CXXFLAGS := $(TARGET) $(ARCH) -std=c++20 -ffreestanding -fno-exceptions -fno-rtti -fno-builtin \
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
OBJCOPY         := $(firstword $(shell command -v llvm-objcopy) /usr/lib/llvm-18/bin/llvm-objcopy)
GNU_OBJCOPY     := objcopy
BOOT_FLAGS := --target=i386-unknown-none-elf -march=pentium -mno-sse -mno-mmx -ffreestanding -fno-exceptions \
              -fno-rtti -fno-builtin -fno-pic -fno-stack-protector -nostdlib -O2 -Wall -Wextra -Werror -Iinclude \
              $(if $(filter 1,$(MCPX16)),-DMCPXREVD5) -DXK_NETBOOT=$(NETBOOT) -DXK_MCPX_DUMP=$(MCPX_DUMP) -DXK_XCODE_ENTRY_SNAPSHOT=$(SERIAL) \
              -DXK_NETBOOT_IP=$(subst .,$(comma),$(NETBOOT_IP))
BOOT := $(BUILD)/boot

$(CROMWELL_DIR)/boot_rom/Xcodes.h:
	git clone -q https://github.com/XboxDev/cromwell $(CROMWELL_DIR)
	git -C $(CROMWELL_DIR) checkout -q $(CROMWELL_COMMIT)

# --- CPU microcode (spec/stock/01 §2 step 3): Intel's updates for the Xbox's Pentium III (signatures 0x686 and
# 0x68A, platform 4), loaded at start-up as the reference kernel does. The files are kept in the repo, unmodified,
# as Intel's licence allows (third_party/intel_ucode: the files, Intel's licence, where they came from); their
# SHA-256 is checked before each use. tools/mkucode.py picks the platform-4 updates, unmodified.
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

$(BOOT)/bl.bin: boot/start.S boot/bl.cpp boot/netboot.h boot/bl.ld include/xk/inflate.h
	@mkdir -p $(dir $@)
	$(CC) $(BOOT_FLAGS) -c boot/start.S -o $(BOOT)/start.o
	$(CXX) $(BOOT_FLAGS) -std=c++20 -c boot/bl.cpp -o $(BOOT)/bl.o
	ld.lld -m elf_i386 -T boot/bl.ld -nostdlib $(BOOT)/start.o $(BOOT)/bl.o -o $(BOOT)/bl.elf
	$(OBJCOPY) -O binary $(BOOT)/bl.elf $@

bios: $(BUILD)/bios.bin
# A 256 KiB image (one bank, for 256 KiB flash chips and modchip banks), with the splash in the bank's spare space.
bios_256k: $(BUILD)/bios_256k.bin

# A release, in order, stopping at the first failure: every host unit test (tools/unit_tests.sh), the release
# images (1 MB and 256 KB, no self-tests, no serial log), the dashboard test on them (make test_dash), and a check
# that the 256 KB image is the 1 MB image's first bank. VERSION=X.Y.Z also packs the release file
# build/release/dist/oxos_vX.Y.Z.zip: bios_vX.Y.Z.bin, bios_vX.Y.Z_256k.bin and the title patches
# (title_patches/title_patches.ini), and prints the SHA-256 of each. SKIP_TESTS=1 skips the unit tests and the dashboard
# test, for fast iterations; it cannot be combined with VERSION, so files made for publishing are always tested.
release:
	$(if $(and $(filter 1,$(SKIP_TESTS)),$(VERSION)),$(error SKIP_TESTS=1 cannot be used with VERSION: a published release runs every test))
	$(if $(filter 1,$(SKIP_TESTS)),@echo "release: SKIP_TESTS=1 (unit tests and test_dash skipped)",$(ROOT)/tools/unit_tests.sh)
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin $(RELEASE_BUILD)/bios_256k.bin
	$(MAKE) BUILD=$(RELEASE16_BUILD) SELFTEST=0 MCPX16=1 $(RELEASE16_BUILD)/bios_256k.bin
	$(if $(filter 1,$(SKIP_TESTS)),,$(MAKE) test_dash)
	head -c 262144 $(RELEASE_BUILD)/bios.bin | cmp - $(RELEASE_BUILD)/bios_256k.bin
	$(if $(VERSION),rm -rf $(RELEASE_BUILD)/dist && mkdir -p $(RELEASE_BUILD)/dist && \
	    cp $(RELEASE_BUILD)/bios.bin $(RELEASE_BUILD)/dist/bios_v$(VERSION).bin && \
	    cp $(RELEASE_BUILD)/bios_256k.bin $(RELEASE_BUILD)/dist/bios_v$(VERSION)_256k.bin && \
	    cp $(RELEASE16_BUILD)/bios_256k.bin $(RELEASE_BUILD)/dist/bios_1.6_v$(VERSION)_256k.bin && \
	    cp title_patches/title_patches.ini $(RELEASE_BUILD)/dist/title_patches.ini && \
	    cd $(RELEASE_BUILD)/dist && \
	    zip -q -X oxos_v$(VERSION).zip bios_v$(VERSION).bin bios_v$(VERSION)_256k.bin bios_1.6_v$(VERSION)_256k.bin \
	        title_patches.ini && \
	    sha256sum oxos_v$(VERSION).zip bios_v$(VERSION).bin bios_v$(VERSION)_256k.bin bios_1.6_v$(VERSION)_256k.bin \
	        title_patches.ini)

# The image to flash once for network boots (tools/netboot/README.md): its 2BL asks tools/netboot/netboot.py for a
# kernel at every power-on and boots its own (a serial-log build) when no PC answers.
NETBOOT_BUILD := build/netboot
netboot:
	$(MAKE) BUILD=$(NETBOOT_BUILD) NETBOOT=1 SERIAL=1 SELFTEST=0 $(NETBOOT_BUILD)/bios_256k.bin $(NETBOOT_BUILD)/bios.bin

# The MCPX ROM dumper's payload (tools/mcpx_dump/README.md): a netboot image whose 2BL also sends the boot ROM.
MCPX_DUMP_BUILD := build/mcpx_dump
mcpx_dump:
	$(MAKE) BUILD=$(MCPX_DUMP_BUILD) NETBOOT=1 MCPX_DUMP=1 SERIAL=1 SELFTEST=0 SPLASH= $(MCPX_DUMP_BUILD)/bios_256k.bin

SPLASH_ARGS = $(if $(wildcard $(SPLASH)),--splash $(SPLASH))
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
$(BUILD)/bios_256k.bin: $(BUILD)/xboxkrnl.exe $(ROOT)/tools/mkbios.py $(wildcard $(SPLASH)) \
                        $(BOOT)/xcodes.bin $(BOOT)/bl.bin $(BOOT)/reset.bin
	python3 -I $(ROOT)/tools/mkbios.py --open --xcodes $(BOOT)/xcodes.bin --loader $(BOOT)/bl.bin \
	    --reset $(BOOT)/reset.bin --cromwell $(CROMWELL_DIR) --kernel $< --out $@ $(SPLASH_ARGS) --size 256K
endif

$(XDASH_HDD):
	python3 -I $(ROOT)/tools/fetch.py $(XDASH_URL)/LICENSE \
	    3c4d8c95fdcd47f61e106f1e597d8c9db37a57193d0239346edcbc9f4f2334f6 $(XDASH_DIR)/LICENSE
	python3 -I $(ROOT)/tools/fetch.py $(XDASH_URL)/xbox_hdd.qcow2 \
	    00d7df7a2bc235f8801764f00b7f40e194d1e392f7a9619d6b2396c89770f6dd $@
hdd_image: $(XDASH_HDD)

test: $(BUILD)/bios.bin $(TEST_TITLE).xbe $(HDD_FETCH)
	python3 -I $(ROOT)/tests/boot_test.py --bios $< $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) --map $(BUILD)/xboxkrnl.map --hdd $(HDD_IMAGE) \
	    --title $(TEST_TITLE).xbe --title-map $(TEST_TITLE).map $(if $(filter 1,$(SERIAL)),--serial)
	$(MAKE) test_dash test_relaunch test_fatal test_udf
	$(if $(MCPX_ROM),$(MAKE) test_mcpx)

# 48-bit addressing (spec/oxos_internal/04_lba48.md): the self-tests on a 2 TB disk, a sparse copy of the test disk
# grown with tests/qcow2.py --grow and kept in xemu_state/oxos_xemu_lba48/ (a few MB on the host). The kernel writes
# test patterns either side of sector 0x0FFFFFFF and at the end of the disk; boot_test.py then finds each one at
# exactly LBA * 512 in the image and checks that the start of the disk is untouched. The image itself is never
# written (boot_test.py attaches a copy).
LBA48_STATE := $(ROOT)/xemu_state/oxos_xemu_lba48
LBA48_HDD   := $(LBA48_STATE)/hdd.qcow2
$(LBA48_HDD): $(HDD_FETCH)
	mkdir -p $(LBA48_STATE)
	python3 -I $(ROOT)/tests/qcow2.py --grow $(HDD_IMAGE) $@ 2T
test_lba48: $(BUILD)/bios.bin $(TEST_TITLE).xbe $(LBA48_HDD)
	python3 -I $(ROOT)/tests/boot_test.py --bios $< $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) --map $(BUILD)/xboxkrnl.map \
	    --hdd $(LBA48_HDD) --title $(TEST_TITLE).xbe --title-map $(TEST_TITLE).map

# exFAT disks (spec/oxos_internal/07_exfat_driver.md): the self-tests on a GPT disk with one exFAT volume (256 GB,
# 16 KB clusters, sparse), the test disk's C: and E: copied into /C and /E by tests/exfat.py; kept in
# xemu_state/oxos_xemu_exfat/. The FATX suite then runs on E: as a folder of the volume, and tests/exfat.py checks the
# whole volume afterwards (checksums, chains, bitmap).
EXFAT_STATE := $(ROOT)/xemu_state/oxos_xemu_exfat
EXFAT_HDD   := $(EXFAT_STATE)/hdd.qcow2
$(EXFAT_HDD): $(HDD_FETCH)
	mkdir -p $(EXFAT_STATE)
	printf 'stale FAT tail test' > $(EXFAT_STATE)/stale.bin
	python3 -I $(ROOT)/tests/exfat.py --from-fatx $(HDD_IMAGE) $@ --size 256G --cluster 16K \
	    --add /E/XkStale.stale=$(EXFAT_STATE)/stale.bin
test_exfat: $(BUILD)/bios.bin $(TEST_TITLE).xbe $(EXFAT_HDD)
	python3 -I $(ROOT)/tests/boot_test.py --bios $< $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) --map $(BUILD)/xboxkrnl.map \
	    --hdd $(EXFAT_HDD) --title $(TEST_TITLE).xbe --title-map $(TEST_TITLE).map

# With a real MCPX boot ROM (MCPX_ROM=...): the dashboard phase again, reached through the overflow trick.
test_mcpx: $(HDD_FETCH)
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin --mcpx-rom $(MCPX_ROM) \
	    --map $(RELEASE_BUILD)/xboxkrnl.map --hdd $(HDD_IMAGE) --no-disc --dash

# Dashboard phase: a build without self-tests, booted without a disc, so C:\xboxdash.xbe is launched.
test_dash: $(HDD_FETCH)
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) \
	    --map $(RELEASE_BUILD)/xboxkrnl.map --hdd $(HDD_IMAGE) --no-disc --dash
	# ... and with EvoX installed (C:\evoxdash.xbe): it is the dashboard started (spec/oxos_internal/19).
	python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) \
	    --map $(RELEASE_BUILD)/xboxkrnl.map --hdd $(HDD_IMAGE) --no-disc --dash --evox
	# ... and with C:\xboxdash.xbe moved to C:\Dashboard\default.xbe: it is started from there (spec/oxos_internal/19).
	python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) \
	    --map $(RELEASE_BUILD)/xboxkrnl.map --hdd $(HDD_IMAGE) --no-disc --dash --xbl

# Quick-reboot phase: the release build runs the kernel test title from a disc with relaunch.txt; it relaunches itself as
# media\second.xbe through a persisted LaunchDataPage and HalReturnToFirmware(2).
test_relaunch: $(TEST_TITLE).xbe $(HDD_FETCH)
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) \
	    --map $(RELEASE_BUILD)/xboxkrnl.map --hdd $(HDD_IMAGE) --title $(TEST_TITLE).xbe --title-map $(TEST_TITLE).map \
	    --relaunch --boot-wait 1

# Video encoder phase: the encoder tables must be what tools/gen_av_tables.py makes from the specs
# (spec/stock/18-20); then the release build on each encoder xemu can present (Conexant, Focus, Xcalibur): the bus
# probe finds it, XboxHardwareInfo reports it, and the dashboard's mode set programs it (Conexant and Focus registers
# read back as written; the Xcalibur's writes are compared with spec/stock/20); then the self-test build on each
# encoder, whose boot self-test sets 15 mode words and the encoder options and checks them.
test_encoders: $(HDD_FETCH)
	@python3 -I $(ROOT)/tools/gen_av_tables.py --check src/hardware/video_modes.inc || { echo "src/hardware/video_modes.inc is stale: run tools/gen_av_tables.py --out src/hardware/video_modes.inc"; exit 1; }
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	for e in conexant focus xcalibur; do python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin \
	    $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) --map $(RELEASE_BUILD)/xboxkrnl.map --hdd $(HDD_IMAGE) --no-disc \
	    --encoder $$e --boot-wait 2 || exit 1; done
	$(MAKE) $(BUILD)/bios.bin
	for e in conexant focus xcalibur; do python3 -I $(ROOT)/tests/boot_test.py --bios $(BUILD)/bios.bin \
	    $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) --map $(BUILD)/xboxkrnl.map --hdd $(HDD_IMAGE) --no-disc \
	    --encoder $$e --boot-wait 2 || exit 1; done

# Fatal-error phase: no hard disk, so the disk driver reports a fatal error (EEPROM history, code on screen).
test_fatal:
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) \
	    --map $(RELEASE_BUILD)/xboxkrnl.map --no-disc --fatal --boot-wait 2

# Formatting at boot (spec/oxos_internal/30 §4): a new blank 16 GB disk (5 s countdown: not a release build), a
# blank one with the partition delete's marker asking for 0 s, and a PC disk (MBR, NTFS; 5 s); each is formatted,
# then, with no dashboard on it, the upload server starts (no network link in these runs: it waits for one).
FORMAT_TEST = python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin \
    $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) --map $(RELEASE_BUILD)/xboxkrnl.map --no-disc --blank-hdd 16 --boot-wait 2
test_format:
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	$(FORMAT_TEST)
	$(FORMAT_TEST) --blank-marker 0
	$(FORMAT_TEST) --foreign
	$(FORMAT_TEST) --blank-marker 0 --x-at-error

# The upload server (spec/oxos_internal/31): a blank disk formatted at once (exFAT), no dashboard on it, so the kernel
# serves the upload page; the test uploads a file and the FTP server's XBE through xemu's NAT network and launches it.
# Then the same on two FATX disks (copies), opened with X held during the splash: the 8 GB dashboard disk (no F:), and
# that disk grown to 16 GB with an empty F: (tests/fatx.py --format-f; kept in xemu_state/oxos_xemu_fatx16/).
FATX16_STATE := $(ROOT)/xemu_state/oxos_xemu_fatx16
FATX16_HDD   := $(FATX16_STATE)/hdd.qcow2
$(FATX16_HDD): $(XDASH_HDD)
	mkdir -p $(FATX16_STATE)
	python3 -I $(ROOT)/tests/qcow2.py --grow $(XDASH_HDD) $(FATX16_STATE)/grown.qcow2 16G
	python3 -I $(ROOT)/tests/fatx.py $(FATX16_STATE)/grown.qcow2 --format-f --out $@
	rm -f $(FATX16_STATE)/grown.qcow2
UPLOAD_FATX_TEST = python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin \
    $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) --map $(RELEASE_BUILD)/xboxkrnl.map --no-disc --boot-wait 2 \
    --upload 18080 --upload-xbe $(ROOT)/tools/ftp_server/bin/default.xbe
test_upload: ftp_server $(XDASH_HDD) $(FATX16_HDD)
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	$(FORMAT_TEST) --blank-marker 0 --upload 18080 --upload-xbe $(ROOT)/tools/ftp_server/bin/default.xbe
	$(UPLOAD_FATX_TEST) --hdd $(XDASH_HDD)
	$(UPLOAD_FATX_TEST) --hdd $(FATX16_HDD)

# UDF phase (milestone 8): the self-test build with a UDF disc (tests/mkudf.py) instead of the XISO.
test_udf: $(BUILD)/bios.bin $(HDD_FETCH)
	python3 -I $(ROOT)/tests/boot_test.py --bios $(BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) --map $(BUILD)/xboxkrnl.map \
	    --hdd $(HDD_IMAGE) --udf --boot-wait 4

# Interactive: boot the release BIOS in xemu; optional ISO=path/to/game.iso.
# The HDD copy and EEPROM persist in STATE (saves and settings survive; made from PLAY_HDD the first time, which is
# never written): FRESH=1 starts it over, STATE= (empty) uses a throwaway copy. Display, by default widescreen in a
# 1920x1080 window at twice the render resolution: WIDE=0 (no EEPROM widescreen flag; a flag already saved in STATE
# stays until FRESH=1 or the dashboard clears it), ASPECT=auto|native|4x3|16x9 (xemu's picture aspect; auto follows the
# widescreen signal each game sends), SIZE= (window size), SCALE=1..10, FULL=1 (full screen). HD=1: the HDTV AV pack
# and the EEPROM's 480p/720p/1080i flags (HD=480p,720p for a subset), so titles use their HD modes. ENCODER=focus or
# xcalibur: the video encoder xemu presents (default conexant).
# PLAY_HDD: the disk image the run targets start from, by default the stock Microsoft dashboard image when you have
# one (its Settings set up video modes, clock, language); the tests keep using HDD_IMAGE. One state directory per
# image, so switching images never mixes saves.
PLAY_HDD    ?= $(firstword $(wildcard $(ROOT)/xbox_files/hdd/msdash.qcow2) $(HDD_IMAGE))
# Each xemu instance keeps its state (hard-disk copy, EEPROM) in xemu_state/<bios>_<INSTANCE>: oxos_xemu (the
# default xemu_dashboard.qcow2), oxos_msdash (xbox_files/hdd/msdash.qcow2), or INSTANCE=name for any other disk
# (default: its file name); stock_<INSTANCE> for make run_stock.
INSTANCE    ?= $(if $(filter $(XDASH_HDD),$(abspath $(PLAY_HDD))),xemu,$(if $(findstring msdash,$(notdir $(PLAY_HDD))),msdash,$(subst -,_,$(basename $(notdir $(PLAY_HDD))))))
STATE       ?= $(ROOT)/xemu_state/oxos_$(INSTANCE)
# run_stock with another BIOS (BIOS_BASE=...): BIOS_NAME names its state, e.g. BIOS_NAME=cerbios -> cerbios_<INSTANCE>.
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

# The same with the debug serial port (spec/oxos_internal/03_debug_serial.md), whose output is printed in this terminal. No import tracing, so
# titles see the memory of a release build (plus the serial code); also the build to flash for troubleshooting on
# hardware.
run_trace_basic: $(PLAY_FETCH)
	$(MAKE) BUILD=$(TRACE_BASIC_BUILD) SELFTEST=0 SERIAL=1 $(TRACE_BASIC_BUILD)/bios.bin
	python3 -I $(ROOT)/tools/run.py $(TRACE_BASIC_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) $(if $(STATE),--state $(STATE)) $(RUN_OPTS) --serial

# Source-level debugging: the same as run_trace_basic with the debug kernel (DEBUG=1) and xemu's GDB server on
# localhost:$(GDB_PORT), paused at power-on. Attach with gdb-multiarch (symbols: $(DEBUG_BUILD)/xboxkrnl.elf, plus an
# app's own); tools/debug_app.py does the same for an nxdk app, and tools/vscode/ sets it up for VS Code.
GDB_PORT    ?= 1234
run_gdb: $(PLAY_FETCH)
	$(MAKE) BUILD=$(DEBUG_BUILD) SELFTEST=0 SERIAL=1 DEBUG=1 $(DEBUG_BUILD)/bios.bin
	@echo "attach from another terminal: gdb-multiarch $(CURDIR)/$(DEBUG_BUILD)/xboxkrnl.elf -ex 'target remote localhost:$(GDB_PORT)'"
	python3 -I $(ROOT)/tools/run.py $(DEBUG_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) $(if $(STATE),--state $(STATE)) $(RUN_OPTS) \
	    --serial --gdb $(GDB_PORT)

# Import tracing: the same as run_trace_basic, plus a record of the title's last 32768 kernel calls. They are dumped by
# themselves when the kernel reports a crash and when xemu is closed (the last 30 in this terminal, all of them in
# $(TRACE_BUILD)/dumps/); at any other time, `make print_trace_dump` in another terminal prints them (tools/trace.py; LAST=N calls,
# ONLY=Name,Name, ALL=1 shows the common calls too). TRACE=2 (default) keeps the record above 64 MB, with xemu set
# to 128 MB, so titles get the memory of a release build (Halo 2 needs it) and the calls from before a quick reboot
# are kept. TRACE=1 keeps it in the console's 64 MB (2 MB less for the title). Each mode has its own build folder,
# because make does not rebuild when only the flags change; pass the same TRACE to print_trace_dump.
TRACE       ?= 2
TRACE_BUILD := $(OUT)/trace_full$(if $(filter 1,$(TRACE)),-64mb)
run_trace_full: $(PLAY_FETCH)
	$(MAKE) BUILD=$(TRACE_BUILD) SELFTEST=0 TRACE_IMPORTS=$(TRACE) SERIAL=1 $(TRACE_BUILD)/bios.bin
	python3 -I $(ROOT)/tools/run.py $(TRACE_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) $(if $(STATE),--state $(STATE)) $(RUN_OPTS) \
	    --serial --qmp $(TRACE_BUILD)/qmp.sock $(if $(filter 2,$(TRACE)),--memory 128) \
	    --trace-map $(TRACE_BUILD)/xboxkrnl.map --dump-dir $(TRACE_BUILD)/dumps

print_trace_dump:
	python3 -I $(ROOT)/tools/trace.py --qmp $(TRACE_BUILD)/qmp.sock --map $(TRACE_BUILD)/xboxkrnl.map \
	    --exports exports.tsv $(if $(LAST),--last $(LAST)) $(if $(ONLY),--only $(ONLY)) $(if $(filter 1,$(ALL)),--all)

# Controller input test (tests/controller_test_title, an nxdk title): a virtual uinput gamepad presses every input
# and the title reports what it received. The title is built on demand (its disc image below) with nxdk, which
# `make nxdk` (or the first make test_controller) fetches into third_party/ (not committed). CONTROLLER_BIOS=stock
# compares with the reference BIOS.
NXDK_DIR    := $(abspath $(ROOT)/third_party/nxdk)
NXDK_COMMIT := 14d5ee97e73347c973f1f57b68b79ec08c9e77f2
NXDK_ENV    := NXDK_DIR=$(NXDK_DIR) PATH=$(NXDK_DIR)/bin:/usr/lib/llvm-18/bin:$$PATH
CONTROLLER_BIOS ?= ours
CONTROLLER_DIR  := $(ROOT)/tests/controller_test_title
CONTROLLER_ISO  := $(CONTROLLER_DIR)/controller_test_title.iso

$(NXDK_DIR)/Makefile:
	git clone -q https://github.com/XboxDev/nxdk $(NXDK_DIR)
	git -C $(NXDK_DIR) checkout -q $(NXDK_COMMIT)
	git -C $(NXDK_DIR) submodule update -q --init --recursive
nxdk: $(NXDK_DIR)/Makefile

$(CONTROLLER_ISO): $(NXDK_DIR)/Makefile $(CONTROLLER_DIR)/controller_test_title.cpp $(CONTROLLER_DIR)/Makefile
	$(NXDK_ENV) $(MAKE) -C $(CONTROLLER_DIR) -j8

# Image test title (tests/image_test_title, an nxdk title): a picture decoded on the console, a status panel and
# the front LED changing once a second: the kernel, a title and video work. make image_test builds its disc image;
# make run_image_test boots it in xemu with the release BIOS (RUN_OPTS as make run).
IMAGE_TEST_DIR := $(ROOT)/tests/image_test_title
IMAGE_TEST_ISO := $(IMAGE_TEST_DIR)/image_test.iso
$(IMAGE_TEST_ISO): $(NXDK_DIR)/Makefile $(IMAGE_TEST_DIR)/image_test.cpp $(IMAGE_TEST_DIR)/Makefile $(IMAGE_TEST_DIR)/picture.jpg
	$(NXDK_ENV) $(MAKE) -C $(IMAGE_TEST_DIR) -j8
image_test: $(IMAGE_TEST_ISO)
run_image_test: $(IMAGE_TEST_ISO) $(PLAY_FETCH)
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	python3 -I $(ROOT)/tools/run.py $(RELEASE_BUILD)/bios.bin --iso $(IMAGE_TEST_ISO) $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) $(RUN_OPTS)

test_controller: $(CONTROLLER_ISO) $(HDD_FETCH)
	$(if $(filter stock,$(CONTROLLER_BIOS)),,$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin)
	python3 -I $(ROOT)/tests/controller_test.py --bios $(if $(filter stock,$(CONTROLLER_BIOS)),$(BIOS_BASE),$(RELEASE_BUILD)/bios.bin) \
		$(if $(filter stock,$(CONTROLLER_BIOS)),--mcpx-rom $(STOCK_MCPX),$(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM))) \
		--hdd $(HDD_IMAGE) --iso $(CONTROLLER_ISO)

# Run an XBE from the hard disk, e.g. a homebrew app copied to F: on an exFAT disk: a disc with the XBE launcher
# (tools/xbe_launcher, nxdk) and launch.txt = XBE boots, and the launcher starts that XBE the way a dashboard does (the
# XBE then runs from the disk, with D: its folder). XBE='F:\apps\demo\default.xbe' (quoted, for the backslashes).
# The disk: xemu_state/oxos_exfat/hdd.qcow2, an exFAT copy of PLAY_HDD made the first time (mount it on the PC to copy
# files in; unmount it before running), or STATE=... for another state folder. run_trace_xbe is the same with the
# trace build (run_trace_full): the debug serial port in this terminal, the kernel call record dumped on a crash.
LAUNCHER_DIR := $(ROOT)/tools/xbe_launcher
LAUNCHER_XBE := $(LAUNCHER_DIR)/bin/default.xbe
XBE_STATE    := $(if $(filter command line,$(origin STATE)),$(STATE),$(ROOT)/xemu_state/oxos_exfat)

$(LAUNCHER_XBE): $(NXDK_DIR)/Makefile $(LAUNCHER_DIR)/xbe_launcher.cpp $(LAUNCHER_DIR)/Makefile
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

# Disc images (spec/oxos_internal/16_virtual_discs.md): the ISO launcher (tools/iso_launcher, nxdk) mounts an image
# from the hard disk as the console's disc and starts its game. make test_iso: the release build boots a disc with
# the launcher; the image (the kernel test title, relaunch variant) is on an exFAT disk's F:\titles, as one .iso, a
# split .1.iso/.2.iso pair, a .cci and an image over 4 GB; the title runs from it, relaunches itself and ends in the
# dashboard. make run_iso ISO='F:\titles\Halo 2.iso' boots the launcher in xemu for an image on the run_xbe disk.
ISO_LAUNCHER_DIR := $(ROOT)/tools/iso_launcher
ISO_LAUNCHER_XBE := $(ISO_LAUNCHER_DIR)/bin/default.xbe
ISO_KINDS ?= plain split cci splitcci large

$(ISO_LAUNCHER_XBE): $(NXDK_DIR)/Makefile $(ISO_LAUNCHER_DIR)/iso_launcher.cpp $(ISO_LAUNCHER_DIR)/mount_iso.h $(ISO_LAUNCHER_DIR)/Makefile
	$(NXDK_ENV) $(MAKE) -C $(ISO_LAUNCHER_DIR) -j8
iso_launcher: $(ISO_LAUNCHER_XBE)

# Title launcher (tools/title_launcher, nxdk): lists F:\games, F:\titles (and game folders) and F:\Applications and starts a disc image (mounted) or an XBE chosen with
# the controller. make title_launcher builds bin/default.xbe and title_launcher.iso.
TITLE_LAUNCHER_DIR := $(ROOT)/tools/title_launcher
TITLE_LAUNCHER_XBE := $(TITLE_LAUNCHER_DIR)/bin/default.xbe
$(TITLE_LAUNCHER_XBE): $(NXDK_DIR)/Makefile $(TITLE_LAUNCHER_DIR)/title_launcher.cpp $(ISO_LAUNCHER_DIR)/mount_iso.h $(TITLE_LAUNCHER_DIR)/Makefile
	$(NXDK_ENV) $(MAKE) -C $(TITLE_LAUNCHER_DIR) -j8
title_launcher: $(TITLE_LAUNCHER_XBE)

# FTP server (tools/ftp_server, nxdk): the console's disks over the network (copy, move, delete with any FTP client).
# make ftp_server builds bin/default.xbe and ftp_server.iso; netboot.py IMAGE --title tools/ftp_server/bin runs it.
FTP_SERVER_DIR := $(ROOT)/tools/ftp_server
FTP_SERVER_XBE := $(FTP_SERVER_DIR)/bin/default.xbe
$(FTP_SERVER_XBE): $(NXDK_DIR)/Makefile $(FTP_SERVER_DIR)/ftp_server.cpp $(FTP_SERVER_DIR)/Makefile
	$(NXDK_ENV) $(MAKE) -C $(FTP_SERVER_DIR) -j8
ftp_server: $(FTP_SERVER_XBE)

test_iso: $(ISO_LAUNCHER_XBE) $(TEST_TITLE).xbe $(HDD_FETCH)
	$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin
	set -e; for kind in $(ISO_KINDS); do \
	    python3 -I $(ROOT)/tests/boot_test.py --bios $(RELEASE_BUILD)/bios.bin $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM)) \
	        --map $(RELEASE_BUILD)/xboxkrnl.map --hdd $(HDD_IMAGE) --title $(TEST_TITLE).xbe \
	        --title-map $(TEST_TITLE).map --iso-launch $(ISO_LAUNCHER_XBE) --iso-kind $$kind --boot-wait 1; done

iso_disc: $(ISO_LAUNCHER_XBE) $(PLAY_FETCH)
	@test -n '$(ISO)' || { printf '%s\n' "usage: make run_iso ISO='F:\\titles\\Halo 2.iso' [STATE=...]"; exit 1; }
	@test -f $(XBE_STATE)/hdd.qcow2 || { mkdir -p $(XBE_STATE) && \
	    python3 -I $(ROOT)/tests/exfat.py --from-fatx $(PLAY_HDD) $(XBE_STATE)/hdd.qcow2 --size 256G --cluster 16K; }
	@mkdir -p $(OUT)/run_iso
	python3 -I $(ROOT)/tests/mkxiso.py $(OUT)/run_iso/launcher.iso --add default.xbe=$(ISO_LAUNCHER_XBE) --text 'iso.txt=$(ISO)'

run_iso: iso_disc
	$(MAKE) run ISO=$(OUT)/run_iso/launcher.iso STATE=$(XBE_STATE)

# Memory test (tests/memory_test_title, an nxdk title): how much memory a title is given, as built (XBE flag
# "limit to 64 MB") and with the flag cleared. MEMORY=128 for a 128 MB console; MEMORY_BIOS=... (with MCPX_ROM=...)
# runs another BIOS instead of the release build, e.g. for comparison.
MEMORY_DIR  := $(ROOT)/tests/memory_test_title
MEMORY_XBE  := $(MEMORY_DIR)/bin/default.xbe

$(MEMORY_XBE): $(NXDK_DIR)/Makefile $(MEMORY_DIR)/memory_test_title.cpp $(MEMORY_DIR)/Makefile
	$(NXDK_ENV) $(MAKE) -C $(MEMORY_DIR) -j8

test_memory: $(MEMORY_XBE) $(HDD_FETCH)
	$(if $(MEMORY_BIOS),,$(MAKE) BUILD=$(RELEASE_BUILD) SELFTEST=0 $(RELEASE_BUILD)/bios.bin)
	python3 -I $(ROOT)/tests/memory_test.py --bios $(or $(MEMORY_BIOS),$(RELEASE_BUILD)/bios.bin) --xbe $(MEMORY_XBE) \
	    --both --memory $(or $(MEMORY),64) --hdd $(HDD_IMAGE) $(if $(MCPX_ROM),--mcpx-rom $(MCPX_ROM))

# The same session on the reference BIOS (A/B comparison; your own BIOS and boot ROM, never committed). Its own
# state directory: the stock kernel signs saves with keys OXOS does not have, so the two cannot share saves.
# Another BIOS to compare with: BIOS_BASE=its image BIOS_NAME=name (state xemu_state/<name>_<INSTANCE>); MEMORY=128
# for a 128 MB console (any run target).
run_stock: $(PLAY_FETCH)
	python3 -I $(ROOT)/tools/run.py $(BIOS_BASE) --mcpx-rom $(STOCK_MCPX) $(if $(STATE_STOCK),--state $(STATE_STOCK)) $(RUN_OPTS)

clean:
	rm -rf $(OUT)

.PHONY: all bios bios_256k release netboot mcpx_dump image_test run_image_test hdd_image test test_dash test_encoders test_mcpx test_relaunch test_fatal test_format test_upload test_udf test_controller test_memory nxdk run run_xbe run_trace_xbe xbe_disc run_trace_basic run_trace_full print_trace_dump run_gdb run_stock clean iso_launcher title_launcher ftp_server test_iso iso_disc run_iso
