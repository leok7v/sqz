#define  UNSTD_NO_RT_IMPLEMENTATION
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

enum { max_window = 1u << 16, min_len = 2, max_len = 254 };

struct map_entry {
    const uint8_t* data;
    uint64_t hash;
};

struct map {
    struct map_entry* entry;
    int32_t  n; // signed because map_get() returns -1
    int32_t  bytes;
    int32_t  entries;
    // stats:
    int32_t  max_chain;
    uint64_t sum_chain;
    uint64_t searches;
    uint64_t inserted;
    uint64_t removed;
    uint64_t replaced;
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
    size_t   prev[max_window]; // previous `i` of 2 byte prefix
    size_t   map2[1u << (sizeof(uint16_t) * 8)]; // `i` of 2 byte prefixes
    struct map map3;
    struct map map4;
    struct map_entry me3[max_window * 3];
    struct map_entry me4[max_window * 3];
};

static void    map_init(struct map* m, struct map_entry entry[], size_t n, int32_t bytes);
static int32_t map_get(struct map* m, const void* data);
static int32_t map_put(struct map* m, const void* data);
static void    map_remove(struct map* m, int32_t i);
static void    map_delete(struct map* m, const void* d);
static void    map_clear(struct map *m);

static void lz_init(struct lz *lz, const uint8_t* in, size_t n, size_t window) {
    assert(n > 0);
    assert(2 <= window && window <= max_window);
    assert(((window - 1) & window) == 0); // window is power of 2
    memset(lz, 0, sizeof(*lz));
    lz->in = in;
    lz->n  = n;
    lz->i  = 0;
    lz->w  = window;
    map_init(&lz->map3, lz->me3, sizeof(lz->me3) / sizeof(lz->me3[0]), 3);
    map_init(&lz->map4, lz->me4, sizeof(lz->me4) / sizeof(lz->me4[0]), 4);
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
        assert(in[p] == in[i] && in[p + 1] == in[i + 1]);
        d = lz->prev[p % w];
    }
}

static void lz_insert(struct lz* lz) {
    const uint8_t* in = lz->in;
    const size_t n = lz->n;
    const size_t i = lz->i;
    const size_t w = lz->w;
    if (i < n - 4) {
        if (i >= w) {
            int32_t i3 = map_get(&lz->map3, in + i - w);
            if (i3 >= 0) {
                size_t d = in + i - lz->map3.entry[i3].data;
                if (d >= w) { map_remove(&lz->map3, i3); }
            }
            int32_t i4 = map_get(&lz->map4, in + i - w);
            if (i4 >= 0) {
                size_t d = in + i - lz->map4.entry[i4].data;
                if (d >= w) { map_remove(&lz->map4, i4); }
            }
        }
        int32_t me3 = lz->map3.entries;
        int32_t me4 = lz->map4.entries;
        swear(me3 <= max_window, "inserted: %lld removed: %lld replaced: %lld",
            lz->map3.inserted, lz->map3.removed, lz->map3.replaced);
        swear(me4 <= max_window);
        map_put(&lz->map3, in + i);
//      map_put(&lz->map4, in + i);
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
                trace("%zu\n", i - pp);
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
            lz_chain(lz, index);
        #endif
        #ifdef DEBUG
            lz_verify(lz, index);
        #endif
    }
}

// both lz_find and lz_search function search longest match
// at a shortest distance from position `i` and return
// ml: [2..max_len] inclusive
// md: [1..window] inclusive

static inline void lz_find(struct lz* lz, size_t *ml, size_t *md) {
    // TODO: store bits_in_window in `w` and use mask instead of modulo
    assert(*ml == 0);
    assert(*md == 0);
    const size_t n = lz->n;
    const size_t i = lz->i;
    const size_t w = lz->w;
    const uint8_t* in = lz->in;
    trace("[%zd] \"%c%c\"\n", i, in[i], in[i + 1]);
    if (1 <= i && i < n - 4) {
//      if (i == 29 && w == 2) { rt_breakpoint(); }
        size_t len = 0;
        size_t dst = 0;
        size_t p = lz->map2[in[i] | (in[i + 1] << 8)];
        if (0 < p && p - 1 < i && i <= p - 1 + w) {
            p--;
            assert(p < i && i - p <= w);
            size_t max_k = n - i > max_len ? max_len : n - i;
            assert(in[p] == in[i] && in[p + 1] == in[i + 1]);
            size_t index = p & (w - 1); // same as p % w for w = 2^x
            size_t k = 2; // start with at least 2 because:
            while (k < max_k && in[p + k] == in[i + k]) { k++; }
            if (i - p <= w) { len = k; dst = i - p; }
            size_t d = lz->prev[index];
            assert(d == 0 || d <= p);
            while (len < max_len && 0 < d && d <= p && p - d < i && i <= p - d + w) {
                p -= d;
                if (len == 2 || memcmp(in + p + 2, in + i + 2, len - 2) == 0) {
                    k = len; // because with `len` bytes are the same
                    while (k < max_k && in[p + k] == in[i + k]) { k++; }
                    if (k > len) { len = k; dst = i - p; }
                }
                index = p & (w - 1); // same as p % w for w = 2^x
                d = lz->prev[index];
            }
            *ml = len;
            *md = dst;
        }
    }
}

static void lz_search(struct lz* lz, size_t *ml, size_t *md) { // linear
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

// maps
// map_put()  is no operation if map is filled to 75% or more
// map_get()  returns index of matching entry or -1

// FNV Fowler Noll Vo hash function
// https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function

// FNV offset basis for 64-bit:
static const uint64_t map_hash_init = 0xCBF29CE484222325;

// FNV prime for 64-bit
static const uint64_t map_prime64   = 0x100000001B3;

static const size_t map_deleted_unique = 0xC001F00Du;
static const void*  map_deleted = &map_deleted_unique;

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

static void map_init(struct map* m, struct map_entry entry[], size_t n, int32_t bytes) {
    // map_get() returns index of matching entry or -1 which limits
    // size of map to INT32_MAX
    swear(16 < n && n < INT32_MAX);
    swear(bytes == 3 || bytes == 4);
    m->entry = entry;
    m->n = (int32_t)n;
    m->bytes = bytes;
    memset(m->entry, 0, n * sizeof(m->entry[0]));
    m->entries = 0;
    m->max_chain = 0;
    m->sum_chain = 0;
    m->searches = 0;
    m->inserted = 0;
    m->replaced = 0;
    m->removed  = 0;
}

static void map_reduce_chain(struct map* m, int32_t i) {
    assert(m->entry[i].data == map_deleted);
    const int32_t n = m->n;
    int32_t k = i + 1;
    while (k < i + n && m->entry[k % n].data == map_deleted) { k++; }
    if (!m->entry[(i + k + 1) % n].data) {
//      printf("%d\n", k - i);
        for (int32_t j = i; j < k; j++) { m->entry[j % n].data = null; }
    }
}

static int32_t map_get_hashed(struct map* m, uint64_t h, const void* d) {
    const struct map_entry* en = m->entry;
    const int32_t n = m->n;
    const int32_t b = m->bytes;
    int32_t i = (int32_t)(h % n);
    int32_t f = -1; // found index
    int32_t c = 0;  // chain
    int32_t dix = -1; // first deleted index seen
    bool all_deleted_after_dix = true; // assumption
    const struct map_entry* e = en + i;
    while (e->data && f < 0) {
        bool deleted = e->data == map_deleted;
        if (dix <  0 &&  deleted) { dix = i; }
        if (dix >= 0 && !deleted) { all_deleted_after_dix = false; }
        if (e->data != map_deleted && e->hash == h &&
            memcmp(e->data, d, b) == 0) {
            f = i;
        } else {
            i = (i + 1) % m->n;
            e = en + i;
            c++;
        }
    }
    if (dix >= 0 && all_deleted_after_dix) { map_reduce_chain(m, dix); }
    if (c > m->max_chain) { m->max_chain = c;
printf("bytes: %d max_chain: %d entries: %d\n", b, c, m->entries);
}
    m->sum_chain += c;
    m->searches++;
    return f;
}

static int32_t map_get(struct map* m, const void* d) {
    int32_t i = map_get_hashed(m, map_hash64(d, m->bytes), d);
    return i;
}

static void map_remove(struct map* m, int32_t i) {
    struct map_entry* e = m->entry + i;
    assert(m->entries > 0 && e->data && e->data != map_deleted);
    e->hash = 0;
    e->data = map_deleted;
    m->removed++;
    m->entries--;
    map_reduce_chain(m, i);
}

static void map_delete(struct map* m, const void* d) {
    int32_t i = map_get(m, d);
    if (i >= 0) { map_remove(m, i); }
}

static int32_t map_put(struct map* m, const void* data) {
    int32_t f = -1; // found index
    const uint8_t*  d = (const uint8_t*)data;
    const int32_t   n = m->n;
    const int32_t   b = m->bytes;
    const int32_t n34 = m->n * 3 / 4;
    enum { max_bytes = sizeof(m->entry[0]) - 1 };
    assert(2 <= b && b <= UINT32_MAX);
    if (m->entries > n34) {
        swear(m->entries <= n34); // too many entries
    } else {
        struct map_entry* en = m->entry;
        const uint64_t h = map_hash64(d, b);
        int32_t i = (int32_t)(h % n);
        int32_t c = 0; // max chain length
        bool all_deleted_after_dix = true; // assumption
        int32_t dix = -1; // first deleted index seen
        struct map_entry* e = en + i;
        while (e->data && f < 0) {
            const bool deleted = e->data == map_deleted;
            if (!deleted && e->hash == h && memcmp(e->data, d, b) == 0) {
                f = i;   // found match with existing entry
            } else {
                if (dix <  0 &&  deleted) { dix = i; }
                if (dix >= 0 && !deleted) { all_deleted_after_dix = false; }
                c++;
                assert(c <= n34);
                i = (i + 1) % n;
                e = en + i;
            }
        }
        if (f < 0) {
            assert(!e->data);
            if (dix >= 0) {
                if (all_deleted_after_dix) { map_reduce_chain(m, dix); }
                i = dix;  // put into first slot marked as deleted
                e = en + i;
            }
            e->data = d;
            e->hash = h;
            m->inserted++;
            m->entries++;
        } else {
            assert(e->hash == h && e->data < d);
            e->data = d; // update to shorter distance
            m->replaced++;
        }
        if (c > m->max_chain) { m->max_chain = c;
printf("bytes: %d max_chain: %d entries: %d\n", b, c, m->entries);
}
        // stats:
        m->sum_chain += c;
        m->searches++;
    }
    return f;
}

static void map_clear(struct map *m) {
    memset(m, 0, sizeof(*m));
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
                size_t ml2 = 0, md2 = 0;
                lz_search(lz, &ml2, &md2);
                if (ml1 > 0 || ml2 > 0) {
                    if (ml1 != ml2 || md1 != md2) {
                        printf("[%zd] longest %zd:%zd \"%.2s\" \n",
                               lz->i, ml1, md1, lz->in + lz->i);
                        printf("[%zd] linear  %zd:%zd \"%.2s\" \n",
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
    enum { N = 16, M = 256, T = 32 };
//  enum { N = 16, M = 256 * 1024, T = 32 };
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
            printf("n: %6zd window: %5zd\n", n, w);
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
    printf("map3: max_chain: %zd map4: max_chain: %zd\n",
           lz->map3.max_chain, lz->map4.max_chain);
    return r;
    #else
    return lz == 0;
    #endif
}

static int test5(struct lz* lz) {
    #if !defined(DEBUG) || defined(LZ_ALL_TESTS)
    #ifdef DEBUG
//      enum { n = 128 * 1024};
        enum { n = 256 * 1024};
    #else
        enum { n = 32 * 1024 * 1024};
    #endif
    static uint8_t in[n];
    for (int i = 0; i < n; i++) {
        in[i] = (uint8_t)(256 * rand64(&seed));
    }
    uint64_t t = nanoseconds();
    printf("n: %6d window: %5d\n", n, max_window);
    int r = test(lz, in, n, max_window, false);
    t = nanoseconds() - t;
    printf("Time: %6.3fs Throughput: %7.3f MiB/s\n",
            t / 1.0e9, n / (t / 1.0e9) / (1024 * 1024));
    // 32MB on Mac Book Air 2024 M3
    // RELEASE Time:  0.608s Throughput:  52.612 MiB/s
    double chain3 = (double)lz->map3.sum_chain / lz->map3.searches;
    double chain4 = (double)lz->map4.sum_chain / lz->map4.searches;
    printf("map3: max_chain: %zd (%.1f) map4: max_chain: %zd (%.1f)\n",
           lz->map3.max_chain, chain3, lz->map4.max_chain, chain4);
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
    printf("n: %6d window: %5d\n", n, max_window);
    int r = test(lz, in, n, max_window, false);
    t = nanoseconds() - t;
    printf("Time: %6.3fs Throughput: %7.3f MiB/s\n",
            t / 1.0e9, n / (t / 1.0e9) / (1024 * 1024));
    // 64MB on Mac Book Air 2024 M3
    // RELEASE Time:  1.159s Throughput:  55.222 MiB/s
    printf("map3: max_chain: %zd map4: max_chain: %zd\n",
           lz->map3.max_chain, lz->map4.max_chain);
    return r;
    #else
    return lz == 0;
    #endif
}

int xxx_main(int argc, const char* argv[]) {
    (void)argc; (void)argv; // unused
    static struct lz lz77;
    struct lz* lz = &lz77;
    return test5(lz);
    return test0(lz) || test1(lz) || test2(lz) || test3(lz) ||
           test4(lz) || test5(lz) || test6(lz);
}
