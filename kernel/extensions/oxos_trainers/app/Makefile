# oxos_trainers.xbe: Universal Trainers' menu (nxdk + SDL3, tools/nxdk_app.mk; spec/oxos_internal/37 §5). `make -C kernel extensions` builds it (into
# build/kernel/extensions/oxos_trainers/app/bin/default.xbe) and copies it next to oxos_trainers.xos as oxos_trainers.xbe. Its logo, oxos_trainers_logo.svg,
# becomes a scene header at build time (tools/svg_splash.py) that the kernel's vector renderer (kernel/src/graphics/vector_renderer.cpp, built in) draws.
XBE_TITLE = OXOS\ Trainers
APP_SOURCES = oxos_trainers_app.cpp vector_renderer.cpp
include $(dir $(lastword $(MAKEFILE_LIST)))../../../../tools/nxdk_app.mk

ifeq ($(abspath $(CURDIR)),$(NXDK_APP_BUILD))
vpath %.cpp $(OXOS_ROOT)/kernel/src/graphics
CXXFLAGS += -std=c++20 -I$(CURDIR) -idirafter $(OXOS_ROOT)/kernel/src -DXK_VECTOR_RENDERER_IN_APP
oxos_trainers_app.obj: oxos_trainers_logo.h
oxos_trainers_logo.h: $(NXDK_APP_DIR)/oxos_trainers_logo.svg $(OXOS_ROOT)/tools/svg_splash.py
	python3 -I $(OXOS_ROOT)/tools/svg_splash.py $< $@
endif
