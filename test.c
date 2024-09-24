#include "rt/rt.h"

#undef assert

#define assert(b, ...) rt_assert(b, __VA_ARGS__)
#define printf(...)    rt_printf(__VA_ARGS__)

#include "rt/file.h"

#include "sqz/sqz.h"

#ifdef SQUEEZE_MAX_WINDOW // maximum window
enum { window_bits = 15 }; // 32KB
#elif defined(DEBUG) || defined(_DEBUG)
enum { window_bits = 10 }; // 1KB
#else
enum { window_bits = 11 }; // 2KB
#endif

// window_bits = 15:
// 4436173 -> 1451352 32.7% of "bible.txt"
// zip: (MS Windows)
// 4436173 -> 1398871 31.5% of "bible.txt"

// Test is limited to "size_t" and "int" precision

static double entropy(const struct huffman* t) { // Shannon entropy
    double total = 0;
    for (int32_t i = 0; i < t->n; i++) { total += (double)t->node[i].freq; }
    double e = 0.0;
    for (int32_t i = 0; i < t->n; i++) {
        if (t->node[i].freq > 0) {
            double p_i = (double)t->node[i].freq / total;
            e -= p_i * log2(p_i);
        }
    }
    return e;
}


static inline errno_t write_file(struct bitstream* bs) {
    size_t written = fwrite(&bs->b64, 8, 1, (FILE*)bs->stream);
    return written == 1 ? 0 : errno;
}

static errno_t compress(const char* from, const char* to,
                        const uint8_t* data, size_t bytes) {
    FILE* out = null; // compressed file
    errno_t r = fopen_s(&out, to, "wb") != 0;
    if (r != 0 || out == null) {
        printf("Failed to create \"%s\": %s\n", to, strerror(r));
        return r;
    }
    static struct sqz s; // static to avoid >64KB stack warning
    sqz_init(&s);
    struct bitstream bs = { .stream = out, .write64 = write_file };
    sqz_write_header(&bs, bytes);
    if (bs.error != 0) {
        r = bs.error;
        printf("Failed to create \"%s\": %s\n", to, strerror(r));
    } else {
        sqz_compress(&s, &bs, data, bytes, 1u << window_bits);
        assert(s.error == 0);
    }
    errno_t rc = fclose(out) == 0 ? 0 : errno; // error writing buffered output
    if (rc != 0) {
        printf("Failed to flush on file close: %s\n", strerror(rc));
        if (r == 0) { r = rc; }
    }
    if (r == 0) { r = s.error; }
    if (r != 0) {
        printf("Failed to compress: %s\n", strerror(r));
    } else {
        char* fn = from == null ? null : strrchr(from, '\\'); // basename
        if (fn == null) { fn = from == null ? null : strrchr(from, '/'); }
        if (fn != null) { fn++; } else { fn = (char*)from; }
        const uint64_t written = bs.bytes;
        double pc = written * 100.0 / bytes; // percent
        double bps = written * 8.0 / bytes;  // bits per symbol
        printf("bps: %.1f ", bps);
        printf("H.lit: %.1f H.pos: %.1f ", entropy(&s.lit), entropy(&s.pos));
        if (from != null) {
            printf("%7lld -> %7lld %5.1f%% of \"%s\"\n",
                  (uint64_t)bytes, written, pc, fn);
        } else {
            printf("%7lld -> %7lld %5.1f%%\n", (uint64_t)bytes, written, pc);
        }
    }
    return r;
}

static inline errno_t read_file(struct bitstream* bs) {
    size_t read = fread(&bs->b64, 8, 1, (FILE*)bs->stream);
    return read == 1 ? 0 : errno;
}

static errno_t verify(const char* fn, const uint8_t* input, size_t size) {
    // decompress and compare
    FILE* in = null; // compressed file
    errno_t r = fopen_s(&in, fn, "rb");
    if (r != 0 || in == null) {
        printf("Failed to open \"%s\"\n", fn);
    }
    struct bitstream bs = { .stream = in, .read64 = read_file };
    uint64_t bytes = 0;
    if (r == 0) {
        sqz_read_header(&bs, &bytes);
        if (bs.error != 0) {
            printf("Failed to read header from \"%s\"\n", fn);
            r = bs.error;
        }
    }
    if (bytes > SIZE_MAX) {
        printf("File too large to decompress\n");
        r = EFBIG;
    }
    if (r == 0) {
        static struct sqz s; // static to avoid >64KB stack warning
        sqz_init(&s);
        assert(bytes == size);
        uint8_t* data = (uint8_t*)calloc(1, (size_t)bytes);
        if (data == null) {
            printf("Failed to allocate memory for decompressed data\n");
            fclose(in);
            return ENOMEM;
        }
        sqz_decompress(&s, &bs, data, (size_t)bytes);
        fclose(in);
        assert(s.error == 0);
        if (s.error == 0) {
            const bool same = size == bytes &&
                       memcmp(input, data, (size_t)bytes) == 0;
            if (!same) {
                int64_t k = -1;
                for (size_t i = 0; i < rt_min(bytes, size) && k < 0; i++) {
                    if (input[i] != data[i]) { k = (int64_t)i; }
                }
                printf("compress() and decompress() differ @%d\n", (int)k);
                // ENODATA is not original posix error but is OpenGroup error
                r = ENODATA; // or EIO
            }
            assert(same); // to trigger breakpoint while debugging
        } else {
            r = s.error;
        }
        free(data);
        if (r != 0) {
            printf("Failed to decompress\n");
        }
    }
    return r;
}

const char* compressed = "~compressed~.bin";

static errno_t test(const char* fn, const uint8_t* data, size_t bytes) {
    errno_t r = compress(fn, compressed, data, bytes);
    if (r == 0) {
        r = verify(compressed, data, bytes);
    }
    (void)remove(compressed);
    return r;
}

static errno_t test_compression(const char* fn) {
    uint8_t* data = null;
    size_t bytes = 0;
    errno_t r = file_read_fully(fn, &data, &bytes);
    if (r != 0) { return r; }
    return test(fn, data, bytes);
}

static errno_t locate_test_folder(void) {
    // on Unix systems with "make" executable usually resided
    // and is run from root of repository... On Windows with
    // MSVC it is buried inside bin/... folder depths
    // on X Code in MacOS it can be completely out of tree.
    // So we need to find the test files.
    for (;;) {
        if (file_exist("test/bible.txt")) { return 0; }
        if (file_chdir("..") != 0) { return errno; }
    }
}

int main(int argc, const char* argv[]) {
    (void)argc; (void)argv; // unused
    printf("Compression Window: 2^%d %d bytes size_t: %d int: %d\n",
            window_bits, 1u << window_bits, sizeof(size_t), sizeof(int));
    errno_t r = locate_test_folder();
    if (r == 0) {
        uint8_t data[4 * 1024] = {0};
        r = test(null, data, sizeof(data));
        // lz77 deals with run length encoding in amazing overlapped way
        for (int32_t i = 0; i < sizeof(data); i += 4) {
            memcpy(data + i, "\x01\x02\x03\x04", 4);
        }
        r = test(null, data, sizeof(data));
    }
    if (r == 0) {
        const char* data = "Hello World Hello.World Hello World";
        size_t bytes = strlen((const char*)data);
        r = test(null, (const uint8_t*)data, bytes);
    }
    if (r == 0 && file_exist(__FILE__)) { // test.c source code:
        r = test_compression(__FILE__);
    }
    // argv[0] executable filepath (Windows) or possibly name (Unix)
    if (r == 0 && file_exist(argv[0])) {
        r = test_compression(argv[0]);
    }
    static const char* files[] = {
        "test/bible.txt",
        "test/hhgttg.txt",
        "test/confucius.txt",
        "test/laozi.txt",
        "test/sqlite3.c",
        "test/arm64.elf",
        "test/x64.elf",
        "test/mandrill.bmp",
        "test/mandrill.png",
    };
    for (int i = 0; i < sizeof(files)/sizeof(files[0]) && r == 0; i++) {
        if (file_exist(files[i])) {
            r = test_compression(files[i]);
        }
    }
    return r;
}
