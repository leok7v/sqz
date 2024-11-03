#define  UNSTD_NO_RT_IMPLEMENTATION
#include "rt/ustd.h"
#include "rt/fileio.h"

enum { max_window = 1u << 16, min_len = 2, max_len = 254 };

struct map { // single threaded use only
    const void** p; // p[n]
    size_t   n;
    uint32_t m; // mask 0xFFFFFF for 3 bytes and 0xFFFFFFFFu for 4 bytes
    size_t   e; // number of entries in the map
    #ifdef MAPS_STATS
    size_t   c; // stats: max chain
    uint64_t q; // stats: number of put/get queries
    uint64_t t; // stats: total sum of chains in all queries
    #endif
};

static void map_init(struct map* m, uint8_t** p, size_t n, size_t b);
static bool map_get3(struct map* m, uint32_t b3, size_t* ix);
static bool map_get4(struct map* m, uint32_t b4, size_t* ix);
static bool map_put3(struct map* m, const void* p, uint32_t b3);
static bool map_put4(struct map* m, const void* p, uint32_t b4);
static void map_remove(struct map* m, size_t ix);

#ifdef DEBUG
#define MAP_STATS
#endif

static const size_t map_deleted_ = 0xC001F00Du;
static const void*  map_deleted  = &map_deleted_;

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

// Bob Jenkins' hash function below has been compared and
// measured against more complex MurmurHash2A MurmurHash3
// and xxHash and no significant differences were found

static inline uint32_t map_hash_key(uint32_t k) {
    k ^= k >> 13;
    k *= 0x85EBCA6Bu;
    k ^= k >> 16;
    return k;
}

static inline size_t map_hash(struct map* m, uint32_t k) {
    return (size_t)(map_hash_key(k) % m->n);
}

static void map_init(struct map* m, uint8_t** p, size_t n, size_t b) {
    assert(16 < n && n <= (1u << 24) && 3 <= b && b <= 4);
    memset(m, 0, sizeof(*m));
    m->p = p;
    m->n = map_prime_under(n);
    m->m = b == 3 ? 0x00FFFFFFu : 0xFFFFFFFFu;
    memset(m->p, 0, n * sizeof(m->p[0]));
}

static inline void map_reduce_chain(struct map* m, size_t i) {
    assert(m->p[i] == map_deleted);
    const size_t n = m->n;
    size_t e = i + 1; // end of deleted chain
    while (e < i + n && m->p[e % n] == map_deleted) { e++; }
    if (!m->p[e % n]) {
        for (size_t j = i; j < e; j++) { m->p[j % n] = 0; }
    }
}

static inline bool map_get_hashed(struct map* m, uint32_t k,
                                  size_t i, size_t* ix) {
    const uint32_t mask = m->m;
    bool reduced = false;
    #ifdef MAPS_STATS
    m->q++;       // stats
    size_t c = 0; // stats: chain
    #endif
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
            #ifdef MAPS_STATS
            m->t++; // total
            if (++c > m->c) { m->c = c; }
            #endif
            i = (i + 1) % m->n;
        }
    }
    return false;
}

static bool map_get3(struct map* m, uint32_t b3, size_t* ix) {
    assert((b3 & ~0xFFFFFFu) == 0 && m->b == 3);
    return map_get_hashed(m, b3, map_hash(m, b3), ix);
}

static bool map_get4(struct map* m, uint32_t b4, size_t* ix) {
    assert(m->b == 4);
    return map_get_hashed(m, b4, map_hash(m, b4), ix);
}

static void map_remove(struct map* m, size_t ix) {
    assert(m->e > 0);
    m->p[ix] = m->p[(ix + 1) % m->n] ? map_deleted : 0;
    if (m->p[ix]) { map_reduce_chain(m, ix); }
    m->e--;
}

static bool map_put_h(struct map* m, const void* p,
                      const size_t h, uint32_t k) {
    const uint32_t mask = m->m;
    size_t d = (size_t)-1; // index of first deleted
    size_t i = h;
    #ifdef MAPS_STATS
    m->q++;       // stats
    size_t c = 0; // stats: chain
    #endif
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
            #ifdef MAPS_STATS
            m->t++; // total
            if (++c > m->c) { m->c = c; }
            #endif
            assert(i != h); // in theory should not happen
            if (i == h) { // overflow
                if (d == (size_t)-1) { return false; }
                break; // will put `p` into m->p[d]
            }
        }
    }
    if (d != (size_t)-1) {
        map_reduce_chain(m, d);
        m->p[d] = p;
    } else {
        m->p[i] = p;
    }
    m->e++;
    return true;
}

static bool map_put3(struct map* m, const void* p, uint32_t b3) {
    assert((b3 & ~0xFFFFFFu) == 0 && m->b == 3);
    return map_put_h(m, p, map_hash(m, b3), b3);
}

static bool map_put4(struct map* m, const void* p, uint32_t b4) {
    assert(m->b == 4);
    return map_put_h(m, p, map_hash(m, b4), b4);
}

// tests:

static void map_stats(const struct map* m) {
    #ifdef MAPS_STATS
    double a = m->q == 0 ? 0 : (double)m->t / (double)m->q;
    printf("map%d stats e:%zu c:%zu q:%zu t:%zu a:%.3f\n",
            m->m < UINT32_MAX ? 3 : 4, m->e, m->c, m->q, m->t, 1.0 + a);
    #else
    (void)m;
    #endif
}

static void sliding_window(struct map* m3, struct map* m4,
        uint8_t in[], size_t n, size_t window) {
    size_t count3 = 0;
    size_t count4 = 0;
    bool b = false;
    uint64_t t = nanoseconds();
    for (size_t i = 0; i < n; i++) {
        uint32_t b4 = *((uint32_t*)(in + i));
        uint32_t b3 = b4 & 0xFFFFFF;
        uint32_t b2 = b3 & 0xFFFF; (void)b2; // TODO: will need later
        if (i >= window) {
            uint32_t w4 = *((uint32_t*)(in + i - window));
            uint32_t w3 = w4 & 0xFFFFFF;
            uint32_t w2 = w3 & 0xFFFF; (void)w2; // TODO: will need later
            size_t ix;
            b = map_get3(m3, w3, &ix);
            if (b && (uint8_t*)m3->p[ix] <= in + i - window) {
                map_remove(m3, ix);
            }
            b = map_get4(m4, w4, &ix);
            if (b && (uint8_t*)m4->p[ix] <= in + i - window) {
                map_remove(m4, ix);
            }
        }
        // lz77 like lookup:
        size_t ix;
        b = map_get3(m3, b3, &ix);
        if (b) { count3++; }
        b = map_get4(m4, b4, &ix);
        if (b) { count4++; }
        // insert:
        if (m3->e >= window) { map_stats(m3); }
        if (m4->e >= window) { map_stats(m4); }
        assert(m3->e < window && m4->e < window);
        b = map_put3(m3, in + i, b3);
        if (!b) { map_stats(m3); }
        assert(b);
        b = map_put4(m4, in + i, b4);
        if (!b) { map_stats(m4); }
        assert(b);
    }
    t = nanoseconds() - t;
    printf("%6.3fs Throughput: %7.3f MiB/s count3:%d count4:%d\n",
            t / 1.0e9, n / (t / 1.0e9) / (1024 * 1024), count3, count4);
    map_stats(m3);
    map_stats(m4);
}

static int test(void) {
//  enum { window = 1 << 4, e = window * 4 };
    enum { window = 1 << 16, e = window * 4 };
    static uint8_t* p3[e];
    static uint8_t* p4[e];
    static struct map map3;
    static struct map map4;
    struct map* m3 = &map3;
    struct map* m4 = &map4;
    printf("random\n");
    #ifdef DEBUG
    static uint8_t in[4 * 1024 * 1024]; // debug
    #else
    static uint8_t in[16 * 1024 * 1024]; // release
    #endif
    for (size_t i = 0; i < countof(in); i++) {
        in[i] = (uint8_t)(256 * ((double)rand() / ((double)RAND_MAX + 1.0)));
    }
    map_init(m3, p3, e, 3);
    map_init(m4, p4, e, 4);
    sliding_window(m3, m4, in, countof(in), window);
    printf("bible.txt\n");
    uint8_t* data = null;
    size_t bytes = 0;
    errno_t r = file_read_fully("test/bible.txt", &data, &bytes);
    if (r != 0) { return r; }
    map_init(m3, p3, e, 3);
    map_init(m4, p4, e, 4);
    sliding_window(m3, m4, data, bytes, window);
    free(data);
    printf("mandrill.png\n");
    data = null;
    bytes = 0;
    r = file_read_fully("test/mandrill.png", &data, &bytes);
    if (r != 0) { return r; }
    map_init(m3, p3, e, 3);
    map_init(m4, p4, e, 4);
    sliding_window(m3, m4, data, bytes, window);
    free(data);
    return 0;
}

static errno_t locate_test_folder(void) {
    // on Unix systems with "make" executable usually resided
    // and is run from root of repository... On Windows with
    // MSVC it is buried inside bin/... folder depths
    // on X Code in MacOS it can be completely out of tree.
    // So we need to find the test files.
    for (;;) {
        if (file_exist("test/bible.txt")) { return 0; }
        if (file_chdir("..") != 0) { return errno; }
    }
}

/*

2024 MacBook Air M3 processor

with MAP_STATS:

test random
sliding_window 2.829s Throughput:   5.657 MiB/s count3:64699 count4:447
map_stats      map3 stats e:65408 c:112 q:50266112 t:45836201 a:1.912
map_stats      map4 stats e:65536 c:121 q:50266112 t:47408682 a:1.943
test           bible.txt
sliding_window 0.070s Throughput:  60.574 MiB/s count3:4349567 count4:4081387
map_stats      map3 stats e:3818 c:4 q:13242983 t:26652 a:1.002
map_stats      map4 stats e:9750 c:7 q:13242983 t:97795 a:1.007
test           mandrill.png
sliding_window 0.057s Throughput:  10.462 MiB/s count3:4000 count4:414
map_stats      map3 stats e:65297 c:52 q:1818152 t:810971 a:1.446
map_stats      map4 stats e:65503 c:49 q:1818152 t:826612 a:1.455

without MAP_STATS:

test random
sliding_window 2.810s Throughput:   5.694 MiB/s count3:64699 count4:447
test           bible.txt
sliding_window 0.059s Throughput:  71.776 MiB/s count3:4349567 count4:4081387
test           mandrill.png
sliding_window 0.056s Throughput:  10.718 MiB/s count3:4000 count4:414

*/

int main(int argc, const char* argv[]) {
    (void)argc; (void)argv; // unused
    locate_test_folder();
    return test();
}
