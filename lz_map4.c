#include "rt/ustd.h"
#include "rt/fileio.h"

// https://en.wikipedia.org/wiki/Ukkonen%27s_algorithm
// https://en.wikipedia.org/wiki/Two-way_string-matching_algorithm
// https://en.wikipedia.org/wiki/LZ77_and_LZ78

#undef  LZ_TEST_STATS // to measure throughput
#define LZ_TEST_STATS

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

enum { min_window = 4, max_window = 1u << 16, min_len = 2, max_len = 254 };

struct map {
    const void** p; // p[n]
    size_t       n; // number of entries in the map (max_window * 4)
    uint32_t     m; // mask 0xFFFFFF for 3 bytes and 0xFFFFFFFFu for 4 bytes
};

struct lz {
    size_t   prev[max_window]; // previous `i` of 4 bytes entry
    size_t   map2[1u << (sizeof(uint16_t) * 8)]; // `i` + 1 of 2 bytes
    struct map map3;
    struct map map4;
    // entries for the maps (75% occupancy):
    void* map3e[max_window + max_window / 2];
    void* map4e[max_window + max_window / 2];
    uint64_t prev_count; // number of times `prev` was used
    size_t prev_max;     // maximum length of prev[] chain
};

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


static void lz_init(struct lz *lz) {
    memset(lz->prev, 0, sizeof(lz->prev));
    memset(lz->map2, 0, sizeof(lz->map2));
    map_init(&lz->map3, lz->map3e, sizeof(lz->map3e) / sizeof(lz->map3e[0]), 3);
    map_init(&lz->map4, lz->map4e, sizeof(lz->map4e) / sizeof(lz->map4e[0]), 4);
    lz->prev_count = 0;
    lz->prev_max   = 0;
}

static void lz_insert(struct lz* lz, const uint8_t* in, const size_t n,
                      const size_t w, const size_t i, uint32_t incoming,
                      uint32_t leaving) {
    assert(min_window <= w && w <= max_window);
    assert(((w - 1) & w) == 0); // window is power of 2
    if (i < n - 4) {
        bool b;
        if (i >= w) {
            #ifdef LZ_CHECK_INCOMING_AND_LEAVING
            const uint32_t w4 = *((uint32_t*)(in + i - w));
            swear(w4 == leaving);
            #else
            const uint32_t w4 = leaving;
            #endif
            size_t ix;
            b = map_get3(&lz->map3, w4 & 0xFFFFFF, &ix);
            if (b && (uint8_t*)lz->map3.p[ix] <= in + i - w) {
                map_remove(&lz->map3, ix);
            }
            b = map_get4(&lz->map4, w4, &ix);
            if (b && (uint8_t*)lz->map4.p[ix] <= in + i - w) {
                map_remove(&lz->map4, ix);
            }
        }
        #ifdef LZ_CHECK_INCOMING_AND_LEAVING
        const uint32_t b4 = *((uint32_t*)(in + i));
        swear(b4 == incoming);
        #else
        const uint32_t b4 = incoming;
        #endif
        b = map_put3(&lz->map3, in + i, b4 & 0x00FFFFFFu);
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
        const size_t index = i % w;
        if (!seen) {
            lz->prev[index] = 0;
        } else {
            // `i`, `pp` and `w` are unsigned,
            // if `i` may is less than `w` pp <= i - w is incorrect
            if (i <= w || i - w <= pp && pp < i) {
                lz->prev[index] = i - pp;
            } else {
                lz->prev[index] = 0;
            }
        }
        lz->map2[ b4 & 0xFFFFu] = i + 1;
    }
}

// both lz_find and lz_linear function search longest match
// at a shortest distance from position `i` and return
// ml: [2..max_len] inclusive
// md: [1..window] inclusive

static inline void lz_find(struct lz* lz, const uint8_t* in, const size_t n,
                           const size_t w, const size_t i,
                           uint32_t incoming,
                           size_t *ml, size_t *md) {
    assert(min_window <= w && w <= max_window);
    assert(((w - 1) & w) == 0); // window is power of 2
    assert(*ml == 0); // caller's responsibility
    assert(*md == 0);
    size_t len = 0;
    size_t dst = 0;
    #ifdef LZ_CHECK_INCOMING_AND_LEAVING
    const uint32_t b4 = *((uint32_t*)(in + i));
    swear(b4 == incoming);
    #else
    const uint32_t b4 = incoming;
    #endif
    size_t p;
    size_t ix;
    bool b = map_get4(&lz->map4, b4, &ix);
    if (b) {
        p = (const uint8_t*)lz->map4.p[ix] - in;
        size_t max_k = n - i > max_len ? max_len : n - i;
        size_t k = 4; // start with at least 4
        while (k < max_k && in[p + k] == in[i + k]) { k++; }
        if (i - p <= w) { len = k; dst = i - p; }
        size_t index = p & (w - 1); // same as p % w for w = 2^x
        size_t d = lz->prev[index];
        size_t prev_chain = 0;
        while (len < max_len && 0 < d && d <= p && p - d < i && i <= p - d + w) {
            p -= d;
            if (len == 4 || memcmp(in + p + 4, in + i + 4, len - 4) == 0) {
                k = len; // because with `len` bytes are the same
                while (k < max_k && in[p + k] == in[i + k]) { k++; }
                if (k > len) { len = k; dst = i - p; }
            }
            index = p & (w - 1); // same as p % w for w = 2^x
            d = lz->prev[index];
            prev_chain++;
        }
        lz->prev_count += prev_chain;
        lz->prev_max = max(lz->prev_max, prev_chain);
    }
    if (len == 0) {
        b = map_get3(&lz->map3, b4 & 0xFFFFFF, &ix);
        if (b) {
            p = (const uint8_t*)lz->map3.p[ix] - in;
            if (i - p <= w) { len = 3; dst = i - p; }
        }
    }
    if (len == 0) {
        p = lz->map2[b4 & 0xFFFF];
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

static void lz_linear(const uint8_t* in, const size_t n,
                      const size_t w, const size_t i,
                      size_t *ml, size_t *md) {
    assert(*ml == 0); // caller's responsibility
    assert(*md == 0);
    if (1 <= i && i < n - 4) {
        size_t len = 0;
        size_t dst = 0;
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

// tests:

static bool is_ascii(const uint8_t* s, const size_t n) {
    bool ascii = true;
    for (size_t i = 0; i < n && ascii; i++) {
        ascii = 0x20 <= s[i] && s[i] <= 0x7F;
    }
    return ascii;
}

static void print_ascii_or_hex(const uint8_t* s, const size_t n) {
    if (is_ascii(s, n)) {
        size_t mn = min(n, 32);
        if (mn == n) {
            printf("\"%.*s\"", (int)mn, s);
        } else {
            printf("\"%.*s...\"", (int)mn, s);
        }
    } else {
        printf("0x");
        size_t mn = min(n, 16);
        for (size_t i = 0; i < mn; i++) { printf("%02X", s[i]); }
        if (mn != n) { printf("..."); }
    }
    printf("\n");
}

static inline void debug_dump_input(const uint8_t* in, size_t n, bool verbose) {
    #ifdef DEBUG
    if (verbose && is_ascii(in, n)) {
        printf("\"%.*s\": %zu\n ", (int)n, in, n);
        for (size_t i = 0; i < n; i++) { printf("%d", i % 10); }
        printf("\n ");
        for (size_t i = 0; i < n; i++) {
            printf("%c", i % 10 == 0 ? '0' + (i / 10) % 10 : 0x20);
        }
        printf("\n");
    }
    #else
    (void)in; (void)n; (void)verbose;
    #endif
}

static inline int debug_check_match(const uint8_t* in, size_t n, size_t w,
                                    size_t i, size_t ml1, size_t md1) {
    #ifdef DEBUG
    size_t ml2 = 0;
    size_t md2 = 0;
    lz_linear(in, n, w, i, &ml2, &md2);
    if (ml1 != ml2 || md1 != md2) {
        printf("[%zd] longest len:dst %3zd:%-5zd ", i, ml1, md1);
        print_ascii_or_hex(in + i, ml1);
        printf("\n");
        printf("[%zd] linear  len:dst %3zd:%-5zd ", i, ml2, md2);
        print_ascii_or_hex(in + i, ml2);
        printf("\n");
        assert(ml1 == ml2 && md1 == md2);
    }
    return ml1 != ml2 || md1 != md2;
    #else
    (void)in; (void)n; (void)w; (void)i; (void)ml1; (void)md1;
    return 0;
    #endif
}

enum { test_stats_count = 9 };

static struct {
    uint64_t bytes;                 // total number of bytes in in[n]
    uint64_t matches;               // total number of matches (including no match)
    uint64_t seq[test_stats_count]; // number of match of this length
    uint64_t sum[test_stats_count]; // sum of bytes per sequence
    // running averages:
    double   avg_byte;                  // of bits for a single byte
    double   avg_len[test_stats_count]; // of bits per sequence length
    double   avg_dst[test_stats_count]; // of bits per sequence distance
    double   avg_l2d;                   // of bits per 2 bytes sequence
} ts; // test stats

static inline uint8_t bits_of(uint64_t i) {
    uint8_t bits = 0;
    while (i > 0) { i >>= 1; bits++; }
    return bits;  // 0 bits for i == 0
}

static void running_average(double *ra, uint64_t v) {
    *ra = (*ra * (ts.matches - 1) + (double)v) / ts.matches;
}

static inline void debug_test_stat(uint8_t b, size_t ml, size_t md) {
    // `ml` match length >= 2 or zero
    // `md` match distance >= 1
    #if defined(DEBUG) || defined(LZ_TEST_STATS)
    ts.matches++;
    assert(ml != 1);
    assert(md >= 1 || md == 0);
    uint8_t bl = bits_of(ml);
    uint8_t bm = bits_of(md);
    if (ml == 0) { ml++; }
    size_t ix = min(test_stats_count - 1, ml);
    ts.seq[ix]++;
    ts.sum[ix] += ml;
    if (ix == 1) {
        uint8_t bb = bits_of(b); // bits in directly encoded byte
        running_average(&ts.avg_byte, bb);
    }
    if (md > 0) {
        running_average(&ts.avg_len[ix], bl);
        running_average(&ts.avg_dst[ix], bm);
        // Special case 2 bytes. Only take into account distances <= 0xFF
        if (ml == 2 && md <= 0xFF) {
            running_average(&ts.avg_l2d, bm);
            // seq[0] used for special case of 2 bytes 1 byte distance
            ts.seq[0]++;
            ts.sum[0] += ml;
        }
    }
    #else
    (void)ml;
    #endif
}

static inline void matches(void) {
    #if defined(DEBUG) || defined(LZ_TEST_STATS)
    enum { n = test_stats_count };
    // sanity check
    uint64_t tm = 0; // total number of matches
    for (size_t i = 1; i < n; i++) { tm += ts.seq[i]; }
    assert(tm == ts.matches);
    uint64_t tb = 0; // total number of bytes
    for (size_t i = 1; i < n; i++) { tb += ts.sum[i]; }
    assert(ts.bytes - tb <= 5); // because we can have match at the end
    // Stats per match
    printf("per match: %7lld ", ts.matches);
    const double m = (double)ts.matches;
    double p = 100.0 * (double)ts.seq[1] / m;
    if (p >= 0.1) {
        // interested in %% of "as is" bytes in respect of matches
        // as well as %% in respect of total input bytes:
        printf("\"as is\" %4.1f%% ", p);
    }
    for (size_t i = 2; i < n - 1; i++) {
        p = 100.0 * (double)ts.seq[i] / m;
        if (p >= 0.1) { printf("%zd: %4.1f%% ", i, p); }
    }
    p = 100.0 * (double)ts.seq[n - 1] / m;
    if (p >= 0.1) { printf("%d+: %4.1f%% ", n - 1, p); }
    p = 100.0 * (double)ts.seq[0] / m;
    printf("l2:d1: %4.1f%%\n", p);
    // Stats per input byte
    printf("per byte:  %7lld ", ts.bytes);
    const double b = (double)ts.bytes;
    p = 100.0 * (double)ts.sum[1] / b;
    if (p >= 0.01) {
        printf("\"as is\" %4.1f%% ", p);
    }
    for (size_t i = 2; i < n - 1; i++) {
        p = 100.0 * (double)ts.sum[i] / b;
        if (p >= 0.1) { printf("%zd: %4.1f%% ", i, p); }
    }
    p = 100.0 * (double)ts.sum[n - 1] / b;
    if (p >= 0.1) { printf("%d+: %4.1f%% ", n - 1, p); }
    p = 100.0 * (double)ts.sum[0] / b;
    printf("l2:d1: %4.1f%%\n", p);
    // Averages per match:
    printf("bits [len:dst] ");
    printf("\"as is\"     %4.1f  ", ts.avg_byte);
    for (size_t i = 2; i < n; i++) {
        if (ts.avg_len[i] + ts.avg_dst[i] > 0.01) {
            printf("%zd: [%.1f:%.1f] ", i, ts.avg_len[i], ts.avg_dst[i]);
        }
    }
    printf("l2:d1: %.1f\n", ts.avg_l2d); // clipped distance
    #endif
    printf("\n");
}

#define update_incoming_leaving(incoming, leaving, in, i, w) do {           \
    incoming = (incoming >> 8) | (((uint32_t)in[(i) + 3]) << 24);           \
    if (i > w) {                                                            \
        leaving  = (leaving  >> 8) | (((uint32_t)in[(i) - (w) + 3]) << 24); \
    }                                                                       \
} while (0)

#define insert_next(ml1, update) do {                           \
    size_t next_i = i + ((ml1) > 0 ? (ml1) : 1);                \
    while (i < next_i) {                                        \
        lz_insert(lz, in, n, w, i, incoming, leaving);          \
        i++;                                                    \
        update_incoming_leaving(incoming, leaving, in, i, w);   \
    }                                                           \
} while (0)

static int test(struct lz* lz, const uint8_t* in, const size_t n,
                const size_t w, bool verbose) {
    assert(n >= 4);
    assert(min_window <= w && w <= max_window);
    lz_init(lz);
    memset(&ts, 0, sizeof(ts));
    ts.bytes = n;
    debug_dump_input(in, n, verbose);
    uint32_t incoming = *(uint32_t*)in;
    uint32_t leaving = incoming;
    lz_insert(lz, in, n, w, 0, incoming, leaving);
    size_t i = 1;
    update_incoming_leaving(incoming, leaving, in, 1, w);
    const size_t n4 = n - 4;
    while (i < n4) {
        size_t ml1 = 0, md1 = 0;
        lz_find(lz, in, n, w, i, incoming, &ml1, &md1);
        debug_test_stat((uint8_t)(incoming & 0xFF), ml1, md1);
        if (debug_check_match(in, n, w, i, ml1, md1)) { return 1; }
//      printf("[%2zd] incoming: %.4s 0x%08X leaving: %.4s 0x%08X\n",
//             i, &incoming, incoming, &leaving, leaving);
        insert_next(ml1, update_incoming_leaving);
//      printf("[%2zd] incoming: %.4s 0x%08X leaving: %.4s 0x%08X\n",
//             i, &incoming, incoming, &leaving, leaving);
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
    const size_t w = min_window;
    printf("n: %6zd window: %5zd\n", n, w);
    return test(lz, (const uint8_t*)in, n, w, true);
}

static int test2(struct lz* lz) {
    const char* in = "a_aa_aa_aaa_aaaa_aaaaa_aaaa_aaa_aa_a";
    const size_t n = strlen(in);
    size_t max_w = n < max_window ? n : max_window;
    for (size_t w = min_window; w < max_w; w += w) {
        printf("n: %6zd window: %5zd\n", n, w);
        if (test(lz, (const uint8_t*)in, n, w, true)) { return 1; }
    }
    return 0;
}

static uint64_t seed = 1; // random generator seed/state

static int test3(struct lz* lz) {
    enum { N = 16, M = 256 * 1024, T = 1 };
    printf("### Random \"aaa_aa_aaaa...\" patterns\n");
    static int pl[N] = { 0 };
    for (int t = 0; t < T; t++) {
        for (int i = 0; i < 16; i++) {
            pl[i] = 2 + (int)(rand64(&seed) * 2);
        }
        static uint8_t in[M] = {0};
        int k = 0;
        int j = 0;
        while (k < M - 2) {
            for (int i = 0; i < pl[j] && k < M - 2; i++) {
                in[k++] = 'a';
            }
            j = (j + 1) % N;
            if (k < M - 2) { in[k++] = '_'; }
        }
        in[sizeof(in) - 1] = 0;
        const size_t n = strlen((char*)in);
        size_t max_w = n < max_window ? n : max_window;
        for (size_t w = min_window; w < max_w; w += w) {
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
    printf("all 0x00 bytes\n");
    printf("n: %6d window: %5d\n", n, w);
    uint64_t t = nanoseconds();
    int r = test(lz, in, n, w, false);
    t = nanoseconds() - t;
    printf("Time: %6.3fs Throughput: %7.3f MiB/s ",
            t / 1.0e9, n / (t / 1.0e9) / (1024 * 1024));
    double prev_per_byte = (double)lz->prev_count / (double)n;
    printf("prev[] per byte: %7.3f max: %zd\n",
            prev_per_byte, lz->prev_max);
    matches();
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
    printf("### All random bytes\n");
    printf("n: %6d window: %5d\n", n, max_window);
    uint64_t t = nanoseconds();
    int r = test(lz, in, n, max_window, false);
    t = nanoseconds() - t;
    printf("Time: %6.3fs Throughput: %7.3f MiB/s ",
            t / 1.0e9, n / (t / 1.0e9) / (1024 * 1024));
    double prev_per_byte = (double)lz->prev_count / (double)n;
    printf("prev[] per byte: %7.3f max: %zd\n",
            prev_per_byte, lz->prev_max);
    matches();
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
    printf("### Random sequences of same byte at random positions\n");
    printf("n: %6d window: %5d\n", n, max_window);
    uint64_t t = nanoseconds();
    int r = test(lz, in, n, max_window, false);
    t = nanoseconds() - t;
    printf("Time: %6.3fs Throughput: %7.3f MiB/s ",
            t / 1.0e9, n / (t / 1.0e9) / (1024 * 1024));
    double prev_per_byte = (double)lz->prev_count / (double)n;
    printf("prev[] per byte: %7.3f max: %zd\n",
            prev_per_byte, lz->prev_max);
    matches();
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
    printf("### file: \"%s\" %zd bytes\n", fn, n);
    uint64_t t = nanoseconds();
    r = test(lz, in, n, max_window, false);
    t = nanoseconds() - t;
    printf("Time: %6.3fs Throughput: %7.3f MiB/s ",
            t / 1.0e9, n / (t / 1.0e9) / (1024 * 1024));
    double prev_per_byte = (double)lz->prev_count / (double)n;
    printf("prev[] per byte: %7.3f max: %zd\n",
            prev_per_byte, lz->prev_max);
    matches();
    free(in);
    return r;
}

static int test_files(struct lz* lz) {
    const char* files[] = {
        "test/hhgttg.txt",
        "test/bible.txt",
        "test/confucius.txt",
        "test/lao-tzu.txt",
        "test/laozi.txt",
        "test/mandrill.bmp",
        "test/mandrill.png",
        "test/sqlite3.c",
        "test/arm64.elf",
        "test/x64.elf",
    };
    for (size_t i = 0; i < sizeof(files) / sizeof(files[0]); i++) {
        int r = test_file(lz, files[i]);
        if (r) { return r; }
    }
    return 0;
}

int lz_maps_test(void) {
    static struct lz lz77;
    struct lz* lz = &lz77;
    #if defined(LZ_TEST_STATS) && !defined(DEBUG)
    if (test_files(lz)) { return 1; } // very slow RELEASE only
    #endif
    return test0(lz) || test1(lz) || test2(lz) || test3(lz) ||
           test4(lz) || test5(lz) || test6(lz);
}
