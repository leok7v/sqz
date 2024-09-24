#ifndef squeeze_header_included
#define squeeze_header_included

#include "bitstream.h"
#include "huffman.h"
#include "map.h"


enum {
    squeeze_deflate_sym_min   = 257, // minimum literal for length base
    squeeze_deflate_sym_max   = 284, // maximum literal for length base
    squeeze_deflate_pos_max   = 29,   // maximum pos base index
    squeeze_deflate_len_min   = 3,
    // value is the same but unrelated to squeeze_deflate_sym_min:
    squeeze_deflate_len_max   = 257
};

enum { // "nyt" stands for Not Yet Transmitted (see Vitter Algorithm)
    squeeze_min_win_bits  =  10,
    squeeze_max_win_bits  =  15,
    squeeze_min_map_bits  =  16,
    squeeze_max_map_bits  =  28,
    squeeze_lit_nyt       =  squeeze_deflate_sym_max + 1,
    squeeze_pos_nyt       =  squeeze_deflate_pos_max + 1,
};

// deflate tables

static const uint16_t squeeze_len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10,  // 257-264
    11, 13, 15, 17,           // 265-268
    19, 23, 27, 31,           // 269-272
    35, 43, 51, 59,           // 273-276
    67, 83, 99, 115,          // 277-280
    131, 163, 195, 227, 258   // 281-285
};

static const uint8_t squeeze_len_xb[29] = { // extra bits
    0, 0, 0, 0, 0, 0, 0, 0,   // 257-264
    1, 1, 1, 1,               // 265-268
    2, 2, 2, 2,               // 269-272
    3, 3, 3, 3,               // 273-276
    4, 4, 4, 4,               // 277-280
    5, 5, 5, 5, 0             // 281-285 (len = 258 has no extra bits)
};

static const uint16_t squeeze_pos_base[30] = {
    1, 2, 3, 4,               // 0-3
    5, 7,                     // 4-5
    9, 13,                    // 6-7
    17, 25,                   // 8-9
    33, 49,                   // 10-11
    65, 97,                   // 12-13
    129, 193,                 // 14-15
    257, 385,                 // 16-17
    513, 769,                 // 18-19
    1025, 1537,               // 20-21
    2049, 3073,               // 22-23
    4097, 6145,               // 24-25
    8193, 12289,              // 26-27
    16385, 24577              // 28-29
};

static const uint8_t squeeze_pos_xb[30] = { // extra bits
    0, 0, 0, 0,               // 0-3
    1, 1,                     // 4-5
    2, 2,                     // 6-7
    3, 3,                     // 8-9
    4, 4,                     // 10-11
    5, 5,                     // 12-13
    6, 6,                     // 14-15
    7, 7,                     // 16-17
    8, 8,                     // 18-19
    9, 9,                     // 20-21
    10, 10,                   // 22-23
    11, 11,                   // 24-25
    12, 12,                   // 26-27
    13, 13                    // 28-29
};

typedef struct {
    errno_t error; // sticky
    huffman_tree  lit; // 0..255 literal bytes; 257-285 length
    huffman_tree  pos; // positions tree of 1^win_bits
    huffman_node* lit_nodes; // 512 * 2 - 1
    huffman_node* pos_nodes; //  32 * 2 - 1
    map_type           map;
    map_entry*         map_entry;
    bitstream*         bs;
    uint8_t len_index[squeeze_deflate_sym_max + 1]; // index of squeeze_len_base
    uint8_t pos_index[1u << 15];                    // index of squeeze_pos_base
} squeeze_type;

#define squeeze_size_mul(name, count) (                                 \
    ((uint64_t)(count) >= ((SIZE_MAX / 4) / (uint64_t)sizeof(name))) ?  \
    0 : (size_t)((uint64_t)sizeof(name) * (uint64_t)(count))            \
)

#define squeeze_sizeof(map_bits) (                                      \
    (sizeof(squeeze_type)) +                                            \
    /* lit_nodes: */                                                    \
    squeeze_size_mul(huffman_node, (512uLL * 2ULL - 1ULL)) +            \
    /* pos_nodes: */                                                    \
    squeeze_size_mul(huffman_node, ((1uLL << 5) * 2ULL - 1ULL)) +       \
    /* pos_nodes: */                                                    \
    squeeze_size_mul(map_entry, (1uLL << (map_bits)))                   \
)

typedef struct {
    squeeze_type* (*alloc)(uint8_t map_bits);
    errno_t (*init_with)(squeeze_type* s,
                         void* memory, size_t size, uint8_t map_bits);
    // `win_bits` is a log2 of window size in bytes in range
    // [squeeze_min_win_bits..squeeze_max_win_bits]
    void (*write_header)(bitstream* bs, uint64_t bytes, uint8_t win_bits);
    void (*compress)(squeeze_type* s,
                     bitstream* bs,
                     const uint8_t* data, size_t bytes,
                     uint16_t window);
    void (*read_header)(bitstream* bs, uint64_t *bytes, uint8_t *win_bits);
    void (*decompress)(squeeze_type* s,
                       bitstream* bs,
                       uint8_t* data, size_t bytes);
    void (*free)(squeeze_type* s);
} squeeze_interface;

#if defined(__cplusplus)
extern "C" squeeze_interface squeeze;
#else
extern squeeze_interface squeeze;
#endif

#endif // squeeze_header_included

#if defined(squeeze_implementation) && !defined(squeeze_implemented)

#define squeeze_implemented

#ifndef null
#define null ((void*)0) // like null_ptr better than NULL (0)
#endif

#ifndef countof
#define countof(a) (sizeof(a) / sizeof((a)[0]))
#endif

#ifndef assert
#include <assert.h>
#endif

static void squeeze_deflate_init(squeeze_type* s) {
    uint8_t  j = 0;
    uint16_t n = squeeze_len_base[j] + (1u << squeeze_len_xb[j]);
    for (int16_t i = 3; i < countof(s->len_index); i++) {
        if (i == n) {
            j++;
            n = squeeze_len_base[j] + (1u << squeeze_len_xb[j]);
        }
        s->len_index[i] = j;
    }
    assert(j == countof(squeeze_len_base) - 1);
    j = 0;
    n = squeeze_pos_base[j] + (1u << squeeze_pos_xb[j]);
    for (int32_t i = 0; i < countof(s->pos_index); i++) {
        if (i == n) {
            j++;
            n = squeeze_pos_base[j] + (1u << squeeze_pos_xb[j]);
        }
        s->pos_index[i] = j;
    }
    assert(j == countof(squeeze_pos_base) - 1);
}

static squeeze_type* squeeze_alloc(uint8_t map_bits) {
    const uint64_t bytes = squeeze_sizeof(map_bits);
    squeeze_type* s = (squeeze_type*)calloc(1, (size_t)bytes);
    if (s != null) {
        errno_t r = squeeze.init_with(s, s, bytes, map_bits);
        assert(r == 0);
        if (r != 0) { free(s); s = null; }
    }
    return s;
}

static void squeeze_free(squeeze_type* s) {
    free(s);
}

static errno_t squeeze_init_with(squeeze_type* s, void* memory, size_t size,
                                 uint8_t map_bits) {
    errno_t r = (map_bits == 0 ||
                 squeeze_min_map_bits <= map_bits &&
                 map_bits <= squeeze_max_map_bits) ? 0 : EINVAL;
    assert(r == 0);
    size_t expected = r == 0 ? squeeze_sizeof(map_bits) : 0;
    // 167,936,192 bytes for (win_bits = 11, map_bits = 19)
    assert(size == expected);
    if (r != 0 || memory == null || size != expected) {
        r = EINVAL;
    } else {
        uint8_t* p = (uint8_t*)memory;
        memset(memory, 0, sizeof(squeeze_type));
        p += sizeof(squeeze_type);
        const size_t lit_n = 512; // literals in range [0..286] always 512
        const size_t pos_n = 32;
        const size_t map_n = 1uLL << map_bits;
        const size_t lit_m = lit_n * 2 - 1;
        const size_t pos_m = pos_n * 2 - 1;
        s->lit_nodes = (huffman_node*)p; p += sizeof(huffman_node) * lit_m;
        s->pos_nodes = (huffman_node*)p; p += sizeof(huffman_node) * pos_m;
        s->map_entry = (map_entry*)p;    p += sizeof(map_entry)    * map_n;
        assert(p == (uint8_t*)memory + size);
        huffman_init(&s->lit, s->lit_nodes, lit_m);
        huffman_init(&s->pos, s->pos_nodes, pos_m);
        if (map_bits != 0) {
            map_init(&s->map, s->map_entry, map_n);
        } else { // map is not used in decompress
            memset(&s->map, 0x00, sizeof(s->map));
        }
    }
    return r;
}

static inline void squeeze_write_bit(squeeze_type* s, bool bit) {
    if (s->error == 0) {
        bitstream_write_bit(s->bs, bit);
        s->error = s->bs->error;
    }
}

static inline void squeeze_write_bits(squeeze_type* s,
                                      uint64_t b64, uint8_t bits) {
    if (s->error == 0) {
        bitstream_write_bits(s->bs, b64, bits);
        s->error = s->bs->error;
    }
}

static inline void squeeze_write_huffman(squeeze_type* s, huffman_tree* t,
                                         int32_t i) {
    assert(t != null && t->node != null);
    assert(0 <= i && i < t->n); // leaf symbol (literal)
    assert(1 <= t->node[i].bits && t->node[i].bits < 64);
    squeeze_write_bits(s, t->node[i].path, (uint8_t)t->node[i].bits);
    huffman_inc_frequency(t, i); // after the path is written
}

static inline void squeeze_flush(squeeze_type* s) {
    if (s->error == 0) {
        bitstream_flush(s->bs);
        s->error = s->bs->error;
    }
}

static void squeeze_write_header(bitstream* bs, uint64_t bytes,
                                 uint8_t win_bits) {
    if (win_bits < squeeze_min_win_bits || win_bits > squeeze_max_win_bits) {
        bs->error = EINVAL;
    } else {
        enum { bits64 = sizeof(uint64_t) * 8 };
        bitstream_write_bits(bs, (uint64_t)bytes, bits64);
        enum { bits8 = sizeof(uint8_t) * 8 };
        bitstream_write_bits(bs, win_bits, bits8);
    }
}

static uint8_t squeeze_log2_of_pow2(uint64_t pow2) {
    assert(pow2 > 0 && (pow2 & (pow2 - 1)) == 0);
    if (pow2 > 0 && (pow2 & (pow2 - 1)) == 0) {
        uint8_t bit = 0;
        while (pow2 >>= 1) { bit++; }
        return bit;
    } else {
        return 0xFF; // error
    }
}

static inline void squeeze_encode_literal(squeeze_type* s, const uint16_t lit) {
    assert(0 <= lit && lit <= squeeze_deflate_sym_max);
    if (s->lit.node[lit].bits == 0) {
        assert(s->lit.node[squeeze_lit_nyt].bits != 0);
        squeeze_write_huffman(s, &s->lit, squeeze_lit_nyt);
        squeeze_write_bits(s, lit, 9);
        if (!huffman_insert(&s->lit, lit)) { s->error = E2BIG; }
    } else {
        squeeze_write_huffman(s, &s->lit, lit);
    }
}

static inline void squeeze_encode_len(squeeze_type* s, const uint16_t len) {
    assert(3 <= len && len < sizeof(s->len_index));
    uint8_t  i = s->len_index[len];
    uint16_t b = squeeze_len_base[i];
    uint8_t  x = squeeze_len_xb[i];
    squeeze_encode_literal(s, (uint16_t)(squeeze_deflate_sym_min + i));
    assert(b <= len && len - b <= (uint16_t)(1u << x));
    if (x > 0) { squeeze_write_bits(s, (len - b), x); }
}

static inline void squeeze_encode_pos(squeeze_type* s, const uint16_t pos) {
    assert(0 <= pos && pos <= 0x7FFF);
    uint8_t  i = s->pos_index[pos];
    uint16_t b = squeeze_pos_base[i];
    uint8_t  x = squeeze_pos_xb[i];
    if (s->pos.node[i].bits == 0) {
        assert(s->pos.node[squeeze_pos_nyt].bits != 0);
        squeeze_write_huffman(s, &s->pos, squeeze_pos_nyt);
        squeeze_write_bits(s, i, 5); // 0..29
        if (!huffman_insert(&s->pos, i)) { s->error = E2BIG; }
    } else {
        squeeze_write_huffman(s, &s->pos, i);
    }
    assert(b <= pos && pos - b <= (uint16_t)(1u << x));
    if (x > 0) { squeeze_write_bits(s, (pos - b), x); }
}

// #define SQUEEZE_MAP_STATS // uncomment to print stats

static void squeeze_compress(squeeze_type* s, bitstream* bs,
                             const uint8_t* data, uint64_t bytes,
                             uint16_t window) {
    #ifdef SQUEEZE_MAP_STATS
        static double   map_distance_sum;
        static double   map_len_sum;
        static uint64_t map_count;
        map_distance_sum = 0;
        map_len_sum = 0;
        map_count = 0;
        size_t br_bytes = 0; // source bytes encoded as back references
        size_t li_bytes = 0; // source bytes encoded "as is" literals
    #endif
    s->bs = bs;
    if (!huffman_insert(&s->lit, squeeze_lit_nyt)) { s->error = EINVAL; }
    if (!huffman_insert(&s->pos, squeeze_pos_nyt)) { s->error = EINVAL; }
    squeeze_deflate_init(s);
    size_t i = 0;
    while (i < bytes && s->error == 0) {
        size_t len = 0;
        size_t pos = 0;
        if (i >= 1) {
            size_t j = i - 1;
            size_t min_j = i >= window ? i - window + 1 : 0;
            for (;;) {
                assert((i - j) < window);
                const size_t n = bytes - i;
                size_t k = 0;
                while (k < n && data[j + k] == data[i + k] && k < squeeze_deflate_len_max) {
                    k++;
                }
                if (k >= squeeze_deflate_len_min && k > len) {
                    len = k;
                    pos = i - j;
                    if (len == squeeze_deflate_len_max) { break; }
                }
                if (j == min_j) { break; }
                j--;
            }
        }
        if (s->map.n > 0) {
            int32_t  best = map_best(&s->map, data + i, bytes - i);
            uint32_t best_bytes =
                     best < 0 ? 0 : s->map.entry[best].bytes;
            uint32_t best_distance = best < 0 ?
                     0 : (uint32_t)(data + i - s->map.entry[best].data);
            if (best_distance < 0x7FFF && best_bytes > len && best_bytes > 4) {
                assert(best_bytes >= squeeze_deflate_len_min);
                assert(memcmp(data + i - best_distance, data + i, best_bytes) == 0);
                len = best_bytes;
                pos = best_distance;
                #ifdef SQUEEZE_MAP_STATS
                    map_distance_sum += pos;
                    map_len_sum += len;
                    map_count++;
                #endif
            }
        }
        if (len >= squeeze_deflate_len_min) {
            assert(0 < pos && pos <= 0x7FFF);
            squeeze_encode_len(s, (uint16_t)len);
            squeeze_encode_pos(s, (uint16_t)pos);
            if (s->map.n > 0) {
                map_put(&s->map, data + i, (uint32_t)len);
            }
            i += len;
            #ifdef SQUEEZE_MAP_STATS
                br_bytes += len;
            #endif
        } else {
            #ifdef SQUEEZE_MAP_STATS
            li_bytes++;
            #endif
            squeeze_encode_literal(s, data[i]);
            i++;
        }
    }
    squeeze_flush(s);
    #ifdef SQUEEZE_MAP_STATS
        double br_percent = (100.0 * br_bytes) / (br_bytes + li_bytes);
        double li_percent = (100.0 * li_bytes) / (br_bytes + li_bytes);
        printf("entropy literals: %.2f %.2f%% back references: %.2f %.2f%%\n",
            huffman_entropy(&s->lit), li_percent,
            huffman_entropy(&s->pos), br_percent);
        if (map_count > 0) {
            printf("avg dic distance: %.1f length: %.1f count: %lld\n",
                    map_distance_sum / map_count,
                    map_len_sum / map_count, map_count);
        }
    #endif
}

static inline uint64_t squeeze_read_bit(squeeze_type* s) {
    bool bit = 0;
    if (s->error == 0) {
        bit = bitstream_read_bit(s->bs);
        s->error = s->bs->error;
    }
    return bit;
}

static inline uint64_t squeeze_read_bits(squeeze_type* s, uint32_t n) {
    assert(n <= 64);
    uint64_t bits = 0;
    if (s->error == 0) {
        bits = bitstream_read_bits(s->bs, n);
    }
    return bits;
}

static inline uint64_t squeeze_read_huffman(squeeze_type* s, huffman_tree* t) {
    const int32_t m = t->n * 2 - 1;
    int32_t i = m - 1; // root
    bool bit = squeeze_read_bit(s);
    while (s->error == 0) {
        i = bit ? t->node[i].rix : t->node[i].lix;
        assert(0 <= i && i < m);
        if (t->node[i].lix < 0 && t->node[i].rix < 0) { break; } // leaf
        bit = squeeze_read_bit(s);
    }
    assert(0 <= i && i < t->n); // leaf symbol (literal)
    huffman_inc_frequency(t, i);
    return (uint64_t)i;
}

static void squeeze_read_header(bitstream* bs, uint64_t *bytes,
                                uint8_t *win_bits) {
    uint64_t b = bitstream_read_bits(bs, sizeof(uint64_t) * 8);
    uint64_t w = bitstream_read_bits(bs, sizeof(uint8_t) * 8);
    if (bs->error == 0) {
        if (w < squeeze_min_win_bits || w > squeeze_max_win_bits) {
            bs->error = EINVAL;
        } else if (bs->error == 0) {
            *bytes = b;
            *win_bits = (uint8_t)w;
        }
    }
}

static uint16_t squeeze_read_length(squeeze_type* s, uint16_t lit) {
    const uint8_t base = (uint8_t)(lit - squeeze_deflate_sym_min);
    if (base >= countof(squeeze_len_base)) {
        s->error = EINVAL;
        return 0;
    } else {
        const uint8_t bits = squeeze_len_xb[base];
        if (bits != 0) {
            uint64_t extra = squeeze_read_bits(s, bits);
            assert(squeeze_len_base[base] + extra <= UINT16_MAX);
            return s->error == 0 ?
                (uint16_t)squeeze_len_base[base] + (uint16_t)extra : 0;
        } else {
            return squeeze_len_base[base];
        }
    }
}

static uint32_t squeeze_read_pos(squeeze_type* s) {
    uint32_t pos = 0;
    uint64_t base = squeeze_read_huffman(s, &s->pos);
    if (s->error == 0 && base == squeeze_pos_nyt) {
        base = squeeze_read_bits(s, 5);
        if (s->error == 0) {
            if (!huffman_insert(&s->pos, (int32_t)base)) {
                s->error = E2BIG;
            }
        }
    }
    if (s->error == 0 && base >= countof(squeeze_pos_base)) {
        s->error = EINVAL;
    } else {
        pos = squeeze_pos_base[base];
    }
    if (s->error == 0) {
        uint8_t bits  = squeeze_pos_xb[base];
        if (bits > 0) {
            uint64_t extra = squeeze_read_bits(s, bits);
            if (s->error == 0) { pos += (uint32_t)extra; }
        }
    }
    return pos;
}

static void squeeze_decompress(squeeze_type* s, bitstream* bs,
                               uint8_t* data, uint64_t bytes) {
    s->bs = bs;
    if (!huffman_insert(&s->lit, squeeze_lit_nyt)) { s->error = EINVAL; }
    if (!huffman_insert(&s->pos, squeeze_pos_nyt)) { s->error = EINVAL; }
    squeeze_deflate_init(s);
    size_t i = 0; // output b64[i]
    while (i < bytes && s->error == 0) {
        uint64_t lit = squeeze_read_huffman(s, &s->lit);
        if (s->error != 0) { break; }
        if (lit == squeeze_lit_nyt) {
            lit = squeeze_read_bits(s, 9);
            if (s->error != 0) { break; }
            // TODO: error if insert fails
            if (!huffman_insert(&s->lit, (int32_t)lit)) {
                s->error = E2BIG;
                break;
            }
        }
        if (lit <= 0xFF) {
            data[i] = (uint8_t)lit;
            i++;
        } else {
            assert(squeeze_deflate_sym_min <= lit && lit < squeeze_lit_nyt);
            if (squeeze_deflate_sym_min <= lit && lit <= squeeze_lit_nyt) {
                uint32_t len = squeeze_read_length(s, (uint16_t)lit);
                if (s->error != 0) { break; }
                assert(squeeze_deflate_len_min <= len && len <= squeeze_deflate_len_max);
                if (squeeze_deflate_len_min <= len && len <= squeeze_deflate_len_max) {
                    uint32_t pos = squeeze_read_pos(s);
                    if (s->error != 0) { break; }
                    assert(0 < pos && pos <= 0x7FFF);
                    if (0 < pos && pos <= 0x7FFF) {
                        // Cannot do memcpy() because of overlapped regions.
                        // memcpy() may read more than one byte at a time.
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

#if defined(__cplusplus)
extern "C" { // avoid mangling of global variable "squeeze" by C++ compilers
#endif

squeeze_interface squeeze = {
    .alloc        = squeeze_alloc,
    .init_with    = squeeze_init_with,
    .write_header = squeeze_write_header,
    .compress     = squeeze_compress,
    .read_header  = squeeze_read_header,
    .decompress   = squeeze_decompress,
    .free         = squeeze_free
};

#if defined(__cplusplus)
} // extern "C"
#endif

#endif
