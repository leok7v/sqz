#include "rt/ustd.h"

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

enum { max_window = 2u << 16, max_len = 254 };

struct lz77 {
    const uint8_t* in; // pointer to array of bytes[n]
    size_t   n;        // number of bytes in "in" array
    size_t   i;        // current position in "in" array after window
    size_t   w;        // window size
    // TODO: all the fields above is debugging convenience
    //       and can be passed as parameters to all the lz_* functions
    //       possibly assisting compiler to optimize better and use
    //       registers instead of writing values back to memory.
    size_t   prev[max_window]; // previous "i" of 2 byte prefix
    size_t   map2[1u << (sizeof(uint16_t) * 8)]; // "i" of 2 byte prefixes
};

static void lz_init(struct lz77 *lz, const uint8_t* in, size_t n, size_t window) {
    assert(n > 0);
    assert(2 <= window && window <= max_window);
    assert(((window - 1) & window) == 0); // window is power of 2
    memset(lz, 0, sizeof(*lz));
    lz->in = in;
    lz->n  = n;
    lz->i  = 0;
    lz->w  = window;
}

static void lz_chain(struct lz77 *lz, size_t index) {
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

static void lz_verify(struct lz77 *lz, size_t index) {
    const uint8_t* in = lz->in; (void)in;
    const size_t i = lz->i;
    const size_t w = lz->w;
    size_t p = i;
    size_t d = lz->prev[index];
    while (d > 0 && d <= p && p - d < i && i <= p - d + w) {
        p -= d;
        assert(in[p] == in[i] && in[p + 1] == in[i + 1]);
        d = lz->prev[p % w];
    }
}

static void lz_insert(struct lz77* lz) {
    const uint8_t* in = lz->in;
    const size_t   n = lz->n;
    const size_t   i = lz->i;
    const size_t   w = lz->w;
    if (i < n - 1) {
        const uint16_t prefix = in[i] | (in[i + 1] << 8);
        const size_t index = i % lz->w;
        size_t pp = lz->map2[prefix]; // previous position
        trace("[%zd] map[\"%c%c\":0x%04x(%u)] = %zu prev[%zd] := ",
               i, in[i], in[i + 1], prefix, prefix, pp, index);
        if (pp == 0) {
            trace("0\n");
            lz->prev[index] = 0;
        } else {
            pp--;
            if (i - pp <= w) {
                assert(in[pp] == in[i] && in[pp + 1] == in[i + 1]);
                assert(pp < i);
                trace("%zd\n", i - pp);
                lz->prev[index] = i - pp;
            } else {
                trace("0 (out of window)\n");
                lz->prev[index] = 0;
            }
        }
        lz->map2[prefix] = i + 1;
        trace("[%zd] map2[\"%c%c\":0x%04x(%u)] := %zu\n",
               i, in[i], in[i + 1], prefix, prefix, index + 1);
        #ifdef LZ_VERBOSE
            chain(lz, index);
        #endif
        #ifdef DEBUG
            lz_verify(lz, index);
        #endif
    }
}

// both lz_find and lz_search function search longest match
// at a shortest distance from position "i" and return
// ml: [2..max_len] inclusive
// md: [1..window] inclusive

static inline void lz_find(struct lz77* lz, size_t *ml, size_t *md) {
    assert(*ml == 0);
    assert(*md == 0);
    const size_t n = lz->n;
    const size_t i = lz->i;
    const size_t w = lz->w;
    const uint8_t* in = lz->in;
    trace("[%zd] \"%c%c\"\n", i, in[i], in[i + 1]);
    if (1 <= i && i < n - 1) {
//      if (i == 15 && w == 8) rt_breakpoint();
        size_t len = 0;
        size_t dst = 0;
        size_t p = lz->map2[in[i] | (in[i + 1] << 8)];
        if (0 < p && p - 1 < i && i <= p - 1 + w) {
            p--;
            assert(p < i && i - p <= w);
            size_t max_k = n - i > max_len ? max_len : n - i;
            size_t k = 2; // start with 2 because:
            assert(in[p] == in[i] && in[p + 1] == in[i + 1]);
            while (k < max_k && in[p + k] == in[i + k]) { k++; }
            if (i - p <= w) { len = k; dst = i - p; }
            size_t d = lz->prev[p % w];
            while (len < max_len && d > 0 && d <= p && p - d < i && i <= p - d + w) {
                p -= d;
                assert(in[p] == in[i] && in[p + 1] == in[i + 1]);
                if (memcmp(in + p, in + i, len) == 0) {
                    k = len; // because with "len" bytes are the same
                    while (k < max_k && in[p + k] == in[i + k]) { k++; }
                    if (k > len) { len = k; dst = i - p; }
                }
                d = lz->prev[p % w];
            }
            *ml = len;
            *md = dst;
        }
    }
}

static void lz_search(struct lz77* lz, size_t *ml, size_t *md) { // linear
    assert(*ml == 0);
    assert(*md == 0);
    const size_t n = lz->n;
    const size_t i = lz->i;
    const size_t w = lz->w;
    if (1 <= i && i < n - 1) {
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

/*

    Optimization:

    It is possible to create hash maps for 3,4,5,6,7,8 bytes
    look ahead suffixes (searching to n - 8)
    chain link only 8 suffixes, and then search starting with 8 bytes.

    The maps has to be at least 4/3 of window size to minimize rehash
    and collision and support deleted items with linear collision rehash.
    Because the source position in input[] window can be addressed
    no need to keep anything but position and empty and deleted tag.
    Because index search is limited to n - 8 (SIZE_T_MAX - 8) the
    highest two values of size_t can be used to mark empty and deleted.

    If 8 bytes chain finds nothing, 7,6,5,4,3,2 bytes can be direct
    indexed without chaining because if longer sequence was not found
    the shorter sequence does not even need to be extended it is
    guaranteed to be best match.

    When byte leaves the window it would need to be hashed again
    and removed from all 8,7,6,5,4,3 hash maps.

    For all hash maps only latest entry need to be held in the map
    and in case of longest (8 bytes) it needs to be chained.

    The expense of building 6 additional hash maps is per entering
    exiting byte and is not dependent on window size.

    It is rather complex optimization, but it is not clear if it will
    increase the throughput because of expense of maintaining extra
    hash maps and removing from them.

    https://en.wikipedia.org/wiki/Suffix_tree
    https://en.wikipedia.org/wiki/Suffix_array
    (^^^ suffix link table)

*/

// tests:

uint64_t seed = 1; // random generator seed/state

static int test(struct lz77* lz, const uint8_t* in, const size_t n,
                const size_t w, bool verbose) {
    lz_init(lz, in, n, w);
    if (verbose) {
        printf("window: %zd\n", w);
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
    lz->i = 0;
    lz_insert(lz);
    while (lz->i < lz->n) {
        if (1 <= lz->i && lz->i < lz->n - 1) {
            size_t ml1 = 0, md1 = 0;
            lz_find(lz, &ml1, &md1);
            #ifdef DEBUG
                size_t ml2 = 0, md2 = 0;
                lz_search(lz, &ml2, &md2);
                if (ml1 > 0 || ml2 > 0) {
                    if (ml1 != ml2 && md1 != md2) {
                        printf("[%zd] longest %zd:%zd \"%.2s\" \n", lz->i, ml1, md1, lz->in + lz->i);
                        printf("[%zd] linear  %zd:%zd \"%.2s\" \n", lz->i, ml2, md2, lz->in + lz->i);
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

static int test1(struct lz77* lz) {
    const char* in = "aaaaa";
    return test(lz, (const uint8_t*)in, strlen(in), 2, true);
}

static int test2(struct lz77* lz) {
    const char* in = "a_aa_aa_aaa_aaaa_aaaaa_aaaa_aaa_aa_a";
    const size_t n = strlen(in);
    size_t max_w = n < max_window ? n : max_window;
    for (size_t w = 2; w < max_w; w += w) {
        if (test(lz, (const uint8_t*)in, strlen(in), w, true)) { return 1; }
    }
    return 0;
}

static int test3(struct lz77* lz) {
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
            if (test(lz, (const uint8_t*)in, n, w, false)) { return 1; }
        }
    }
    return 0;
}

static int test4(struct lz77* lz) {
    #if !defined(DEBUG) || defined(LZ_ALL_TESTS)
    #ifdef DEBUG
        enum { n = max_window / 2, window = max_window / 8 };
    #else
        enum { n = 32 * 1024 * 1024, window = max_window };
    #endif
    static uint8_t in[n];
    memset(in, 0x00, n);
    uint64_t t = nanoseconds();
    int r = test(lz, in, n, window, false);
    t = nanoseconds() - t;
    printf("Time: %6.3fs Throughput: %7.3f MiB/s\n",
            t / 1.0e9, n / (t / 1.0e9) / (1024 * 1024));
    // 32MB on Mac Book Air 2024 M3
    // RELEASE Time:  0.096s Throughput: 332.5 MiB/s
    return r;
    #else
    return lz == 0;
    #endif
}

static int test5(struct lz77* lz) {
    #if !defined(DEBUG) || defined(LZ_ALL_TESTS)
    #ifdef DEBUG
        enum { n = 128 * 1024};
    #else
        enum { n = 32 * 1024 * 1024};
    #endif
    static uint8_t in[n];
    for (int i = 0; i < n; i++) {
        in[i] = (uint8_t)(256 * rand64(&seed));
    }
    uint64_t t = nanoseconds();
    int r = test(lz, in, n, max_window, false);
    t = nanoseconds() - t;
    printf("Time: %6.3fs Throughput: %7.3f MiB/s\n",
            t / 1.0e9, n / (t / 1.0e9) / (1024 * 1024));
    // 32MB on Mac Book Air 2024 M3
    // RELEASE Time:  0.847s Throughput:  37.8 MiB/s
    return r;
    #else
    return lz == 0;
    #endif
}

static int test6(struct lz77* lz) {
    #if !defined(DEBUG) || defined(LZ_ALL_TESTS)
    #ifdef DEBUG
        enum { n = 128 * 1024};
    #else
        enum { n = 64 * 1024 * 1024 };
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
    uint64_t t = nanoseconds();
    int r = test(lz, in, n, max_window, false);
    t = nanoseconds() - t;
    printf("Time: %6.3fs Throughput: %7.3f MiB/s\n",
            t / 1.0e9, n / (t / 1.0e9) / (1024 * 1024));
    // 64MB on Mac Book Air 2024 M3
    // RELEASE Time:  2.653s Throughput:  24.1 MiB/s
    return r;
    #else
    return lz == 0;
    #endif
}

int main(int argc, const char* argv[]) {
    (void)argc; (void)argv; // unused
    static struct lz77 lz77;
    struct lz77* lz = &lz77;
    return test1(lz) || test2(lz) || test3(lz) || test4(lz) || test5(lz) || test6(lz);
}
