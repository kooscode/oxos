# ISO launcher (nxdk): mounts the disc image named in D:\iso.txt (or the one next to it) and starts its title
# (spec/oxos-internal/16-virtual-discs.md). Built with nxdk (../../third_party/nxdk).
XBE_TITLE = OXOS\ ISO\ launcher
SRCS = $(CURDIR)/iso_launcher.cpp
NXDK_CXX = y
NXDK_DIR ?= $(CURDIR)/../../third_party/nxdk
include $(NXDK_DIR)/Makefile

# The OXOS boot splash, shown for a second before the title starts (after nxdk's rules: its "all" stays the default).
$(CURDIR)/splash_image.h: $(CURDIR)/gen_splash.py $(CURDIR)/../../kernel/splash/splash.png
	python3 -I $^ $@
$(CURDIR)/iso_launcher.obj: $(CURDIR)/splash_image.h
