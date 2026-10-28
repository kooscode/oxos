# Host unit tests for the OXOS core runtime (README.md in this folder). The kernel's core sources are built as a
# 32-bit Linux program; privileged instructions fault and are emulated by the harness (host/hostcpu.cpp), and a
# fake MMU maps the kernel's virtual addresses onto a 64 MB "physical RAM" the way a TLB would.
#
#   make                      build $(BUILD)/oxos_unit_tests (tools/unit-tests.sh builds and runs it)
#   make COVERAGE=1           kernel sources instrumented for llvm-cov (own build directory)
#   make KERNEL_OPT=-O2       kernel sources at the console build's optimisation (own build directory)
#   make clean                remove this configuration's build directory
#
# Needs a 32-bit (multilib) clang toolchain and GoogleTest's sources in $(GTEST_SRC) (Debian/Ubuntu: googletest).

CXX        ?= clang++
CC         ?= clang
AR         ?= ar
GTEST_SRC  ?= /usr/src/googletest/googletest
COVERAGE   ?= 0
KERNEL_OPT ?=
KERNEL     := ../../kernel
BUILD      ?= build$(if $(filter 1,$(COVERAGE)),-cov)$(if $(KERNEL_OPT),-opt$(subst -,,$(subst =,,$(KERNEL_OPT))))

# Everything: 32-bit, not position independent (the binary sits at 0x08048000, above the kernel's low window,
# host/hostcpu.cpp; host_asm.S addresses KiPcr directly). -mstackrealign: kernel threads start on stacks laid out
# by the kernel, not 16-byte aligned as the i386 Linux ABI assumes; functions that need the alignment (SSE spills
# in the tests and libraries) realign their own frames.
M32      := -m32 -march=pentium3
COMMON   := $(M32) -g -O1 -fno-omit-frame-pointer -fno-pic -fno-pie -mstackrealign -MMD -MP
DEFINES  := -DXK_HOST_TEST=1 -DXK_BOOT_SELFTEST=0 -DXK_DEBUG_SERIAL=1 -DXK_TRACE_IMPORTS=0 \
            -DXK_RETAIL_MEDIA_POLICY=0
COVFLAGS := $(if $(filter 1,$(COVERAGE)),-fprofile-instr-generate -fcoverage-mapping)

# The core runtime under test: the kernel's own sources, unchanged apart from XK_HOST_TEST hooks.
KERNEL_SOURCES := \
    src/ke/apc.cpp src/ke/bugcheck.cpp src/ke/clock.cpp src/ke/cpu.cpp src/ke/dpc.cpp src/ke/event.cpp \
    src/ke/irql.cpp src/ke/npx.cpp src/ke/queue.cpp src/ke/sched.cpp src/ke/thread.cpp src/ke/timer.cpp \
    src/ke/wait.cpp \
    src/mm/init.cpp src/mm/memory.cpp src/mm/pool.cpp src/mm/virtual.cpp \
    src/ob/object.cpp src/ex/objects.cpp \
    src/ps/thread.cpp \
    src/rtl/memory.cpp src/rtl/runtime.cpp src/rtl/sprintf.cpp src/rtl/status.cpp src/rtl/string.cpp \
    src/rtl/sync.cpp src/rtl/time.cpp \
    src/hal/pic_timer.cpp
KERNEL_FLAGS := -std=gnu++20 $(COMMON) $(DEFINES) -I$(KERNEL)/include $(KERNEL_OPT) -mno-sse -mno-mmx \
                -ffreestanding -fno-exceptions -fno-rtti -fno-builtin -fno-stack-protector -fno-threadsafe-statics \
                -Wall -Wextra -Wno-unused-parameter $(COVFLAGS)

# The harness and the tests (host C++: exceptions, RTTI, the C++ library).
TEST_SOURCES := host/hostcpu.cpp host/boot.cpp host/stubs.cpp host/checks.cpp $(sort $(wildcard test_*.cpp))
TEST_FLAGS   := -std=gnu++20 $(COMMON) $(DEFINES) -DGTEST_HAS_PTHREAD=1 -Ihost -I$(KERNEL)/include \
                -isystem $(GTEST_SRC)/include -isystem $(GTEST_SRC)

# GoogleTest itself, from its sources (one translation unit), 32-bit with everything else.
GTEST_FLAGS  := -std=c++20 $(COMMON) -DGTEST_HAS_PTHREAD=1 -fexceptions -I$(GTEST_SRC)/include -I$(GTEST_SRC)

KERNEL_OBJS := $(patsubst src/%.cpp,$(BUILD)/kernel/%.o,$(KERNEL_SOURCES))
TEST_OBJS   := $(patsubst %.cpp,$(BUILD)/%.o,$(TEST_SOURCES)) $(BUILD)/host/host_asm.o
GTEST_OBJ   := $(BUILD)/gtest/gtest-all.o
# The kernel objects go into an archive of this name: host/kernel_text.ld places its code (and only its code) in
# the section where the harness delivers asynchronous interrupts.
CORE_LIB    := $(BUILD)/liboxos_core.a
BIN         := $(BUILD)/oxos_unit_tests

.PHONY: all clean
all: $(BIN)

$(BIN): $(TEST_OBJS) $(CORE_LIB) $(GTEST_OBJ) host/kernel_text.ld
	$(CXX) $(M32) -static $(if $(filter 1,$(COVERAGE)),-fprofile-instr-generate) -Wl,-T,host/kernel_text.ld \
	    -o $@ $(TEST_OBJS) $(CORE_LIB) $(GTEST_OBJ)

$(CORE_LIB): $(KERNEL_OBJS)
	rm -f $@
	$(AR) rcs $@ $^

$(BUILD)/kernel/%.o: $(KERNEL)/src/%.cpp
	@mkdir -p $(@D)
	$(CXX) $(KERNEL_FLAGS) -c $< -o $@

$(BUILD)/%.o: %.cpp
	@mkdir -p $(@D)
	$(CXX) $(TEST_FLAGS) -c $< -o $@

$(BUILD)/host/host_asm.o: host/host_asm.S
	@mkdir -p $(@D)
	$(CC) $(COMMON) $(DEFINES) -Ihost -I$(KERNEL)/include -c $< -o $@

$(GTEST_OBJ): $(GTEST_SRC)/src/gtest-all.cc
	@mkdir -p $(@D)
	$(CXX) $(GTEST_FLAGS) -c $< -o $@

clean:
	rm -rf $(BUILD)

-include $(KERNEL_OBJS:.o=.d) $(TEST_OBJS:.o=.d) $(GTEST_OBJ:.o=.d)
