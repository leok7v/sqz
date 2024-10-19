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

// #define sqz_err_io            5 // EIO   : I/O error
// #define sqz_err_too_big       7 // E2BIG : Argument list too long
// #define sqz_err_no_memory    12 // ENOMEM: Out of memory
// #define sqz_err_invalid      22 // EINVAL: Invalid argument
// #define sqz_err_range        34 // ERANGE: Result too large
// #define sqz_err_data         42 // EILSEQ: Illegal byte sequence
// #define sqz_err_unsupported  40 // ENOSYS: Functionality not supported
// #define sqz_err_no_space     55 // ENOBUFS: No buffer space available

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
    int32_t  padding;
};

struct map_entry {
    const uint8_t* data;
    uint64_t hash;
    uint32_t bytes;
};

struct map {
    struct map_entry* entry;
    uint32_t n;
    uint32_t entries;
    uint32_t max_chain;
    uint32_t max_bytes;
};

struct eviction_queue {
    int32_t positions[1u < sqz_max_win_bits]; // circular buffer of entry positions
    int32_t start;                            // index oldest entry
    int32_t end;                              // index to the next free slot
    int32_t count;                            // number of valid entries
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
    struct map         map;         // caller supplied memory for map
    struct eviction_queue eq;       // eviction queue
};

static_assert(offsetof(struct sqz, rc) == 0, "rc must be first field of sqz");

#if defined(__cplusplus)
extern "C" {
#endif

void     sqz_init(struct sqz* s, struct map_entry entry[], size_t n);
void     sqz_compress(struct sqz* s, const void* d, size_t b, uint16_t window);
uint64_t sqz_decompress(struct sqz* s, void* data, size_t bytes);

// Because in C arrays are indexed by both positive and negative index values
// for the simplicity of memory handling the compress/decompress is limited
// to less than 2 ^ (sizeof(size_t) * 8 - 1) bytes.
// It is possible to compress/decompress to be able to handle 2^32 - 1 on
// 32-bit platform but probably does not worth the battle.
// Larger files on 32 bit architectures can be handled in chunks.

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

#define UNSTD_NO_RT_IMPLEMENTATION // TODO: remove
#include "rt/ustd.h"               // TODO: remove

static_assert(sizeof(int) >= 4, "32 bits minimum"); // 16 bit int unsupported

#ifndef countof
#define countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

static void        map_init(struct sqz* s, struct map_entry entry[], size_t n);
static int32_t     map_get(const struct map* m, const void* data, uint32_t bytes);
static int32_t     map_put(struct sqz* s, const void* data, uint32_t bytes);
static int32_t     map_best(const struct map* m, const void* data, size_t bytes);
static void        map_clear(struct map *m);

// map_put()  is no operation if map is filled to 75% or more
// map_get()  returns index of matching entry or -1
// map_best() returns index of longest matching entry or -1

static void eviction_queue_init(struct eviction_queue* q) {
    q->start = 0;
    q->end   = 0;
    q->count = 0;
    memset(q->positions, -1, sizeof(q->positions));
}

static void eviction_queue_add(struct eviction_queue* q, int32_t position) {
    q->positions[q->end] = position;
    q->end = (q->end + 1) % countof(q->positions);
    if (q->count < countof(q->positions)) {
        q->count++;
    } else { // Overwrite oldest entry
        q->start = (q->start + 1) % countof(q->positions);
    }
}

static int32_t eviction_queue_get_oldest(struct eviction_queue* q) {
    int32_t oldest = -1;
    if (q->count > 0) {
        oldest = q->positions[q->start];
        q->start = (q->start + 1) % countof(q->positions);
        q->count--;
    }
    return oldest;
}

// FNV Fowler Noll Vo hash function
// https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function

// FNV offset basis for 64-bit:
static const uint64_t map_hash_init = 0xCBF29CE484222325;

// FNV prime for 64-bit
static const uint64_t map_prime64   = 0x100000001B3;

static inline uint64_t map_hash64_byte(uint64_t hash, const uint32_t byte) {
    return (hash ^ (uint64_t)byte) * map_prime64;
}

static inline uint64_t map_hash64(const uint8_t* data, size_t bytes) {
    assert(2 <= bytes && bytes <= UINT32_MAX);
    uint64_t hash = map_hash_init;
    for (size_t i = 0; i < bytes; i++) {
        hash = map_hash64_byte(hash, data[i]);
    }
    return hash;
}

static void map_init(struct sqz* s, struct map_entry entry[], size_t n) {
    assert(16 < n && n < UINT32_MAX);
    struct map* m = &s->map;
    eviction_queue_init(&s->eq);
    m->entry = entry;
    m->n = (int32_t)n;
    memset(m->entry, 0, n * sizeof(m->entry[0]));
    m->entries = 0;
    m->max_chain = 0;
    m->max_bytes = 0;
}

static int32_t map_get_hashed(const struct map* m, uint64_t hash,
                              const void* d, uint32_t b) {
    assert(2 <= b);
    const struct map_entry* entries = m->entry;
    size_t i = (size_t)hash % m->n;
    // Because map is filled to 3/4 only there will always be
    // an empty slot at the end of the chain.
    while (entries[i].bytes > 0) {
        if (entries[i].bytes == b && entries[i].hash == hash &&
            memcmp(entries[i].data, d, b) == 0) {
            return (int32_t)i;
        }
        i = (i + 1) % m->n;
    }
    return -1;
}

static int32_t map_get(const struct map* m, const void* d, uint32_t b) {
    return map_get_hashed(m, map_hash64(d, b), d, b);
}

static int32_t map_put(struct sqz* s, const void* data, uint32_t b) {
    const uint8_t* d = (const uint8_t*)data;
    struct map* m = &s->map;
    enum { max_bytes = sizeof(m->entry[0]) - 1 };
    assert(2 <= b && b <= UINT32_MAX);
    if (m->entries >= m->n * 3 / 4) {
        // Evict the oldest entry to make space for the new one
        int32_t oldest = eviction_queue_get_oldest(&s->eq);
        if (oldest != -1) { // TODO: assertion?
            m->entry[oldest].bytes = 0; // Mark the entry as evicted
            m->entries--;
        }
    }
    if (m->entries < m->n * 3 / 4) {
        struct map_entry* entries = m->entry;
        uint64_t hash = map_hash64(d, b);
        size_t i = (size_t)hash % m->n;
        uint32_t chain = 0; // max chain length
        while (entries[i].bytes > 0) {
            if (entries[i].bytes == b && entries[i].hash == hash &&
                memcmp(entries[i].data, d, b) == 0 && d > entries[i].data) {
                entries[i].data = d; // update to shorter distance
                return (int32_t)i; // found match with existing entry
            }
            chain++;
            i = (i + 1) % m->n;
            assert(chain < m->n); // looping endlessly?
        }
        if (chain > m->max_chain) { m->max_chain = chain; }
        if (b > m->max_bytes) { m->max_bytes = b; }
        entries[i].data = d;
        entries[i].hash = hash;
        entries[i].bytes = b;
        m->entries++;
        eviction_queue_add(&s->eq, (int32_t)i); // Add to eviction queue
        return (int32_t)i;
    }
    return -1;
}

static int32_t map_best(const struct map* m, const void* data, size_t bytes) {
    int32_t best = -1; // best (longest) result
    if (bytes >= 3) {
        const uint32_t b = (uint32_t)(bytes < UINT32_MAX ? bytes : UINT32_MAX);
        const uint8_t* d = (uint8_t*)data;
        uint64_t hash = map_hash64_byte(map_hash_init, d[0]);
        hash = map_hash64_byte(hash, d[1]);
        for (uint8_t i = 2; i < b - 1; i++) {
            hash = map_hash64_byte(hash, d[i]);
            int32_t r = map_get_hashed(m, hash, data, i + 1);
            if (r != -1) {
                best = r;
            } else {
                break; // will return longest matching entry index
            }
        }
    }
    return best;
}

static void map_clear(struct map *m) {
    memset(m->entry, 0, m->n * sizeof(m->entry[0]));
    m->entries = 0;
    m->max_chain = 0;
    m->max_bytes = 0;
}

static inline int sqz_bytes_of(int i) {
    assert(i < (1u << 24));
    return (i & 0x00FF0000) ? 3 :
           (i & 0x0000FF00) ? 2 : 1;
}

static inline int sqz_bits_of(int i) {
    int bits = 0;
    while (i > 0) {
        i >>= 1;  // Right shift 'i' by 1 bit
        bits++;   // Increment bits count for each shift
    }
    return bits > 0 ? bits : 1;  // Return at least 1 bit for '0'
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
    if (total < 1) { return rc_err(rc, EINVAL); }
    if (rc->range < total) {
        rc_consume(rc);
        rc_consume(rc);
        rc->range = UINT64_MAX - rc->low;
    }
    uint64_t sum   = (rc->code - rc->low) / (rc->range / total);
    int32_t  sym   = pm_index_of(pm, sum);
    if (sym < 0 || pm->freq[sym] == 0) { return rc_err(rc, EILSEQ); }
    uint64_t start = pm_sum_of(pm, sym);
    uint64_t size  = pm->freq[sym];
    if (size == 0 || rc->range < total) { return rc_err(rc, EILSEQ); }
    rc->range /= total;
    rc->low   += start * rc->range;
    rc->range *= size;
    pm_update(pm, (uint8_t)sym, 1);
    while (rc_leftmost_byte_is_same(rc)) { rc_consume(rc); }
    return (uint8_t)sym;
}

void sqz_init(struct sqz* s, struct map_entry entry[], size_t n) {
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
    if (entry != null) {
        map_init(s, entry, n);
    } else {
        memset(&s->map, 0, sizeof(s->map));
    }
}

enum { sqz_min_len =   3 };
enum { sqz_max_len = 254 };

#define SQUEEZE_MAP_STATS

#ifdef SQUEEZE_MAP_STATS

static double sqz_entropy(uint64_t* freq, size_t n) { // Shannon entropy
    double total = 0;
    for (size_t i = 0; i < n; i++) {
        if (freq[i] > 1) {
            total += (double)freq[i];
        }
    }
    double e = 0.0;
    for (size_t i = 0; i < n; i++) {
        if (freq[i] > 1) {
            double p_i = (double)freq[i] / total;
            e -= p_i * log2(p_i);
        }
    }
    return e;
}

#endif

#if 0
// sqz_compress: version0
void sqz_compress(struct sqz* s, const void* memory, size_t bytes,
                  uint16_t window) {
    static_assert(sizeof(size_t) == 4 || sizeof(size_t) == 8, "32|64 only");
    #ifdef SQUEEZE_MAP_STATS
        static double   map_distance_sum;
        static double   map_len_sum;
        static uint64_t map_count;
        map_distance_sum = 0;
        map_len_sum = 0;
        map_count = 0;
        size_t br_bytes = 0; // source bytes encoded as back references
        size_t li_bytes = 0; // source bytes encoded "as is" literals
    #endif
    if (bytes > (uint64_t)INT32_MAX && sizeof(size_t) == 4) {
        s->rc.error = E2BIG;
    }
    const uint8_t* d = (const uint8_t*)memory;
    size_t i = 0;
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
        if (s->map.n > 0) {
            int32_t  best = map_best(&s->map, d + i, bytes - i);
            uint32_t best_bytes =
                     best < 0 ? 0 : s->map.entry[best].bytes;
            uint32_t best_distance = best < 0 ?
                     0 : (uint32_t)(d + i - s->map.entry[best].data);
            if (best_bytes > 5 && best_bytes > size &&
                best_distance < ((1u << 24) - 1)) {
                assert(best_bytes >= sqz_min_len);
                assert(memcmp(d + i - best_distance, d + i, best_bytes) == 0);
                size = best_bytes;
                dist = best_distance;
                #ifdef SQUEEZE_MAP_STATS
                    map_distance_sum += dist;
                    map_len_sum += size;
                    map_count++;
                #endif
            }
        }
        if (size >= sqz_min_len) {
//          printf("[%d,%d]", (int)size, (int)dist);
            rc_encode(&s->rc, &s->pm_size, (uint8_t)size);
            assert(1 <= dist && dist < (1u << 24));
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
            if (s->map.n > 0) {
                map_put(s, d + i, (uint32_t)size);
            }
            i += size;
            #ifdef SQUEEZE_MAP_STATS
                br_bytes += size;
            #endif
        } else {
            #ifdef SQUEEZE_MAP_STATS
            li_bytes++;
            #endif
            rc_encode(&s->rc, &s->pm_size, 0);
            rc_encode(&s->rc, &s->pm_byte, d[i]);
//          printf("%c", d[i]);
            i++;
        }
    }
//  printf("\n");
    static_assert(sqz_min_len < 0xFF);
    rc_encode(&s->rc, &s->pm_size, 0xFF);
    rc_flush(&s->rc);
    #ifdef SQUEEZE_MAP_STATS
        double br_percent = (100.0 * br_bytes) / (br_bytes + li_bytes);
        double li_percent = (100.0 * li_bytes) / (br_bytes + li_bytes);
        printf("literals: %.2f%% back references: %.2f%%\n", li_percent, br_percent);
        double e_b  = sqz_entropy(s->pm_byte.freq, 256);
        double e_s  = sqz_entropy(s->pm_size.freq, 256);
        double e_db = sqz_entropy(s->pm_dist.freq, 256);
        double e_d1 = sqz_entropy(s->pm_dist1.freq, 256);
        double e_d2_0 = sqz_entropy(s->pm_dist2[0].freq, 256);
        double e_d2_1 = sqz_entropy(s->pm_dist2[1].freq, 256);
        double e_d3_0 = sqz_entropy(s->pm_dist3[0].freq, 256);
        double e_d3_1 = sqz_entropy(s->pm_dist3[1].freq, 256);
        double e_d3_2 = sqz_entropy(s->pm_dist3[2].freq, 256);
        printf("entropies: byte: %.2f size: %.2f dist bytes: %.2f "
               "dist1: %.2f dist2: [%.2f %.2f] dist3: [%.2f %.2f %.2f]\n",
            e_b, e_s, e_db, e_d1, e_d2_0, e_d2_1, e_d3_0, e_d3_1, e_d3_2);
        if (map_count > 0) {
            printf("avg dic distance: %.1f length: %.1f mapped count: %lld of %u\n",
                    map_distance_sum / map_count,
                    map_len_sum / map_count, map_count, s->map.n);
            printf("map.max_bytes: %u .max_chain: %u\n",
                    s->map.max_bytes, s->map.max_chain);
        }
    #endif
}

#else

// sqz_compress: version1

void sqz_compress(struct sqz* s, const void* memory, size_t bytes, uint16_t window) {
    static_assert(sizeof(size_t) == 4 || sizeof(size_t) == 8, "32|64 only");
    const uint8_t* d = (const uint8_t*)memory;
    size_t i = 0;
    // Initial error check for size overflow
    if (bytes > (uint64_t)INT32_MAX && sizeof(size_t) == 4) {
        s->rc.error = E2BIG;
        return;
    }
    #ifdef SQUEEZE_MAP_STATS
        static double   map_distance_sum;
        static double   map_len_sum;
        static uint64_t map_count;
        map_distance_sum = 0;
        map_len_sum = 0;
        map_count = 0;
        size_t br_bytes   = 0; // source bytes encoded as back references
        size_t li_bytes   = 0; // source bytes encoded "as is" literals
        size_t rejections = 0; // count of rejected back references
        static size_t histogram[32];
        memset(histogram, 0, sizeof(histogram));
    #endif
    while (i < bytes && s->rc.error == 0) {
        size_t best_size = 0;
        size_t best_dist = 0;
        // Evict map entry if window shifts
        if (i >= window) {
            int32_t evict = eviction_queue_get_oldest(&s->eq);
            while (evict != -1) {
                uint32_t entry_distance = (uint32_t)(d + i - s->map.entry[evict].data);
                if (entry_distance > window) {
                    s->map.entry[evict].bytes = 0;  // Mark entry as evicted
                    s->map.entries--;
                    evict = eviction_queue_get_oldest(&s->eq);
                } else {
                    evict = -1;
                }
            }
        }
        // Use map_best() before O(n²) LZ search
        int32_t best = s->map.n == 0 ? -1 : map_best(&s->map, d + i, bytes - i);
        if (best != -1) {
            uint32_t best_bytes = s->map.entry[best].bytes;
            uint32_t best_distance = (uint32_t)(d + i - s->map.entry[best].data);
            assert(best_bytes >= sqz_min_len);
            assert(memcmp(d + i - best_distance, d + i, best_bytes) == 0);
            // TODO: try to extend the match here
            const uint8_t* p0 = d + i - best_distance + best_bytes;
            const uint8_t* p1 = d + i + best_bytes;
            const uint8_t* pe = d + bytes;
            uint32_t best_bytes_ex = best_bytes;
            while (p1 < pe && *p0 == *p1 && best_bytes_ex < sqz_max_len) {
                best_bytes_ex++;
                p0++;
                p1++;
            }
            if (best_bytes_ex != best_bytes) {
//              printf("best_bytes: %d extended to: %d\n", best_bytes, best_bytes_ex);
                best_bytes = best_bytes_ex;
                map_put(s, d + i, best_bytes_ex);
            }
            // If a good match found in the map, use it
            if (best_bytes >= sqz_min_len && best_distance < window) {
                best_size = best_bytes;
                best_dist = best_distance;
                #ifdef SQUEEZE_MAP_STATS
                    map_distance_sum += best_distance;
                    map_len_sum += best_bytes;
                    map_count++;
                #endif
            }
        }
        // Perform O(n²) LZ search only if map_best() didn't find a match
        if (best_size < sqz_min_len && i >= sqz_min_len) {
            size_t j = i - 1;
            size_t min_j = i >= window ? i - window + 1 : 0;
            // Perform back search in window
            for (;;) {
                const size_t n = bytes - i;
                size_t k = 0;
                // Compare sequences
                while (k < n && d[j + k] == d[i + k] && k < sqz_max_len) {
                    k++;
                }
                // Update best match if necessary
                if (k >= sqz_min_len && k > best_size) {
                    best_size = k;
                    best_dist = i - j;
                    if (best_size == sqz_max_len) break;
                }
                if (j == min_j) break;
                j--;
            }
        }
        // reject back references that take too much compressed space:
        int bc = best_dist > 0 ? sqz_bytes_of((uint32_t)best_dist) : 0;
        if (best_size >= sqz_min_len) {
//          printf("bc: %d best_size: %d\n", bc, (int)best_size);
        }
        if (best_size >= sqz_min_len && (best_size < (size_t)bc + 1 ||
                                         best_dist >= window)) {
            best_size = 0;
            #ifdef SQUEEZE_MAP_STATS
            rejections++;
            #endif
        }
        // Encode back-reference if found
        if (best_size >= sqz_min_len) {
            rc_encode(&s->rc, &s->pm_size, (uint8_t)best_size);
            // Encode distance
            rc_encode(&s->rc, &s->pm_dist, (uint8_t)(bc - 1));
            if (bc == 1) {
                rc_encode(&s->rc, &s->pm_dist1, (uint8_t)best_dist);
            } else if (bc == 2) {
                rc_encode(&s->rc, &s->pm_dist2[0], (uint8_t)(best_dist));
                rc_encode(&s->rc, &s->pm_dist2[1], (uint8_t)(best_dist >> 8));
            } else if (bc == 3) {
                rc_encode(&s->rc, &s->pm_dist3[0], (uint8_t)(best_dist));
                rc_encode(&s->rc, &s->pm_dist3[1], (uint8_t)(best_dist >> 8));
                rc_encode(&s->rc, &s->pm_dist3[2], (uint8_t)(best_dist >> 16));
            }
            // Add this match to the map
            if (s->map.n > 0) {
                map_put(s, d + i, (uint32_t)best_size);
            }
            i += best_size; // Move forward
            #ifdef SQUEEZE_MAP_STATS
                br_bytes += best_size;
                if (best_dist > 0) {
                    int bits = sqz_bits_of((uint32_t)best_dist);
                    histogram[bits]++;
                }
            #endif
        } else {
            #ifdef SQUEEZE_MAP_STATS
                li_bytes++;
            #endif
            // Otherwise encode literal byte
            rc_encode(&s->rc, &s->pm_size, 0);
            rc_encode(&s->rc, &s->pm_byte, d[i]);
            if (s->map.n > 0 && i >= sqz_min_len) {
                map_put(s, d + i, sqz_min_len);
                if (i + 1 < bytes) { map_put(s, d + i, 2); } // Add 2-byte sequence
                if (i + 2 < bytes) { map_put(s, d + i, 3); } // Add 3-byte sequence
                if (i + 3 < bytes) { map_put(s, d + i, 4); } // Add 4-byte sequence
            }
            i++;
        }
    }
    // Mark end of stream
    rc_encode(&s->rc, &s->pm_size, 0xFF);
    rc_flush(&s->rc);
    #ifdef SQUEEZE_MAP_STATS
        double br_percent = (100.0 * br_bytes) / (br_bytes + li_bytes);
        double li_percent = (100.0 * li_bytes) / (br_bytes + li_bytes);
        printf("literals: %.2f%% back references: %.2f%%\n", li_percent, br_percent);
        double e_b  = sqz_entropy(s->pm_byte.freq, 256);
        double e_s  = sqz_entropy(s->pm_size.freq, 256);
        double e_db = sqz_entropy(s->pm_dist.freq, 256);
        double e_d1 = sqz_entropy(s->pm_dist1.freq, 256);
        double e_d2_0 = sqz_entropy(s->pm_dist2[0].freq, 256);
        double e_d2_1 = sqz_entropy(s->pm_dist2[1].freq, 256);
        double e_d3_0 = sqz_entropy(s->pm_dist3[0].freq, 256);
        double e_d3_1 = sqz_entropy(s->pm_dist3[1].freq, 256);
        double e_d3_2 = sqz_entropy(s->pm_dist3[2].freq, 256);
        printf("entropies: byte: %.2f size: %.2f dist bytes: %.2f "
               "dist1: %.2f dist2: [%.2f %.2f] dist3: [%.2f %.2f %.2f]\n",
            e_b, e_s, e_db, e_d1, e_d2_0, e_d2_1, e_d3_0, e_d3_1, e_d3_2);
        if (map_count > 0) {
            printf("avg dic distance: %.1f length: %.1f mapped count: %lld of %u\n",
                    map_distance_sum / map_count,
                    map_len_sum / map_count, map_count, s->map.n);
            printf("map map.entries: %lld .max_bytes: %u .max_chain: %u\n",
                    (uint64_t)s->map.entries, s->map.max_bytes, s->map.max_chain);
        }
        printf("rejections: %lld\n", (uint64_t)rejections);
        double total = 0;
        double cummulative = 0;
        for (int j = 0; j < countof(histogram); j++) { total += histogram[j]; }
        for (int j = 0; j < countof(histogram); j++) {
            if (histogram[j] > 0) {
                double p = (100.0 * histogram[j]) / total;
                cummulative += p;
                printf("histogram[%2d]: %7.3f%% %7.3f%%\n", j,
                    p, cummulative);
            }
        }
    #endif
}

#endif


uint64_t sqz_decompress(struct sqz* s, void* data, size_t bytes) {
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
                s->rc.error = ENOBUFS;
            }
        } else if (size < sqz_min_len || size > sqz_max_len) {
            s->rc.error = ERANGE;
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
                s->rc.error = EILSEQ;
            }
            if (s->rc.error == 0) {
                const size_t n = i + size;
                if (i < dist) {
                    s->rc.error = ERANGE;
                } else if (i >= dist && n <= bytes) {
                    // memcpy() cannot be used on overlapped regions
                    // because it may read more than one byte at a time.
                    uint8_t* p = d - (size_t)dist;
                    while (i < n) { d[i] = p[i]; i++; }
                } else {
                    s->rc.error = ENOBUFS;
                }
            }
        }
    }
    return i;
}

#endif // sqz_implementation

