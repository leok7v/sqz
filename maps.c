#define  UNSTD_NO_RT_IMPLEMENTATION
#include "rt/ustd.h"
#include "rt/fileio.h"

enum { max_window = 1u << 16, min_len = 2, max_len = 254 };

struct map { // single threaded use only
    const void** p; // p[n]
    size_t n;
    size_t b;
    uint32_t m;     // mask for 3 or 4 bytes
    size_t e;       // number of entries in the map
    size_t c;       // stats: max chain
    uint64_t q;     // stats: number of put/get queries
    uint64_t t;     // stats: total sum of chains in all queries
};

static void map_init(struct map* m, uint8_t** p, size_t n, size_t b);
static bool map_get(struct map* m, const void* p, size_t* ix);
static bool map_put(struct map* m, const void* p);
static void map_remove(struct map* m, size_t ix);

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

#if 1

static inline uint32_t map_hash_key(uint32_t k, size_t bytes) {
    (void)bytes; // Bob Jenkins' hash function
    k ^= k >> 13;
    k *= 0x85EBCA6Bu;
    k ^= k >> 16;
    return k;
}

#elif 0

static inline uint32_t map_hash_key(uint32_t k, size_t bytes) {
    const uint32_t m    = 0x5BD1E995u; // MurmurHash2A
    const uint32_t seed = 0x85EBCA6Bu;
    uint32_t h = seed ^ bytes;
    uint32_t key_part = 0;
    if (bytes == 4) {
        key_part = k & 0xFFFFFFFFu;
        key_part *= m;
        key_part ^= key_part >> 24;
        key_part *= m;
    } else if (bytes == 3) {
        key_part = k & 0xFFFFFFu;
        key_part *= m;
        key_part ^= key_part >> 24;
        key_part *= m;
    }
    h *= m;
    h ^= key_part;
    // Final mix to ensure good distribution
    h ^= h >> 13;
    h *= m;
    h ^= h >> 15;
    return h;
}

#elif 0

static inline uint32_t map_hash_key(uint32_t k, size_t bytes) { // MurmurHash3
    const uint32_t c1 = 0xcc9e2d51, c2 = 0x1b873593;
    uint32_t h = 0;
    if (bytes == 4) {
        uint32_t k1 = k & 0xFFFFFFFFu;
        k1 *= c1;
        k1 = (k1 << 15) | (k1 >> (32 - 15));
        k1 *= c2;
        h ^= k1;
        h = (h << 13) | (h >> (32 - 13));
        h = h * 5 + 0xe6546b64;
    } else if (bytes == 3) {
        uint32_t k1 = k & 0xFFFFFFu;
        k1 *= c1;
        k1 = (k1 << 15) | (k1 >> (32 - 15));
        k1 *= c2;
        h ^= k1;
        h = (h << 13) | (h >> (32 - 13));
        h = h * 5 + 0xe6546b64;
    }
    h ^= bytes;
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

#else

static inline uint32_t map_hash_key(uint32_t k, size_t bytes) { // xxHash
    const uint32_t prime1 = 0x9e3779b1, prime2 = 0x85ebca77;
    uint32_t h = (uint32_t)bytes + prime1;
    if (bytes == 4) {
        uint32_t k1 = k & 0xFFFFFFFFu;
        k1 *= prime2;
        k1 ^= k1 >> 15;
        k1 *= prime1;
        h ^= k1;
        h = (h << 13) | (h >> (32 - 13));
        h = h * prime1 + prime2;
    } else if (bytes == 3) {
        uint32_t k1 = k & 0xFFFFFFu;
        k1 *= prime2;
        k1 ^= k1 >> 15;
        k1 *= prime1;
        h ^= k1;
        h = (h << 13) | (h >> (32 - 13));
        h = h * prime1 + prime2;
    }
    h ^= h >> 16;
    h *= 0x85ebca6b;
    h ^= h >> 13;
    h *= 0xc2b2ae35;
    h ^= h >> 16;
    return h;
}

#endif

static inline size_t map_hash(struct map* m, const void* p) {
    uint32_t k = (*((uint32_t*)p)) & m->m;
    return (size_t)(map_hash_key(k, m->b) % m->n);
}

static void map_init(struct map* m, uint8_t** p, size_t n, size_t b) {
    assert(16 < n && n <= (1u << 24) && 3 <= b && b <= 4);
    memset(m, 0, sizeof(*m));
    m->p = p;
    m->n = map_prime_under(n);
    m->b = b;
    m->m = b == 3 ? 0xFFFFFFu : UINT32_MAX;
    memset(m->p, 0, n * sizeof(m->p[0]));
}

static inline void map_reduce_chain(struct map* m, size_t i) {
    assert(m->p[i] == map_deleted);
    const size_t n = m->n;
    size_t k = i + 1;
    while (k < i + n && m->p[k % n] == map_deleted) { k++; }
    if (!m->p[k % n]) {
        for (size_t j = i; j < k; j++) { m->p[j % n] = 0; }
    }
}

static inline bool map_eq(const struct map* m, const void* a, const void* b) {
    return (m->m & *((uint32_t*)a)) == (m->m & *((uint32_t*)b));
}

static inline bool map_get_hashed(struct map* m, const void* p, size_t* ix) {
    size_t i = *ix;
    bool reduced = false;
    m->q++;       // stats
    size_t c = 0; // stats: chain
    while (m->p[i]) {
        const bool deleted = m->p[i] == map_deleted;
        if (!deleted && map_eq(m, m->p[i], p)) {
            *ix = i;
            return true;
        } else {
            if (deleted && reduced) {
                map_reduce_chain(m, i);
                reduced = true;
            }
            if (++c > m->c) { m->c = c; } // stats
            i = (i + 1) % m->n;
        }
    }
    return false;
}

static bool map_get(struct map* m, const void* p, size_t* ix) {
    *ix = map_hash(m, p);
    return map_get_hashed(m, p, ix);
}

static void map_remove(struct map* m, size_t ix) {
    assert(m->e > 0);
    m->p[ix] = m->p[(ix + 1) % m->n] ? map_deleted : 0;
    m->e--;
}

static bool map_put(struct map* m, const void* p) {
    size_t d = (size_t)-1; // index of first deleted
    const size_t h = map_hash(m, p);
    size_t i = h;
    m->q++;       // stats
    size_t c = 0; // stats: chain
    while (m->p[i]) {
        const bool deleted = m->p[i] == map_deleted;
        if (!deleted && map_eq(m, m->p[i], p)) {
            m->p[i] = p;
            return true;
        } else {
            if (deleted && d == (size_t)-1) { d = i; }
            i = (i + 1) % m->n;
            m->t++;                       // stats: total
            if (++c > m->c) { m->c = c; } // stats
            if (i == h) { // overflow
                if (d == (size_t)-1) { return false; }
                break; // will put `p` into m->p[d]
            }
        }
    }
    if (d != (size_t)-1) { map_reduce_chain(m, d); }
    m->p[d != (size_t)-1 ? d : i] = p;
    m->e++;
    return true;
}

// tests:

static void map_stats(const struct map* m) {
    double a = m->q == 0 ? 0 : (double)m->t / (double)m->q;
    printf("map stats e:%zu c:%zu q:%zu t:%zu a:%.3f\n",
            m->e, m->c, m->q, m->t, 1.0 + a);
}

static void sliding_window(struct map* m, uint8_t in[], size_t n, size_t window) {
    size_t count = 0;
    bool b = false;
    uint64_t t = nanoseconds();
    // sliding window
    for (size_t i = 0; i < n; i++) {
        if (i >= window) {
            size_t ix;
            b = map_get(m, in + i - window, &ix);
            if (b && (uint8_t*)m->p[ix] <= in + i - window) {
                map_remove(m, ix);
            }
        }
        // lz77 like lookup:
        size_t ix;
        b = map_get(m, in + i, &ix);
        if (b) { count++; }
        // insert:
        if (m->e >= window) { map_stats(m); }
        assert(m->e < window);
        b = map_put(m, in + i);
        if (!b) { map_stats(m); }
        assert(b);
    }
    t = nanoseconds() - t;
    printf("Time: %6.3fs Throughput: %7.3f MiB/s count:%d\n",
            t / 1.0e9, n / (t / 1.0e9) / (1024 * 1024), count);
    map_stats(m);
}

static int test(void) {
//  enum { window = 1 << 4, e = window * 4 };
    enum { window = 1 << 16, e = window * 4, b = 3 };
    static uint8_t* p[e];
    static struct map map;
    struct map* m = &map;
    map_init(m, p, e, b);
    // input:
//  static uint8_t in[8 * 1024 * 1024]; // release
    static uint8_t in[4 * 1024 * 1024]; // debug
    for (size_t i = 0; i < countof(in); i++) {
        in[i] = (uint8_t)(256 * ((double)rand() / ((double)RAND_MAX + 1.0)));
    }
    printf("random\n");
    sliding_window(m, in, countof(in), window);
    // bible:
    uint8_t* data = null;
    size_t bytes = 0;
    errno_t r = file_read_fully("test/bible.txt", &data, &bytes);
    if (r != 0) { return r; }
    map_init(m, p, e, b);
    printf("bible.txt\n");
    sliding_window(m, data, bytes, window);
    free(data);
    // mandrill.png:
    data = null;
    bytes = 0;
    r = file_read_fully("test/mandrill.png", &data, &bytes);
    if (r != 0) { return r; }
    map_init(m, p, e, b);
    printf("mandrill.png\n");
    sliding_window(m, data, bytes, window);
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

test random
sliding_window Time:  0.637s Throughput:   6.282 MiB/s count:16117
map_stats      map stats e:65408 c:205 q:12517376 t:26075707 a:3.083
test           bible.txt
sliding_window Time:  0.033s Throughput: 127.097 MiB/s count:4349567
map_stats      map stats e:3818 c:4 q:13242983 t:27087 a:1.002
test           mandrill.png
sliding_window Time:  0.025s Throughput:  23.531 MiB/s count:4000
map_stats      map stats e:65297 c:53 q:1818152 t:1110237 a:1.611

*/

int main(int argc, const char* argv[]) {
    (void)argc; (void)argv; // unused
    locate_test_folder();
    return test();
}
