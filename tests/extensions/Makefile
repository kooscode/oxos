# Test extensions for the kernel extension loader (spec/oxos_internal/34 §9.3), built with the SDK as it ships (build/sdk/oxos_sdk/: make -C sdk first);
# tests/boot_test.py --extensions copies them to the test disk, with the SDK's example (build/sdk/examples/hello/hello.xos, which kernel/Makefile copies next to
# them). make OXOS_OUT=DIR (DIR must exist).
include ../../build/sdk/oxos_sdk/oxos_extension.mk

CPP_TEST_EXTENSIONS := extension_test chain failload badversion
$(foreach e,$(CPP_TEST_EXTENSIONS),$(eval $(call OXOS_EXTENSION_RULES,$(e),$(e).cpp)))
$(eval $(call OXOS_EXTENSION_RULES,plain_c,plain_c.c))

all: $(CPP_TEST_EXTENSIONS:%=$(OXOS_OUT)/%.xos) $(OXOS_OUT)/plain_c.xos
.DEFAULT_GOAL := all
