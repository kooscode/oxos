# ISO launcher (nxdk): mounts the disc image named in D:\iso.txt (or the one next to it) and starts its game
# (spec/oxos-internal/16-virtual-discs.md). Built with nxdk (../../third_party/nxdk).
XBE_TITLE = OXOS\ ISO\ launcher
SRCS = $(CURDIR)/iso_launcher.cpp
NXDK_CXX = y
NXDK_DIR ?= $(CURDIR)/../../third_party/nxdk
include $(NXDK_DIR)/Makefile
