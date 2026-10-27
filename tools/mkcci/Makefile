# mkcci (a host tool): make -C tools/mkcci
CXX ?= g++
CXXFLAGS ?= -std=c++20 -O2 -Wall -Wextra

bin/mkcci: mkcci.cpp
	@mkdir -p bin
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -rf bin

.PHONY: clean
