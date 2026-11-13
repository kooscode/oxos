# SPDX-License-Identifier: MIT
# OXOS kernel extension SDK: build rules for .xos extensions (spec/oxos_internal/34 §9), for GNU make on Windows, macOS
# and Linux.
#
#   NAME := hello                    # builds $(OXOS_OUT)/hello.xos
#   SOURCES := hello.cpp             # C (.c) or C++ (.cpp), in the Makefile's folder
#   include path/to/oxos_sdk/oxos_extension.mk
#
# Needs only clang and lld-link (LLVM 18 or newer) on the PATH, or in the folder LLVM names (LLVM=C:/LLVM/bin). The
# libraries are prebuilt in lib/. The rules run no other command (no shell utilities), so they work from cmd.exe,
# PowerShell, bash or zsh alike. OXOS_OUT (default: the current folder) must exist; the object files go there too,
# named after the extension. Several extensions in one Makefile: define each with
# $(eval $(call OXOS_EXTENSION_RULES,name,sources)).

OXOS_SDK := $(patsubst %/,%,$(dir $(lastword $(MAKEFILE_LIST))))
OXOS_OUT ?= .
LLVM ?=
OXOS_CC := $(if $(LLVM),$(LLVM)/)clang
OXOS_LD := $(if $(LLVM),$(LLVM)/)lld-link

# Kernel code: no floating point or SIMD (the kernel does not save those registers for extensions), no exceptions,
# RTTI or static constructors (spec/oxos_internal/34 §8), and no headers but the SDK's (-nostdinc: nothing from the
# host's C library or SDKs); objects without a time stamp, so the same sources build the same bytes on any system.
OXOS_TARGET_FLAGS := --target=i386-pc-win32 -march=pentium -mno-sse -mno-mmx -mno-80387 -ffreestanding -fno-builtin \
                     -fno-stack-protector -mno-incremental-linker-compatible -nostdinc -nostdlib -O2 -Wall -Wextra \
                     -Werror -Wno-unused-parameter -Wno-missing-field-initializers -I$(OXOS_SDK)/include
OXOS_CFLAGS := $(OXOS_TARGET_FLAGS) -std=c17 $(EXTRA_OXOS_CFLAGS)
OXOS_CXXFLAGS := $(OXOS_TARGET_FLAGS) -std=c++20 -fno-exceptions -fno-rtti -fno-threadsafe-statics \
                 $(EXTRA_OXOS_CXXFLAGS)
# A kernel-mode image (/DRIVER), its sections packed at 32 bytes, as the kernel links itself: a loaded image takes the
# pages its bytes need, not one per section.
OXOS_LDFLAGS := /DLL /DRIVER /NOENTRY /NODEFAULTLIB /SUBSYSTEM:NATIVE /MACHINE:X86 /BASE:0x10000000 /SAFESEH:NO \
                /Brepro /DEBUG:NONE /ALIGN:0x20 /FILEALIGN:0x20 /NOIMPLIB

# The prebuilt libraries: the SDK's runtime and the kernel's import library (oxos_sdk/Makefile rebuilds them).
OXOS_LIBRARIES := $(OXOS_SDK)/lib/oxos_runtime.lib $(OXOS_SDK)/lib/xboxkrnl.lib
OXOS_HEADERS := $(wildcard $(OXOS_SDK)/include/oxos/*.h)

# --- One extension: $(1) its name, $(2) its sources

define OXOS_EXTENSION_RULES
$(OXOS_OUT)/$(1)_%.obj: %.c $(OXOS_HEADERS)
	$(OXOS_CC) $(OXOS_CFLAGS) -c $$< -o $$@

$(OXOS_OUT)/$(1)_%.obj: %.cpp $(OXOS_HEADERS)
	$(OXOS_CC) $(OXOS_CXXFLAGS) -c $$< -o $$@

$(OXOS_OUT)/$(1).xos: $(patsubst %,$(OXOS_OUT)/$(1)_%.obj,$(basename $(2))) $(OXOS_LIBRARIES)
	$(OXOS_LD) $(OXOS_LDFLAGS) /OUT:$$@ $$^
endef

ifdef NAME
$(eval $(call OXOS_EXTENSION_RULES,$(NAME),$(SOURCES)))
all: $(OXOS_OUT)/$(NAME).xos
endif
