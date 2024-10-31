#include "rt/ustd.h"

enum { max_window = 2u << 16, max_len = 254 };

struct lz77_entry {
    size_t p; // position in input[] + 1
    size_t h; // hash
};

struct lz77 {
    const uint8_t* in; // pointer to array of bytes[n]
    size_t   n;        // number of bytes in "in" array
    size_t   i;        // current position in "in" array after window
    size_t   w;        // window size
    size_t   prev[max_window]; // previous "i" of 2 byte prefix
    size_t   map2[1u << (sizeof(uint16_t) * 8)]; // "i" of 2 byte prefixes
};

static void init(struct lz77 *lz, const uint8_t* in, size_t n, size_t window) {
    assert(n > 0);
    assert(2 <= window && window <= max_window);
    assert(((window - 1) & window) == 0); // window is power of 2
    memset(lz, 0, sizeof(*lz));
    lz->in = in;
    lz->n  = n;
    lz->i  = 0;
    lz->w  = window;
}

#ifdef verbose
#define trace(...) printf(__VA_ARGS__)
#else
#define trace(...) ((void)0)
#endif

static void chain(struct lz77 *lz, size_t index) {
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

static void verify_chain(struct lz77 *lz, size_t index) {
    const uint8_t* in = lz->in;
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

static void insert(struct lz77* lz) {
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
        #ifdef verbose
        chain(lz, index);
        #endif
        verify_chain(lz, index);
    }
}

// both function lz77_find_* return
// ml: [2..max_len] inclusive
// md: [1..window] inclusive

static inline void lz77_find_longest_match(struct lz77* lz, size_t *ml, size_t *md) {
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
            size_t k = 2;
            while (k < max_k && in[p + k] == in[i + k]) { k++; }
            if (i - p <= w) {
                len = k;
                dst = i - p;
            }
            size_t d = lz->prev[p % w];
            while (d > 0 && d <= p && p - d < i && i <= p - d + w) {
                p -= d;
                assert(in[p] == in[i] && in[p + 1] == in[i + 1]);
                if (memcmp(in + p, in + i, len) == 0) {
                    k = 2;
                    while (k < max_k && in[p + k] == in[i + k]) { k++; }
                    if (k > len) {
                        len = k;
                        dst = i - p;
                    }
                }
                d = lz->prev[p % w];
            }
            *ml = len;
            *md = dst;
        }
    }
}

static void lz77_linear_search(struct lz77* lz, size_t *ml, size_t *md) {
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

static int test(struct lz77* lz, const uint8_t* in, const size_t n, const size_t w) {
    init(lz, in, strlen((char*)in), w);
    printf("window: %zd\n", w);
    printf("\"%.*s: %zu\"\n ", (int)n, in, n);
    for (size_t i = 0; i < n; i++) {
        printf("%d", i % 10);
    }
    printf("\n ");
    for (size_t i = 0; i < n; i++) {
        printf("%c", i % 10 == 0 ? '0' + (i / 10) % 10 : 0x20);
    }
    printf("\n");
    lz->i = 0;
    insert(lz);
    while (lz->i < lz->n) {
        if (1 <= lz->i && lz->i < lz->n - 1) {
            size_t ml1 = 0, md1 = 0;
            lz77_find_longest_match(lz, &ml1, &md1);
            size_t ml2= 0, md2 = 0;
            lz77_linear_search(lz, &ml2, &md2);
            if (ml1 > 0 || ml2 > 0) {
                if (ml1 != ml2 && md1 != md2) {
                    printf("[%zd] longest %zd:%zd \"%.2s\" \n", lz->i, ml1, md1, lz->in + lz->i);
                    printf("[%zd] linear  %zd:%zd \"%.2s\" \n", lz->i, ml2, md2, lz->in + lz->i);
                }
                assert(ml1 == ml2 && md1 == md2);
                if (ml1 != ml2 || md1 != md2) { return 1; }
                size_t next_i = lz->i + ml1;
                while (lz->i < next_i) {
                    insert(lz);
                    lz->i++;
                }
            } else {
                insert(lz);
                lz->i++;
            }
        } else {
            lz->i++;
        }
    }
    return 0;
}

static int test1(struct lz77* lz) {
    const char* in = "aaaaa";
    return test(lz, (const uint8_t*)in, strlen(in), 2);
}

static int test2(struct lz77* lz) {
    const char* in = "a_aa_aa_aaa_aaaa_aaaaa_aaaa_aaa_aa_a";
    const size_t n = strlen(in);
    size_t max_w = n < max_window ? n : max_window;
    for (size_t w = 2; w < max_w; w += w) {
        if (test(lz, (const uint8_t*)in, strlen(in), w)) { return 1; }
    }
    return 0;
}

static uint64_t seed = 1; // random seed start value (must be odd)

static uint64_t random64(uint64_t* state) {
    // Linear Congruential Generator with inline mixing
    thread_local static bool initialized;
    if (!initialized) { initialized = true; *state |= 1; };
    *state = (*state * 0xD1342543DE82EF95uLL) + 1;
    uint64_t z = *state;
    z = (z ^ (z >> 32)) * 0xDABA0B6EB09322E3uLL;
    z = (z ^ (z >> 32)) * 0xDABA0B6EB09322E3uLL;
    return z ^ (z >> 32);
}

static double rand64(uint64_t *state) { // [0.0..1.0) exclusive to 1.0
    return (double)random64(state) / ((double)UINT64_MAX + 1.0);
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
        const size_t n = strlen(in);
        size_t max_w = n < max_window ? n : max_window;
        for (size_t w = 2; w < max_w; w += w) {
            if (test(lz, (const uint8_t*)in, strlen(in), w)) { return 1; }
        }
    }
    return 0;
}

int main(int argc, const char* argv[]) {
    (void)argc; (void)argv; // unused
    static struct lz77 lz77;
    struct lz77* lz = &lz77;
//  return test1(lz) || test2(lz);
    return test3(lz);
}
