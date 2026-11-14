# Title launcher (nxdk + SDL3, tools/nxdk_app.mk): lists F:\games, F:\titles and F:\Applications (.iso, first
# parts of split .iso/.cci, .cci, .xbe) and starts the one chosen with the controller; disc images are mounted first
# (../iso_launcher/mount_iso.h). `make -C kernel title_launcher`; builds in build/tools/title_launcher.
XBE_TITLE = OXOS\ title\ launcher
GEN_XISO = title_launcher.iso
APP_SOURCES = title_launcher.cpp
include $(dir $(lastword $(MAKEFILE_LIST)))../../tools/nxdk_app.mk
