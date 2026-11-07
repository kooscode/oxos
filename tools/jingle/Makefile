# The boot jingle on the host (a WAV file to listen to): make -C tools/jingle && tools/jingle/bin/jingle out.wav
CXX ?= g++
# The kernel's synthesizer uses no floating point (the console build has no x87/SSE).
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra

bin/jingle: render.cpp ../../kernel/src/bootscreen/jingle.cpp ../../kernel/include/xk/jingle.h
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -I../../kernel/include -o $@ render.cpp ../../kernel/src/bootscreen/jingle.cpp

clean:
	rm -rf bin

.PHONY: clean
