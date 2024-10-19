#include "rt/ustd.h"
#include "shl/sqz/sqz.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

struct {
    uint8_t data[1024];
    size_t  written;
    size_t  bytes;
} io;

static void put(struct range_coder* rc, uint8_t b) {
    (void)rc; // `rc` unused. No bounds check:
    io.data[io.written++] = b;
}

static uint8_t get(struct range_coder* rc) {
    (void)rc; // `rc` unused. No bounds check:
    return io.data[io.bytes++];
}

static errno_t lorem_ipsum(void) {
    const char* text = "Lorem ipsum dolor sit amet. "
                       "Lorem ipsum dolor sit amet. "
                       "Lorem ipsum dolor sit amet. ";
    size_t input_size = strlen(text);
    uint64_t compressed_size = 0;
    {
        static struct sqz compress;
        compress.that = 0;
        assert(sizeof(io.data) > input_size * 2);
        sqz_init(&compress, null, 0);
        compress.rc.write = put;
        // window_bits: 11 (2KB)
        sqz_compress(&compress, text, input_size, 1u << 11);
        if (compress.rc.error != 0) {
            printf("Compression error: %d\n", compress.rc.error);
            return compress.rc.error;
        }
        compressed_size = io.written;
        printf("%d into %d bytes\n", (int)input_size, (int)compressed_size);
    }
    {
        static char decompressed_data[1024];
        static struct sqz decompress;
        assert(sizeof(decompressed_data) > input_size);
        sqz_init(&decompress, null, 0);
        decompress.rc.read = get;
        uint64_t decompressed = sqz_decompress(&decompress, decompressed_data,
                                               input_size);
        if (decompress.rc.error != 0) {
            printf("Decompression error: %d\n", decompress.rc.error);
            return decompress.rc.error;
        } else {
            if (decompressed != strlen(text)) {
                printf("Decompressed size does not match original size\n");
                return EINVAL;
            }
        }
        if (memcmp(decompressed_data, text, (size_t)decompressed) != 0) {
            printf("Decompressed data does not match original data\n");
            return EINVAL;
        }
        printf("Decompression successful.\n");
    }
    return 0;
}

int main(int argc, const char* argv[]) {
    (void)argc; (void)argv;
    return (int)lorem_ipsum();
}

#define sqz_implementation
#include "shl/sqz/sqz.h"
