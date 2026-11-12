# ISO launcher (nxdk + SDL3): mounts the disc image named in D:\iso.txt (or the one next to it) and starts its title
# (spec/oxos_internal/16_virtual_discs.md). Built with nxdk and SDL3 (../../third_party/nxdk and nxdk-sdl3, fetched
# by `make -C kernel nxdk`); messages use ../sdl_screen.
XBE_TITLE = OXOS\ ISO\ launcher
SRCS = $(CURDIR)/iso_launcher.cpp $(CURDIR)/../sdl_screen/sdl_screen.cpp
NXDK_CXX = y
NXDK_DIR ?= $(CURDIR)/../../third_party/nxdk
include $(NXDK_DIR)/Makefile
include $(CURDIR)/../sdl_screen/sdl_screen.mk
