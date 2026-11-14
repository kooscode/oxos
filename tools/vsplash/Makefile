# The vector boot splash on the host, drawn by the kernel's own code (kernel/src/graphics/vector_renderer.cpp):
#   make -C tools/vsplash && build/tools/vsplash/vsplash SCENE.bin OUT   (OUT-start.ppm, OUT-end.ppm, OUT-mid.ppm)
# SCENE.bin: tools/svg_splash.py IN.svg SCENE.bin
CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra
OUT := ../../build/tools/vsplash

$(OUT)/vsplash: render.cpp ../../kernel/src/graphics/vector_renderer.cpp ../../kernel/src/graphics/vector_renderer.h
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -I../../kernel/src -o $@ render.cpp ../../kernel/src/graphics/vector_renderer.cpp

clean:
	rm -rf $(OUT)

.PHONY: clean
