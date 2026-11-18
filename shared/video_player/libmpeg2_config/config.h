// libmpeg2's build configuration (the config.h its sources include; written by hand in place of its configure script) for the console's Pentium III: x86
// with MMX, the CPU's abilities read with CPUID at start-up.
#pragma once

#define ARCH_X86 1
#define ACCEL_DETECT 1
#define HAVE_BUILTIN_EXPECT 1
#define HAVE_INTTYPES_H 1
#define HAVE_STDINT_H 1
#define HAVE_STDLIB_H 1
#define HAVE_STRING_H 1
#define STDC_HEADERS 1
#define ATTRIBUTE_ALIGNED_MAX 64
#define restrict __restrict__
