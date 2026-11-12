# Title launcher (nxdk + SDL3): lists F:\games, F:\titles and F:\Applications (.iso, first parts of split .iso/.cci,
# .cci, .xbe) and starts the one chosen with the controller; disc images are mounted first
# (../iso_launcher/mount_iso.h). Built with nxdk and SDL3 (../../third_party/nxdk and nxdk-sdl3, fetched by
# `make -C kernel nxdk`); the screen and controllers are ../sdl_screen. `make -C kernel title_launcher`.
XBE_TITLE = OXOS\ title\ launcher
GEN_XISO = title_launcher.iso
SRCS = $(CURDIR)/title_launcher.cpp $(CURDIR)/../sdl_screen/sdl_screen.cpp
NXDK_CXX = y
NXDK_DIR ?= $(CURDIR)/../../third_party/nxdk
include $(NXDK_DIR)/Makefile
include $(CURDIR)/../sdl_screen/sdl_screen.mk
