#include "sqz/sqz.h"

#include "rt/ustd.h" // Only for debugging convinient. TODO: remove me

#ifndef assert // allows to overide assert in single header lib
#include <assert.h>
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _MSC_VER // overzealous /Wall in cl.exe compiler:
#pragma warning(disable: 4710) // '...': function not inlined
#pragma warning(disable: 4711) // function '...' selected for automatic inline expansion
#pragma warning(disable: 5045) // Compiler will insert Spectre mitigation
#endif

static_assert(sizeof(int) >= 4, "32 bits minimum"); // 16 bit int unsupported

#ifndef countof
#define countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

enum { sqz_min_len =   2 };
enum { sqz_max_len = 254 };
enum { sqz_max_len2_dist = 0xFF };

// must be power of 2 - 1:
static_assert(((sqz_max_len2_dist + 1) & sqz_max_len2_dist) == 0, "");

// must fit into 8 bits:
static_assert(sqz_max_len2_dist <= 0xFF, "");

bool sqz_debug;

#define sqz_trace(...) do { if (sqz_debug) { printf(__VA_ARGS__); } } while (0)

static void map_init(struct map* m, void** p, size_t n, size_t b) {
    assert(16 < n && n <= (1u << 24) && 3 <= b && b <= 8);
    if ( !(16 < n && n <= (1u << 24) && 3 <= b && b <= 8) ) {
        exit(1); // this is not a generic map implementation
    }
    memset(m, 0, sizeof(*m));
    m->p = p;
    m->n = n;
    assert(3 <= b && b <= 8);
    m->m = b < 8 ? (1uLL << (b * 8)) - 1 : UINT64_MAX;
    memset(m->p, 0, n * sizeof(m->p[0]));
}

static inline uint64_t map_hash_key(uint64_t k) {
    k ^= k >> 33; // Bob Jenkins' hash (modified for 64-bit keys)
    k *= 0xFF51AFD7ED558CCDu;
    k ^= k >> 33;
    k *= 0xC4CEB9FE1A85EC53u;
    k ^= k >> 33;
    return k;
}

static inline size_t map_hash(struct map* m, uint64_t k) {
    return (size_t)(map_hash_key(k) % m->n);
}

static inline bool map_get(struct map* m, uint64_t k,
                           const size_t s, size_t* ix) {
    size_t i = s; // start
    const uint64_t mask = m->m;
    while (m->p[i]) {
        if ((mask & *((uint64_t*)m->p[i])) == k) {
            *ix = i;
            return true;
        } else {
            i = (i + 1) % m->n;
            if (i == s) { return false; }
        }
    }
    return false;
}

static inline void map_remove(struct map* m, size_t i) {
    m->p[i] = 0;
    size_t x = i; // next
    for (;;) {
        x = (x + 1) % m->n;
        if (!m->p[x]) { break; }
        const uint64_t b64 = *(uint64_t*)m->p[x];
        size_t h = map_hash(m, b64 & m->m);
        // Check if `h` lies within [i, x), accounting for wrap-around:
        const bool can_move = i <= x ? x < h || h <= i :
                                       x < h && h <= i;
        if (can_move) {
            m->p[i] = m->p[x];
            m->p[x] = 0;
            i = x;
        }
    }
}

static inline bool map_put(struct map* m, const void* p,
                           const size_t h, uint64_t k) {
    const uint64_t mask = m->m;
    size_t i = h;
    while (m->p[i]) {
        if ((mask & *((uint64_t*)m->p[i])) == k) {
            m->p[i] = p;
            return true;
        } else {
            i = (i + 1) % m->n;
            assert(i != h); // in the way map is used should not happen
            if (i == h) { return false; }
        }
    }
    m->p[i] = p;
    return true;
}

static inline int32_t ft_lsb(int32_t i) { // least significant bit only
    return i & (~i + 1); // (i & -i)
}

// TODO: pm_init and ft_init can be collapsed to single function

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

// TODO: it is possible to have different sized trees using generics - don't know it it worth it

static inline void ft_update(uint64_t tree[], size_t n, int32_t i, uint64_t inc) {
    while (i < (int32_t)n) { tree[i] += inc; i += ft_lsb(i + 1); }
}

static inline uint64_t ft_query(const uint64_t tree[], size_t n, int32_t i) {
    uint64_t sum = 0;
    while (i >= 0) {
        if (i < (int32_t)n) { sum += tree[i]; }
        i -= ft_lsb(i + 1);
    }
    return sum;
}

static inline int32_t ft_index_of(uint64_t tree[], size_t n, uint64_t const sum) {
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

static void pm_init(struct prob_model* pm, uint32_t n) {
    for (size_t i = 0; i < countof(pm->freq); i++) {
        pm->freq[i] = i < n ? 1 : 0;
    }
    ft_init(pm->tree, countof(pm->tree), pm->freq);
}

static inline void pm_update(struct prob_model* pm, uint8_t sym, uint64_t inc) {
    // (1uLL << (64 - 8)) maximum frequency
    if (pm->tree[countof(pm->tree) - 1] < (1uLL << (64 - 8))) {
        pm->freq[sym] += inc;
        ft_update(pm->tree, countof(pm->tree), sym, inc);
    }
}

static inline void rc_emit(struct range_coder* rc) {
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
    for (size_t i = 0; i < sizeof(rc->low); i++) {
        rc->range = UINT64_MAX;
        rc_emit(rc);
    }
}

static inline void rc_consume(struct range_coder* rc) {
    const uint8_t byte   = rc->read(rc);
    rc->code    = (rc->code << 8) + byte;
    rc->low   <<= 8;
    rc->range <<= 8;
}

static inline void rc_encode(struct range_coder* rc, struct prob_model* pm,
               uint8_t sym) {
    uint64_t total = pm_total_freq(pm);
    uint64_t start = pm_sum_of(pm, sym);
    uint64_t size  = pm->freq[sym];
    assert(size > 0);
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

static inline uint8_t rc_decode(struct range_coder* rc, struct prob_model* pm) {
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

void sqz_init(struct sqz* s) {
    rc_init(&s->rc, 0);
    pm_init(&s->pm_bit0,    2);
    pm_init(&s->pm_byte,  256);
    pm_init(&s->pm_tag,    16);
    pm_init(&s->pm_dist,  sqz_max_len2_dist + 1);
    pm_init(&s->pm_rep,     4);
    pm_init(&s->pm_lix,    16);
    pm_init(&s->pm_dix,    16);
    pm_init(&s->pm_len,   256);
    pm_init(&s->pm_lsb,   256);
    pm_init(&s->pm_msb,   256);
}

static void sqz_init_compress(struct sqz* s) {
    memset(s->prev,   0, sizeof(s->prev));
    memset(s->map2,   0, sizeof(s->map2));
    memset(s->freq2,  0, sizeof(s->freq2));
    memset(s->freq3,  0, sizeof(s->freq3));
    for (size_t j = 0; j < countof(s->maps); j++) {
        map_init(&s->maps[j], s->map_e[j], countof(s->map_e[j]), j + 3);
    }
}

static const char* esc(const void* a, size_t n) {
    static int ix;
    static char text[16][2 * 1024];
    const char* s = (const char*)a;
    char* d = text[ix];
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\r') {
            d[j++] = '\\';
            d[j++] = 'r';
        } else if (s[i] == '\n') {
            d[j++] = '\\';
            d[j++] = 'n';
        } else if (0x20 <= s[i] && s[i] <= 0x7F) {
            d[j++] = s[i];
        } else {
            d[j++] = '\\';
            d[j++] = 'x';
            d[j++] = "0123456789ABCDEF"[(((uint8_t)s[i]) / 16) & 0xF];
            d[j++] = "0123456789ABCDEF"[((uint8_t)s[i]) % 16];
        }
    }
    d[j] = 0;
    ix = (ix + 1) % countof(text);
    return d;
}

static void sqz_insert(struct sqz* s, const uint8_t* in, const size_t n,
                       const size_t w, const size_t i, uint64_t incoming,
                       uint64_t leaving) {
    assert(sqz_min_window <= w && w <= sqz_max_window);
    assert(((w - 1) & w) == 0); // window is power of 2
    if (i < n) {
        for (size_t j = 3; j <= 8; j++) {
            struct map* m = &s->maps[j - 3];
            if (i >= w) {
                const uint64_t mlb = leaving & m->m; // masked leaving bytes
                size_t ix;
                bool seen = map_get(m, mlb, map_hash(m, mlb), &ix);
                if (seen && (uint8_t*)m->p[ix] <= in + i - w) {
                    map_remove(m, ix);
                }
            }
            if (j < 8) {
                const uint64_t mib = incoming & m->m; // masked incoming bytes
                bool b = map_put(m, in + i, map_hash(m, mib), mib);
                assert(b); (void)b;
            } else { // map8 has linked list of distance to previous
                const size_t index = i & (w - 1);
                const uint64_t i64 = incoming; // 64 bit of incoming
                size_t ix;
                bool seen = map_get(m, i64, map_hash(m, i64), &ix);
                if (!seen) {
                    bool b = map_put(m, in + i, map_hash(m, i64), i64);
                    assert(b); (void)b;
                    s->prev[index] = 0;
                } else {
                    const uint8_t* mp = (const uint8_t*)m->p[ix];
                    // previous position of 8 bytes entry of 'pm' previous match:
                    // overwrite with shorter distance for the same 8 bytes:
                    m->p[ix] = in + i;
                    // the previous map entry is already removed
                    // if it is outside the window
                    assert(mp + w >= in + i); // inside window: pm >= in + i - w
                    size_t pp = mp - in;
                    s->prev[index] = i - pp;
                }
            }
        }
        s->map2[incoming & 0xFFFFu] = i + 1;
    }
}

static uint64_t prev_sum;
static size_t   prev_count;
static size_t   prev_max;
static size_t   prev_last;

static uint64_t sqz_debug_ix = UINT64_MAX; // nothing
// static uint64_t sqz_debug_ix = 0; // everything
// static uint64_t sqz_debug_ix = 54; // specific

#define LZ_COMPARE_LINEAR
#undef  LZ_COMPARE_LINEAR

#ifdef LZ_COMPARE_LINEAR

static void lz_linear(const uint8_t* in, const size_t n,
                      const size_t w, const size_t i,
                      size_t *ml, size_t *md) {
    assert(*ml == 0); // caller's responsibility
    assert(*md == 0);
    if (1 <= i && i < n - 8) {
        size_t len = 0;
        size_t dst = 0;
        size_t j = i - 1;
        size_t min_j = i >= w ? i - w : 0;
        for (;;) {
            size_t max_k = n - i > sqz_max_len ? sqz_max_len : n - i;
            size_t k = 0;
            while (k < max_k && in[j + k] == in[i + k]) { k++; }
            if (k >= 2 && k > len) {
                len = k;
                dst = i - j;
                if (len == sqz_max_len) { break; }
            }
            if (j == min_j) { break; }
            j--;
        }
        *ml = len;
        *md = dst;
    }
}

#endif

// lz_find() function finds longest match at a shortest distance and returns
// ml: [2..max_len] inclusive
// md: [1..window] inclusive

static inline void sqz_find(struct sqz* s, const uint8_t* in, const size_t n,
                            const size_t w, const size_t i, uint64_t incoming,
                            size_t *ml, size_t *md) {
    assert(sqz_min_window <= w && w <= sqz_max_window);
    assert(((w - 1) & w) == 0); // window is power of 2
    assert(*ml == 0); // caller's responsibility
    assert(*md == 0);
    const size_t max_k = n - i > sqz_max_len ? sqz_max_len : n - i;
    size_t len = 0;
    size_t dst = 0;
    const uint64_t i64 = incoming;
    for (size_t j = 8; j >= 3 && len == 0; j--) {
        struct map* m = &s->maps[j - 3];
        size_t ix;
        if (j == 8) {
            if (map_get(m, i64, map_hash(m, i64), &ix)) {
                const uint8_t* mp = (const uint8_t*)m->p[ix];
                size_t p = mp - in;
                if (sqz_debug_ix == 0 || sqz_debug_ix == i) {
                    sqz_trace("[%2zd] found in: \"%s\" map%d: \"%s\"\n", i, esc(in + i, 8), j, esc(mp, 8));
                }
                len = 8;
                dst = i - p;
                // simple average iterations of the following while loop
                // on "silesia.tar" is ~154 upto ~430 on test_long (random)
                size_t prev_chain = 0;
int no_improve = 0;
                for (;;) {
                    if (sqz_debug_ix == 0 || sqz_debug_ix == i) {
                        size_t ln = len + 1;
                        if (ln > 16) { ln = 16; }
                        sqz_trace("[%2zu] p: %2zu \"%.*s\" \"%.*s\" len: %2zu ",
                            i, p, (int)len + 1, in + i, (int)ln, esc(mp, ln), len);
                    }
                    size_t k = 8; // because first 8 bytes are the same
                    while (k < max_k && in[p + k] == in[i + k]) { k++; }
                    if (k > len) {
if (8 < len && len < 24 && no_improve > 500 && incoming != 0) {
//  printf("len: %5zd := %5zd dist: %5zd := %5zd chain: %5d no improve: %5d \"%s\" \"%s\"\n", len, k, dst, i - p,prev_chain, no_improve, esc(in + i, len), esc(in + p, k));
}
                        len = k;
                        dst = i - p;
                        if (sqz_debug_ix == 0 || sqz_debug_ix == i) {
                            sqz_trace("len: %zu, dst: %zu \"%.*s\"\n", len, dst, (int)len, in + i);
                        }
                        if (len == max_k) { break; }
                        no_improve = 0;
                    } else {
                        if (sqz_debug_ix == 0 || sqz_debug_ix == i) {
                            sqz_trace("\n");
                        }
                        no_improve++;
                    }
                    size_t d = s->prev[p & (w - 1)];
                    if (d == 0) { break; }
                    p -= d;
if (prev_chain > 200 && (p + w < i)) {
//  printf("len: %5zd dist: %5zd chain: %3d no improve: %d \"%s\" \"%s\"\n", len, dst, prev_chain, no_improve, esc(in + i, len), esc(in + p + d, len));
}
                    // p < i - w won't work because unsigned i - w for i < w
                    if (p + w < i) { break; } // unsigned version of: p < i - w
                    prev_chain++;
//                  if (prev_chain > 100 && no_improve > 10) { break; }
                }
                if (prev_chain > 0) {
                    prev_sum += prev_chain;
                    prev_count++;
                    prev_last = prev_chain;
                    if (prev_chain > prev_max) {
                        prev_max = prev_chain;
//                      printf("prev_max: %zd len: %zd dist: %zd\n", prev_max, len, dst);
//                      if (prev_max == 301) { rt_breakpoint(); }
                    }
                }
            }
        } else if (len == 0) {
            if (map_get(m, i64 & m->m, map_hash(m, i64 & m->m), &ix)) {
                const uint8_t* mp = (const uint8_t*)m->p[ix];
                const size_t p = mp - in;
                if (sqz_debug_ix == 0 || sqz_debug_ix == i) {
                    sqz_trace("[%2zd] found in: \"%s\" map%d: \"%s\"\n", i, in + i, j, esc(mp, 8));
                }
                if (i - p <= w) {
                    len = j;
                    dst = i - p;
                }
            }
        }

        if (j == 3 && len == 3) {
            s->freq3[incoming & 0xFFFFFFu]++;
        }

    }
    if (len == 0) {
        size_t p = s->map2[incoming & 0xFFFFu];
        if (p > 0) {
            p--; // because map2[] keep position + 1
            if (i - p <= w) {
                len = 2;
                dst = i - p;
                s->freq2[incoming & 0xFFFFu]++;
            }
        }
    }
    if (len > 0) {
        *ml = len;
        *md = dst;
    }
}

#define sqz_update_incoming_leaving(incoming, leaving, in, i, n8, w) do {   \
    incoming >>= 8;                                                         \
    if (i < n8) {                                                           \
        incoming |= (((uint64_t)in[(i) + 7]) << 56);                        \
    }                                                                       \
    if (i > w) {                                                            \
        leaving  = (leaving >> 8) | (((uint64_t)in[(i) - (w) + 7]) << 56);  \
    }                                                                       \
} while (0)

#define sqz_insert_next(s, in, n8, w, i, len, incoming, leaving) do { \
    const size_t next_i = i + ((len) > 0 ? (len) : 1);                \
    while (i < next_i) {                                              \
        sqz_insert(s, in, n8, w, i, incoming, leaving);               \
        i++;                                                          \
        sqz_update_incoming_leaving(incoming, leaving, in, i, n8, w); \
    }                                                                 \
} while (0)

// delta coding: delta >= 7 will code XOR with next byte `after` last match

static const uint8_t sqz_lit[12]   = {0, 0, 0, 0, 1, 2, 3, 4,  5,  6,   4,  5};
static const uint8_t sqz_match[12] = {7, 7, 7, 7, 7, 7, 7, 10, 10, 10, 10, 10};
static const uint8_t sqz_rep[12]   = {8, 8, 8, 8, 8, 8, 8, 11, 11, 11, 11, 11};
static const uint8_t sqz_short[12] = {9, 9, 9, 9, 9, 9, 9, 11, 11, 11, 11, 11};


struct freq_entry {
    size_t index;
    uint64_t freq;
};

static int compare_freq(const void* a, const void* b) {
    const struct freq_entry* fa = (const struct freq_entry*)a;
    const struct freq_entry* fb = (const struct freq_entry*)b;
    if (fa->freq < fb->freq) return 1;
    if (fa->freq > fb->freq) return -1;
    return 0;
}

static void freq2(struct sqz* s, size_t n) {
    static struct freq_entry sorted[countof(s->freq2)];
    size_t sorted_count = 0;
    uint64_t total = 0;
    uint64_t cumulative = 0;
    for (size_t i = 0; i < countof(s->freq2); i++) {
        if (s->freq2[i] > 0) {
            total += s->freq2[i];
            sorted[sorted_count++] = (struct freq_entry){ .index = i, .freq = s->freq2[i] };
        }
    }
    printf("FREQ2: %5.2f%% of %zd non-zero: %zd\n", 100.0 * 2 * total / n, n, sorted_count);
    qsort(sorted, sorted_count, sizeof(struct freq_entry), compare_freq);
    for (size_t i = 0; i < sorted_count && i < 16; i++) {
        cumulative += sorted[i].freq;
        printf("0x%04X, %10lld %5.2f%% %5.2f%%\n",
               sorted[i].index, sorted[i].freq,
               100.0 * 2 * sorted[i].freq / total, 100.0 * 2 * cumulative / n);
    }
}

static void freq3(struct sqz* s, size_t n) {
    static struct freq_entry sorted[countof(s->freq3)];
    size_t sorted_count = 0;
    uint64_t total = 0;
    for (size_t i = 0; i < countof(s->freq3); i++) {
        if (s->freq3[i] > 0) {
            total += s->freq3[i];
            sorted[sorted_count++] = (struct freq_entry){ .index = i, .freq = s->freq3[i] };
        }
    }
    printf("FREQ3 %5.2f%% of %zd non-zero: %zd\n", 100.0 * 3 * total / n, n, sorted_count);
    qsort(sorted, sorted_count, sizeof(struct freq_entry), compare_freq);
    uint64_t cumulative = 0;
    for (size_t i = 0; i < sorted_count && i < 16; i++) {
        cumulative += sorted[i].freq;
        printf("0x%06X, %10lld %5.2f%% %5.2f%%\n",
               sorted[i].index, sorted[i].freq,
               100.0 * 3 * sorted[i].freq / n, 100.0 * 3 * cumulative / n);
    }
}

void sqz_compress(struct sqz* s, const void* memory, size_t n, uint32_t w) {
    static_assert(sizeof(size_t) == 4 || sizeof(size_t) == 8, "32|64 only");
    if (n > (uint64_t)INT32_MAX && sizeof(size_t) == 4) {
        s->rc.error = E2BIG;
        return;
    }
    // TODO: preprocessing delta filter with subtract or maybe a + 128 - b
prev_sum = 0;
prev_max = 0;
prev_last = 0;
prev_count = 0;
sqz_debug = sqz_debug_ix < UINT64_MAX;
    sqz_init_compress(s);
    uint8_t  delta   = 5; // >= 7 use XOR delta predictor
    size_t   after   = 0; // index of first byte after last match
    uint64_t last_dist = 0;
    const uint8_t* in = (const uint8_t*)memory;
    assert(sqz_min_window <= w && w <= sqz_max_window);
    if (n > 0) {
        const uint8_t literal = 1;
        rc_encode(&s->rc, &s->pm_bit0, literal);
        rc_encode(&s->rc, &s->pm_byte, in[0]);
        delta = sqz_lit[delta];
    }
    uint64_t incoming = *(uint64_t*)in;
    uint64_t leaving = incoming;
    sqz_insert(s, in, n, w, 0, incoming, leaving);
    const size_t n8 = n >= 8 ? n - 8 : 0;
    sqz_update_incoming_leaving(incoming, leaving, in, 1, n8, w);
    size_t i = 1;
    while (i < n8) {
        size_t len = 0;
        size_t dst = 0;
//      if (i == 54) { rt_breakpoint(); }
        sqz_find(s, in, n, w, i, incoming, &len, &dst);
        assert(dst == 0 || 1 <= dst && dst <= UINT16_MAX + 1);
        #ifdef LZ_COMPARE_LINEAR
            size_t lin_len = 0;
            size_t lin_dst = 0;
            lz_linear(in, n, w, i, &lin_len, &lin_dst);
            swear(lin_len == len && lin_dst == dst);
        #endif
        int rep = -1;
        if (len > 0) {
            if (dst - 1 == ((last_dist >>  0) & 0xFFFF)) { rep = 0; }
            if (dst - 1 == ((last_dist >> 16) & 0xFFFF)) { rep = 1; }
            if (dst - 1 == ((last_dist >> 32) & 0xFFFF)) { rep = 2; }
            if (dst - 1 == ((last_dist >> 48) & 0xFFFF)) { rep = 3; }
        }
        const uint8_t too_far = len == 2 && rep < 0 && dst > sqz_max_len2_dist;
//      if (rep >= 0 && dst > 0xFF) { printf("rep: %d len: %5zd   dist: %5zd prev_chain: %zd\n", rep, len, dst - 1, prev_last); }
        const uint8_t literal = len == 0 || too_far;
        rc_encode(&((s)->rc), &((s)->pm_bit0), literal);
        if (literal) { // encode literal byte
            len = 0;
            uint8_t c = (uint8_t)incoming;
            if (delta >= 7 && after) { c ^= in[after]; after = 0; }
            rc_encode(&((s)->rc), &((s)->pm_byte), c);
            delta = sqz_lit[delta];
        } else {
            after = i - dst + len;
            dst--; // [0..UINT16_MAX]
            if (len <= 4) {
                rc_encode(&s->rc, &s->pm_tag, ((uint8_t)(len - 1)) << 1 | (rep >= 0));
                if (rep >= 0) {
                    rc_encode(&s->rc, &s->pm_rep, (uint8_t)rep);
                    delta = rep == 0 ? sqz_short[delta] : sqz_rep[delta];
                } else if (len == 2) {
                    assert(dst <= UINT8_MAX);
                    rc_encode(&s->rc, &s->pm_dist, (uint8_t)dst);
                    delta = sqz_match[delta];
                } else {
                    assert(dst <= UINT16_MAX);
                    rc_encode(&s->rc, &s->pm_lsb, (uint8_t)(dst & 0xFF));
                    rc_encode(&s->rc, &s->pm_msb, (uint8_t)(dst >> 8));
                    delta = sqz_match[delta];
                }
            } else {
                rc_encode(&s->rc, &s->pm_tag, rep >= 0);
                rc_encode(&s->rc, &s->pm_len, (uint8_t)len);
                if (rep >= 0) {
                    rc_encode(&s->rc, &s->pm_rep, (uint8_t)rep);
                    delta = rep == 0 ? sqz_short[delta] : sqz_rep[delta];
                } else {
                    assert(dst <= UINT16_MAX);
                    rc_encode(&s->rc, &s->pm_lsb, (uint8_t)(dst & 0xFF));
                    rc_encode(&s->rc, &s->pm_msb, (uint8_t)(dst >> 8));
                    delta = sqz_match[delta];
                }
            }
            last_dist <<= 16;
            last_dist |= (uint16_t)dst;
            if (rep >= 0) {
                // TODO: bring rep to the front of the queue
            }
        }
        sqz_insert_next(s, in, n8, w, i, len, incoming, leaving);
    }
    while (i < n) {
        const uint8_t literal = 1;
        rc_encode(&((s)->rc), &((s)->pm_bit0), literal);
        uint8_t c = (uint8_t)in[i];
        if (delta >= 7 && after) { c ^= in[after]; after = 0; }
        rc_encode(&((s)->rc), &((s)->pm_byte), c);
        i++;
        delta = sqz_lit[delta];
    }
    const uint8_t literal = 0;
    rc_encode(&((s)->rc), &((s)->pm_bit0), literal);
    rc_encode(&s->rc, &s->pm_tag, 0b00);
    rc_encode(&s->rc, &s->pm_len, 0xFF);
    rc_flush(&s->rc);
    freq2(s, n);
    freq3(s, n);
    if (prev_count > 0 && prev_max > 1) {
        printf("prev[] simple avg: %.1f max: %2zu\n",
               (double)prev_sum / prev_count, prev_max);
    }
}

uint64_t sqz_decompress(struct sqz* s, void* data, size_t n) {
    s->rc.code = 0;  // read first 8 bytes
    for (size_t i = 0; i < sizeof(s->rc.code); i++) {
        s->rc.code = (s->rc.code << 8) + s->rc.read(&s->rc);
    }
    uint8_t  delta   = 5; // >= 7 use XOR delta predictor
    size_t   after   = 0; // index of first byte after last match
    uint64_t last_dist = 0;
    uint8_t* d = (uint8_t*)data;
    size_t i = 0;
    while (s->rc.error == 0) {
        const uint8_t literal = rc_decode(&s->rc, &s->pm_bit0);
        if (s->rc.error != 0) { break; }
        if (literal) {
            if (i < n) {
                uint8_t c = rc_decode(&s->rc, &s->pm_byte);
                // TODO: dump XOR results and eyeball them in respect of the input arguments
                if (delta >= 7 && after) { c ^= d[after]; after = 0; }
                d[i++] = c;
                delta = sqz_lit[delta];
            } else {
                s->rc.error = ENOBUFS;
            }
        } else {
            uint32_t dist;
            uint8_t tag = rc_decode(&s->rc, &s->pm_tag);
            uint8_t len = (tag >> 1) + 1;
            if (len == 2) {
                if (tag & 1) {
                    uint8_t rep = rc_decode(&s->rc, &s->pm_rep);
                    dist = (uint32_t)(last_dist >> (rep * 16)) & 0xFFFF;
                    delta = rep == 0 ? sqz_short[delta] : sqz_rep[delta];
                } else {
                    dist = rc_decode(&s->rc, &s->pm_dist);
                    delta = sqz_match[delta];
                }
            } else {
                if (len == 1) {
                    len = rc_decode(&s->rc, &s->pm_len);
                    if (len == 0xFF) { break; }
                }
                if (tag & 1) {
                    uint8_t rep = rc_decode(&s->rc, &s->pm_rep);
                    dist = (uint32_t)(last_dist >> (rep * 16)) & 0xFFFF;
                    delta = rep == 0 ? sqz_short[delta] : sqz_rep[delta];
                } else {
                    dist  = rc_decode(&s->rc, &s->pm_lsb); // See Note 1
                    dist |= (((uint16_t)rc_decode(&s->rc, &s->pm_msb)) << 8);
                    delta = sqz_match[delta];
                }
            }
            assert(sqz_min_len <= len && len <= sqz_max_len);
            last_dist <<= 16;
            last_dist  |= (uint16_t)dist;
            dist++;
            after = i - dist + len;
            if (s->rc.error == 0) {
                const size_t next_i = i + len;
                if (i < dist) {
                    s->rc.error = ERANGE;
                } else if (i >= dist && next_i <= n) {
                    // memcpy() cannot be used on overlapped regions
                    // because it may read more than one byte at a time.
                    uint8_t* p = d - (size_t)dist;
                    while (i < next_i) { d[i] = p[i]; i++; }
                } else {
                    s->rc.error = ENOBUFS;
                }
            }
        }
    }
    return i;
}

// Note 1:
// In C, the arguments to the bitwise OR operator (|) are evaluated
// in an unspecified order, meaning the compiler is free to evaluate
// the left-hand or right-hand operand first. This behavior can lead
// to issues if the two operands have side effects that depend on a
// specific order of evaluation.
//    dist  = rc_decode(&s->rc, &s->pm_lsb)
//         |  (((uint16_t)rc_decode(&s->rc, &s->pm_msb)) << 8);
// may not work and it did not in x86 release.

// Note 2:
// "funny xor thing" in
// https://cbloomrants.blogspot.com/2014/06/06-12-14-some-lzma-notes.html
// https://cbloomrants.blogspot.com/2010/08/08-20-10-deobfuscating-lzma.html
// without XOR:
// 4,436,173   -> 1,343,976    30.30% of "bible.txt"
// pm_byte[ 80]: 4.33 bits  32.70%   9.24%    229,608
// with XOR:
// 4,436,173   -> 1,366,965    30.81% of "bible.txt"
// pm_byte[148]: 6.01 bits  32.70%  12.62%    229,608

// Note 3:
// https://cbloomrants.blogspot.com/2008/10/10-01-08-first-look-at-lzma.html
// https://cbloomrants.blogspot.com/2010/08/08-20-10-deobfuscating-lzma.html
// https://cbloomrants.blogspot.com/2012/10/10-02-12-small-note-on-lzham.html
// https://cbloomrants.blogspot.com/2014/06/06-12-14-some-lzma-notes.html
// https://cbloomrants.blogspot.com/2014/06/06-16-14-rep0-exclusion-in-lzma-like.html
// https://cbloomrants.blogspot.com/2016/06/06-09-16-fundamentals-of-modern-lz-two.html
// https://cbloomrants.blogspot.com/2017/07/09-27-08-2.html
// https://cbloomrants.blogspot.com/2015/01/01-23-15-lza-new-optimal-parse.html

// TODO:
// prev[] can be a balanced tree (will it speed up?)
