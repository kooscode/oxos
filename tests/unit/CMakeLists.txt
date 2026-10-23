# Host unit tests for the OXOS core runtime (README.md in this folder). The kernel's core sources are built as a
# 32-bit Linux program; privileged instructions fault and are emulated by the harness (host/hostcpu.cpp), and a
# fake MMU maps the kernel's virtual addresses onto a 64 MB "physical RAM" the way a TLB would.
cmake_minimum_required(VERSION 3.20)
project(oxos_unit_tests C CXX ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(KERNEL ${CMAKE_CURRENT_SOURCE_DIR}/../../kernel)

option(OXOS_COVERAGE "Instrument the kernel sources for llvm-cov" OFF)

set(M32 -m32 -march=pentium3)
# -mstackrealign: kernel threads start on stacks laid out by the kernel, not 16-byte aligned as the i386 Linux ABI
# assumes; functions that need the alignment (SSE spills in the tests and libraries) realign their own frames.
add_compile_options(${M32} -g -O1 -fno-omit-frame-pointer -fno-pic -fno-pie -mstackrealign)
# Not position independent: the binary sits at 0x08048000, above the kernel's low window (host/hostcpu.cpp), and
# host_asm.S addresses KiPcr directly.
add_link_options(${M32} -static) # static: no shared libraries landing in the kernel windows
set(CMAKE_POSITION_INDEPENDENT_CODE OFF)

# GoogleTest from the distribution's source package, built 32-bit with everything else.
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
add_subdirectory(/usr/src/googletest ${CMAKE_BINARY_DIR}/googletest EXCLUDE_FROM_ALL)

# The core runtime under test: the kernel's own sources, unchanged apart from XK_HOST_TEST hooks.
set(KERNEL_SOURCES
    src/ke/apc.cpp src/ke/bugcheck.cpp src/ke/clock.cpp src/ke/cpu.cpp src/ke/dpc.cpp src/ke/event.cpp
    src/ke/irql.cpp src/ke/npx.cpp src/ke/queue.cpp src/ke/sched.cpp src/ke/thread.cpp src/ke/timer.cpp
    src/ke/wait.cpp
    src/mm/init.cpp src/mm/memory.cpp src/mm/pool.cpp src/mm/virtual.cpp
    src/ob/object.cpp src/ex/objects.cpp
    src/ps/thread.cpp
    src/rtl/memory.cpp src/rtl/runtime.cpp src/rtl/sprintf.cpp src/rtl/status.cpp src/rtl/string.cpp
    src/rtl/sync.cpp src/rtl/time.cpp
    src/hal/pic_timer.cpp)
list(TRANSFORM KERNEL_SOURCES PREPEND ${KERNEL}/)
add_library(oxos_core STATIC ${KERNEL_SOURCES})
target_include_directories(oxos_core PUBLIC ${KERNEL}/include)
target_compile_definitions(oxos_core PUBLIC XK_HOST_TEST=1 XK_BOOT_SELFTEST=0 XK_DEBUG_SERIAL=1 XK_NETLOG=0
                           XK_TRACE_IMPORTS=0 XK_RETAIL_MEDIA_POLICY=0 XK_NETLOG_IP=0,0,0,0 XK_NETLOG_DEST=0,0,0,0)
target_compile_options(oxos_core PRIVATE -mno-sse -mno-mmx -ffreestanding -fno-exceptions -fno-rtti -fno-builtin -fno-stack-protector
                       -fno-threadsafe-statics -Wall -Wextra -Wno-unused-parameter)
if(OXOS_COVERAGE)
    target_compile_options(oxos_core PRIVATE -fprofile-instr-generate -fcoverage-mapping)
endif()

file(GLOB TEST_SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/test_*.cpp)
add_executable(oxos_unit_tests
    host/hostcpu.cpp host/host_asm.S host/boot.cpp host/stubs.cpp host/checks.cpp ${TEST_SOURCES})
target_include_directories(oxos_unit_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/host)
target_link_libraries(oxos_unit_tests PRIVATE oxos_core gtest)
if(OXOS_COVERAGE)
    target_link_options(oxos_unit_tests PRIVATE -fprofile-instr-generate)
endif()
