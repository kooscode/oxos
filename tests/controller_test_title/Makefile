# Controller test title, built with nxdk and SDL3 (../../third_party/nxdk and nxdk-sdl3, fetched by
# `make -C kernel nxdk`); the text screen is ../../tools/sdl_screen.
XBE_TITLE = OXOS\ controller\ test\ title
GEN_XISO = controller_test_title.iso
SRCS = $(CURDIR)/controller_test_title.cpp $(CURDIR)/../../tools/sdl_screen/sdl_screen.cpp
NXDK_CXX = y
NXDK_DIR ?= $(CURDIR)/../../third_party/nxdk
include $(NXDK_DIR)/Makefile
include $(CURDIR)/../../tools/sdl_screen/sdl_screen.mk
