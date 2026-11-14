# XBE launcher (nxdk + SDL3, tools/nxdk_app.mk): started from a disc image that `make run_xbe` builds, it launches
# the XBE named in the disc's launch.txt, e.g. F:\apps\demo\default.xbe, the way a dashboard would. Builds in
# build/tools/xbe_launcher (bin/default.xbe).
XBE_TITLE = OXOS\ XBE\ launcher
APP_SOURCES = xbe_launcher.cpp
include $(dir $(lastword $(MAKEFILE_LIST)))../../tools/nxdk_app.mk
