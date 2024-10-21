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

enum { sqz_min_len =   2 };
enum { sqz_max_len = 254 };

static void    map_init(struct sqz* s, struct map_entry entry[], size_t n);
static int32_t map_get(const struct map* m, const void* data, uint32_t bytes);
static int32_t map_put(struct sqz* s, const void* data, uint32_t bytes);
static void    map_best(struct sqz* s, const void* data, size_t bytes,
                        uint32_t* distance, uint8_t* size, uint32_t window);
static void    map_remove(struct map* m, int32_t i);
static void    map_clear(struct map *m);

// map_put()  is no operation if map is filled to 75% or more
// map_get()  returns index of matching entry or -1
// map_best() returns distance and size for best match

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
    while (entries[i].bytes != 0) {
        if (entries[i].bytes == (int32_t)b && entries[i].hash == hash &&
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

static void map_remove(struct map* m, int32_t i) {
    assert(m->entry[i].bytes > 0 && m->entries > 0);
    m->entry[i].bytes = -1;
    m->entry[i].data = null;
    m->entries--;
}

static int32_t map_put(struct sqz* s, const void* data, uint32_t b) {
    const uint8_t* d = (const uint8_t*)data;
    struct map* m = &s->map;
    enum { max_bytes = sizeof(m->entry[0]) - 1 };
    assert(2 <= b && b <= UINT32_MAX);
    if (m->entries < m->n * 3 / 4) {
        struct map_entry* entries = m->entry;
        uint64_t hash = map_hash64(d, b);
        size_t i = (size_t)hash % m->n;
        uint32_t chain = 0; // max chain length
        while (entries[i].bytes != 0) {
            if (entries[i].bytes == (int32_t)b && entries[i].hash == hash &&
                memcmp(entries[i].data, d, b) == 0) {
                assert(d >= entries[i].data); // shorter distance
                entries[i].data = d; // update to shorter distance
                return (int32_t)i;   // found match with existing entry
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
        return (int32_t)i;
    }
    return -1;
}

static void map_best(struct sqz* s, const void* data, size_t bytes,
                     uint32_t* distance, uint8_t* size, uint32_t window) {
    *size = 0;
    *distance = 0;
    struct map* m = &s->map;
    const uint8_t* d = (uint8_t*)data;
    int32_t prev = -1; // previous result
    int32_t best = -1; // best (longest) result
    if (bytes >= sqz_min_len) {
        const uint32_t b = (uint32_t)(bytes < UINT32_MAX ? bytes : UINT32_MAX);
        uint64_t hash = map_hash64_byte(map_hash_init, d[0]);
        hash = map_hash64_byte(hash, d[1]);
        for (uint8_t i = 2; i < b - 1; i++) {
            hash = map_hash64_byte(hash, d[i]);
            int32_t r = map_get_hashed(m, hash, data, i + 1);
            if (r != -1 && d - m->entry[r].data >= window) {
                map_remove(m, r);
            } else if (r != -1) {
                if (prev != best) { prev = best; }
                best = r;
            } else {
                break;
            }
        }
    }
    if (best >= 0) {
        *distance = (uint32_t)(d - m->entry[best].data);
        assert(*distance < window);
        uint32_t b = m->entry[best].bytes;
        const uint8_t* p0 = m->entry[best].data + b;
        const uint8_t* p1 = d + b;
        const uint8_t* pe = d + bytes;
        uint32_t ex = b;
        while (p1 < pe && *p0 == *p1 && ex < sqz_max_len) {
            ex++;
            p0++;
            p1++;
        }
        assert(ex <= sqz_max_len);
        *size = (uint8_t)ex;
// if (prev >= 0 && prev != best) {
//     uint32_t dist = (uint32_t)(d - m->entry[prev].data);
//     if (dist != *distance) {
//         printf("dist: %d bytes: %d %d bytes: %d\n", dist, m->entry[prev].bytes, *distance, ex);
//     }
// }
        if (ex != b) {
            assert(memcmp(m->entry[best].data, d, ex) == 0);
//          printf("[%d] best_bytes: %d extended to: %d\n", best, b, ex);
            map_put(s, d, ex);
        }
        if (ex == 3 && *distance >= 0) {
//          printf("bytes: %d distance: %d\n", ex, *distance);
            *size = 0;
            *distance = 0;
        }
    }
}

static void map_clear(struct map *m) {
    memset(m->entry, 0, m->n * sizeof(m->entry[0]));
    m->entries = 0;
    m->max_chain = 0;
    m->max_bytes = 0;
}

static inline uint8_t sqz_bits_of(uint32_t i) {
    uint8_t bits = 0;
    while (i > 0) { i >>= 1; bits++; }
    return bits;  // 0 bits for i == 0
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
    pm_init(&s->pm_literal, 2);
    pm_init(&s->pm_size, 256);
    pm_init(&s->pm_byte, 256);
    pm_init(&s->pm_bits, 32);
    for (size_t b = 0; b < countof(s->pm_dist); b++) {
        pm_init(&s->pm_dist[b], 2);
    }
    if (entry != null) {
        map_init(s, entry, n);
    } else {
        memset(&s->map, 0, sizeof(s->map));
    }
}

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

void sqz_compress(struct sqz* s, const void* memory, size_t bytes, uint32_t window) {
    static_assert(sizeof(size_t) == 4 || sizeof(size_t) == 8, "32|64 only");
    // Initial error check for size overflow
    if (bytes > (uint64_t)INT32_MAX && sizeof(size_t) == 4) {
        s->rc.error = E2BIG;
        return;
    }
    const uint8_t* d = (const uint8_t*)memory;
    size_t i = 0;
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
        static size_t size_histogram[256];
        static size_t distance_bits_histogram[32];
        memset(distance_bits_histogram, 0, sizeof(distance_bits_histogram));
        memset(size_histogram, 0, sizeof(size_histogram));
    #endif
    while (i < bytes && s->rc.error == 0) {
        uint8_t  best_size = 0;
        uint32_t best_dist = 0;
        // Use map_best() before O(n²) LZ search
        map_best(s, d + i, bytes - i, &best_dist, &best_size, window);
        if (best_size >= sqz_min_len) {
            #ifdef SQUEEZE_MAP_STATS
                map_distance_sum += best_dist;
                map_len_sum += best_size;
                map_count++;
            #endif
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
                    best_size = (uint8_t)k;
                    best_dist = (uint32_t)(i - j);
                    if (best_size == sqz_max_len) break;
                }
                if (j == min_j) break;
                j--;
            }
        }
        // reject back references that take too much compressed space:
        uint8_t bits = sqz_bits_of((uint32_t)best_dist);
        // lit: 0.95 + size: 2.48 + dist bits: 3.35 ~= 6.78
        if (best_size <= 3 && bits > 8) {
//          printf("bytes: %d distance: %d(%d)\n", best_size, best_dist, bits);
            best_size = 0;
            best_dist = 0;
            #ifdef SQUEEZE_MAP_STATS
            rejections++;
            #endif
        }
        if (best_size >= sqz_min_len) {
            rc_encode(&s->rc, &s->pm_literal, 0);
            rc_encode(&s->rc, &s->pm_size, (uint8_t)best_size);
            rc_encode(&s->rc, &s->pm_bits, bits);
            #ifdef SQUEEZE_MAP_STATS
            size_histogram[best_size]++;
            #endif
            uint32_t distance = (uint32_t)best_dist;
            for (int b = 0; b < bits - 1; b++) {
                rc_encode(&s->rc, &s->pm_dist[b], distance & 0x1);
                distance >>= 1;
            }
            if (s->map.n > 0) { map_put(s, d + i, (uint32_t)best_size); }
            i += best_size; // Move forward
            #ifdef SQUEEZE_MAP_STATS
                br_bytes += best_size;
                if (best_dist > 0) {
                    distance_bits_histogram[bits]++;
                }
            #endif
        } else {
            #ifdef SQUEEZE_MAP_STATS
                li_bytes++;
            #endif
            // Otherwise encode literal byte
            rc_encode(&s->rc, &s->pm_literal, 1);
            rc_encode(&s->rc, &s->pm_byte, d[i]);
            if (s->map.n > 0 && i >= sqz_min_len) {
                map_put(s, d + i, 2); // 2-byte sequence
                map_put(s, d + i, sqz_min_len); // 3 bytes
                if (i + 3 < bytes) { map_put(s, d + i, 4); } // 4-bytes
            }
            i++;
        }
    }
    rc_encode(&s->rc, &s->pm_literal, 0);
    rc_encode(&s->rc, &s->pm_size, 0xFF);
    rc_flush(&s->rc);
    #ifdef SQUEEZE_MAP_STATS
        double br_percent = (100.0 * br_bytes) / (br_bytes + li_bytes);
        double li_percent = (100.0 * li_bytes) / (br_bytes + li_bytes);
        printf("literals: %.2f%% back references: %.2f%%\n", li_percent, br_percent);
        printf("entropies: lit: %.2f byte: %.2f size: %.2f dist bits: %.2f",
                sqz_entropy(s->pm_literal.freq, 256),
                sqz_entropy(s->pm_byte.freq, 256),
                sqz_entropy(s->pm_size.freq, 256),
                sqz_entropy(s->pm_bits.freq, 256));
        double h = 0;
        for (int b = 0; b < 24; b++) {
            double e = sqz_entropy(s->pm_dist[b].freq, 2);
            printf(" %.2f", e);
            h += e;
        }
        printf(" sum: %.2f\n", h);
        if (map_count > 0) {
            printf("avg dic distance: %.1f length: %.1f mapped count: %lld of %u\n",
                    map_distance_sum / map_count,
                    map_len_sum / map_count, map_count, s->map.n);
            printf("map map.entries: %lld .max_bytes: %u .max_chain: %u\n",
                    (uint64_t)s->map.entries, s->map.max_bytes, s->map.max_chain);
        }
        printf("rejections: %lld\n", (uint64_t)rejections);
        double total = 0;
        double cumulative = 0;
        for (int j = 0; j < countof(distance_bits_histogram); j++) { total += distance_bits_histogram[j]; }
        for (int j = 0; j < countof(distance_bits_histogram); j++) {
            if (distance_bits_histogram[j] > 0) {
                double p = (100.0 * distance_bits_histogram[j]) / total;
                cumulative += p;
                printf("distance_bits[%2d]: %7.3f%% %7.3f%%\n", j,
                    p, cumulative);
            }
        }
        total = 0;
        cumulative = 0;
        for (int j = 0; j < countof(size_histogram); j++) { total += size_histogram[j]; }
        for (int j = 0; j < countof(size_histogram); j++) {
            if (size_histogram[j] > 0) {
                double p = (100.0 * size_histogram[j]) / total;
                cumulative += p;
//              printf("size[%2d]: %7.3f%% %7.3f%%\n", j,
//                  p, cumulative);
            }
        }
    #endif
}

uint64_t sqz_decompress(struct sqz* s, void* data, size_t bytes) {
    s->rc.code = 0;  // read first 8 bytes
    for (size_t i = 0; i < sizeof(s->rc.code); i++) {
        s->rc.code = (s->rc.code << 8) + s->rc.read(&s->rc);
    }
    uint8_t* d = (uint8_t*)data;
    size_t i = 0;
    while (s->rc.error == 0) {
        uint8_t lit  = rc_decode(&s->rc, &s->pm_literal);
        if (s->rc.error != 0) { break; }
        if (lit) {
            if (i < bytes) {
                d[i++] = rc_decode(&s->rc, &s->pm_byte);
            } else {
                s->rc.error = ENOBUFS;
            }
        } else {
            uint8_t size = rc_decode(&s->rc, &s->pm_size);
            if (size == 0xFF) { break; } // end of stream
            if (size < sqz_min_len || size > sqz_max_len) {
                s->rc.error = ERANGE;
            } else {
                uint8_t bits = rc_decode(&s->rc, &s->pm_bits);
                if (s->rc.error != 0) { break; }
                uint32_t dist = 0;
                for (int b = 0; b < bits - 1 && s->rc.error == 0; b++) {
                    dist |= (uint32_t)rc_decode(&s->rc, &s->pm_dist[b]) << b;
                }
                if (bits > 0) { dist |= (1u << bits); }
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
    }
    return i;
}
