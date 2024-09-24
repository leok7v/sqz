#include "rt/rt.h"
#define assert(b, ...) rt_assert(b, __VA_ARGS__)
#define printf(...)    rt_printf(__VA_ARGS__)

#include "shl/sqz/sqz.h"
#include <stdio.h>

static errno_t lorem_ipsum(void) {
    const char* text = "Lorem ipsum dolor sit amet. "
                       "Lorem ipsum dolor sit amet. "
                       "Lorem ipsum dolor sit amet. ";
    static uint8_t compressed[1024]; // 1KB
    uint64_t compressed_size = 0;
    {
        struct bitstream write = { .data = compressed,
                                   .capacity = sizeof(compressed) };
        size_t input_size = strlen(text);
        assert(sizeof(compressed) > input_size * 2);
        sqz_write_header(&write, input_size);
        static struct sqz compress;
        sqz_init(&compress);
        sqz_compress(&compress, &write, text,
                         input_size, 1u << 11); // window_bits: 11 (2KB)
        if (compress.error != 0) {
            printf("Compression error: %d\n", compress.error);
            return compress.error;
        }
        compressed_size = write.bytes;
        printf("%d into %d bytes\n", (int)input_size, (int)compressed_size);
    }
    {
        struct bitstream read = { .data = compressed,
                                  .capacity = compressed_size };
        uint64_t decompressed = 0;
        sqz_read_header(&read, &decompressed);
        static char decompressed_data[1024];
        assert(sizeof(decompressed_data) > decompressed * 2);
        static struct sqz decompress;
        sqz_init(&decompress);
        if (decompressed != strlen(text)) {
            printf("Decompressed size does not match original size\n");
            return EINVAL;
        }
        sqz_decompress(&decompress, &read, decompressed_data,
                                       (size_t)decompressed);
        if (decompress.error != 0) {
            printf("Decompression error: %d\n", decompress.error);
            return decompress.error;
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
