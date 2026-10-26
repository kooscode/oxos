# Image test title, built with nxdk (../../third_party/nxdk, fetched by `make -C kernel nxdk`):
# make -C kernel image-test -> tests/image-test-title/image-test.iso (README.md in this folder).
# PICTURE=file.jpg puts another picture on the disc (as D:\picture.jpg).
XBE_TITLE = OXOS\ image\ test
GEN_XISO = image-test.iso
SRCS = $(CURDIR)/image_test.cpp
NXDK_CXX = y
PICTURE ?= $(CURDIR)/picture.jpg
NXDK_DIR ?= $(CURDIR)/../../third_party/nxdk
include $(NXDK_DIR)/Makefile

TARGET += $(OUTPUT_DIR)/picture.jpg
$(GEN_XISO): $(OUTPUT_DIR)/picture.jpg
$(OUTPUT_DIR)/picture.jpg: $(PICTURE) $(OUTPUT_DIR)
	$(VE)cp '$<' '$@'
