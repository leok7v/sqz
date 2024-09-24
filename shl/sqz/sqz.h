#ifndef sqz_h
#define sqz_h

#include <errno.h>
#include <stdint.h>

enum {
    sqz_deflate_sym_max   = 284, // maximum literal for length base
    sqz_deflate_distance  = 0x7FFF // maximum position distance
};

enum {
    sqz_min_win_bits  =  10,
    sqz_max_win_bits  =  15,
};

struct huffman_node {
    uint64_t freq;
    uint64_t path;
    int32_t  bits; // least significant root turn
    int32_t  pix;  // parent
    int32_t  lix;  // left
    int32_t  rix;  // right
};

struct huffman {
    struct huffman_node* node;
    int32_t n;
    int32_t next;  // next non-terminal nodes in the tree >= n
    int32_t depth; // max tree depth seen
    int32_t complete; // tree is too deep or freq too high - no more updates
    struct { // stats:
        size_t updates;
        size_t swaps;
        size_t moves;
    } stats;
};

struct bitstream {
    void*    stream; // stream and (data,capacity) are exclusive
    uint8_t* data;
    uint64_t capacity; // data[capacity]
    uint64_t bytes; // number of bytes written or read
    uint64_t b64;   // bit shifting buffer
    int32_t  bits;  // bit count inside b64
    errno_t  error; // sticky error
    errno_t (*write64)(struct bitstream* bs); // write 64 bits from b64
    errno_t (*read64)(struct bitstream* bs);  // read 64 bits to b64
};

struct sqz {
    struct huffman lit; // 0..255 literal bytes; 257-285 length
    struct huffman pos; // positions tree of 1^win_bits
    struct huffman_node lit_nodes[512 * 2 - 1];
    struct huffman_node pos_nodes[32 * 2 - 1];
    struct bitstream* bs;
    errno_t error; // sticky
    uint8_t len_index[sqz_deflate_sym_max  + 1]; // sqz_len_base index
    uint8_t pos_index[sqz_deflate_distance + 1]; // sqz_pos_base index
};

#if defined(__cplusplus)
extern "C" {
#endif

void sqz_init(struct sqz* s);
void sqz_write_header(struct bitstream* bs, uint64_t bytes);
void sqz_compress(struct sqz* s, struct bitstream* bs,
                      const void* data, size_t bytes, uint16_t window);
void sqz_read_header(struct bitstream* bs, uint64_t *bytes);
void sqz_decompress(struct sqz* s, struct bitstream* bs,
                        void* data, size_t bytes);

#if defined(__cplusplus)
} // extern "C"
#endif

#endif // sqz_h

#ifdef sqz_implementation
#ifdef _MSC_VER // cl.exe compiler:
#pragma warning(disable: 4710) // '...': function not inlined
#pragma warning(disable: 4711) // function '...' selected for automatic inline expansion
#pragma warning(disable: 4820) // '...' bytes padding added after data member '...'
#pragma warning(disable: 4996) // The POSIX name for this item is deprecated.
#pragma warning(disable: 5045) // Compiler will insert Spectre mitigation
#pragma warning(disable: 4820) // bytes padding added after data member
#endif

#include "sqz/sqz.h"

#ifndef assert // allows to overide assert in single header lib
#include <assert.h>
#endif
#include <math.h>
#include <stdbool.h>
#include <string.h>

enum {
    sqz_deflate_sym_min  = 257, // minimum literal for length base
    sqz_deflate_pos_max  = 29,  // maximum pos base index
    sqz_deflate_len_min  = 3,
    // value is the same but unrelated to sqz_deflate_sym_min:
    sqz_deflate_len_max  = 257,
};

#ifndef null
#define null ((void*)0) // like null_ptr a bit better than NULL (0)
#endif

#ifndef countof
#define countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

static inline void bitstream_write_bit(struct bitstream* bs, int bit) {
    if (bs->error == 0) {
        bs->b64 <<= 1;
        bs->b64 |= (bit & 1);
        bs->bits++;
        if (bs->bits == 64) {
            if (bs->data != null) {
                if (bs->bytes + 8 > bs->capacity) {
                    bs->error = E2BIG;
                } else {
                    memcpy(bs->data + bs->bytes, &bs->b64, 8);
                }
            } else {
                bs->error = bs->write64(bs);
            }
            if (bs->error == 0) { bs->bytes += 8; }
            bs->bits = 0;
            bs->b64 = 0;
        }
    }
}

static inline void bitstream_write_bits(struct bitstream* bs,
                                        uint64_t data, int32_t bits) {
    while (bits > 0 && bs->error == 0) {
        bitstream_write_bit(bs, data & 1);
        bits--;
        data >>= 1;
    }
}

static inline int bitstream_read_bit(struct bitstream* bs) {
    int bit = 0;
    if (bs->error == 0) {
        if (bs->bits == 0) {
            bs->b64 = 0;
            if (bs->data != null) {
                if (bs->bytes + 8 > bs->capacity) {
                    bs->error = E2BIG;
                } else {
                    memcpy(&bs->b64, bs->data + bs->bytes, 8);
                }
            } else {
                bs->error = bs->read64(bs);
            }
            if (bs->error == 0) { bs->bytes += 8; }
            bs->bits = 64;
        }
        bit = ((int64_t)bs->b64) < 0; // same as (bs->b64 >> 63) & 1;
        bs->b64 <<= 1;
        bs->bits--;
    }
    return bit;
}

static inline uint64_t bitstream_read_bits(struct bitstream* bs, int32_t bits) {
    uint64_t data = 0;
    for (int32_t b = 0; b < bits && bs->error == 0; b++) {
        int bit = bitstream_read_bit(bs);
        if (bit) { data |= ((uint64_t)bit) << b; }
    }
    return data;
}

static inline void bitstream_flush(struct bitstream* bs) {
    while (bs->bits > 0 && bs->error == 0) { bitstream_write_bit(bs, 0); }
}

// Huffman Adaptive Coding https://en.wikipedia.org/wiki/Adaptive_Huffman_coding

static void huffman_update_paths(struct huffman* t, int32_t i) {
    t->stats.updates++;
    const int32_t m = t->n * 2 - 1;
    if (i == m - 1) { t->depth = 0; } // root
    const int32_t  bits = t->node[i].bits;
    const uint64_t path = t->node[i].path;
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

static inline int32_t huffman_swap_siblings(struct huffman* t,
                                            const int32_t i) {
    const int32_t m = t->n * 2 - 1;
    if (i < m - 1) { // not root
        const int32_t pix = t->node[i].pix;
        const int32_t lix = t->node[pix].lix;
        const int32_t rix = t->node[pix].rix;
        if (lix >= 0 && rix >= 0) {
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

static inline void huffman_frequency_changed(struct huffman* t, int32_t i);

static inline void huffman_update_freq(struct huffman* t, int32_t i) {
    const int32_t lix = t->node[i].lix;
    const int32_t rix = t->node[i].rix;
    t->node[i].freq = (lix >= 0 ? t->node[lix].freq : 0) +
                      (rix >= 0 ? t->node[rix].freq : 0);
}

static inline void huffman_move_up(struct huffman* t, int32_t i) {
    const int32_t pix = t->node[i].pix; // parent
    const int32_t gix = t->node[pix].pix; // grandparent
    // Is parent grandparent`s left or right child?
    const bool parent_is_left_child = pix == t->node[gix].lix;
    const int32_t psx = parent_is_left_child ? // parent sibling index
        t->node[gix].rix : t->node[gix].lix;   // aka auntie/uncle
    if (t->node[i].freq > t->node[psx].freq) {
        // Move grandparents left or right subtree to be
        // parents right child instead of 'i'.
        t->stats.moves++;
        t->node[i].pix = gix;
        if (parent_is_left_child) {
            t->node[gix].rix = i;
        } else {
            t->node[gix].lix = i;
        }
        t->node[pix].rix = psx;
        t->node[psx].pix = pix;
        huffman_update_freq(t, pix);
        huffman_update_freq(t, gix);
        huffman_swap_siblings(t, i);
        huffman_swap_siblings(t, psx);
        huffman_swap_siblings(t, pix);
        huffman_update_paths(t, gix);
        huffman_frequency_changed(t, gix);
    }
}

static inline void huffman_frequency_changed(struct huffman* t, int32_t i) {
    const int32_t m = t->n * 2 - 1; (void)m;
    const int32_t pix = t->node[i].pix;
    if (pix == -1) { // `i` is root
        huffman_update_freq(t, i);
        i = huffman_swap_siblings(t, i);
    } else {
        huffman_update_freq(t, pix);
        i = huffman_swap_siblings(t, i);
        huffman_frequency_changed(t, pix);
    }
    if (pix != -1 && t->node[pix].pix != -1 && i == t->node[pix].rix) {
        huffman_move_up(t, i);
    }
}

static bool huffman_insert(struct huffman* t, int32_t i) {
    bool done = true;
    const int32_t root = t->n * 2 - 1 - 1;
    int32_t ipx = root;
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
            ipx = t->node[ipx].lix;
        }
    }
    if (ipx >= t->n) { // not a leaf, inserted
        t->node[ipx].freq++;
        i = huffman_swap_siblings(t, i);
    } else { // leaf
        if (t->next == t->n) {
            done = false; // cannot insert
            t->complete = true;
        } else {
            t->next--;
            int32_t nix = t->next;
            t->node[nix] = (struct huffman_node){
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
    return done;
}

static inline bool huffman_inc_frequency(struct huffman* t, int32_t i) {
    bool done = true;
    if (t->node[i].pix == -1) {
        done = huffman_insert(t, i); // Unseen terminal node.
    } else if (!t->complete && t->depth < 63 && t->node[i].freq < UINT64_MAX - 1) {
        t->node[i].freq++;
        huffman_frequency_changed(t, i);
    } else {
        // ignore future frequency updates
        t->complete = 1;
        done = false;
    }
    return done;
}

static inline void huffman_init(struct huffman* t,
                                struct huffman_node nodes[],
                                const size_t count) {
    // `count` must pow(2, bits_per_symbol) * 2 - 1
    assert(7 <= count && count < INT32_MAX);
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
        t->node[i] = (struct huffman_node){
            .freq = 0, .pix = -1, .lix = -1, .rix = -1, .bits = 0, .path = 0
        };
    }
}

// Deflate https://en.wikipedia.org/wiki/Deflate

static const uint16_t sqz_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, // 257-264
    11, 13, 15, 17,          // 265-268
    19, 23, 27, 31,          // 269-272
    35, 43, 51, 59,          // 273-276
    67, 83, 99, 115,         // 277-280
    131, 163, 195, 227, 258  // 281-285
};

static const uint8_t sqz_len_xb[29] = { // extra bits
    0, 0, 0, 0, 0, 0, 0, 0, // 257-264
    1, 1, 1, 1,             // 265-268
    2, 2, 2, 2,             // 269-272
    3, 3, 3, 3,             // 273-276
    4, 4, 4, 4,             // 277-280
    5, 5, 5, 5, 0           // 281-285 (len = 258 has no extra bits)
};

static const uint16_t sqz_pos_base[30] = {
    1, 2, 3, 4,  // 0-3
    5, 7,        // 4-5
    9, 13,       // 6-7
    17, 25,      // 8-9
    33, 49,      // 10-11
    65, 97,      // 12-13
    129, 193,    // 14-15
    257, 385,    // 16-17
    513, 769,    // 18-19
    1025, 1537,  // 20-21
    2049, 3073,  // 22-23
    4097, 6145,  // 24-25
    8193, 12289, // 26-27
    16385, 24577 // 28-29
};

static const uint8_t sqz_pos_xb[30] = { // extra bits
    0, 0, 0, 0, // 0-3
    1, 1,       // 4-5
    2, 2,       // 6-7
    3, 3,       // 8-9
    4, 4,       // 10-11
    5, 5,       // 12-13
    6, 6,       // 14-15
    7, 7,       // 16-17
    8, 8,       // 18-19
    9, 9,       // 20-21
    10, 10,     // 22-23
    11, 11,     // 24-25
    12, 12,     // 26-27
    13, 13      // 28-29
};

static void sqz_deflate_init(struct sqz* s) {
    uint8_t  j = 0;
    uint16_t n = sqz_len_base[j] + (1u << sqz_len_xb[j]);
    for (int16_t i = 3; i < countof(s->len_index); i++) {
        if (i == n) {
            j++;
            n = sqz_len_base[j] + (1u << sqz_len_xb[j]);
        }
        s->len_index[i] = j;
    }
    j = 0;
    n = sqz_pos_base[j] + (1u << sqz_pos_xb[j]);
    for (int32_t i = 0; i < countof(s->pos_index); i++) {
        if (i == n) {
            j++;
            n = sqz_pos_base[j] + (1u << sqz_pos_xb[j]);
        }
        s->pos_index[i] = j;
    }
}

void sqz_init(struct sqz* s) {
    s->error = 0;
    huffman_init(&s->lit, s->lit_nodes, countof(s->lit_nodes));
    huffman_init(&s->pos, s->pos_nodes, countof(s->pos_nodes));
}

static inline void sqz_write_bit(struct sqz* s, bool bit) {
    if (s->error == 0) {
        bitstream_write_bit(s->bs, bit);
        s->error = s->bs->error;
    }
}

static inline void sqz_write_bits(struct sqz* s,
                                      uint64_t b64, uint8_t bits) {
    if (s->error == 0) {
        bitstream_write_bits(s->bs, b64, bits);
        s->error = s->bs->error;
    }
}

static inline void sqz_write_huffman(struct sqz* s, struct huffman* t,
                                         int32_t i) {
    sqz_write_bits(s, t->node[i].path, (uint8_t)t->node[i].bits);
    (void)huffman_inc_frequency(t, i); // after the path is written
    // (void) because inability to update frequency is OK here
}

static inline void sqz_flush(struct sqz* s) {
    if (s->error == 0) {
        bitstream_flush(s->bs);
        s->error = s->bs->error;
    }
}

void sqz_write_header(struct bitstream* bs, uint64_t bytes) {
    bitstream_write_bits(bs, (uint64_t)bytes, sizeof(uint64_t) * 8);
}

enum { // "nyt" stands for Not Yet Transmitted (see Vitter Algorithm)
    sqz_lit_nyt = sqz_deflate_sym_max + 1,
    sqz_pos_nyt = sqz_deflate_pos_max + 1
};

static inline void sqz_encode_literal(struct sqz* s, uint16_t lit) {
    if (s->lit.node[lit].bits == 0) {
        sqz_write_huffman(s, &s->lit, sqz_lit_nyt);
        sqz_write_bits(s, lit, 9);
        if (!huffman_insert(&s->lit, lit)) { s->error = E2BIG; }
    } else {
        sqz_write_huffman(s, &s->lit, lit);
    }
}

static inline void sqz_encode_len(struct sqz* s, uint16_t len) {
    const uint8_t  i = s->len_index[len];
    const uint16_t b = sqz_len_base[i];
    const uint8_t  x = sqz_len_xb[i];
    const uint16_t c = sqz_deflate_sym_min + i; // length code
    sqz_encode_literal(s, c);
    if (x > 0) { sqz_write_bits(s, (len - b), x); }
}

static inline void sqz_encode_pos(struct sqz* s, uint16_t pos) {
    const uint8_t  i = s->pos_index[pos];
    const uint16_t b = sqz_pos_base[i];
    const uint8_t  x = sqz_pos_xb[i];
    if (s->pos.node[i].bits == 0) {
        sqz_write_huffman(s, &s->pos, sqz_pos_nyt);
        sqz_write_bits(s, i, 5); // 0..29
        if (!huffman_insert(&s->pos, i)) { s->error = E2BIG; }
    } else {
        sqz_write_huffman(s, &s->pos, i);
    }
    if (x > 0) {
        sqz_write_bits(s, (pos - b), x);
    }
}

void sqz_compress(struct sqz* s, struct bitstream* bs,
                      const void* memory, size_t bytes,
                      uint16_t window) {
    const uint8_t* data = (const uint8_t*)memory;
    s->bs = bs;
    if (!huffman_insert(&s->lit, sqz_lit_nyt)) { s->error = EINVAL; }
    if (!huffman_insert(&s->pos, sqz_pos_nyt)) { s->error = EINVAL; }
    sqz_deflate_init(s);
    size_t i = 0;
    while (i < bytes && s->error == 0) {
        size_t len = 0;
        size_t pos = 0;
        // https://en.wikipedia.org/wiki/LZ77_and_LZ78
        if (i >= sqz_deflate_len_min) {
            size_t j = i - 1;
            size_t min_j = i >= window ? i - window + 1 : 0;
            for (;;) {
                const size_t n = bytes - i;
                size_t k = 0;
                while (k < n && data[j + k] == data[i + k] &&
                       k < sqz_deflate_len_max) {
                    k++;
                }
                if (k >= sqz_deflate_len_min && k > len) {
                    len = k;
                    pos = i - j;
                    if (len == sqz_deflate_len_max) { break; }
                }
                if (j == min_j) { break; }
                j--;
            }
        }
        if (len >= sqz_deflate_len_min) {
            sqz_encode_len(s, (uint16_t)len);
            sqz_encode_pos(s, (uint16_t)pos);
            i += len;
        } else {
            uint16_t b = data[i];
            sqz_encode_literal(s, b);
            i++;
        }
    }
    sqz_flush(s);
}

static inline uint64_t sqz_read_bit(struct sqz* s) {
    int bit = 0;
    if (s->error == 0) {
        bit = bitstream_read_bit(s->bs);
        s->error = s->bs->error;
    }
    return bit;
}

static inline uint64_t sqz_read_bits(struct sqz* s, uint32_t n) {
    uint64_t bits = 0;
    if (s->error == 0) {
        bits = bitstream_read_bits(s->bs, n);
    }
    return bits;
}

static inline uint64_t sqz_read_huffman(struct sqz* s, struct huffman* t) {
    const int32_t m = t->n * 2 - 1;
    int32_t i = m - 1; // root
    int bit = (int)sqz_read_bit(s);
    while (s->error == 0) {
        i = bit ? t->node[i].rix : t->node[i].lix;
        if (t->node[i].lix < 0 && t->node[i].rix < 0) { break; } // leaf
        bit = (int)sqz_read_bit(s);
    }
    // (void) because inability to update frequency is OK here:
    if (s->error == 0) { (void)huffman_inc_frequency(t, i); }
    return (uint64_t)i;
}

void sqz_read_header(struct bitstream* bs, uint64_t *bytes) {
    uint64_t b = bitstream_read_bits(bs, sizeof(uint64_t) * 8);
    if (bs->error == 0) { *bytes = b; }
}

static uint16_t sqz_read_length(struct sqz* s, uint16_t lit) {
    const uint8_t base = (uint8_t)(lit - sqz_deflate_sym_min);
    if (base >= countof(sqz_len_base)) {
        s->error = EINVAL;
        return 0;
    } else {
        const uint8_t bits = sqz_len_xb[base];
        if (bits != 0) {
            uint64_t extra = sqz_read_bits(s, bits);
            return s->error == 0 ?
                (uint16_t)sqz_len_base[base] + (uint16_t)extra : 0;
        } else {
            return sqz_len_base[base];
        }
    }
}

static uint32_t sqz_read_pos(struct sqz* s) {
    uint32_t pos = 0;
    uint64_t base = sqz_read_huffman(s, &s->pos);
    if (s->error == 0 && base == sqz_pos_nyt) {
        base = sqz_read_bits(s, 5);
        if (s->error == 0) {
            if (!huffman_insert(&s->pos, (int32_t)base)) {
                s->error = E2BIG;
            }
        }
    }
    if (s->error == 0 && base >= countof(sqz_pos_base)) {
        s->error = EINVAL;
    } else {
        pos = sqz_pos_base[base];
    }
    if (s->error == 0) {
        uint8_t bits  = sqz_pos_xb[base];
        if (bits > 0) {
            uint64_t extra = sqz_read_bits(s, bits);
            if (s->error == 0) { pos += (uint32_t)extra; }
        }
    }
    return pos;
}

void sqz_decompress(struct sqz* s, struct bitstream* bs,
                        void* memory, size_t bytes) {
    uint8_t* data = (uint8_t*)memory;
    s->bs = bs;
    if (!huffman_insert(&s->lit, sqz_lit_nyt)) { s->error = EINVAL; }
    if (!huffman_insert(&s->pos, sqz_pos_nyt)) { s->error = EINVAL; }
    sqz_deflate_init(s);
    size_t i = 0; // output b64[i]
    while (i < bytes && s->error == 0) {
        uint64_t lit = sqz_read_huffman(s, &s->lit);
        if (s->error != 0) { break; }
        if (lit == sqz_lit_nyt) {
            lit = sqz_read_bits(s, 9);
            if (s->error != 0) { break; }
            if (!huffman_insert(&s->lit, (int32_t)lit)) {
                s->error = E2BIG;
                break;
            }
        }
        if (lit <= 0xFF) {
            data[i] = (uint8_t)lit;
            i++;
        } else {
            if (sqz_deflate_sym_min <= lit && lit <= sqz_lit_nyt) {
                uint32_t len = sqz_read_length(s, (uint16_t)lit);
                if (s->error != 0) { break; }
                if (sqz_deflate_len_min <= len && len <= sqz_deflate_len_max) {
                    uint32_t pos = sqz_read_pos(s);
                    if (s->error != 0) { break; }
                    if (0 < pos && pos <= sqz_deflate_distance) {
                        // memcpy() cannot be used on overlapped regions
                        // because it may read more than one byte at a time.
                        uint8_t* d = data - (size_t)pos;
                        const size_t n = i + (size_t)len;
                        while (i < n) { data[i] = d[i]; i++; }
                    } else {
                        s->error = EINVAL;
                    }
                } else {
                    s->error = EINVAL;
                }
            } else {
                s->error = EINVAL;
            }
        }
    }
}

#endif // sqz_implementation

