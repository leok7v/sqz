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
