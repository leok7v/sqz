#include "rt/unstd.h"
#include "rt/file.h"
#include "sqz/sqz.h"

#if 0

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

static double entropy(const struct sqz* s) { // Shannon entropy
    const int32_t n = (int32_t)(sizeof(s->freq) / sizeof(s->freq[0]));
    double total = 0;
    for (int32_t i = 0; i < n; i++) {
        if (s->freq[i] > 1) {
            total += (double)s->freq[i];
        }
    }
    double e = 0.0;
    for (int32_t i = 0; i < n; i++) {
        if (s->freq[i] > 1) {
            double p_i = (double)s->freq[i] / total;
            e -= p_i * log2(p_i);
        }
    }
    return e;
}

static inline void write_byte(struct sqz* s, uint8_t b) {
    if (s->error == 0) {
        size_t written = fwrite(&b, 1, 1, (FILE*)s->stream);
        s->error = written == 1 ? 0 : errno;
//      printf("0x%02X\n", b);
    }
}

static errno_t compress(const char* from, const char* to,
                        const uint8_t* data, size_t bytes) {
    FILE* io = null; // compressed file
    errno_t r = fopen_s(&io, to, "wb") != 0;
    if (r != 0 || io == null) {
        printf("Failed to create \"%s\": %s\n", to, strerror(r));
        return r;
    }
    static struct sqz encoder; // static to avoid >64KB stack warning
    encoder.stream = io;
    encoder.write = write_byte;
    sqz_init_encoder(&encoder);
    sqz_write_header(&encoder, bytes);
    if (encoder.error != 0) {
        r = encoder.error;
        printf("Failed to create \"%s\": %s\n", to, strerror(r));
    } else {
        sqz_compress(&encoder, data, bytes, 1u << window_bits);
        assert(encoder.error == 0);
    }
    errno_t rc = fclose(io) == 0 ? 0 : errno; // error writing buffered output
    if (rc != 0) {
        printf("Failed to flush on file close: %s\n", strerror(rc));
        if (r == 0) { r = rc; }
    }
    if (r == 0) { r = encoder.error; }
    if (r != 0) {
        printf("Failed to compress: %s\n", strerror(r));
    } else {
        char* fn = from == null ? null : strrchr(from, '\\'); // basename
        if (fn == null) { fn = from == null ? null : strrchr(from, '/'); }
        if (fn != null) { fn++; } else { fn = (char*)from; }
        const uint64_t written = encoder.bytes;
        double pc = written * 100.0 / bytes; // percent
        double bps = written * 8.0 / bytes;  // bits per symbol
        printf("bps: %.1f ", bps);
        printf("H: %.1f ", entropy(&encoder));
        if (from != null) {
            printf("%7lld -> %7lld %5.1f%% of \"%s\"\n",
                  (uint64_t)bytes, written, pc, fn);
        } else {
            printf("%7lld -> %7lld %5.1f%%\n", (uint64_t)bytes, written, pc);
        }
    }
    return r;
}

static inline void read_byte(struct sqz* s, uint8_t* b) {
    if (s->error == 0) {
        size_t read = fread(b, 1, 1, (FILE*)s->stream);
        s->error = read == 1 ? 0 : errno;
//      printf("0x%02X\n", *b);
    }
}


static errno_t verify(const char* fn, const uint8_t* input, size_t size) {
    // decompress and compare
    FILE* in = null; // compressed file
    errno_t r = fopen_s(&in, fn, "rb");
    if (r != 0 || in == null) {
        printf("Failed to open \"%s\"\n", fn);
    }
    uint64_t bytes = 0;
    static struct sqz decoder; // static to avoid >64KB stack warning
    decoder.stream = in;
    decoder.read = read_byte;
    if (decoder.error != 0) {
        printf("Failed to read header from \"%s\"\n", fn);
        r = decoder.error;
    }
    sqz_read_header(&decoder, &bytes);
    if (r == 0 && bytes > SIZE_MAX) {
        printf("File too large to decompress\n");
        r = EFBIG;
    }
    if (r == 0) {
        sqz_init_decoder(&decoder);
        assert(bytes == size);
        uint8_t* data = (uint8_t*)calloc(1, (size_t)bytes);
        if (data == null) {
            printf("Failed to allocate memory for decompressed data\n");
            fclose(in);
            return ENOMEM;
        }
        sqz_decompress(&decoder, data, (size_t)bytes);
        fclose(in);
        assert(decoder.error == 0);
        if (decoder.error == 0) {
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
            r = decoder.error;
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
#if 0
    if (r == 0) {
        uint8_t data[4 * 1024] = {0};
        r = test(null, data, sizeof(data));
        // lz77 deals with run length encoding in amazing overlapped way
        for (int32_t i = 0; i < sizeof(data); i += 4) {
            memcpy(data + i, "\x01\x02\x03\x04", 4);
        }
        r = test(null, data, sizeof(data));
    }
#endif
    if (r == 0) {
        const char* data = "Hello World Hello.World Hello World";
        size_t bytes = strlen((const char*)data);
        r = test(null, (const uint8_t*)data, bytes);
    }
#if 0
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
#endif
    return r;
}

#endif // 0

#include "rt/file.h"

static uint8_t squeeze_id[8] = { 's', 'q', 'u', 'e', 'e', 'z', 'e', '4' };


static void put(struct range_coder* rc, uint8_t b) {
    struct sqz* s = (struct sqz*)rc;
    struct io* io = s->that;
    if (rc->error == 0) {
        io_put(io, b);
        rc->error = io->error;
    }
}

static uint8_t get(struct range_coder* rc) {
    struct sqz* s = (struct sqz*)rc;
    struct io* io = s->that;
    uint8_t b = 0;
    if (rc->error == 0) {
        b = io_get(io);
        rc->error = io->error;
    }
    return b;
}

int main(int argc, const char* argv[]) {
    (void)argc; (void)argv; // unused
    static struct sqz squeeze = {0};
    struct sqz* s = &squeeze;
    static uint8_t memory[1024 * 1024];
    struct io io = { 0 };
    io_init(&io, memory, sizeof(memory));
    s->that = &io;
    s->rc.write = put;
    s->rc.read = get;
    const char input[] = "abcd.abcd - Hello World Hello.World Hello World";
    uint64_t bytes = strlen(input);
    uint64_t ecs = 0; // encoder checksum
    {   // compress:
        sqz_init(s);
        io_write(&io, squeeze_id, sizeof(squeeze_id));
        io_put64(&io, bytes);
        sqz_compress(s, input, bytes, 1u << 10);
        ecs = io.checksum;
        io_put64(&io, io.checksum);
        assert(s->rc.error == 0);
        printf("\"%.*s\" 0x%016llX\n", (int)bytes, input, ecs);
    }
    uint64_t dcs = 0; // decoder checksum
    {   // decompress:
        io_rewind(&io);
        uint8_t id[8] = {0};
        io_read(&io, id, sizeof(id));
        swear(memcmp(id, squeeze_id, sizeof(id)) == 0);
        uint64_t written = io_get64(&io);
        uint8_t output[1024] = { 0 };
        sqz_init(s);
        uint64_t k = sqz_decompress(s, output, sizeof(output));
        dcs = io.checksum;
        assert(k == bytes && written == bytes);
        assert(s->rc.error == 0);
        printf("\"%.*s\" 0x%016llX\n", (int)k, output, dcs);
        dcs = io_get64(&io); // checksum
        assert(ecs == dcs);
        bool equal = memcmp(input, output, bytes) == 0;
        assert(equal);
    }
    return 0;
}
