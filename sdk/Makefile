# SPDX-License-Identifier: MIT
# Builds the SDK for its maintainers (extensions only include oxos_extension.mk), into ../build/sdk/:
#
#   make                     ../build/sdk/oxos_sdk/: the SDK as it ships: include/ (oxos_sdk.h, oxos_kernel.h), lib/oxos_sdk.lib (the SDK's functions
#                            from src/oxos_sdk.cpp, the C functions of src/oxos_c_library.cpp and the kernel's import library from src/oxoskrnl.def),
#                            oxos_extension.mk, examples/, README.md, LICENSE; then each example built with that package, as a user builds it, into
#                            ../build/sdk/examples/NAME/ (NAME.xos)
#   make VERSION=X.Y.Z       also ../build/sdk/oxos_sdk_vX.Y.Z.zip, the release file
#
# include/oxos_kernel.h, src/oxoskrnl.def, src/kernel_table_check.c and src/oxos_c_library.cpp are generated from the kernel by tools/gen_oxos_sdk.py; the
# table check is linked against the import library here, so a kernel prototype that does not match its export fails this build. Needs clang, lld-link,
# python3 and a POSIX shell (mkdir, rm).
include oxos_extension.mk

BUILD_DIR := ../build/sdk
PACKAGE := $(BUILD_DIR)/oxos_sdk
HEADERS := $(wildcard include/*.h)
PACKAGE_FILES := $(HEADERS) oxos_extension.mk README.md LICENSE make_sdk_package.py $(wildcard examples/*/*)

EXAMPLES := $(notdir $(wildcard examples/*))

all: $(PACKAGE)/lib/oxos_sdk.lib $(BUILD_DIR)/kernel_table_check.dll $(PACKAGE)/include/oxos_sdk.h $(EXAMPLES) \
     $(if $(VERSION),$(BUILD_DIR)/oxos_sdk_v$(VERSION).zip)

$(BUILD_DIR)/oxoskrnl.lib: src/oxoskrnl.def
	mkdir -p $(BUILD_DIR)
	$(OXOS_LD) /lib /def:$< /machine:x86 /out:$@

$(BUILD_DIR)/oxos_sdk.obj: src/oxos_sdk.cpp src/oxos_extension_context.h $(HEADERS)
	mkdir -p $(BUILD_DIR)
	$(OXOS_CC) $(OXOS_CXXFLAGS) -Iinclude -c $< -o $@

# The C functions the kernel does not have itself (generated from the kernel's runtime/c_library_extensions.cpp), one section per function: an extension
# links only the ones it calls.
$(BUILD_DIR)/oxos_c_library.obj: src/oxos_c_library.cpp $(HEADERS)
	mkdir -p $(BUILD_DIR)
	$(OXOS_CC) $(OXOS_CXXFLAGS) -ffunction-sections -Iinclude -c $< -o $@

$(PACKAGE)/lib/oxos_sdk.lib: $(BUILD_DIR)/oxos_sdk.obj $(BUILD_DIR)/oxos_c_library.obj $(BUILD_DIR)/oxoskrnl.lib
	mkdir -p $(PACKAGE)/lib
	$(OXOS_LD) /lib /out:$@ $^

# Every kernel function of oxos_kernel.h, linked against the import library: the check, not something that ships.
$(BUILD_DIR)/kernel_table_check.dll: src/kernel_table_check.c $(HEADERS) $(BUILD_DIR)/oxoskrnl.lib
	$(OXOS_CC) $(OXOS_CFLAGS) -Iinclude -c $< -o $(BUILD_DIR)/kernel_table_check.obj
	$(OXOS_LD) $(OXOS_LDFLAGS) /OUT:$@ $(BUILD_DIR)/kernel_table_check.obj $(BUILD_DIR)/oxoskrnl.lib

$(PACKAGE)/include/oxos_sdk.h: $(PACKAGE_FILES)
	python3 -I make_sdk_package.py $(PACKAGE)

# An example, built from the package's copy with the package's build rules; its output stays out of the package.
$(EXAMPLES): $(PACKAGE)/lib/oxos_sdk.lib $(PACKAGE)/include/oxos_sdk.h
	mkdir -p $(BUILD_DIR)/examples/$@
	$(MAKE) -C $(PACKAGE)/examples/$@ OXOS_OUT=$(abspath $(BUILD_DIR)/examples/$@)

$(BUILD_DIR)/oxos_sdk_v$(VERSION).zip: $(PACKAGE)/lib/oxos_sdk.lib $(PACKAGE)/include/oxos_sdk.h $(EXAMPLES)
	rm -f $@
	cd $(BUILD_DIR) && zip -q -r -X oxos_sdk_v$(VERSION).zip oxos_sdk

.PHONY: all $(EXAMPLES)
.DEFAULT_GOAL := all
