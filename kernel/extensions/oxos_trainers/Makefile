# Builds oxos_trainers.xos (Universal Trainers, spec/oxos_internal/37) with the kernel extension SDK as it ships, like any extension:
# make -C kernel extensions builds it into build/kernel/extensions/oxos_trainers/ (OXOS_OUT), after the SDK (make -C sdk: build/sdk/oxos_sdk/).
NAME := oxos_trainers
SOURCES := oxos_trainers.cpp
OXOS_SDK_PACKAGE ?= ../../../build/sdk/oxos_sdk
include $(OXOS_SDK_PACKAGE)/oxos_extension.mk
