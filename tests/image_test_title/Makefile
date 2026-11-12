# Image test title, built with nxdk and SDL3 (../../third_party/nxdk and nxdk-sdl3, fetched by
# `make -C kernel nxdk`; the screen is ../../tools/sdl_screen):
# make -C kernel image_test -> tests/image_test_title/image_test.iso (README.md in this folder).
# PICTURE=file.jpg puts another picture on the disc (as D:\picture.jpg).
XBE_TITLE = OXOS\ image\ test
GEN_XISO = image_test.iso
SRCS = $(CURDIR)/image_test.cpp $(CURDIR)/../../tools/sdl_screen/sdl_screen.cpp
NXDK_CXX = y
PICTURE ?= $(CURDIR)/picture.jpg
NXDK_DIR ?= $(CURDIR)/../../third_party/nxdk
include $(NXDK_DIR)/Makefile
include $(CURDIR)/../../tools/sdl_screen/sdl_screen.mk

TARGET += $(OUTPUT_DIR)/picture.jpg
$(GEN_XISO): $(OUTPUT_DIR)/picture.jpg
$(OUTPUT_DIR)/picture.jpg: $(PICTURE) $(OUTPUT_DIR)
	$(VE)cp '$<' '$@'
