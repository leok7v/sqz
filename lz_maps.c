#define  UNSTD_NO_RT_IMPLEMENTATION
#include "rt/ustd.h"
#include "rt/fileio.h"

// https://en.wikipedia.org/wiki/Ukkonen%27s_algorithm
// https://en.wikipedia.org/wiki/Two-way_string-matching_algorithm
// https://en.wikipedia.org/wiki/LZ77_and_LZ78

#define LZ_VERBOSE
#undef  LZ_VERBOSE

// in DEBUG lz_linear_search() + verify is incredibly slow
#undef  LZ_ALL_TESTS
#define LZ_ALL_TESTS

#ifdef LZ_VERBOSE
#define trace(...) printf(__VA_ARGS__)
#else
#define trace(...) ((void)0)
#endif

enum { max_window = 1u << 16, min_len = 2, max_len = 254 };

struct map { // single threaded use only
    const void** p; // p[n]
    size_t       n; // number of entries in the map (max_window * 4)
    uint32_t     m; // mask 0xFFFFFF for 3 bytes and 0xFFFFFFFFu for 4 bytes
};

struct lz {
    const uint8_t* in; // pointer to array of bytes[n]
    size_t   n;        // number of bytes in `in` array
    size_t   i;        // current position in `in` array after window
    size_t   w;        // window size
    // TODO: all the fields above is debugging convenience
    //       and can be passed as parameters to all the lz_* functions
    //       possibly assisting compiler to optimize better and use
    //       registers instead of writing values back to memory.
    size_t   prev[max_window]; // previous `i` of 4 bytes entry
    size_t   map2[1u << (sizeof(uint16_t) * 8)]; // `i` + 1 of 2 bytes
    struct map map3;
    struct map map4;
    // entries for the maps:
    void* map3e[max_window * 4];
    void* map4e[max_window * 4];
};

static void map_init(struct map* m, void** p, size_t n, size_t b);
static bool map_get3(struct map* m, uint32_t b3, size_t* ix);
static bool map_get4(struct map* m, uint32_t b4, size_t* ix);
static bool map_put3(struct map* m, const void* p, uint32_t b3);
static bool map_put4(struct map* m, const void* p, uint32_t b4);
static void map_remove(struct map* m, size_t ix);

static void lz_init(struct lz *lz, const uint8_t* in, size_t n, size_t window) {
    assert(n > 0);
    assert(2 <= window && window <= max_window);
    assert(((window - 1) & window) == 0); // window is power of 2
    memset(lz, 0, sizeof(*lz));
    lz->in = in;
    lz->n  = n;
    lz->i  = 0;
    lz->w  = window;
    map_init(&lz->map3, lz->map3e, sizeof(lz->map3e) / sizeof(lz->map3e[0]), 3);
    map_init(&lz->map4, lz->map4e, sizeof(lz->map4e) / sizeof(lz->map4e[0]), 4);
}

static void lz_chain(struct lz *lz, size_t index) {
    const uint8_t* in = lz->in;
    const size_t i = lz->i;
    const size_t w = lz->w;
    printf("[%zd] \"%c%c\" index(i mod %zd)=%zu ", i, in[i], in[i + 1], w, index);
    size_t p = i;
    size_t d = lz->prev[index];
    while (d > 0 && d <= p && p - d < i && i <= p - d + w) {
        p -= d;
        printf(" \"%c%c\" %zu", in[p], in[p + 1], p);
        d = lz->prev[p % w];
    }
    printf("\n");
}

static void lz_verify(struct lz *lz, size_t index) {
    const uint8_t* in = lz->in; (void)in;
    const size_t i = lz->i;
    const size_t w = lz->w;
    size_t p = i;
    size_t d = lz->prev[index];
    while (d > 0 && d <= p && p - d < i && i <= p - d + w) {
        p -= d;
        assert(p != i && memcmp(in + p, in + i, 4) == 0);
        d = lz->prev[p % w];
    }
}

static void lz_insert(struct lz* lz) {
    const uint8_t* in = lz->in;
    const size_t n = lz->n;
    const size_t i = lz->i;
    const size_t w = lz->w;
    if (i < n - 4) {
        bool b;
        if (i >= w) {
            uint32_t w4 = *((uint32_t*)(in + i - w));
            uint32_t w3 = w4 & 0xFFFFFF;
            size_t ix;
            b = map_get3(&lz->map3, w3, &ix);
            // TODO: ??? <= or < ???
            if (b && (uint8_t*)lz->map3.p[ix] <= in + i - w) {
                map_remove(&lz->map3, ix);
            }
            b = map_get4(&lz->map4, w4, &ix);
            if (b && (uint8_t*)lz->map4.p[ix] <= in + i - w) {
                map_remove(&lz->map4, ix);
            }
        }
        uint32_t b4 = *((uint32_t*)(in + i));
        uint32_t b3 = b4 & 0xFFFFFF;
        uint32_t b2 = b3 & 0xFFFF; (void)b2; // TODO: will need later
        b = map_put3(&lz->map3, in + i, b3);
        assert(b);
        size_t pp = 0; // previous position of 4 bytes entry
        size_t ix;
        bool seen = map_get4(&lz->map4, b4, &ix);
        if (!seen) {
            b = map_put4(&lz->map4, in + i, b4);
            assert(b);
        } else {
            pp = (const uint8_t*)lz->map4.p[ix] - in;
            lz->map4.p[ix] = in + i;
        }
        const size_t index = i % lz->w;
        trace("[%zd] map[\"%c%c\":0x%04x(%u)] = %zu prev[%zd] := ",
               i, in[i], in[i + 1], b2, b2, pp, index);
        if (!seen) {
            trace("0\n");
            lz->prev[index] = 0;
        } else {
            assert(pp != i); // cannot be the same!
            // `i`, `pp` and `w` are unsigned,
            // if `i` may is less than `w` pp <= i - w is incorrect
            if (i <= w || i - w <= pp && pp < i) {
                assert(pp != i); // guarantees i - p != 0
                assert(memcmp(in + pp, in + i, 4) == 0);
                trace("%zu\n", i - pp);
                lz->prev[index] = i - pp;
            } else {
                trace("(out of window)\n");
                lz->prev[index] = 0;
            }
        }
        lz->map2[b2] = i + 1;
        trace("[%zd] map2[\"%c%c\":0x%04x(%u)] := %zu\n",
               i, in[i], in[i + 1], b2, b2, index + 1);
        #ifdef LZ_VERBOSE
            lz_chain(lz, index);
        #endif
        #ifdef DEBUG
            lz_verify(lz, index);
        #endif
    }
}

// both lz_find and lz_linear function search longest match
// at a shortest distance from position `i` and return
// ml: [2..max_len] inclusive
// md: [1..window] inclusive

static inline void lz_find(struct lz* lz, size_t *ml, size_t *md) {
    const uint8_t* in = lz->in;
    const size_t i = lz->i;
    const size_t n = lz->n;
    const size_t w = lz->w;
    const uint32_t b4 = *((uint32_t*)(in + i));
    size_t len = 0;
    size_t dst = 0;
    size_t p;
    size_t ix;
//  if (i == 31 && w == 8) { rt_breakpoint(); }

    // TODO: search for longer sequence (RLE) can be done
    //       onle after map4/map3/map2?

    bool b = map_get4(&lz->map4, b4, &ix);
    if (b) {
        p = (const uint8_t*)lz->map4.p[ix] - in;
assert(memcmp(lz->map4.p[ix], &b4, 4) == 0);
assert(memcmp(lz->map4.p[ix], in + i, 4) == 0);
assert(memcmp(in + p, in + i, 4) == 0);
        size_t max_k = n - i > max_len ? max_len : n - i;
        size_t k = 4; // start with at least 4
        while (k < max_k && in[p + k] == in[i + k]) { k++; }
        if (i - p <= w) { len = k; dst = i - p; }
        size_t index = p & (w - 1); // same as p % w for w = 2^x
        size_t d = lz->prev[index];
        while (len < max_len && 0 < d && d <= p && p - d < i && i <= p - d + w) {
            p -= d;
assert(memcmp(in + p, in + i, 4) == 0);
            if (len == 4 || memcmp(in + p + 4, in + i + 4, len - 4) == 0) {
                k = len; // because with `len` bytes are the same
                while (k < max_k && in[p + k] == in[i + k]) { k++; }
                if (k > len) { len = k; dst = i - p; }
            }
            index = p & (w - 1); // same as p % w for w = 2^x
            d = lz->prev[index];
        }
    }
    // consider:
    // "a_aa_aa_aa" window = 2
    //  0123456789
    //  at in[i:3] len: 7 dst: 3
    // when map4[] and map3[] do not hold anything yet (w:2)
    if (len == 0) {
        b = map_get3(&lz->map3, b4 & 0xFFFFFF, &ix);
        if (b) {
            p = (const uint8_t*)lz->map3.p[ix] - in;
assert(memcmp(lz->map3.p[ix], &b4, 3) == 0);
assert(memcmp(lz->map3.p[ix], in + i, 3) == 0);
assert(memcmp(in + p, in + i, 3) == 0);
            // the map4[] may now be found because it does not
            // take into account RLE overlapped sequences
            size_t max_k = n - i > max_len ? max_len : n - i;
            size_t k = 3; // start with at least 3
            while (k < max_k && in[p + k] == in[i + k]) { k++; }
            if (i - p <= w) { len = k; dst = i - p; }
        }
    }
    if (len == 0) {
        p = lz->map2[b4 & 0xFFFF];
        if (p > 0) {
            p--; // because map2[] keep position + 1
assert(memcmp(in + p, in + i, 2) == 0);
            // the map3[] and map4[] may now be found because
            // take into account RLE overlapped sequences
            size_t max_k = n - i > max_len ? max_len : n - i;
            size_t k = 2; // start with at least 2
            while (k < max_k && in[p + k] == in[i + k]) { k++; }
            if (i - p <= w) { len = k; dst = i - p; }
        }
    }
    if (len > 0) {
        *ml = len;
        *md = dst;
    }
}

static void lz_linear(struct lz* lz, size_t *ml, size_t *md) {
    assert(*ml == 0);
    assert(*md == 0);
    const size_t n = lz->n;
    const size_t i = lz->i;
    const size_t w = lz->w;
    if (1 <= i && i < n - 4) {
        size_t len = 0;
        size_t dst = 0;
        const uint8_t* in = lz->in;
        size_t j = i - 1;
        size_t min_j = i >= w ? i - w : 0;
        for (;;) {
            size_t max_k = n - i > max_len ? max_len : n - i;
            size_t k = 0;
            while (k < max_k && in[j + k] == in[i + k]) { k++; }
            if (k >= 2 && k > len) {
                len = k;
                dst = i - j;
                if (len == max_len) { break; }
            }
            if (j == min_j) { break; }
            j--;
        }
        *ml = len;
        *md = dst;
    }
}

// maps

static const size_t map_deleted_ = 0xC01DF00Du;
static const void* map_deleted = &map_deleted_;

static size_t map_prime_under(size_t n) {
    static const size_t table_of_primes[] = {
        3, 7, 13, 31, 61, 127, 251, 509, 1021, 2039, 4093, 8191, 16381,
        32749, 65521, 131071, 262139, 524287, 1048573, 2097143, 4194301,
        8388593, 16777213
    };
    assert(4 <= n && ((n - 1) & n) == 0 && n <= (1u << 24));
    uint8_t bit = 0;
    size_t bits = 1 << 2;
    while (bits < n) { bits <<= 1; bit++; }
    return table_of_primes[bit];
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

static void map_init(struct map* m, uint8_t** p, size_t n, size_t b) {
    if (!(16 < n && n <= (1u << 24) && 3 <= b && b <= 4)) { exit(1); }
    memset(m, 0, sizeof(*m));
    m->p = p;
    m->n = map_prime_under(n);
    m->m = b == 3 ? 0x00FFFFFFu : 0xFFFFFFFFu;
    memset(m->p, 0, n * sizeof(m->p[0]));
}

static inline void map_reduce_chain(struct map* m, size_t i) {
    const size_t n = m->n;
    size_t e = i + 1; // end of deleted sequence
    while (e < i + n && m->p[e % n] == map_deleted) { e++; }
    if (!m->p[e % n]) {
        for (size_t j = i; j < e; j++) { m->p[j % n] = (void*)0; }
    }
}

static inline bool map_get_hashed(struct map* m, uint32_t k, size_t s,
                                  size_t* ix) {
    size_t i = s; // starting point
    const uint32_t mask = m->m;
    bool reduced = false;
    while (m->p[i]) {
        const bool deleted = m->p[i] == map_deleted;
        const bool not_deleted_and_equal = !deleted &&
                                           (mask & *((uint32_t*)m->p[i])) == k;
        if (not_deleted_and_equal) {
            *ix = i;
            return true;
        } else {
            if (deleted && !reduced) {
                map_reduce_chain(m, i);
                reduced = true;
            }
            i = (i + 1) % m->n;
            assert(i != s);
        }
    }
    return false;
}

static inline bool map_get3(struct map* m, uint32_t b3, size_t* ix) {
    assert((b3 & ~0xFFFFFFu) == 0 && m->m == 0x00FFFFFFu);
    return map_get_hashed(m, b3, map_hash(m, b3), ix);
}

static inline bool map_get4(struct map* m, uint32_t b4, size_t* ix) {
    assert(m->m == 0xFFFFFFFFu);
    return map_get_hashed(m, b4, map_hash(m, b4), ix);
}

static inline void map_remove(struct map* m, size_t ix) {
    m->p[ix] = m->p[(ix + 1) % m->n] ? map_deleted : (void*)0;
    if (m->p[ix]) { map_reduce_chain(m, ix); }
}

static inline bool map_put(struct map* m, const void* p, const size_t h,
                           uint32_t k) {
    const uint32_t mask = m->m;
    size_t d = (size_t)-1;
    size_t i = h;
    while (m->p[i]) {
        const bool deleted = m->p[i] == map_deleted;
        const bool not_deleted_and_equal = !deleted &&
                                           (mask & *((uint32_t*)m->p[i])) == k;
        if (not_deleted_and_equal) {
            m->p[i] = p;
            return true;
        } else {
            if (deleted && d == (size_t)-1) { d = i; }
            i = (i + 1) % m->n;
            if (i == h) {
                if (d == (size_t)-1) { return false; }
                break;
            }
        }
    }
    if (d != (size_t)-1) {
        map_reduce_chain(m, d);
        m->p[d] = p;
    } else {
        m->p[i] = p;
    }
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


// tests:

static uint64_t seed = 1; // random generator seed/state

static int test(struct lz* lz, const uint8_t* in, const size_t n,
                const size_t w, bool verbose) {
    assert(w <= max_window);
    lz_init(lz, in, n, w);
    #ifdef DEBUG
    if (verbose) {
        printf("\"%.*s\": %zu\n ", (int)n, in, n);
        for (size_t i = 0; i < n; i++) {
            printf("%d", i % 10);
        }
        printf("\n ");
        for (size_t i = 0; i < n; i++) {
            printf("%c", i % 10 == 0 ? '0' + (i / 10) % 10 : 0x20);
        }
        printf("\n");
    }
    #else
    (void)verbose;
    #endif
    lz->i = 0;
    lz_insert(lz);
    while (lz->i < lz->n) {
        if (1 <= lz->i && lz->i < lz->n - 4) {
            size_t ml1 = 0, md1 = 0;
            lz_find(lz, &ml1, &md1);
            #ifdef DEBUG
                // TODO: this is incredibly slow
                //       need an option (compile or runtime)
                //       to turn it on/off
                size_t ml2 = 0, md2 = 0;
                lz_linear(lz, &ml2, &md2);
                if (ml1 > 0 || ml2 > 0) {
                    if (ml1 != ml2 || md1 != md2) {
                        printf("[%zd] longest len:dst %3zd:%zd \"%.2s\" \n",
                               lz->i, ml1, md1, lz->in + lz->i);
                        printf("[%zd] linear  len:dst %3zd:%zd \"%.2s\" \n",
                               lz->i, ml2, md2, lz->in + lz->i);
                    }
                    assert(ml1 == ml2 && md1 == md2);
                    if (ml1 != ml2 || md1 != md2) { return 1; }
                    size_t next_i = lz->i + ml1;
                    while (lz->i < next_i) { lz_insert(lz); lz->i++; }
                } else {
                    lz_insert(lz); lz->i++;
                }
            #else
                if (ml1 > 0) {
                    size_t next_i = lz->i + ml1;
                    while (lz->i < next_i) {
                        lz_insert(lz);
                        lz->i++;
                    }
                } else {
                    lz_insert(lz);
                    lz->i++;
                }
            #endif
        } else {
            lz->i++;
        }
    }
    return 0;
}

static int test0(struct lz* lz) {
    const char* in = "_abc;abd:abe_";
    const size_t n = strlen(in);
    const size_t w = 8;
    printf("n: %6zd window: %5zd\n", n, w);
    return test(lz, (const uint8_t*)in, n, w, true);
}

static int test1(struct lz* lz) {
    const char* in = "aaaaa";
    const size_t n = strlen(in);
    const size_t w = 2;
    printf("n: %6zd window: %5zd\n", n, w);
    return test(lz, (const uint8_t*)in, n, w, true);
}

static int test2(struct lz* lz) {
    const char* in = "a_aa_aa_aaa_aaaa_aaaaa_aaaa_aaa_aa_a";
    const size_t n = strlen(in);
    size_t max_w = n < max_window ? n : max_window;
    for (size_t w = 2; w < max_w; w += w) {
        printf("n: %6zd window: %5zd\n", n, w);
        if (test(lz, (const uint8_t*)in, n, w, true)) { return 1; }
    }
    return 0;
}

static int test3(struct lz* lz) {
//  enum { N = 16, M = 256 * 1024, T = 1 }; // experimental
    enum { N = 16, M = 256, T = 32 };
    int pl[N] = { 0 };
    for (int t = 0; t < T; t++) {
        for (int i = 0; i < 16; i++) {
            pl[i] = 2 + (int)(rand64(&seed) * 2);
        }
        uint8_t in[M] = {0};
        int k = 0;
        int j = 0;
        while (k < M - 2) {
            for (int i = 0; i < pl[j] && k < M - 2; i++) {
                in[k++] = 'a';
            }
            j = (j + 1) % N;
            if (k < M - 2) { in[k++] = '_'; }
        }
        assert(in[sizeof(in) - 1] == 0);
        const size_t n = strlen((char*)in);
        size_t max_w = n < max_window ? n : max_window;
        for (size_t w = 2; w < max_w; w += w) {
            trace("n: %6zd window: %5zd\n", n, w);
            if (test(lz, (const uint8_t*)in, n, w, false)) { return 1; }
        }
    }
    return 0;
}

static int test4(struct lz* lz) {
    #if !defined(DEBUG) || defined(LZ_ALL_TESTS)
    #ifdef DEBUG
        enum { n = (max_window + 1) / 2, w = max_window / 8 };
    #else
        enum { n = 32 * 1024 * 1024, w = max_window };
    #endif
    static uint8_t in[n];
    memset(in, 0x00, n);
    printf("n: %6d window: %5d\n", n, w);
    uint64_t t = nanoseconds();
    int r = test(lz, in, n, w, false);
    t = nanoseconds() - t;
    printf("Time: %6.3fs Throughput: %7.3f MiB/s\n",
            t / 1.0e9, n / (t / 1.0e9) / (1024 * 1024));
    // 32MB on Mac Book Air 2024 M3
    // RELEASE Time:  0.102s Throughput: 312.227 MiB/s
    return r;
    #else
    return lz == 0;
    #endif
}

static int test5(struct lz* lz) {
    #if !defined(DEBUG) || defined(LZ_ALL_TESTS)
    #ifdef DEBUG
        enum { n = 128 * 1024};
    #else
        enum { n = 16 * 1024 * 1024};
    #endif
    static uint8_t in[n];
    for (int i = 0; i < n; i++) {
        in[i] = (uint8_t)(256 * rand64(&seed));
    }
    printf("n: %6d window: %5d\n", n, max_window);
    uint64_t t = nanoseconds();
    int r = test(lz, in, n, max_window, false);
    t = nanoseconds() - t;
    printf("Time: %6.3fs Throughput: %7.3f MiB/s\n",
            t / 1.0e9, n / (t / 1.0e9) / (1024 * 1024));
    // 32MB on Mac Book Air 2024 M3
    // RELEASE Time:  0.608s Throughput:  52.612 MiB/s
    return r;
    #else
    return lz == 0;
    #endif
}

static int test6(struct lz* lz) {
    #if !defined(DEBUG) || defined(LZ_ALL_TESTS)
    #ifdef DEBUG
        enum { n = 128 * 1024};
    #else
        enum { n = 32 * 1024 * 1024 };
    #endif
    static uint8_t in[n];
    for (int i = 0; i < n; i++) {
        in[i] = (uint8_t)(256 * rand64(&seed));
    }
    for (int i = 0; i < n / 256; i++) {
        int len = 2 + (int)(rand64(&seed) * 256);
        int pos = (int)(rand64(&seed) * (n - len));
        uint8_t b = (uint8_t)(rand64(&seed) * 256);
        for (int j = pos; j < pos + len; j++) {
            assert(j < n);
            in[j] = b;
        }
    }
    printf("n: %6d window: %5d\n", n, max_window);
    uint64_t t = nanoseconds();
    int r = test(lz, in, n, max_window, false);
    t = nanoseconds() - t;
    printf("Time: %6.3fs Throughput: %7.3f MiB/s\n",
            t / 1.0e9, n / (t / 1.0e9) / (1024 * 1024));
    // 64MB on Mac Book Air 2024 M3
    // RELEASE Time:  1.159s Throughput:  55.222 MiB/s
    return r;
    #else
    return lz == 0;
    #endif
}

static int test_file(struct lz* lz, const char* fn) {
    uint8_t* in = null;
    size_t n = 0;
    errno_t r = file_read_fully(fn, &in, &n);
    if (r != 0) { return r; }
    printf("\"%s\" %zd bytes\n", fn, n);
    uint64_t t = nanoseconds();
    r = test(lz, in, n, max_window, false);
    t = nanoseconds() - t;
    printf("Time: %6.3fs Throughput: %7.3f MiB/s\n",
            t / 1.0e9, n / (t / 1.0e9) / (1024 * 1024));
    free(in);
    return r;
}

static errno_t locate_test_folder(void) {
    for (;;) {
        if (file_exist("test/bible.txt")) { return 0; }
        if (file_chdir("..") != 0) { return errno; }
    }
}

int main(int argc, const char* argv[]) {
    (void)argc; (void)argv; // unused
    errno_t r = locate_test_folder();
    if (r != 0) { return r; }
    static struct lz lz77;
    struct lz* lz = &lz77;
    return test0(lz) || test1(lz) || test2(lz) || test3(lz) ||
           test4(lz) || test5(lz) || test6(lz) ||
           test_file(lz, "test/bible.txt") ||
           test_file(lz, "test/mandrill.png");
}
