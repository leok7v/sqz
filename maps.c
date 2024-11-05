#define  UNSTD_NO_RT_IMPLEMENTATION
#include "rt/ustd.h"
#include "rt/fileio.h"

enum { max_window = 1u << 16, min_len = 2, max_len = 254 };

#ifdef DEBUG
#define MAP_STATS
#else
#undef  MAP_STATS
#endif

#define MAP_VERBOSE
#undef  MAP_VERBOSE

#ifdef MAP_VERBOSE
#define trace(...) printf(__VA_ARGS__)
#else
#define trace(...) (void)0
#endif

// single threaded open-addressed map with linear probing
// and no tombstones

struct map {
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
    swear(8 < n && n <= (1u << 24) && 3 <= b && b <= 4);
    memset(m, 0, sizeof(*m));
    m->p = p;
    m->n = map_prime_under(n);
    m->m = b == 3 ? 0x00FFFFFFu : 0xFFFFFFFFu;
    memset(m->p, 0, n * sizeof(m->p[0]));
}

static void map_print(struct map* m) {
    trace("[");
    for (size_t i = 0; i < m->n; i++) {
        if (!m->p[i]) {
            trace(" [%2zd] null\n", i);
        } else {
            const uint32_t b4 = *(uint32_t*)m->p[i];
            size_t h = map_hash(m, b4 & m->m); // ideal position
            trace(" [%2zd] 0x%08X h:%d\n", i, b4, h); (void)h;
        }
    }
    trace("] n:e %zd:%zd\n", m->n, m->e);
}

static inline bool map_get(struct map* m, uint32_t k,
                                  const size_t s, size_t* ix) {
    size_t i = s; // start
    const uint32_t mask = m->m;
    #ifdef MAP_STATS
    m->q++;       // stats
    size_t c = 1; // stats: chain
    m->t++; // total
    #endif
    while (m->p[i]) {
        if ((mask & *((uint32_t*)m->p[i])) == k) {
            *ix = i;
            return true;
        } else {
            i = (i + 1) % m->n;
            if (i == s) { return false; }
            #ifdef MAP_STATS
            m->t++; // total
            if (++c > m->c) { m->c = c; }
            #endif
        }
    }
    return false;
}

static inline void map_remove(struct map* m, size_t i) {
    #ifdef MAP_STATS
    assert(m->e > 0);
    m->e--;
    #endif
    #ifdef MAP_VERBOSE
        const uint32_t b4 = *(uint32_t*)m->p[i];
        trace("[%2zd] removed  0x%08X h:%2zd entries %zd\n",
               i, b4, map_hash(m, b4 & m->m), m->e);
    #endif
    m->p[i] = 0;
    size_t x = i; // next
    for (;;) {
        x = (x + 1) % m->n;
        if (!m->p[x]) { break; }
        const uint32_t b4 = *(uint32_t*)m->p[x];
        size_t h = map_hash(m, b4 & m->m);
        // Check if `h` lies within [i, x), accounting for wrap-around:
        if ((x < i) ^ (h <= i) ^ (h > x)) { // can move
            m->p[i] = m->p[x];
            m->p[x] = 0;
            i = x;
        }
    }
}

/*

   Case 1: No wrap-around, i < x and h lies between i and x
   [__0__|__1__|__2__|__3__|__4__|__5__|__6__|__7__]
                i:2         h:4         x:6
   In this case, i < x and h is within the straightforward range [i, x).

   Case 2: Wrap-around, x < i, and h lies before i
   [__0__|__1__|__2__|__3__|__4__|__5__|__6__|__7__]
          h:1   x:2                     i:6
   Here, with x < i and h <= i, h falls in the wrapped-around portion
   [i, n-1] + [0, x).

   Case 3: Wrap-around, x < i, and h lies beyond x
   [__0__|__1__|__2__|__3__|__4__|__5__|__6__|__7__]
                x:2                     i:6   h:7
   In this case, x < i and h > x, so h is in the range [i, n-1] + [0, x).

   This combined condition (x < i) ^ (h <= i) ^ (h > x) with exclusive OR
   clauses accurately identifies if h is in [i, x), regardless of wrap-around.

*/

static inline bool map_put(struct map* m, const void* p,
                      const size_t h, uint32_t k) {
    const uint32_t mask = m->m;
    size_t i = h;
    #ifdef MAP_STATS
    m->q++;       // queries
    size_t c = 0; // chain
    m->t++;       // total
    #endif
    while (m->p[i]) {
        if ((mask & *((uint32_t*)m->p[i])) == k) {
            #ifdef MAP_STATS
            m->r++; // replacing existing key
            #endif
            m->p[i] = p;
            return true;
        } else {
            i = (i + 1) % m->n;
            #ifdef MAP_STATS
            m->t++;
            if (++c > m->c) { m->c = c; }
            #endif
            assert(i != h); // in the way map is used should not happen
            if (i == h) { return false; }
        }
    }
    #ifdef MAP_STATS
    m->e++;
    #endif
    m->p[i] = p;
    trace("[%2zd] inserted 0x%08X h:%2zd entries %zd\n", i, k, h, m->e);
    return true;
}

static inline bool map_get3(struct map* m, uint32_t b3, size_t* ix) {
    assert((b3 & ~0xFFFFFFu) == 0 && m->m == 0x00FFFFFFu);
    return map_get(m, b3, map_hash(m, b3), ix);
}

static inline bool map_get4(struct map* m, uint32_t b4, size_t* ix) {
    assert(m->m == 0xFFFFFFFFu);
    return map_get(m, b4, map_hash(m, b4), ix);
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

static void sliding(struct map* m3, struct map* m4,
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
            size_t ix;
            b = map_get3(m3, w4 & 0xFFFFFF, &ix);
            if (b && (uint8_t*)m3->p[ix] <= in + i - window) {
                map_remove(m3, ix);
            }
            b = map_get4(m4, w4, &ix);
            if (b) {
                trace("[%2zd] found    0x%08X h:%2zd\n", ix, w4, map_hash(m4, w4));
            } else {
                trace("not  found    0x%08X h:%2zd\n", w4, map_hash(m4, w4));
            }
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
        if (m4->e > window) { map_stats(m4); map_print(m4); }
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
    enum { window = 1 << 16, e = window * 2 };
    // number of entries in the map is 2 times the window size.
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
        sliding(m3, m4, in, countof(in), window);
    }
    {
        uint8_t* data = null;
        size_t bytes = 0;
        errno_t r = file_read_fully("test/bible.txt", &data, &bytes);
        if (r != 0) { return r; }
        printf("bible.txt %zd\n", bytes);
        map_init(m3, p3, e, 3);
        map_init(m4, p4, e, 4);
        sliding(m3, m4, data, bytes, window);
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
        sliding(m3, m4, data, bytes, window);
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
