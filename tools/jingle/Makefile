# The boot jingle on the host (a WAV file to listen to): make -C tools/jingle && tools/jingle/bin/jingle out.wav
CXX ?= g++
# The kernel's synthesizer uses no floating point (the console build has no x87/SSE).
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra

bin/jingle: render.cpp ../../kernel/src/hal/jingle.cpp ../../kernel/src/hal/jingle.h
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ render.cpp ../../kernel/src/hal/jingle.cpp

clean:
	rm -rf bin

.PHONY: clean
