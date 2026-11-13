# SPDX-License-Identifier: MIT
# Rebuilds the SDK's prebuilt libraries in lib/ (for the SDK's maintainers; extensions only include
# oxos_extension.mk): the runtime from src/, and the kernel's import library from lib/xboxkrnl.def, which
# tools/gen_oxos_sdk.py generates from kernel/exports.tsv. Needs only clang and lld-link, as extensions do.
include oxos_extension.mk

RUNTIME_OBJECT := lib/oxos_runtime.obj

all: lib/oxos_runtime.lib lib/xboxkrnl.lib

$(RUNTIME_OBJECT): src/oxos_runtime.cpp $(OXOS_HEADERS)
	$(OXOS_CC) $(OXOS_CXXFLAGS) -c $< -o $@

lib/oxos_runtime.lib: $(RUNTIME_OBJECT)
	$(OXOS_LD) /lib /out:$@ $<

lib/xboxkrnl.lib: lib/xboxkrnl.def
	$(OXOS_LD) /lib /def:$< /machine:x86 /out:$@

.PHONY: all
.DEFAULT_GOAL := all
