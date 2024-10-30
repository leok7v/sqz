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

static void insert(struct lz77* lz) {
    const uint8_t* in = lz->in;
    const size_t   n = lz->n;
    const size_t   i = lz->i;
    if (i < n - 2) {
        const uint16_t prefix = in[i] | (in[i + 1] << 8);
        const size_t index = i % lz->w;
//      printf("[%zd] prev[%zd] := %zu\n", i, index, lz->map2[prefix]);
        lz->prev[index]  = lz->map2[prefix];
        lz->map2[prefix] = i + 1;
//      printf("[%zd] map2[\"%c%c\":0x%04x(%u)] := %zu\n", i, in[i+1], in[i], prefix, prefix, index + 1);
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
    if (1 <= i && i < n - 2) {
        size_t len = 0;
        size_t dst = 0;
        const uint8_t* in = lz->in;
        size_t p = lz->map2[in[i] | (in[i + 1] << 8)];
        if (0 < p && i - (p - 1) <= w) {
            p--;
            assert(p < i && i - p <= w);
            size_t max_k = n - i > max_len ? max_len : n - i;
            size_t k = 2;
            while (k < max_k && in[p + k] == in[i + k]) { k++; }
            if (i - p <= w) {
                len = k;
                dst = i - p;
            }
            while (lz->prev[p] > 0 && i - lz->prev[p] <= w) {
                p = lz->prev[p];
                if (i - p > w) { break; }
                if (memcmp(in + p, in + i, len) == 0) {
                    k = 2;
                    while (k < max_k && in[p + k] == in[i + k]) { k++; }
                    if (k > len) {
                        len = k;
                        dst = i - p;
                    }
                }
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
    if (1 < i && i < n - 2) {
//      if (i == 13) rt_breakpoint();
        size_t len = 0;
        size_t dst = 0;
        const uint8_t* in = lz->in;
        size_t j = i - 1;
        size_t min_j = i >= w ? i - w : 0;
        for (;;) {
            size_t max_k = n > max_len ? max_len : n;
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
    printf("%.*s: %zu\n", (int)n, in, n);
    for (size_t i = 0; i < n; i++) {
        printf("%d", i % 10);
    }
    printf("\n");
    for (size_t i = 0; i < n; i++) {
        printf("%c", i % 10 == 0 ? '0' + (i / 10) % 10 : 0x20);
    }
    printf("\n");
    lz->i = 0;
    insert(lz);
    while (lz->i < lz->n) {
        if (1 <= lz->i && lz->i < lz->n - 2) {
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

int main(int argc, const char* argv[]) {
    (void)argc; (void)argv; // unused
    static struct lz77 lz77;
    struct lz77* lz = &lz77;
    const char* in = "aa_aa_aaa_aaaa__aaaaa__";
    const size_t n = strlen(in);
    size_t max_w = n < max_window ? n : max_window;
    for (size_t w = 2; w < max_w; w += w) {
        if (test(lz, (const uint8_t*)in, strlen(in), w)) { return 1; }
    }
    return 0;
}
