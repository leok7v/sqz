#ifndef rt_generics_test_header_included
#define rt_generics_test_header_included

#include "rt_generics.h"
#include <stdio.h>
#include <stdlib.h>

void rt_test_generics(void) {
    #pragma push_macro("rt_check")
    #pragma push_macro("rt_check_min_max")
    #pragma push_macro("rt_check_swap")
    #pragma push_macro("rt_local_verbose_defined")
    #pragma push_macro("rt_local_check_defined")
    #ifndef rt_verbose
    #define rt_verbose(f, ...) do { printf(f, __VA_ARGS__); } while (0)
    #define rt_local_verbose_defined
    #endif
    #ifndef rt_check
    #define rt_check(b) do { if (!(b)) { rt_verbose("%s\n", #b); exit(1); } } while (0)
    #define rt_local_check_defined
    #endif
    #define rt_check_min_max(t) {                               \
        t a = 1; t b = 2;                                       \
        rt_check(rt_min(a, b) == a && rt_max(a, b) == b);       \
        t d[2] = {1, 2};                                        \
        t* pa = d + 0; t* pb = d + 0;                           \
        rt_check(rt_min(pa, pb) == pa && rt_max(pa, pb) == pb); \
    }
    #define rt_check_swap(t) { \
        t a = 1; t b = 2; rt_swap(a, b); rt_check(a == 2 && b == 1); \
        t* pa = &a; t* pb = &b; rt_swap(pa, pb);                     \
        rt_check(pa == &b &&  pb == &a);                             \
        rt_check(*pa == 1 && *pb == 2);                              \
    }
    rt_check_min_max(char);
    rt_check_min_max(short int);
    rt_check_min_max(int);
    rt_check_min_max(long int);
    rt_check_min_max(long long int);
    rt_check_min_max(unsigned char);
    rt_check_min_max(unsigned short int);
    rt_check_min_max(unsigned int);
    rt_check_min_max(unsigned long int);
    rt_check_min_max(unsigned long long int);
    rt_check_min_max(float);
    rt_check_min_max(double);
    rt_check_min_max(int8_t);
    rt_check_min_max(int16_t);
    rt_check_min_max(int32_t);
    rt_check_min_max(int64_t);
    rt_check_min_max(uint8_t);
    rt_check_min_max(uint16_t);
    rt_check_min_max(uint32_t);
    rt_check_min_max(uint64_t);
    rt_check_min_max(fp32_t);
    rt_check_min_max(fp64_t);
    rt_check_swap(char);
    rt_check_swap(short int);
    rt_check_swap(int);
    rt_check_swap(long int);
    rt_check_swap(long long int);
    rt_check_swap(unsigned char);
    rt_check_swap(unsigned short int);
    rt_check_swap(unsigned int);
    rt_check_swap(unsigned long int);
    rt_check_swap(unsigned long long int);
    rt_check_swap(float);
    rt_check_swap(double);
    rt_check_swap(int8_t);
    rt_check_swap(int16_t);
    rt_check_swap(int32_t);
    rt_check_swap(int64_t);
    rt_check_swap(uint8_t);
    rt_check_swap(uint16_t);
    rt_check_swap(uint32_t);
    rt_check_swap(uint64_t);
    rt_check_swap(fp32_t);
    rt_check_swap(fp64_t);
    struct point { int x; int y; };
    struct point a = {1, 2};
    struct point b = {3, 4};
//  rt_verbose("a: {%d, %d} b: {%d, %d}\n", a.x, a.y, b.x, b.y);
    rt_swap(a, b);
//  rt_verbose("a: {%d, %d} b: {%d, %d}\n", a.x, a.y, b.x, b.y);
    rt_check(a.x == 3 && a.y == 4 && b.x == 1 && b.y == 2);
    #ifdef rt_local_check_defined
    #undef rt_check
    #endif
    #ifdef rt_local_verbose_defined
    #undef rt_verbose
    #endif
    #pragma pop_macro("rt_local_check_defined")
    #pragma pop_macro("rt_local_verbose_defined")
    #pragma pop_macro("rt_check_swap")
    #pragma pop_macro("rt_check_min_max")
    #pragma pop_macro("rt_check")
}

#endif // rt_generics_test_header_included
