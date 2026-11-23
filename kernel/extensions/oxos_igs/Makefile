# Builds oxos_igs.xos (in-game shortcuts, spec/oxos_internal/48) with the kernel extension SDK as it ships, like any extension: make -C kernel extensions
# builds it into build/kernel/extensions/oxos_igs/ (OXOS_OUT), after the SDK (make -C sdk: build/sdk/oxos_sdk/), and copies oxos_igs.ini next to it.
NAME := oxos_igs
SOURCES := oxos_igs.cpp
OXOS_SDK_PACKAGE ?= ../../../build/sdk/oxos_sdk
include $(OXOS_SDK_PACKAGE)/oxos_extension.mk
