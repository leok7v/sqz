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

static void map_init(struct map* m, void** p, size_t n, size_t b) {
    assert(16 < n && n <= (1u << 24) && 3 <= b && b <= 4);
    if ( !(16 < n && n <= (1u << 24) && 3 <= b && b <= 4) ) {
        exit(1); // this is not a generic map implementation
    }
    memset(m, 0, sizeof(*m));
    m->p = p;
    m->n = n;
    m->m = b == 3 ? 0x00FFFFFFu : 0xFFFFFFFFu;
    memset(m->p, 0, n * sizeof(m->p[0]));
}

static inline uint32_t map_hash_key(uint32_t k) {
    k ^= k >> 13; // Bob Jenkins' hash
    k *= 0x85EBCA6Bu;
    k ^= k >> 16;
    return k;
}

static inline size_t map_hash(struct map* m, uint32_t k) {
    return (size_t)(map_hash_key(k) % m->n);
}

static inline bool map_get(struct map* m, uint32_t k,
                           const size_t s, size_t* ix) {
    size_t i = s; // start
    const uint32_t mask = m->m;
    while (m->p[i]) {
        if ((mask & *((uint32_t*)m->p[i])) == k) {
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
        const uint32_t b4 = *(uint32_t*)m->p[x];
        size_t h = map_hash(m, b4 & m->m);
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
                      const size_t h, uint32_t k) {
    const uint32_t mask = m->m;
    size_t i = h;
    while (m->p[i]) {
        if ((mask & *((uint32_t*)m->p[i])) == k) {
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

static inline bool map_put3(struct map* m, const void* p, uint32_t b3) {
    assert((b3 & ~0xFFFFFFu) == 0 && m->m == 0x00FFFFFFu);
    return map_put(m, p, map_hash(m, b3), b3);
}

static inline bool map_put4(struct map* m, const void* p, uint32_t b4) {
    assert(m->m == 0xFFFFFFFFu);
    return map_put(m, p, map_hash(m, b4), b4);
}

static inline bool map_get3(struct map* m, uint32_t b3, size_t* ix) {
    assert((b3 & ~0xFFFFFFu) == 0 && m->m == 0x00FFFFFFu);
    return map_get(m, b3, map_hash(m, b3), ix);
}

static inline bool map_get4(struct map* m, uint32_t b4, size_t* ix) {
    assert(m->m == 0xFFFFFFFFu);
    return map_get(m, b4, map_hash(m, b4), ix);
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
    pm_init(&s->pm_bit0,  2);
    pm_init(&s->pm_byte,  256);
    pm_init(&s->pm_tag, 8);
    pm_init(&s->pm_dist,  sqz_max_len2_dist + 1);
    pm_init(&s->pm_dix,   4);
    pm_init(&s->pm_len,   256);
    pm_init(&s->pm_lsb,   256);
    pm_init(&s->pm_msb,   256);
}

static void sqz_init_compress(struct sqz* s) {
    memset(s->prev, 0, sizeof(s->prev));
    memset(s->map2, 0, sizeof(s->map2));
    map_init(&s->map3, s->map3e, sizeof(s->map3e) / sizeof(s->map3e[0]), 3);
    map_init(&s->map4, s->map4e, sizeof(s->map4e) / sizeof(s->map4e[0]), 4);
}

static void sqz_insert(struct sqz* s, const uint8_t* in, const size_t n,
                       const size_t w, const size_t i, uint32_t incoming,
                       uint32_t leaving) {
    assert(sqz_min_window <= w && w <= sqz_max_window);
    assert(((w - 1) & w) == 0); // window is power of 2
    if (i < n - 4) {
        bool b;
        if (i >= w) {
            const uint32_t w4 = leaving;
            size_t ix;
            b = map_get3(&s->map3, w4 & 0xFFFFFF, &ix);
            if (b && (uint8_t*)s->map3.p[ix] <= in + i - w) {
                map_remove(&s->map3, ix);
            }
            b = map_get4(&s->map4, w4, &ix);
            if (b && (uint8_t*)s->map4.p[ix] <= in + i - w) {
                map_remove(&s->map4, ix);
            }
        }
        const uint32_t b4 = incoming;
        b = map_put3(&s->map3, in + i, b4 & 0x00FFFFFFu);
        assert(b);
        size_t pp = 0; // previous position of 4 bytes entry
        size_t ix;
        bool seen = map_get4(&s->map4, b4, &ix);
        if (!seen) {
            b = map_put4(&s->map4, in + i, b4);
            assert(b);
        } else {
            pp = (const uint8_t*)s->map4.p[ix] - in;
            s->map4.p[ix] = in + i;
        }
        const size_t index = i % w;
        if (!seen) {
            s->prev[index] = 0;
        } else {
            // `i`, `pp` and `w` are unsigned,
            // if `i` may is less than `w` pp <= i - w is incorrect
            if (i <= w || i - w <= pp && pp < i) {
                s->prev[index] = i - pp;
            } else {
                s->prev[index] = 0;
            }
        }
        s->map2[ b4 & 0xFFFFu] = i + 1;
    }
}

// lz_find() function finds longest match at a shortest distance and returns
// ml: [2..max_len] inclusive
// md: [1..window] inclusive

static inline void sqz_find(struct sqz* s, const uint8_t* in, const size_t n,
                            const size_t w, const size_t i,
                            uint32_t incoming,
                            size_t *ml, size_t *md) {
    assert(sqz_min_window <= w && w <= sqz_max_window);
    assert(((w - 1) & w) == 0); // window is power of 2
    assert(*ml == 0); // caller's responsibility
    assert(*md == 0);
    size_t len = 0;
    size_t dst = 0;
    const uint32_t b4 = incoming;
    size_t ix;
    bool b = map_get4(&s->map4, b4, &ix);
    if (b) {
        size_t p = (const uint8_t*)s->map4.p[ix] - in;
        size_t max_k = n - i > sqz_max_len ? sqz_max_len : n - i;
        size_t k = 4; // start with at least 4
        while (k < max_k && in[p + k] == in[i + k]) { k++; }
        if (i - p <= w) { len = k; dst = i - p; }
        size_t index = p & (w - 1); // same as p % w for w = 2^x
        size_t d = s->prev[index];
        size_t prev_chain = 0;
        // 0 < d && d <= p && p - d < i && i <= p - d + w
        // 0 < d && i + d <= p + w
        while (len < sqz_max_len && 0 < d && i + d <= p + w) {
            p -= d;
            if (len == 4 || memcmp(in + p + 4, in + i + 4, len - 4) == 0) {
                k = len; // because with `len` bytes are the same
                while (k < max_k && in[p + k] == in[i + k]) { k++; }
                if (k > len) { len = k; dst = i - p; }
            }
            index = p & (w - 1); // same as p % w for w = 2^x
            d = s->prev[index];
            prev_chain++;
        }
    }
    if (len == 0) {
        b = map_get3(&s->map3, b4 & 0xFFFFFF, &ix);
        if (b) {
            size_t p = (const uint8_t*)s->map3.p[ix] - in;
            if (i - p <= w) { len = 3; dst = i - p; }
        }
    }
    if (len == 0) {
        size_t p = s->map2[b4 & 0xFFFF];
        if (p > 0) {
            p--; // because map2[] keep position + 1
            if (i - p <= w) { len = 2; dst = i - p; }
        }
    }
    if (len > 0) {
        *ml = len;
        *md = dst;
    }
}

#define sqz_update_incoming_leaving(incoming, leaving, in, i, n4, w) do {   \
    incoming >>= 8;                                                         \
    if (i < n4) {                                                           \
        incoming |= (((uint32_t)in[(i) + 3]) << 24);                        \
    }                                                                       \
    if (i > w) {                                                            \
        leaving  = (leaving  >> 8) | (((uint32_t)in[(i) - (w) + 3]) << 24); \
    }                                                                       \
} while (0)

#define sqz_insert_next(s, in, n4, w, i, len, incoming, leaving) do { \
    const size_t next_i = i + ((len) > 0 ? (len) : 1);                \
    while (i < next_i) {                                              \
        sqz_insert(s, in, n4, w, i, incoming, leaving);               \
        i++;                                                          \
        sqz_update_incoming_leaving(incoming, leaving, in, i, n4, w); \
    }                                                                 \
} while (0)

/*
LZMA state machine :
Literal :
  state < 7 :
    normal literal
  state >= 7 :
    delta literal
  state [0-3] -> state = 0
  state [4-9] -> state -= 3 ([1-6])
  else state -= 6 [10-11] -> ([4-5])

Match :

  rep0
   len 1 :
     state ->   < 7 ? 9 : 11
   len > 1 :
     state ->   < 7 ? 8 : 11

  rep12
     state ->   < 7 ? 8 : 11

  normal
     state ->   < 7 ? 7 : 10
*/

enum { sqz_states = 12, initial_state = 5 };

static const uint8_t sqz_literal_next[sqz_states]    = {0, 0, 0, 0, 1, 2, 3, 4,  5,  6,   4,  5};
static const uint8_t sqz_match_next[sqz_states]      = {7, 7, 7, 7, 7, 7, 7, 10, 10, 10, 10, 10};
static const uint8_t sqz_rep_match_next[sqz_states]  = {8, 8, 8, 8, 8, 8, 8, 11, 11, 11, 11, 11};
static const uint8_t sqz_short_rep_next[sqz_states]  = {9, 9, 9, 9, 9, 9, 9, 11, 11, 11, 11, 11};

void sqz_compress(struct sqz* s, const void* memory, size_t n, uint32_t w) {
    static_assert(sizeof(size_t) == 4 || sizeof(size_t) == 8, "32|64 only");
    if (n > (uint64_t)INT32_MAX && sizeof(size_t) == 4) {
        s->rc.error = E2BIG;
        return;
    }
    sqz_init_compress(s);
    uint8_t  match_reps = 0; // match repetition count 0|1|2
    uint8_t  dix_reps   = 0; // short match repetition count 0|1|2
    uint8_t  state = initial_state;
    uint8_t  last_byte = 0;
    uint64_t last_dist[256] = {0};
    size_t after = 0; // index of first byte after last match
    const uint8_t* in = (const uint8_t*)memory;
    assert(sqz_min_window <= w && w <= sqz_max_window);
    if (n > 0) {
        const uint8_t predicted_literal = state < 7; // normal literal
        rc_encode(&s->rc, &s->pm_bit0, 1 ^ predicted_literal);
        rc_encode(&s->rc, &s->pm_byte, in[0]);
        state = sqz_literal_next[state];
        last_byte = in[0];
    }
    uint32_t incoming = *(uint32_t*)in;
    uint32_t leaving = incoming;
    sqz_insert(s, in, n, w, 0, incoming, leaving);
    const size_t n4 = n - 4;
    sqz_update_incoming_leaving(incoming, leaving, in, 1, n4, w);
    size_t i = 1;
    double predictions = 0;
    double total = 0;
    while (i < n4) {
        size_t len = 0;
        size_t dist = 0;
        sqz_find(s, in, n, w, i, incoming, &len, &dist);
        assert(dist == 0 || 1 <= dist && dist <= UINT16_MAX + 1);
        int ix = -1;
        if (len > 0) {
            const uint64_t last = last_dist[len];
            if (dist - 1 == ((last >>  0) & 0xFFFF)) { ix = 0; }
            if (dist - 1 == ((last >> 16) & 0xFFFF)) { ix = 1; }
            if (dist - 1 == ((last >> 32) & 0xFFFF)) { ix = 2; }
            if (dist - 1 == ((last >> 48) & 0xFFFF)) { ix = 3; }
        }
        const uint8_t too_far = len == 2 && ix < 0 && dist > sqz_max_len2_dist;
        const uint8_t is_literal = len == 0 || too_far;
        const uint8_t predicted_literal = state < 7; // normal literal
        predictions += predicted_literal == is_literal;
        total++;
        rc_encode(&((s)->rc), &((s)->pm_bit0), is_literal ^ predicted_literal);
        if (is_literal) { // encode literal byte
            len = 0;
            uint8_t c = (uint8_t)incoming;
            const uint8_t delta_literal = state >= 7;
            if (delta_literal) {
                assert(after);
                if (after) { c ^= in[after]; after = 0; } // XOR predictor
            }
            rc_encode(&((s)->rc), &((s)->pm_byte), c);
            match_reps = 0;
            dix_reps = 0;
            state = sqz_literal_next[state];
        } else {
            after = i - dist + len;
            dist--; // [0..UINT16_MAX]
            if (len <= 4) {
                rc_encode(&s->rc, &s->pm_tag, ((uint8_t)(len - 1)) << 1 | (ix >= 0));
                if (ix >= 0) {
                    rc_encode(&s->rc, &s->pm_dix, (uint8_t)ix);
                    if (dix_reps < 2) { dix_reps++; }
                } else if (len == 2) {
                    assert(dist <= UINT8_MAX);
                    rc_encode(&s->rc, &s->pm_dist, (uint8_t)dist);
                    if (match_reps < 2) { match_reps++; }
                } else {
                    assert(dist <= UINT16_MAX);
                    rc_encode(&s->rc, &s->pm_lsb, (uint8_t)(dist & 0xFF));
                    rc_encode(&s->rc, &s->pm_msb, (uint8_t)(dist >> 8));
                    if (match_reps < 2) { match_reps++; }
                }
            } else {
                rc_encode(&s->rc, &s->pm_tag, ix >= 0);
                rc_encode(&s->rc, &s->pm_len, (uint8_t)len);
                if (ix >= 0) {
                    rc_encode(&s->rc, &s->pm_dix, (uint8_t)ix);
                    if (dix_reps < 2) { dix_reps++; }
                } else {
                    assert(dist <= UINT16_MAX);
                    rc_encode(&s->rc, &s->pm_lsb, (uint8_t)(dist & 0xFF));
                    rc_encode(&s->rc, &s->pm_msb, (uint8_t)(dist >> 8));
                    if (match_reps < 2) { match_reps++; }
                }
            }
            state = dix_reps   > 1 ? sqz_short_rep_next[state] :
                    match_reps > 1 ? sqz_rep_match_next[state] :
                                     sqz_match_next[state];
            last_dist[len] <<= 16;
            last_dist[len]  |= (uint16_t)dist;
        }
        sqz_insert_next(s, in, n4, w, i, len, incoming, leaving);
        last_byte = in[i - 1] & 0xFF;
    }
    while (i < n) {
        const uint8_t predicted_literal = state < 7; // normal literal
        rc_encode(&((s)->rc), &((s)->pm_bit0), 1 ^ predicted_literal);
        uint8_t c = (uint8_t)in[i];
        if (after) { c ^= in[after]; after = 0; } // XOR predictor
        rc_encode(&((s)->rc), &((s)->pm_byte), c);
        i++;
        state = sqz_literal_next[state];
        last_byte = in[i - 1] & 0xFF;
    }
    const uint8_t predicted_literal = state < 7; // normal literal
    rc_encode(&s->rc, &s->pm_bit0, 0 ^ predicted_literal);
    rc_encode(&s->rc, &s->pm_tag, 0b00);
    rc_encode(&s->rc, &s->pm_len, 0xFF);
    rc_flush(&s->rc);
    printf("prediction accuracy: %.1f%%\n", 100.0 * predictions / total);
}

uint64_t sqz_decompress(struct sqz* s, void* data, size_t n) {
    s->rc.code = 0;  // read first 8 bytes
    for (size_t i = 0; i < sizeof(s->rc.code); i++) {
        s->rc.code = (s->rc.code << 8) + s->rc.read(&s->rc);
    }
    uint8_t  state = initial_state;
    uint8_t  match_reps = 0; // match repetition count 0|1|2
    uint8_t  dix_reps   = 0; // short match repetition count 0|1|2
    uint8_t  last_byte = 0;
    uint64_t last_dist[256] = {0};
    size_t after = 0; // index of first byte after last match
    uint8_t* d = (uint8_t*)data;
    size_t i = 0;
    while (s->rc.error == 0) {
        const uint8_t predicted_literal = state < 7; // normal literal
        const uint8_t is_literal = predicted_literal ^
                                   rc_decode(&s->rc, &s->pm_bit0);
        if (s->rc.error != 0) { break; }
        if (is_literal) {
            if (i < n) {
                uint8_t c = rc_decode(&s->rc, &s->pm_byte);
                const uint8_t delta_literal = state >= 7;
                if (delta_literal) {
                    assert(after);
                    if (after) { c ^= d[after]; after = 0; } // XOR predictor
                }
                d[i++] = c;
                dix_reps = 0;
                match_reps = 0;
                state = sqz_literal_next[state];
            } else {
                s->rc.error = ENOBUFS;
            }
        } else {
            uint32_t dist;
            uint8_t tag = rc_decode(&s->rc, &s->pm_tag);
            uint8_t len = (tag >> 1) + 1;
            if (len == 2) {
                if (tag & 1) {
                    uint8_t dix = rc_decode(&s->rc, &s->pm_dix);
                    dist = (uint32_t)(last_dist[len] >> (16 * dix)) & 0xFFFF;
                    if (dix_reps < 2) { dix_reps++; }
                } else {
                    dist = rc_decode(&s->rc, &s->pm_dist);
                    if (match_reps < 2) { match_reps++; }
                }
            } else {
                if (len == 1) {
                    len = rc_decode(&s->rc, &s->pm_len);
                    if (len == 0xFF) { break; }
                }
                if (tag & 1) {
                    uint8_t dix = rc_decode(&s->rc, &s->pm_dix);
                    dist = (uint32_t)(last_dist[len] >> (16 * dix)) & 0xFFFF;
                    if (dix_reps < 2) { dix_reps++; }
                } else {
                    dist  = rc_decode(&s->rc, &s->pm_lsb); // See Note 1
                    dist |= (((uint16_t)rc_decode(&s->rc, &s->pm_msb)) << 8);
                    if (match_reps < 2) { match_reps++; }
                }
            }
            state = dix_reps   > 1 ? sqz_short_rep_next[state] :
                    match_reps > 1 ? sqz_rep_match_next[state] :
                                     sqz_match_next[state];
            assert(sqz_min_len <= len && len <= sqz_max_len);
            last_dist[len] <<= 16;
            last_dist[len]  |= (uint16_t)dist;
            dist++;
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
                after = i - dist;
            }
        }
        last_byte = d[i - 1] & 0xFF;
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
