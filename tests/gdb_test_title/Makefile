# GDB test title (nxdk, tools/nxdk_app.mk): builds in build/tests/gdb_test_title_debug (DEBUG=y). The kernel Makefile builds it with LDFLAGS=-debug:dwarf (the
# symbol table tests/gdb_test.py reads the addresses from).
XBE_TITLE = OXOS\ gdb\ test\ title
APP_SOURCES = gdb_test_title.cpp
include $(dir $(lastword $(MAKEFILE_LIST)))../../tools/nxdk_app.mk
