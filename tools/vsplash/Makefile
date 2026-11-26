# The vector boot splash on the host, drawn by the kernel's own code (shared/graphics/vector_renderer.cpp):
#   make -C tools/vsplash && build/tools/vsplash/vsplash SCENE.bin OUT   (OUT-start.ppm, OUT-end.ppm)
# SCENE.bin: tools/svg_splash.py IN.svg SCENE.bin
CXX ?= g++
CXXFLAGS ?= -std=c++23 -O2 -Wall -Wextra
OUT := ../../build/tools/vsplash

$(OUT)/vsplash: render.cpp ../../shared/graphics/vector_renderer.cpp ../../shared/graphics/vector_renderer.h
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -I../../shared -o $@ render.cpp ../../shared/graphics/vector_renderer.cpp

clean:
	rm -rf $(OUT)

.PHONY: clean
