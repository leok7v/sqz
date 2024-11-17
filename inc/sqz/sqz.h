#ifndef sqz_h
#define sqz_h

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum {
    sqz_min_win_bits  =  10,
    sqz_max_win_bits  =  16,
    sqz_min_window    = 1u << sqz_min_win_bits,
    sqz_max_window    = 1u << sqz_max_win_bits
};

// See: posix errno.h https://pubs.opengroup.org/onlinepubs/9699919799/
// Range coder errors can be any values != 0 but for the convenience
// of debugging (e.g. strerror()) and testing de facto errno_t values are used.

// #define sqz_err_io            5 // EIO   : I/O error
// #define sqz_err_too_big       7 // E2BIG : Argument list too long
// #define sqz_err_no_memory    12 // ENOMEM: Out of memory
// #define sqz_err_invalid      22 // EINVAL: Invalid argument
// #define sqz_err_range        34 // ERANGE: Result too large
// #define sqz_err_data         42 // EILSEQ: Illegal byte sequence
// #define sqz_err_unsupported  40 // ENOSYS: Functionality not supported
// #define sqz_err_no_space     55 // ENOBUFS: No buffer space available

struct prob_model  { // probability model
    // TODO: freq[] and tree[] may possibly be collapsed to a single array
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

struct map {
    const void** p; // p[n]
    size_t       n; // number of entries in the map (max_window * 4)
    uint32_t     m; // mask 0xFFFFFF for 3 bytes and 0xFFFFFFFFu for 4 bytes
    uint32_t     padding; // shut up annoying compiler warning
};

struct sqz {
    struct range_coder rc; // must be first field for callbacks
    void*  that;    // convenience for caller i/o override
    void*  padding; // padding for 32-bit compilers with 8 bytes alignment
    struct prob_model  pm_bit0;     // 0..1
    struct prob_model  pm_byte;     // single byte
    struct prob_model  pm_len;      // size: 0..255
    struct prob_model  pm_lsb;      // 0..255 distance least significant byte
    struct prob_model  pm_msb;      // 0..255 distance most  significant byte
    // TODO: we may have 2 types decompressor and compressor
    //       because decompress do not need maps
    size_t prev[sqz_max_window];    // previous `i` of 4 bytes entry
    size_t map2[((size_t)UINT16_MAX) + 1]; // `i` + 1 of 2 bytes
    struct map map3;
    struct map map4;
    // entries for the maps (75% occupancy):
    void* map3e[sqz_max_window + sqz_max_window / 2];
    void* map4e[sqz_max_window + sqz_max_window / 2];
};

// TODO: we need better range coder callback to remove this ugly requirement
static_assert(offsetof(struct sqz, rc) == 0);

#if defined(__cplusplus)
extern "C" {
#endif

void     sqz_init(struct sqz* s, bool compress);
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
