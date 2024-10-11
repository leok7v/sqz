#ifndef unstd_included // unified `unstandard` (non-standard) std header
#define unstd_included

// Copyright (c) 2024, "Leo" Dmitry Kuznetsov
// This code and the accompanying materials are made available under the terms
// of BSD-3 license, which accompanies this distribution. The full text of the
// license may be found at https://opensource.org/license/bsd-3-clause

#include "rt.h"

#ifndef UNSTD_NO_RT_IMPLEMENTATION
#define rt_implementation
#include "rt.h" // implement all functions in header file
#endif

#ifndef UNSTD_NO_SHORTHAND

#undef assert
#undef countof
#undef max
#undef min
#undef swap
#undef swear
#undef printf
#undef println

#ifdef UNSTD_ASSERTS_IN_RELEASE
#define assert(...)     rt_swear(__VA_ARGS__)
#else
#define assert(...)     rt_assert(__VA_ARGS__)
#endif

#define countof(a) rt_countof(a)
#define max(a, b)       rt_max(a, b)
#define min(a, b)       rt_min(a, b)
#define swap(a, b)      rt_swap(a, b)
#define swear(...)      rt_swear(__VA_ARGS__)
#define printf(...)     rt_printf(__VA_ARGS__)
#define println(...)    rt_println(__VA_ARGS__)

#endif

#endif
