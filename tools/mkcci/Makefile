# mkcci (a host tool): make -C tools/mkcci
CXX ?= g++
CXXFLAGS ?= -std=c++23 -O2 -Wall -Wextra
OUT := ../../build/tools/mkcci

$(OUT)/mkcci: mkcci.cpp
	@mkdir -p $(OUT)
	$(CXX) $(CXXFLAGS) -o $@ $<

clean:
	rm -rf $(OUT)

.PHONY: clean
