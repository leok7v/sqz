#ifndef sqz_h
#define sqz_h

#include <stddef.h>
#include <stdint.h>

enum {
    sqz_min_win_bits  =  10,
    sqz_max_win_bits  =  15
};

// See: posix errno.h https://pubs.opengroup.org/onlinepubs/9699919799/
// Range coder errors can be any values != 0 but for the convenience
// of debugging (e.g. strerror()) and testing de facto
// errno_t values are used.

// #define sqz_err_io            5 // EIO   : I/O error
// #define sqz_err_too_big       7 // E2BIG : Argument list too long
// #define sqz_err_no_memory    12 // ENOMEM: Out of memory
// #define sqz_err_invalid      22 // EINVAL: Invalid argument
// #define sqz_err_range        34 // ERANGE: Result too large
// #define sqz_err_data         42 // EILSEQ: Illegal byte sequence
// #define sqz_err_unsupported  40 // ENOSYS: Functionality not supported
// #define sqz_err_no_space     55 // ENOBUFS: No buffer space available

struct tree_node {
    const  uint8_t*   data;
    struct tree_node* ln;
    struct tree_node* rn;
};

struct tree {
    struct tree_node* root;
    struct tree_node  nodes[(1u << sqz_max_win_bits)];
    struct tree_node* free_list;
    size_t used;
};


struct prob_model  { // probability model
    uint64_t freq[256];
    uint64_t tree[256]; // Fenwick Tree (aka BITS)
};

struct range_coder {
    uint64_t low;
    uint64_t range;
    uint64_t code;
    void    (*write)(struct range_coder*, uint8_t);
    uint8_t (*read)(struct range_coder*);
    int32_t  error; // sticky error (e.g. errno_t from read/write)
    int32_t  padding;
};

struct map_entry {
    const uint8_t* data;
    uint64_t hash;
    int32_t  bytes; // 0 empty, -1 removed
};

struct map {
    struct map_entry* entry;
    uint32_t n;
    uint32_t entries;
    uint32_t max_chain;
    uint32_t max_bytes;
};

struct sqz {
    struct range_coder rc;
    void*  that;                    // convenience for caller i/o override
    struct tree        tree;
    struct prob_model  pm_literal;  // 0..1
    struct prob_model  pm_size;     // size: 0..255
    struct prob_model  pm_byte;     // single byte
    struct prob_model  pm_bits;     // 0..31 number of bits in distance
    struct prob_model  pm_dist[32]; // 0..1 per bit distance probability
    struct map         map;         // caller supplied memory for map
};

static_assert(offsetof(struct sqz, rc) == 0, "rc must be first field of sqz");

#if defined(__cplusplus)
extern "C" {
#endif

void     sqz_init(struct sqz* s, struct map_entry entry[], size_t n);
void     sqz_compress(struct sqz* s, const void* d, size_t b, uint32_t window);
uint64_t sqz_decompress(struct sqz* s, void* data, size_t bytes);

// Because in C arrays are indexed by both positive and negative index values
// for the simplicity of memory handling the compress/decompress is limited
// to less than 2 ^ (sizeof(size_t) * 8 - 1) bytes.
// It is possible to compress/decompress to be able to handle 2^32 - 1 on
// 32-bit platform but probably does not worth the battle.
// Larger files on 32 bit architectures can be handled in chunks.

#if defined(__cplusplus)
} // extern "C"
#endif

#endif // sqz_h
