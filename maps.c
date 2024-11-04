#define  UNSTD_NO_RT_IMPLEMENTATION
#include "rt/ustd.h"
#include "rt/fileio.h"

enum { max_window = 1u << 16, min_len = 2, max_len = 254 };

#ifdef DEBUG
#define MAP_STATS
#else
#undef  MAP_STATS
#endif

struct map { // single threaded use only
    const void** p; // p[n]
    size_t   n;
    uint32_t m; // mask 0xFFFFFF for 3 bytes and 0xFFFFFFFFu for 4 bytes
    #ifdef MAP_STATS
    size_t   e; // number of entries in the map
    size_t   c; // stats: max chain
    uint64_t q; // stats: number of put/get queries
    uint64_t t; // stats: total sum of chains in all queries
    uint64_t r; // stats: number of replaced items
    #endif
};

static void map_init(struct map* m, void** p, size_t n, size_t b);
static bool map_get3(struct map* m, uint32_t b3, size_t* ix);
static bool map_get4(struct map* m, uint32_t b4, size_t* ix);
static bool map_put3(struct map* m, const void* p, uint32_t b3);
static bool map_put4(struct map* m, const void* p, uint32_t b4);
static void map_remove(struct map* m, size_t ix);

static const size_t map_deleted_ = 0xC001F00Du;
static const void*  map_deleted  = &map_deleted_;

static size_t map_prime_under(size_t n) {
    static const size_t table_of_primes[] = {
        3, 7, 13, 31, 61, 127, 251, 509, 1021, 2039, 4093, 8191, 16381,
        32749, 65521, 131071, 262139, 524287, 1048573, 2097143, 4194301,
        8388593, 16777213
    };
    swear(4 <= n && ((n - 1) & n) == 0 && n <= (1u << 24));
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

static void map_init(struct map* m, void** p, size_t n, size_t b) {
    swear(16 < n && n <= (1u << 24) && 3 <= b && b <= 4);
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
    #ifdef MAP_STATS
    m->q++;       // stats
    size_t c = 1; // stats: chain
    m->t++; // total
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
            i = (i + 1) % m->n;
            #ifdef MAP_STATS
            m->t++; // total
            if (++c > m->c) { m->c = c; }
            #endif
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
    #ifdef MAP_STATS
    assert(m->e > 0);
    m->e--;
    #endif
    m->p[ix] = m->p[(ix + 1) % m->n] ? map_deleted : 0;
    if (m->p[ix]) { map_reduce_chain(m, ix); }
}

static inline bool map_put(struct map* m, const void* p,
                      const size_t h, uint32_t k) {
    const uint32_t mask = m->m;
    size_t d = (size_t)-1; // index of first deleted
    size_t i = h;
    #ifdef MAP_STATS
    m->q++;       // queries
    size_t c = 0; // chain
    m->t++;       // total
    #endif
    while (m->p[i]) {
        const bool deleted = m->p[i] == map_deleted;
        const bool not_deleted_and_equal = !deleted &&
                   (mask & *((uint32_t*)m->p[i])) == k;
        if (not_deleted_and_equal) {
            #ifdef MAP_STATS
            m->r++; // replacing existing key
            #endif
            m->p[i] = p;
            return true;
        } else {
            if (deleted && d == (size_t)-1) { d = i; }
            i = (i + 1) % m->n;
            #ifdef MAP_STATS
            m->t++;
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
    #ifdef MAP_STATS
    m->e++;
    #endif
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

/*
    Future improvements:

    It is possible to implement circular window[1 << max_window] and
    move start pointer putting incoming LZ77 byte inside it.
    This will allow to replace direct pointers to memory in the maps
    by 32-bit unsigned offsets from the beginning of the window.
    Why 32 bits not 16 because offset of the next byte pointer in lz77
    is max_window (64KB) bytes away from the beginning of the window
    offset. UINT32_MAX can be used as empty entry and UINT32_MAX - 1
    as deleted.

    It will reduce the memory bandwidth for p[*] pointers and half
    their size on 64bit architectures and will also make stream
    processing possible.

    Comparing to 3 and 4 bytes not yet in the window is no issue
    but expanding match beyond that may present a bit of complexity
    in lz77 itself (wrap around bytes inside the window and pointer
    to max_size incoming bytes). Not a big deal - doable.

    Not a goal at the moment.
*/

// tests:

#define MAP_LZ77_LOOKUP
#undef  MAP_LZ77_LOOKUP

static void map_stats(const struct map* m) {
    #ifdef MAP_STATS
    double a = m->q == 0 ? 0 : ((double)m->t / (double)m->q);
    printf("map%d stats e:%zu c:%zu q:%llu t:%llu r:%llu a:%.3f\n",
            m->m < UINT32_MAX ? 3 : 4, m->e, m->c, m->q, m->t,
            m->r, a);
    #else
    (void)m;
    #endif
}

static void sliding_window(struct map* m3, struct map* m4,
        uint8_t in[], size_t n, size_t window) {
    size_t c3 = 0; // count of 3 bytes sequences found in the window
    size_t c4 = 0;
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
        #ifdef MAP_LZ77_LOOKUP
        {
            // lz77 like lookup:
            size_t ix;
            b = map_get3(m3, b3, &ix);
            if (b) { c3++; }
            b = map_get4(m4, b4, &ix);
            if (b) { c4++; }
        }
        #endif
        // insert:
        b = map_put3(m3, in + i, b3);
        if (!b) { map_stats(m3); }
        assert(b);
        #ifdef MAP_STATS
        if (m3->e > window) { map_stats(m3); }
        assert(m3->e <= window);
        #endif
        b = map_put4(m4, in + i, b4);
        if (!b) { map_stats(m4); }
        assert(b);
        #ifdef MAP_STATS
        if (m4->e > window) { map_stats(m4); }
        assert(m4->e <= window);
        #endif
    }
    t = nanoseconds() - t;
    printf("%6.3fs Throughput: %5.1f MiB/s c3:%d c4:%d\n",
            t / 1.0e9, n / (t / 1.0e9) / (1024 * 1024), c3, c4);
    map_stats(m3);
    map_stats(m4);
}

static int test(void) {
    enum { window = 1 << 16, e = window * 4 };
    // number of entries in the map is 4 times
    // the window size. Experimental window * 2
    // leads to enormous amount of collisions
    // with 16 bit rand() generated array of bytes
    static uint8_t* p3[e];
    static uint8_t* p4[e];
    static struct map map3;
    static struct map map4;
    struct map* m3 = &map3;
    struct map* m4 = &map4;
    {
        #ifdef DEBUG
        static uint8_t in[4 * 1024 * 1024]; // debug
        #else
        static uint8_t in[16 * 1024 * 1024]; // release
        #endif
        printf("random %zu\n", countof(in));
        for (size_t i = 0; i < countof(in); i++) {
            in[i] = (uint8_t)(256 * ((double)rand() / ((double)RAND_MAX + 1.0)));
        }
        map_init(m3, p3, e, 3);
        map_init(m4, p4, e, 4);
        sliding_window(m3, m4, in, countof(in), window);
    }
    {
        uint8_t* data = null;
        size_t bytes = 0;
        errno_t r = file_read_fully("test/bible.txt", &data, &bytes);
        if (r != 0) { return r; }
        printf("bible.txt %zd\n", bytes);
        map_init(m3, p3, e, 3);
        map_init(m4, p4, e, 4);
        sliding_window(m3, m4, data, bytes, window);
        free(data);
    }
    {
        uint8_t* data = null;
        size_t bytes = 0;
        errno_t r = file_read_fully("test/mandrill.png", &data, &bytes);
        if (r != 0) { return r; }
        printf("mandrill.png %zd\n", bytes);
        map_init(m3, p3, e, 3);
        map_init(m4, p4, e, 4);
        sliding_window(m3, m4, data, bytes, window);
        free(data);
    }
    return 0;
}

/*

2024 MacBook Air M3 processor ARM64 Release build MSVC 2024

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

without MAP_STATS and without MAP_LZ77_LOOKUP:

test random 16777216
sliding_window  1.941s Throughput:   8.244 MiB/s
test           bible.txt 4436173
sliding_window  0.042s Throughput: 101.627 MiB/s
test           mandrill.png 627896
sliding_window  0.044s Throughput:  13.520 MiB/s

*/

static errno_t locate_test_folder(void) {
    for (;;) {
        if (file_exist("test/bible.txt")) { return 0; }
        if (file_chdir("..") != 0) { return errno; }
    }
}

int maps_main(int argc, const char* argv[]) {
    (void)argc; (void)argv; // unused
    errno_t r = locate_test_folder();
    if (r != 0) { return r; }
    return test();
}
