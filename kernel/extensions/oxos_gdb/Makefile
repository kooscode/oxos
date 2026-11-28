# Builds oxos_gdb.xos (the GDB debugger as a kernel extension, spec/oxos_internal/51) with the kernel extension SDK as it ships, like any extension: make -C
# kernel extensions builds it into build/kernel/extensions/oxos_gdb/ (OXOS_OUT), after the SDK (make -C sdk: build/sdk/oxos_sdk/), and copies oxos_gdb.ini
# next to it. The GDB stub and its serial transport are the ones GDB=1 kernels build in (shared/gdb_stub).
NAME := oxos_gdb
SOURCES := oxos_gdb.cpp gdb_stub.cpp serial_transport.cpp
SHARED := ../../../shared
vpath %.cpp $(SHARED)/gdb_stub
EXTRA_OXOS_CXXFLAGS := -DOXOS_EXTENSION_BUILD -I$(SHARED)
OXOS_SDK_PACKAGE ?= ../../../build/sdk/oxos_sdk
include $(OXOS_SDK_PACKAGE)/oxos_extension.mk
