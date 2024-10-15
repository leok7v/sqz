#ifndef rt_generics_header_included
#define rt_generics_header_included

#if __has_include("alloca.h") // alloca()
#include <alloca.h>
#elif __has_include("malloc.h")
#include <malloc.h>
#endif

#include <float.h>
#include <string.h>  // memcpy()

typedef float  fp32_t;
typedef double fp64_t;

#if DBL_DIG < LDBL_DIG
typedef long double fp80_t; // TODO: rt_implement and rt_dispatch
#endif

inline void rt_cannot_dispatch(void) {
    static void (*crash)(void); // == 0
    crash(); // this should not be called
}

#define rt_max_(n, t) inline t rt_max_ ## n(t x, t y) { return x > y ? x : y; }
#define rt_min_(n, t) inline t rt_min_ ## n(t x, t y) { return x < y ? x : y; }

#define rt_implement_for_scalar_types(f) \
    f(char, char) \
    f(short_int, short int) \
    f(int, int) \
    f(long_int, long int) \
    f(long_long_int, long long int) \
    f(unsigned_char, unsigned char) \
    f(unsigned_short_int, unsigned short int) \
    f(unsigned_int, unsigned int) \
    f(unsigned_long_int, unsigned long int) \
    f(unsigned_long_long_int, unsigned long long int) \
    f(float, float) \
    f(double, double)

#define rt_implement_for_pointer_types(f) \
    f(char_ptr, char*) \
    f(short_int_ptr, short int*) \
    f(int_ptr, int*) \
    f(long_int_ptr, long int*) \
    f(long_long_int_ptr, long long int*) \
    f(unsigned_char_ptr, unsigned char*) \
    f(unsigned_short_int_ptr, unsigned short int*) \
    f(unsigned_int_ptr, unsigned int*) \
    f(unsigned_long_int_ptr, unsigned long int*) \
    f(unsigned_long_long_int_ptr, unsigned long long int*) \
    f(float_ptr, float*) \
    f(double_ptr, double*)

rt_implement_for_scalar_types(rt_max_)
rt_implement_for_scalar_types(rt_min_)
rt_implement_for_pointer_types(rt_max_)
rt_implement_for_pointer_types(rt_min_)

#define rt_dispatch_for_scalar_types(f, x, y) _Generic((x),     \
    char*:                   f ## _char_ptr,                    \
    short int*:              f ## _short_int_ptr,               \
    int*:                    f ## _int_ptr,                     \
    long int*:               f ## _long_int_ptr,                \
    long long int*:          f ## _long_long_int_ptr,           \
    unsigned char*:          f ## _unsigned_char_ptr,           \
    unsigned short int*:     f ## _unsigned_short_int_ptr,      \
    unsigned int*:           f ## _unsigned_int_ptr,            \
    unsigned long int*:      f ## _unsigned_long_int_ptr,       \
    unsigned long long int*: f ## _unsigned_long_long_int_ptr,  \
    float*:                  f ## _float_ptr,                   \
    double*:                 f ## _double_ptr,                  \
    default:                 _Generic((x) - (y),                \
        char:                   f ## _char,                     \
        short int:              f ## _short_int,                \
        int:                    f ## _int,                      \
        long int:               f ## _long_int,                 \
        long long int:          f ## _long_long_int,            \
        unsigned char:          f ## _unsigned_char,            \
        unsigned short int:     f ## _unsigned_short_int,       \
        unsigned int:           f ## _unsigned_int,             \
        unsigned long int:      f ## _unsigned_long_int,        \
        unsigned long long int: f ## _unsigned_long_long_int,   \
        float:                  f ## _float,                    \
        double:                 f ## _double,                   \
        default:                rt_cannot_dispatch))(x, y)

#define rt_max(x, y) rt_dispatch_for_scalar_types(rt_max, x, y)
#define rt_min(x, y) rt_dispatch_for_scalar_types(rt_min, x, y)

#if defined(__clang__) || defined(__GNUC__)
    #define rt_alloca(n)                                       \
        _Pragma("GCC diagnostic push")                         \
        _Pragma("GCC diagnostic ignored \"-Walloca\"")         \
        alloca(n)                                              \
        _Pragma("GCC diagnostic pop")
#elif defined(_MSC_VER)
    #define rt_alloca(n)                                       \
        __pragma(warning(push))                                \
        __pragma(warning(disable: 6255)) /* alloca warning */  \
        alloca(n)                                              \
        __pragma(warning(pop))
#else
    #define rt_alloca(n) alloca(n) // fallback
#endif

static inline void rt_swap_(void* a, void *b, size_t bytes) {
    void* swap = rt_alloca(bytes);
    memcpy(swap,  a, bytes);
    memcpy(a, b, bytes);
    memcpy(b, swap, bytes);
}

#define rt_swap(a, b) _Generic((a), \
    char:                   rt_swap_(&a, &b, sizeof(char)),                   \
    unsigned char:          rt_swap_(&a, &b, sizeof(unsigned char)),          \
    short int:              rt_swap_(&a, &b, sizeof(short int)),              \
    unsigned short int:     rt_swap_(&a, &b, sizeof(unsigned short int)),     \
    int:                    rt_swap_(&a, &b, sizeof(int)),                    \
    unsigned int:           rt_swap_(&a, &b, sizeof(unsigned int)),           \
    long int:               rt_swap_(&a, &b, sizeof(long int)),               \
    unsigned long int:      rt_swap_(&a, &b, sizeof(unsigned long int)),      \
    long long int:          rt_swap_(&a, &b, sizeof(long long int)),          \
    unsigned long long int: rt_swap_(&a, &b, sizeof(unsigned long long int)), \
    float:                  rt_swap_(&a, &b, sizeof(float)),                  \
    double:                 rt_swap_(&a, &b, sizeof(double)),                 \
    long double:            rt_swap_(&a, &b, sizeof(long double)),            \
    default:                rt_swap_(&a, &b, sizeof(a)))

#endif // rt_generics_header_included
