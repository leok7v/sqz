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

#define sqz_err_io            5 // EIO   : I/O error
#define sqz_err_too_big       7 // E2BIG : Argument list too long
#define sqz_err_no_memory    12 // ENOMEM: Out of memory
#define sqz_err_invalid      22 // EINVAL: Invalid argument
#define sqz_err_range        34 // ERANGE: Result too large
#define sqz_err_data         42 // EILSEQ: Illegal byte sequence
#define sqz_err_unsupported  40 // ENOSYS: Functionality not supported
#define sqz_err_no_space     55 // ENOBUFS: No buffer space available

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
};

struct sqz { // range coder
    struct range_coder rc;
    void*  that;                    // convenience for caller i/o override
    struct prob_model  pm_size;     // size: 0..255
    struct prob_model  pm_byte;     // single byte
    struct prob_model  pm_dist;     // number of bytes in distance - 1: 0,1,2
    struct prob_model  pm_dist1;    // single byte distance
    struct prob_model  pm_dist2[2];
    struct prob_model  pm_dist3[3];
};

static_assert(offsetof(struct sqz, rc) == 0, "rc must be first field of sqz");

#if defined(__cplusplus)
extern "C" {
#endif

void     sqz_init(struct sqz* s);
void     sqz_compress(struct sqz* s, const void* data, uint64_t bytes, uint16_t window);
uint64_t sqz_decompress(struct sqz* s, void* data, uint64_t bytes);

#if defined(__cplusplus)
} // extern "C"
#endif

#endif // sqz_h
