#ifndef sqz_h
#define sqz_h

#include <stddef.h>
#include <stdint.h>

enum {
    sqz_min_win_bits  =  10,
    sqz_max_win_bits  =  15
};

// See: posix errno.h https://pubs.opengroup.org/onlinepubs/9699919799/
// Range coder errors can be any values != 0 but for the convenience
// of debugging (e.g. strerror()) and testing de facto
// errno_t values are used.

#define sqz_err_io            5 // EIO   : I/O error
#define sqz_err_too_big       7 // E2BIG : Argument list too long
#define sqz_err_no_memory    12 // ENOMEM: Out of memory
#define sqz_err_invalid      22 // EINVAL: Invalid argument
#define sqz_err_range        34 // ERANGE: Result too large
#define sqz_err_data         42 // EILSEQ: Illegal byte sequence
#define sqz_err_unsupported  40 // ENOSYS: Functionality not supported
#define sqz_err_no_space     55 // ENOBUFS: No buffer space available

struct prob_model  { // probability model
    uint64_t freq[256];
    uint64_t tree[256]; // Fenwick Tree (aka BITS)
};

struct range_coder {
    uint64_t low;
    uint64_t range;
    uint64_t code;
    void    (*write)(struct range_coder*, uint8_t);
    uint8_t (*read)(struct range_coder*);
    int32_t  error; // sticky error (e.g. errno_t from read/write)
};

struct sqz { // range coder
    struct range_coder rc;
    void*  that;                    // convenience for caller i/o override
    struct prob_model  pm_size;     // size: 0..255
    struct prob_model  pm_byte;     // single byte
    struct prob_model  pm_dist;     // number of bytes in distance - 1: 0,1,2
    struct prob_model  pm_dist1;    // single byte distance
    struct prob_model  pm_dist2[2];
    struct prob_model  pm_dist3[3];
};

static_assert(offsetof(struct sqz, rc) == 0, "rc must be first field of sqz");

#if defined(__cplusplus)
extern "C" {
#endif

void     sqz_init(struct sqz* s);
void     sqz_compress(struct sqz* s, const void* data, uint64_t bytes, uint16_t window);
uint64_t sqz_decompress(struct sqz* s, void* data, uint64_t bytes);

#if defined(__cplusplus)
} // extern "C"
#endif

#endif // sqz_h

#ifdef sqz_implementation
#ifdef _MSC_VER // cl.exe compiler:
#pragma warning(disable: 4710) // '...': function not inlined
#pragma warning(disable: 4711) // function '...' selected for automatic inline expansion
#pragma warning(disable: 4820) // '...' bytes padding added after data member '...'
#pragma warning(disable: 4996) // The POSIX name for this item is deprecated.
#pragma warning(disable: 5045) // Compiler will insert Spectre mitigation
#pragma warning(disable: 4820) // bytes padding added after data member
#endif

#include "sqz/sqz.h"

#ifndef assert // allows to overide assert in single header lib
#include <assert.h>
#endif
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

static_assert(sizeof(int) >= 4, "32 bits minimum"); // 16 bit int unsupported

#ifndef countof
#define countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

static inline int sqz_bytes_of(int i) {
    assert(i < (1u << 24));
    return (i & 0x00FF0000) ? 3 :
           (i & 0x0000FF00) ? 2 : 1;
}

static inline int32_t ft_lsb(int32_t i) { // least significant bit only
    return i & (~i + 1); // (i & -i)
}

static void ft_init(uint64_t tree[], size_t n, uint64_t a[]) {
    const int32_t m = (int32_t)n;
    for (int32_t i = 0; i <  m; i++) { tree[i] = a[i]; }
    for (int32_t i = 1; i <= m; i++) {
        int32_t parent = i + ft_lsb(i);
        if (parent <= m) {
            tree[parent - 1] += tree[i - 1];
        }
    }
}

static void ft_update(uint64_t tree[], size_t n, int32_t i, uint64_t inc) {
    while (i < (int32_t)n) {
        tree[i] += inc;
        i += ft_lsb(i + 1);
    }
}

static uint64_t ft_query(const uint64_t tree[], size_t n, int32_t i) {
    uint64_t sum = 0;
    while (i >= 0) {
        if (i < (int32_t)n) {
            sum += tree[i];
        }
        i -= ft_lsb(i + 1);
    }
    return sum;
}

static int32_t ft_index_of(uint64_t tree[], size_t n, uint64_t const sum) {
    if (sum >= tree[n - 1]) { return -1; }
    uint64_t value = sum;
    uint32_t i = 0;
    uint32_t mask = (uint32_t)(n >> 1);
    while (mask != 0) {
        uint32_t t = i + mask;
        if (t <= n && value >= tree[t - 1]) {
            i = t;
            value -= tree[t - 1];
        }
        mask >>= 1;
    }
    return i == 0 && value < sum ? -1 : (int32_t)(i - 1);
}

static inline uint64_t pm_sum_of(struct prob_model* pm, uint32_t sym) {
    return ft_query(pm->tree, countof(pm->tree), sym - 1);
}

static inline uint64_t pm_total_freq(struct prob_model* pm) {
    return pm->tree[countof(pm->tree) - 1];
}

static inline int32_t pm_index_of(struct prob_model* pm, uint64_t sum) {
    return ft_index_of(pm->tree, countof(pm->tree), sum) + 1;
}

void pm_init(struct prob_model* pm, uint32_t n) {
    for (size_t i = 0; i < countof(pm->freq); i++) {
        pm->freq[i] = i < n ? 1 : 0;
    }
    ft_init(pm->tree, countof(pm->tree), pm->freq);
}

void pm_update(struct prob_model* pm, uint8_t sym, uint64_t inc) {
    const uint64_t pm_max_freq = (1uLL << (64 - 8));
    if (pm->tree[countof(pm->tree) - 1] < pm_max_freq) {
        pm->freq[sym] += inc;
        ft_update(pm->tree, countof(pm->tree), sym, inc);
    }
}

static void rc_emit(struct range_coder* rc) {
    const uint8_t byte = (uint8_t)(rc->low >> 56);
    rc->write(rc, byte);
    rc->low   <<= 8;
    rc->range <<= 8;
}

static inline bool rc_leftmost_byte_is_same(struct range_coder* rc) {
    return (rc->low >> 56) == ((rc->low + rc->range) >> 56);
}

void rc_init(struct range_coder* rc, uint64_t code) {
    rc->low   = 0;
    rc->range = UINT64_MAX;
    rc->code  = code;
    rc->error = 0;
}

static void rc_flush(struct range_coder* rc) {
    for (int i = 0; i < sizeof(rc->low); i++) {
        rc->range = UINT64_MAX;
        rc_emit(rc);
    }
}

static void rc_consume(struct range_coder* rc) {
    const uint8_t byte   = rc->read(rc);
    rc->code    = (rc->code << 8) + byte;
    rc->low   <<= 8;
    rc->range <<= 8;
}

static void rc_encode(struct range_coder* rc, struct prob_model* pm,
               uint8_t sym) {
    uint64_t total = pm_total_freq(pm);
    uint64_t start = pm_sum_of(pm, sym);
    uint64_t size  = pm->freq[sym];
    rc->range /= total;
    rc->low   += start * rc->range;
    rc->range *= size;
    pm_update(pm, sym, 1);
    while (rc_leftmost_byte_is_same(rc)) { rc_emit(rc); }
    if (rc->range < total + 1) {
        rc_emit(rc);
        rc_emit(rc);
        rc->range = UINT64_MAX - rc->low;
    }
}

static uint8_t rc_err(struct range_coder* rc, int32_t e) {
    rc->error = e;
    return 0;
}

static uint8_t rc_decode(struct range_coder* rc, struct prob_model* pm) {
    uint64_t total = pm_total_freq(pm);
    if (total < 1) { return rc_err(rc, sqz_err_invalid); }
    if (rc->range < total) {
        rc_consume(rc);
        rc_consume(rc);
        rc->range = UINT64_MAX - rc->low;
    }
    uint64_t sum   = (rc->code - rc->low) / (rc->range / total);
    int32_t  sym   = pm_index_of(pm, sum);
    if (sym < 0 || pm->freq[sym] == 0) { return rc_err(rc, sqz_err_data); }
    uint64_t start = pm_sum_of(pm, sym);
    uint64_t size  = pm->freq[sym];
    if (size == 0 || rc->range < total) { return rc_err(rc, sqz_err_data); }
    rc->range /= total;
    rc->low   += start * rc->range;
    rc->range *= size;
    pm_update(pm, (uint8_t)sym, 1);
    while (rc_leftmost_byte_is_same(rc)) { rc_consume(rc); }
    return (uint8_t)sym;
}

void sqz_init(struct sqz* s) {
    rc_init(&s->rc, 0);
    pm_init(&s->pm_size, 256);
    pm_init(&s->pm_byte, 256);
    pm_init(&s->pm_dist, 4);
    pm_init(&s->pm_dist1, 256);
    pm_init(&s->pm_dist2[0], 256);
    pm_init(&s->pm_dist2[1], 256);
    pm_init(&s->pm_dist3[0], 256);
    pm_init(&s->pm_dist3[1], 256);
    pm_init(&s->pm_dist3[2], 256);
}

enum { sqz_min_len =   3 };
enum { sqz_max_len = 254 };

void sqz_compress(struct sqz* s, const void* memory, uint64_t bytes,
                  uint16_t window) {
    static_assert(sizeof(size_t) == 4 || sizeof(size_t) == 8, "32|64 only");
    if (bytes > (uint64_t)INT32_MAX && sizeof(size_t) == 4) {
        s->rc.error = sqz_err_too_big;
    }
    const uint8_t* d = (const uint8_t*)memory;
    uint64_t i = 0;
    while (i < bytes && s->rc.error == 0) {
        size_t size = 0;
        size_t dist = 0;
        // https://en.wikipedia.org/wiki/LZ77_and_LZ78
        if (i >= sqz_min_len) {
            size_t j = i - 1;
            size_t min_j = i >= window ? i - window + 1 : 0;
            for (;;) {
                const size_t n = bytes - i;
                size_t k = 0;
                while (k < n && d[j + k] == d[i + k] && k < sqz_max_len) {
                    k++;
                }
                if (k >= sqz_min_len && k > size) {
                    size = k;
                    dist = i - j;
                    if (size == sqz_max_len) { break; }
                }
                if (j == min_j) { break; }
                j--;
            }
        }
        if (size >= sqz_min_len) {
            printf("[%d,%d]", (int)size, (int)dist);
            rc_encode(&s->rc, &s->pm_size, (uint8_t)size);
            assert(sqz_min_len <= dist && dist < (1u << 24));
            int bc = sqz_bytes_of((uint32_t)dist); // byte count
            rc_encode(&s->rc, &s->pm_dist, (uint8_t)(bc - 1));
            if (bc == 1) {
                rc_encode(&s->rc, &s->pm_dist1, (uint8_t)dist);
            } else if (bc == 2) {
                rc_encode(&s->rc, &s->pm_dist2[0], (uint8_t)(dist));
                rc_encode(&s->rc, &s->pm_dist2[1], (uint8_t)(dist >> 8));
            } else if (bc == 3) {
                rc_encode(&s->rc, &s->pm_dist3[0], (uint8_t)(dist));
                rc_encode(&s->rc, &s->pm_dist3[1], (uint8_t)(dist >> 8));
                rc_encode(&s->rc, &s->pm_dist3[2], (uint8_t)(dist >> 16));
            } else {
                assert(false);
            }
            i += size;
        } else {
            rc_encode(&s->rc, &s->pm_size, 0);
            rc_encode(&s->rc, &s->pm_byte, d[i]);
            printf("%c", d[i]);
            i++;
        }
    }
    printf("\n");
    static_assert(sqz_min_len < 0xFF);
    rc_encode(&s->rc, &s->pm_size, 0xFF);
    rc_flush(&s->rc);
}

uint64_t sqz_decompress(struct sqz* s, void* data, uint64_t bytes) {
    s->rc.code = 0;  // read first 8 bytes
    for (size_t i = 0; i < sizeof(s->rc.code); i++) {
        s->rc.code = (s->rc.code << 8) + s->rc.read(&s->rc);
    }
    uint8_t* d = (uint8_t*)data;
    size_t i = 0;
    while (s->rc.error == 0) {
        uint8_t size = rc_decode(&s->rc, &s->pm_size);
        if (s->rc.error != 0) { break; }
        if (size == 0xFF) {
            break;
        } else if (size == 0) {
            if (i < bytes) {
                d[i++] = rc_decode(&s->rc, &s->pm_byte);
            } else {
                s->rc.error = sqz_err_no_space;
            }
        } else if (size < sqz_min_len || size > sqz_max_len) {
            s->rc.error = sqz_err_range;
        } else {
            uint8_t bc = rc_decode(&s->rc, &s->pm_dist); // byte count - 1
            if (s->rc.error != 0) { break; }
            uint32_t dist = 0;
            if (bc == 0) {
                dist = rc_decode(&s->rc, &s->pm_dist1);
            } else if (bc == 1) {
                dist  =            rc_decode(&s->rc, &s->pm_dist2[0]);
                dist |= ((uint32_t)rc_decode(&s->rc, &s->pm_dist2[1])) << 8;
            } else if (bc == 2) {
                dist  =            rc_decode(&s->rc, &s->pm_dist3[0]);
                dist |= ((uint32_t)rc_decode(&s->rc, &s->pm_dist3[1])) << 8;
                dist |= ((uint32_t)rc_decode(&s->rc, &s->pm_dist3[2])) << 16;
            } else {
                assert(false);
                s->rc.error = sqz_err_data;
            }
            if (s->rc.error == 0) {
                const size_t n = i + size;
                if (i < dist) {
                    s->rc.error = sqz_err_range;
                } else if (i >= dist && n < bytes) {
                    // memcpy() cannot be used on overlapped regions
                    // because it may read more than one byte at a time.
                    uint8_t* p = d - (size_t)dist;
                    while (i < n) { d[i] = p[i]; i++; }
                } else {
                    s->rc.error = sqz_err_no_space;
                }
            }
        }
    }
    return i;
}

#endif // sqz_implementation

