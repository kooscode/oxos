# SDL3 for an OXOS nxdk app (Ryzee119's nxdk-sdl3, fetched into third_party/nxdk-sdl3 by `make -C kernel nxdk`), with
# the shared text screen (sdl_screen.h). In the app's Makefile:
#   SRCS = $(CURDIR)/app.cpp $(CURDIR)/../../tools/sdl_screen/sdl_screen.cpp
#   include $(NXDK_DIR)/Makefile
#   include $(CURDIR)/../../tools/sdl_screen/sdl_screen.mk
SDL_SCREEN_DIR := $(dir $(abspath $(lastword $(MAKEFILE_LIST))))
NXDK_SDL3_DIR ?= $(abspath $(SDL_SCREEN_DIR)/../../third_party/nxdk-sdl3)

include $(NXDK_SDL3_DIR)/config_sdl.make
CFLAGS += $(SDL3_FLAGS)
CXXFLAGS += $(SDL3_FLAGS) -I$(SDL_SCREEN_DIR)
# nxdk compiles without optimisation; SDL is built for size (its objects are shared by every app).
$(SDL3_OBJS): CFLAGS += -Os
main.exe: libSDL3.lib
