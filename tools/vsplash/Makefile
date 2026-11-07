# The vector boot splash on the host, drawn by the kernel's own code (kernel/src/bootscreen/vsplash.cpp):
#   make -C tools/vsplash && tools/vsplash/bin/vsplash SCENE.bin OUT   (OUT-start.ppm, OUT-end.ppm, OUT-mid.ppm)
# SCENE.bin: tools/svg_splash.py IN.svg SCENE.bin
CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra

bin/vsplash: render.cpp ../../kernel/src/bootscreen/vsplash.cpp ../../kernel/include/xk/vsplash.h
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -I../../kernel/include -o $@ render.cpp ../../kernel/src/bootscreen/vsplash.cpp

clean:
	rm -rf bin

.PHONY: clean
