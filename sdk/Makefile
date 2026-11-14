# SPDX-License-Identifier: MIT
# Builds the SDK for its maintainers (extensions only include oxos_extension.mk), into ../build/sdk/:
#
#   make                     ../build/sdk/oxos_sdk/: the SDK as it ships: include/oxos_sdk.h (one header, the parts
#                            in include/ merged), lib/oxos_sdk.lib (one library: the runtime from src/ and the kernel's import library from src/xboxkrnl.def),
#                            oxos_extension.mk, examples/, README.md, LICENSE; then each example built with that package, as a user builds it, into
#                            ../build/sdk/examples/NAME/ (NAME.xos)
#   make VERSION=X.Y.Z       also ../build/sdk/oxos_sdk_vX.Y.Z.zip, the release file
#
# src/xboxkrnl.def is generated from the kernel's exports by tools/gen_oxos_sdk.py. Needs clang, lld-link, python3 and a POSIX shell (mkdir, rm).
include oxos_extension.mk

BUILD_DIR := ../build/sdk
PACKAGE := $(BUILD_DIR)/oxos_sdk
RUNTIME_OBJECT := $(BUILD_DIR)/oxos_runtime.obj
IMPORT_LIBRARY := $(BUILD_DIR)/xboxkrnl.lib
PACKAGE_FILES := $(OXOS_HEADERS) include/oxos_sdk.h oxos_extension.mk README.md LICENSE make_sdk_package.py \
                 $(wildcard examples/*/*)

EXAMPLES := $(notdir $(wildcard examples/*))

all: $(PACKAGE)/lib/oxos_sdk.lib $(PACKAGE)/include/oxos_sdk.h $(EXAMPLES) \
     $(if $(VERSION),$(BUILD_DIR)/oxos_sdk_v$(VERSION).zip)

$(RUNTIME_OBJECT): src/oxos_runtime.cpp $(OXOS_HEADERS) include/oxos_sdk.h
	mkdir -p $(BUILD_DIR)
	$(OXOS_CC) $(OXOS_CXXFLAGS) -c $< -o $@

$(IMPORT_LIBRARY): src/xboxkrnl.def
	mkdir -p $(BUILD_DIR)
	$(OXOS_LD) /lib /def:$< /machine:x86 /out:$@

$(PACKAGE)/lib/oxos_sdk.lib: $(RUNTIME_OBJECT) $(IMPORT_LIBRARY)
	mkdir -p $(PACKAGE)/lib
	$(OXOS_LD) /lib /out:$@ $^

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
