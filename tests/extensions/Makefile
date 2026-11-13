# Test extensions for the kernel extension loader (spec/oxos_internal/34 §9.3), built with the SDK (oxos_sdk/);
# tests/boot_test.py --extensions copies them to the test disk. make OXOS_OUT=DIR (DIR must exist).
include ../../oxos_sdk/oxos_extension.mk

CPP_TEST_EXTENSIONS := extension_test chain failload badversion
$(foreach e,$(CPP_TEST_EXTENSIONS),$(eval $(call OXOS_EXTENSION_RULES,$(e),$(e).cpp)))
$(eval $(call OXOS_EXTENSION_RULES,plain_c,plain_c.c))
# The SDK's example, so it keeps building and working.
vpath hello.c ../../oxos_sdk/examples/hello
$(eval $(call OXOS_EXTENSION_RULES,hello,hello.c))

all: $(CPP_TEST_EXTENSIONS:%=$(OXOS_OUT)/%.xos) $(OXOS_OUT)/plain_c.xos $(OXOS_OUT)/hello.xos
.DEFAULT_GOAL := all
