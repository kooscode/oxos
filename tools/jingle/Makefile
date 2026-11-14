# The boot jingle on the host (a WAV file to listen to): make -C tools/jingle && build/tools/jingle/jingle out.wav
CXX ?= g++
# The kernel's synthesizer uses no floating point (the console build has no x87/SSE).
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra
OUT := ../../build/tools/jingle

$(OUT)/jingle: render.cpp ../../kernel/src/bootscreen/jingle.cpp ../../kernel/include/xk/jingle.h
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -I../../kernel/include -o $@ render.cpp ../../kernel/src/bootscreen/jingle.cpp

clean:
	rm -rf $(OUT)

.PHONY: clean
