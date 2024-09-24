#ifndef rt_generics_header_included
#define rt_generics_header_included

#include <stdint.h>
#include <malloc.h>

typedef float  fp32_t;
typedef double fp64_t;

// Beware that "Type names ending with a _t are reserved by
// The Open Group Base Specifications Issue 7, 2018 edition
// IEEE Std 1003.1-2017 (Revision of IEEE Std 1003.1-2008)"
// https://pubs.opengroup.org/onlinepubs/9699919799/functions/V2_chap02.html
// and this header might break in 2099 when Open Group decides to
// have name better than "double" and "float" to a couple of fundamental
// compute type (maybe along with fp8_t fp16_t and bf16_t etc)

static inline int8_t   rt_max_int8(int8_t x, int8_t y)       { return x > y ? x : y; }
static inline int16_t  rt_max_int16(int16_t x, int16_t y)    { return x > y ? x : y; }
static inline int32_t  rt_max_int32(int32_t x, int32_t y)    { return x > y ? x : y; }
static inline int64_t  rt_max_int64(int64_t x, int64_t y)    { return x > y ? x : y; }
static inline uint8_t  rt_max_uint8(uint8_t x, uint8_t y)    { return x > y ? x : y; }
static inline uint16_t rt_max_uint16(uint16_t x, uint16_t y) { return x > y ? x : y; }
static inline uint32_t rt_max_uint32(uint32_t x, uint32_t y) { return x > y ? x : y; }
static inline uint64_t rt_max_uint64(uint64_t x, uint64_t y) { return x > y ? x : y; }
static inline fp32_t   rt_max_fp32(fp32_t x, fp32_t y)       { return x > y ? x : y; }
static inline fp64_t   rt_max_fp64(fp64_t x, fp64_t y)       { return x > y ? x : y; }

// MS cl.exe version 19.39.33523 has issues with "long":
// does not pick up int32_t/uint32_t types for "long" and "unsigned long"
// need to handle long / unsigned long separately:

static inline long          rt_max_long(long x, long y)                    { return x > y ? x : y; }
static inline unsigned long rt_max_ulong(unsigned long x, unsigned long y) { return x > y ? x : y; }

static inline int8_t   rt_min_int8(int8_t x, int8_t y)       { return x < y ? x : y; }
static inline int16_t  rt_min_int16(int16_t x, int16_t y)    { return x < y ? x : y; }
static inline int32_t  rt_min_int32(int32_t x, int32_t y)    { return x < y ? x : y; }
static inline int64_t  rt_min_int64(int64_t x, int64_t y)    { return x < y ? x : y; }
static inline uint8_t  rt_min_uint8(uint8_t x, uint8_t y)    { return x < y ? x : y; }
static inline uint16_t rt_min_uint16(uint16_t x, uint16_t y) { return x < y ? x : y; }
static inline uint32_t rt_min_uint32(uint32_t x, uint32_t y) { return x < y ? x : y; }
static inline uint64_t rt_min_uint64(uint64_t x, uint64_t y) { return x < y ? x : y; }
static inline fp32_t   rt_min_fp32(fp32_t x, fp32_t y)       { return x < y ? x : y; }
static inline fp64_t   rt_min_fp64(fp64_t x, fp64_t y)       { return x < y ? x : y; }

static inline long          rt_min_long(long x, long y)                    { return x < y ? x : y; }
static inline unsigned long rt_min_ulong(unsigned long x, unsigned long y) { return x < y ? x : y; }


static inline void rt_min_undefined(void) { }
static inline void rt_max_undefined(void) { }

#define rt_max(X, Y) _Generic((X) + (Y), \
    int8_t:   rt_max_int8,   \
    int16_t:  rt_max_int16,  \
    int32_t:  rt_max_int32,  \
    int64_t:  rt_max_int64,  \
    uint8_t:  rt_max_uint8,  \
    uint16_t: rt_max_uint16, \
    uint32_t: rt_max_uint32, \
    uint64_t: rt_max_uint64, \
    fp32_t:   rt_max_fp32,   \
    fp64_t:   rt_max_fp64,   \
    long:     rt_max_long,   \
    unsigned long: rt_max_ulong, \
    default:  rt_max_undefined)(X, Y)

#define rt_min(X, Y) _Generic((X) + (Y), \
    int8_t:   rt_min_int8,   \
    int16_t:  rt_min_int16,  \
    int32_t:  rt_min_int32,  \
    int64_t:  rt_min_int64,  \
    uint8_t:  rt_min_uint8,  \
    uint16_t: rt_min_uint16, \
    uint32_t: rt_min_uint32, \
    uint64_t: rt_min_uint64, \
    fp32_t:   rt_min_fp32,   \
    fp64_t:   rt_min_fp64,   \
    long:     rt_min_long,   \
    unsigned long: rt_min_ulong, \
    default:  rt_min_undefined)(X, Y)


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

#define rt_swap_scalar(T, a, b) do { \
    T tmp = (a);                     \
    (a) = (b);                       \
    (b) = tmp;                       \
} while (0)

static inline void rt_swap_implementation(void* a, void *b, size_t bytes) {
    void* swap = rt_alloca(bytes);
    memcpy(swap,  a, bytes);
    memcpy(a, b, bytes);
    memcpy(b, swap, bytes);
}

#define rt_swap(a, b) _Generic((a), \
    int8_t:   rt_swap_implementation(&a, &b, sizeof(int8_t)),   \
    int16_t:  rt_swap_implementation(&a, &b, sizeof(int16_t)),  \
    int32_t:  rt_swap_implementation(&a, &b, sizeof(int32_t)),  \
    int64_t:  rt_swap_implementation(&a, &b, sizeof(int64_t)),  \
    uint8_t:  rt_swap_implementation(&a, &b, sizeof(uint8_t)),  \
    uint16_t: rt_swap_implementation(&a, &b, sizeof(uint16_t)), \
    uint32_t: rt_swap_implementation(&a, &b, sizeof(uint32_t)), \
    uint64_t: rt_swap_implementation(&a, &b, sizeof(uint64_t)), \
    fp32_t:   rt_swap_implementation(&a, &b, sizeof(fp32_t)),   \
    fp64_t:   rt_swap_implementation(&a, &b, sizeof(fp64_t)),   \
    long:     rt_swap_implementation(&a, &b, sizeof(long)),     \
    unsigned long: rt_swap_implementation(&a, &b, sizeof(unsigned long)), \
    default:  rt_swap_implementation(&a, &b, sizeof(a)))

#endif // rt_generics_header_included
