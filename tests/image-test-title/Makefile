# Image test title, built with nxdk (../../third_party/nxdk, fetched by `make -C kernel nxdk`):
# make -C kernel image-test -> tests/image-test-title/image-test.iso (README.md in this folder).
# PICTURE=file.png puts another picture on the disc (as D:\picture.png).
XBE_TITLE = OXOS\ image\ test
GEN_XISO = image-test.iso
SRCS = $(CURDIR)/image_test.c
PICTURE ?= $(CURDIR)/picture.png
NXDK_DIR ?= $(CURDIR)/../../third_party/nxdk
include $(NXDK_DIR)/Makefile

TARGET += $(OUTPUT_DIR)/picture.png
$(GEN_XISO): $(OUTPUT_DIR)/picture.png
$(OUTPUT_DIR)/picture.png: $(PICTURE) $(OUTPUT_DIR)
	$(VE)cp '$<' '$@'
