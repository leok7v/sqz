#ifndef huffman_header_included
#define huffman_header_included

#include <stdint.h>
#include <errno.h>
#ifndef assert
#include <assert.h>
#endif

// Adaptive Huffman Coding
// https://en.wikipedia.org/wiki/Adaptive_Huffman_coding

typedef struct huffman_node {
    uint64_t freq;
    uint64_t path;
    int32_t  bits; // 0 for root
    int32_t  pix;  // parent
    int32_t  lix;  // left
    int32_t  rix;  // right
} huffman_node;

typedef struct huffman_tree {
    huffman_node* node;
    int32_t n;
    int32_t next;  // next non-terminal nodes in the tree >= n
    int32_t depth; // max tree depth seen
    int32_t complete; // tree is too deep or freq too high - no more updates
    // stats:
    struct {
        size_t updates;
        size_t swaps;
        size_t moves;
    } stats;
} huffman_tree;

static inline void   huffman_init(huffman_tree* t, huffman_node nodes[], const size_t m);
static inline bool   huffman_insert(huffman_tree* t, int32_t i);
static inline void   huffman_inc_frequency(huffman_tree* t, int32_t symbol);
static inline double huffman_entropy(const huffman_tree* t); // Shannon entropy (bps)

static void huffman_update_paths(huffman_tree* t, int32_t i) {
    t->stats.updates++;
    const int32_t m = t->n * 2 - 1;
    if (i == m - 1) { t->depth = 0; } // root
    const int32_t  bits = t->node[i].bits;
    const uint64_t path = t->node[i].path;
    assert(bits < (int32_t)sizeof(uint64_t) * 8 - 1);
    assert((path & (~((1ULL << (bits + 1)) - 1))) == 0);
    const int32_t lix = t->node[i].lix;
    const int32_t rix = t->node[i].rix;
    if (lix != -1) {
        t->node[lix].bits = bits + 1;
        t->node[lix].path = path;
        huffman_update_paths(t, lix);
    }
    if (rix != -1) {
        t->node[rix].bits = bits + 1;
        t->node[rix].path = path | (1ULL << bits);
        huffman_update_paths(t, rix);
    }
    if (bits > t->depth) { t->depth = bits; }
}

static inline int32_t huffman_swap_siblings(huffman_tree* t,
                                            const int32_t i) {
    const int32_t m = t->n * 2 - 1;
    assert(0 <= i && i < m);
    if (i < m - 1) { // not root
        const int32_t pix = t->node[i].pix;
        assert(pix >= t->n); // parent (cannot be a leaf)
        const int32_t lix = t->node[pix].lix;
        const int32_t rix = t->node[pix].rix;
        if (lix >= 0 && rix >= 0) {
            assert(0 <= lix && lix < m - 1 && 0 <= rix && rix < m - 1);
            if (t->node[lix].freq > t->node[rix].freq) { // swap
                t->stats.swaps++;
                t->node[pix].lix = rix;
                t->node[pix].rix = lix;
                // because swap changed all path below:
                huffman_update_paths(t, pix);
                return i == lix ? rix : lix;
            }
        }
    }
    return i;
}

static inline void huffman_frequency_changed(huffman_tree* t, int32_t ix);

static inline void huffman_update_freq(huffman_tree* t, int32_t i) {
    const int32_t lix = t->node[i].lix;
    const int32_t rix = t->node[i].rix;
    assert(lix != -1 || rix != -1); // at least one leaf present
    t->node[i].freq = (lix >= 0 ? t->node[lix].freq : 0) +
                      (rix >= 0 ? t->node[rix].freq : 0);
}

static inline void huffman_move_up(huffman_tree* t, int32_t ix) {
    const int32_t pix = t->node[ix].pix; // parent
    assert(pix != -1);
    const int32_t gix = t->node[pix].pix; // grandparent
    assert(gix != -1);
    assert(t->node[pix].rix == ix);
    // Is parent grandparent`s left or right child?
    const bool parent_is_left_child = pix == t->node[gix].lix;
    const int32_t psx = parent_is_left_child ? // parent sibling index
        t->node[gix].rix : t->node[gix].lix;   // aka auntie/uncle
    if (t->node[ix].freq > t->node[psx].freq) {
        // Move grandparents left or right subtree to be
        // parents right child instead of 'i'.
        t->stats.moves++;
        t->node[ix].pix = gix;
        if (parent_is_left_child) {
            t->node[gix].rix = ix;
        } else {
            t->node[gix].lix = ix;
        }
        t->node[pix].rix = psx;
        t->node[psx].pix = pix;
        huffman_update_freq(t, pix);
        huffman_update_freq(t, gix);
        huffman_swap_siblings(t, ix);
        huffman_swap_siblings(t, psx);
        huffman_swap_siblings(t, pix);
        huffman_update_paths(t, gix);
        huffman_frequency_changed(t, gix);
    }
}

static inline void huffman_frequency_changed(huffman_tree* t, int32_t i) {
    const int32_t m = t->n * 2 - 1; (void)m;
    const int32_t pix = t->node[i].pix;
    if (pix == -1) { // `i` is root
        assert(i == m - 1);
        huffman_update_freq(t, i);
        i = huffman_swap_siblings(t, i);
    } else {
        assert(0 <= pix && pix < m);
        huffman_update_freq(t, pix);
        i = huffman_swap_siblings(t, i);
        huffman_frequency_changed(t, pix);
    }
    if (pix != -1 && t->node[pix].pix != -1 && i == t->node[pix].rix) {
        assert(t->node[i].freq >= t->node[t->node[pix].lix].freq);
        huffman_move_up(t, i);
    }
}

static bool huffman_insert(huffman_tree* t, int32_t i) {
    bool done = true;
    const int32_t root = t->n * 2 - 1 - 1;
    int32_t ipx = root;
    assert(t->node[i].pix == -1 && t->node[i].lix == -1 && t->node[i].rix == -1);
    assert(t->node[i].freq == 0 && t->node[i].bits == 0 && t->node[i].path == 0);
    t->node[i].freq = 1;
    while (ipx >= t->n) {
        if (t->node[ipx].rix == -1) {
            t->node[ipx].rix = i;
            t->node[i].pix = ipx;
            break;
        } else if (t->node[ipx].lix == -1) {
            t->node[ipx].lix = i;
            t->node[i].pix = ipx;
            break;
        } else {
            assert(t->node[ipx].lix >= 0);
            assert(t->node[i].freq <= t->node[t->node[ipx].lix].freq);
            ipx = t->node[ipx].lix;
        }
    }
    if (ipx >= t->n) { // not a leaf, inserted
        t->node[ipx].freq++;
        i = huffman_swap_siblings(t, i);
        assert(t->node[ipx].lix == i || t->node[ipx].rix);
        assert(t->node[ipx].freq ==
                (t->node[ipx].rix >= 0 ? t->node[t->node[ipx].rix].freq : 0) +
                (t->node[ipx].lix >= 0 ? t->node[t->node[ipx].lix].freq : 0));
    } else { // leaf
        assert(t->next > t->n);
        if (t->next == t->n) {
            done = false; // cannot insert
            t->complete = true;
        } else {
            t->next--;
            int32_t nix = t->next;
            t->node[nix] = (huffman_node){
                .freq = t->node[ipx].freq,
                .lix = ipx,
                .rix = -1,
                .pix = t->node[ipx].pix,
                .bits = t->node[ipx].bits,
                .path = t->node[ipx].path
            };
            if (t->node[ipx].pix != -1) {
                if (t->node[t->node[ipx].pix].lix == ipx) {
                    t->node[t->node[ipx].pix].lix = nix;
                } else {
                    t->node[t->node[ipx].pix].rix = nix;
                }
            }
            t->node[ipx].pix = nix;
            t->node[ipx].bits++;
            t->node[ipx].path = t->node[nix].path;
            t->node[nix].rix = i;
            t->node[i].pix = nix;
            t->node[i].bits = t->node[nix].bits + 1;
            t->node[i].path = t->node[nix].path | (1ULL << t->node[nix].bits);
            huffman_update_freq(t, nix);
            ipx = nix;
        }
    }
    huffman_frequency_changed(t, i);
    huffman_update_paths(t, ipx);
    assert(t->node[i].freq != 0 && t->node[i].bits != 0);
    return done;
}

static inline void huffman_inc_frequency(huffman_tree* t, int32_t i) {
    assert(0 <= i && i < t->n); // terminal
    // If input sequence frequencies are severely skewed (e.g. Lucas numbers
    // similar to Fibonacci numbers) and input sequence is long enough.
    // The depth of the tree will grow past 64 bits.
    // The first Lucas number that exceeds 2^64 is
    // L(81) = 18,446,744,073,709,551,616 not actually realistic but
    // better be safe than sorry:
    if (t->node[i].pix == -1) {
        huffman_insert(t, i); // Unseen terminal node.
    } else if (!t->complete && t->depth < 63 && t->node[i].freq < UINT64_MAX - 1) {
        t->node[i].freq++;
        huffman_frequency_changed(t, i);
    } else {
        // ignore future frequency updates
        t->complete = 1;
    }
}

static inline double huffman_entropy(const huffman_tree* t) {
    // Shannon entropy
    double total = 0;
    double entropy = 0.0;
    for (int32_t i = 0; i < t->n; i++) { total += (double)t->node[i].freq; }
    for (int32_t i = 0; i < t->n; i++) {
        if (t->node[i].freq > 0) {
            double p_i = (double)t->node[i].freq / total;
            entropy += p_i * log2(p_i);
        }
    }
    return -entropy;
}

static inline void huffman_init(huffman_tree* t,
                                huffman_node nodes[],
                                const size_t count) {
    assert(7 <= count && count < INT32_MAX); // must pow(2, bits_per_symbol) * 2 - 1
    const int32_t n = (int32_t)(count + 1) / 2;
    assert(n > 4 && (n & (n - 1)) == 0); // must be power of 2
    memset(&t->stats, 0x00, sizeof(t->stats));
    t->node = nodes;
    t->n = n;
    const int32_t root = n * 2 - 1;
    t->next = root - 1; // next non-terminal node
    t->depth = 0;
    t->complete = 0;
    for (size_t i = 0; i < count; i++) {
        t->node[i] = (huffman_node){
            .freq = 0, .pix = -1, .lix = -1, .rix = -1, .bits = 0, .path = 0
        };
    }
}

#endif
