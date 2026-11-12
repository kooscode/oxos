# XBE launcher (nxdk + SDL3): started from a disc image that `make run_xbe` builds, it launches the XBE named in the
# disc's launch.txt, e.g. F:\apps\demo\default.xbe, the way a dashboard would. Built with nxdk and SDL3
# (../../third_party/nxdk and nxdk-sdl3, fetched by `make -C kernel nxdk`); messages use ../sdl_screen.
XBE_TITLE = OXOS\ XBE\ launcher
SRCS = $(CURDIR)/xbe_launcher.cpp $(CURDIR)/../sdl_screen/sdl_screen.cpp
NXDK_CXX = y
NXDK_DIR ?= $(CURDIR)/../../third_party/nxdk
include $(NXDK_DIR)/Makefile
include $(CURDIR)/../sdl_screen/sdl_screen.mk
