# Image test title (nxdk + SDL3, tools/nxdk_app.mk): make -C kernel image_test -> build/tests/image_test_title/image_test.iso (README.md in this folder).
# PICTURE=file.jpg puts another picture on the disc (as D:\picture.jpg; give its full path).
XBE_TITLE = OXOS\ image\ test
GEN_XISO = image_test.iso
APP_SOURCES = image_test.cpp
include $(dir $(lastword $(MAKEFILE_LIST)))../../tools/nxdk_app.mk

ifeq ($(abspath $(CURDIR)),$(NXDK_APP_BUILD))
PICTURE ?= $(NXDK_APP_DIR)/picture.jpg
TARGET += $(OUTPUT_DIR)/picture.jpg
$(GEN_XISO): $(OUTPUT_DIR)/picture.jpg
$(OUTPUT_DIR)/picture.jpg: $(PICTURE) $(OUTPUT_DIR)
	$(VE)cp '$<' '$@'
endif
