# Title launcher (nxdk): lists F:\titles (.iso, first parts of split .iso/.cci, .cci, .xbe) and starts the one chosen
# with the controller; disc images are mounted first (../iso-launcher/mount_iso.h). Built with nxdk
# (../../third_party/nxdk): `make -C kernel title-launcher`.
XBE_TITLE = OXOS\ title\ launcher
GEN_XISO = title-launcher.iso
SRCS = $(CURDIR)/title_launcher.cpp
NXDK_CXX = y
NXDK_DIR ?= $(CURDIR)/../../third_party/nxdk
include $(NXDK_DIR)/Makefile
