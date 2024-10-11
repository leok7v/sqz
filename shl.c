#include "shl/sqz/sqz.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

static errno_t lorem_ipsum(void) {
    const char* text = "Lorem ipsum dolor sit amet. "
                       "Lorem ipsum dolor sit amet. "
                       "Lorem ipsum dolor sit amet. ";
    static uint8_t compressed[1024]; // 1KB
    uint64_t compressed_size = 0;
    {
        static struct sqz compress;
        compress.bytes = 0;
        compress.data = compressed;
        compress.capacity = sizeof(compressed);
        size_t input_size = strlen(text);
        assert(sizeof(compressed) > input_size * 2);
        sqz_write_header(&compress, input_size);
        sqz_init_encoder(&compress);
        sqz_compress(&compress, text,
                         input_size, 1u << 11); // window_bits: 11 (2KB)
        if (compress.error != 0) {
            printf("Compression error: %d\n", compress.error);
            return compress.error;
        }
        compressed_size = compress.bytes;
        printf("%d into %d bytes\n", (int)input_size, (int)compressed_size);
    }
    {
        static char decompressed_data[1024];
        static struct sqz decompress;
        decompress.bytes = 0;
        decompress.data = decompressed_data;
        decompress.capacity = sizeof(decompressed_data);
        uint64_t decompressed = 0;
        sqz_read_header(&decompress, &decompressed);
        assert(sizeof(decompressed_data) > decompressed * 2);
        sqz_init_decoder(&decompress);
        if (decompressed != strlen(text)) {
            printf("Decompressed size does not match original size\n");
            return EINVAL;
        }
        sqz_decompress(&decompress, decompressed_data,
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
