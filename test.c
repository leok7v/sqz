#include "rt/ustd.h"
#include "rt/fileio.h"
#include "sqz/sqz.h"

#undef  SQUEEZE_MAX_WINDOW
#define SQUEEZE_MAX_WINDOW

enum { window_bits = 16 };

// window_bits = 16:
// 4,436,173 -> 1,346,191 30.35% "bible.txt"
// zip: (MS Windows)
// 4,436,173 -> 1,398,871 31.5%  "bible.txt"

// Test is limited to "size_t" and "int" precision

static double entropy(uint64_t* freq, size_t n) { // Shannon entropy
    double total = 0;
    for (size_t i = 0; i < n; i++) {
        if (freq[i] > 1) {
            total += (double)freq[i];
        }
    }
    double e = 0.0;
    for (size_t i = 0; i < n; i++) {
        if (freq[i] > 1) {
            double p_i = (double)freq[i] / total;
            e -= p_i * log2(p_i);
        }
    }
    return e;
}

static size_t pm_n(struct prob_model* pm) {
    size_t n = 0;
    for (size_t i = 0; i < countof(pm->freq); i++) {
        n += pm->freq[i] > 1;
    }
    return n;
}

static size_t pm_sum(struct prob_model* pm) {
    size_t sum = 0;
    for (size_t i = 0; i < countof(pm->freq); i++) {
        if (pm->freq[i] > 1) { sum += pm->freq[i] - 1; }
    }
    return sum;
}

static double pm_percentage(struct prob_model* pm, size_t total) {
    return 100.0 * pm_sum(pm) / (double)total;
}

static double pm_entropy(struct prob_model* pm) {
    return entropy(pm->freq, pm_n(pm));
}

static void dump_entropy(struct sqz* s) {
    size_t total =
        pm_sum(&s->pm_bit0) +
        pm_sum(&s->pm_bit1) +
        pm_sum(&s->pm_bit2) +
        pm_sum(&s->pm_bit3) +
        pm_sum(&s->pm_size) +
        pm_sum(&s->pm_byte) +
        pm_sum(&s->pm_l3d ) +
        pm_sum(&s->pm_lsb ) +
        pm_sum(&s->pm_msb );
    printf("of: %lld matches\n",total);
    #pragma push_macro("print_entropy")
    #define print_entropy(field)                             \
        printf("%-7s[%3d]: %.2f bits %4.1f%% %9lld\n",       \
        #field, pm_n(&s->field), pm_entropy(&s->field),      \
        pm_percentage(&s->field, total), pm_sum(&s->field));
    print_entropy(pm_bit0);
    print_entropy(pm_bit1);
    print_entropy(pm_bit2);
    print_entropy(pm_bit3);
    print_entropy(pm_size);
    print_entropy(pm_byte);
    print_entropy(pm_l3d );
    print_entropy(pm_lsb );
    print_entropy(pm_msb );
    #pragma pop_macro("print_entropy")
}

static uint8_t squeeze_id[8] = { 's', 'q', 'u', 'e', 'e', 'z', 'e', '4' };

static void write_header(struct io* io, uint64_t bytes) {
    io_write(io, squeeze_id, sizeof(squeeze_id));
    io_put64(io, bytes);
}

static void put(struct range_coder* rc, uint8_t b) {
    struct sqz* s = (struct sqz*)rc;
    struct io* io = s->that;
    if (rc->error == 0) {
        io_put(io, b);
        rc->error = io->error;
    }
}

static errno_t compress(const char* from, const char* to,
                        const uint8_t* data, size_t bytes) {
    struct io out = {0}; // compressed file
    io_create(&out, to);
    if (out.error != 0) {
        printf("Failed to create \"%s\": %s\n", to, strerror(out.error));
        return out.error;
    }
    static struct sqz encoder; // static for testing, can be heap malloc()-ed
    encoder.that = &out;
    encoder.rc.write = put;
    sqz_init(&encoder, true);
    write_header(&out, bytes);
    if (encoder.rc.error != 0) {
        printf("io_create(\"%s\") failed: %s\n", to, strerror(encoder.rc.error));
    } else {
        sqz_compress(&encoder, data, bytes, 1u << window_bits);
        if (encoder.rc.error != 0) {
            printf("Failed to compress: %s\n", strerror(encoder.rc.error));
        }
        swear(encoder.rc.error == 0);
    }
    io_close(&out); // error flushing buffered output
    if (encoder.rc.error == 0 && out.error != 0) {
        printf("io_close(\"%s\") failed: %s\n", to, strerror(out.error));
        encoder.rc.error = out.error;
    }
    if (encoder.rc.error == 0) {
        dump_entropy(&encoder);
        char* fn = from == null ? null : strrchr(from, '\\'); // basename
        if (fn == null) { fn = from == null ? null : strrchr(from, '/'); }
        if (fn != null) { fn++; } else { fn = (char*)from; }
        double pc  = out.written * 100.0 / bytes; // percent
        double bps = out.written * 8.0   / bytes; // bits per symbol
        printf("bps: %4.1f ", bps);
        if (from != null) {
            printf("%7lld -> %7lld %6.2f%% of \"%s\"\n",
                  (uint64_t)bytes, out.written, pc, fn);
        } else {
            printf("%7lld -> %7lld %6.2f%%\n\n",
                  (uint64_t)bytes, out.written, pc);
        }
    }
    return encoder.rc.error;
}

static void read_header(struct io* io, uint64_t *bytes) {
    uint8_t id[8] = {0};
    io_read(io, id, sizeof(id));
    *bytes = io_get64(io);
    if (io->error == 0 && memcmp(id, squeeze_id, sizeof(id)) != 0) {
        io->error = EILSEQ;
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

static errno_t verify(const char* fn, const uint8_t* input, size_t size) {
    // decompress and compare
    struct io in = {0}; // compressed file
    io_open(&in, fn);
    if (in.error != 0) {
        printf("Failed to open \"%s\"\n", fn);
        return in.error;
    }
    uint64_t bytes = 0;
    static struct sqz decoder; // static to avoid >64KB stack warning
    sqz_init(&decoder, false);
    decoder.that = &in;
    decoder.rc.read = get;
    read_header(&in, &bytes);
    if (in.error != 0) {
        printf("Failed to read header from \"%s\"\n", fn);
        io_close(&in); // was opened for reading, close will not fail
        decoder.rc.error = in.error;
    } else if (bytes > SIZE_MAX) {
        printf("File too large to decompress\n");
        decoder.rc.error = EFBIG;
    }
    struct io out = {0}; // decompressed file
    if (decoder.rc.error == 0) {
        io_alloc(&out, (size_t)bytes);
        if (out.error != 0) {
            printf("Failed to allocate memory of %lld bytes"
                   " for decompressed data\n", bytes);
            decoder.rc.error = out.error;
        }
        if (out.error == 0 && bytes > size) { out.error = E2BIG; }
        if (out.error != 0) {
            decoder.rc.error = out.error;
        }
    }
    if (decoder.rc.error == 0) {
        swear(bytes == size);
        sqz_decompress(&decoder, out.data, (size_t)bytes);
        if (decoder.rc.error == 0) {
            const bool same = size == bytes &&
                       memcmp(input, out.data, (size_t)bytes) == 0;
            if (!same) {
                int64_t k = -1;
                for (size_t i = 0; i < rt_min(bytes, size) && k < 0; i++) {
                    if (input[i] != out.data[i]) { k = (int64_t)i; }
                }
                printf("compress() and decompress() differ @%d\n", (int)k);
                // ENODATA is not original posix error; it is OpenGroup error
                decoder.rc.error = ENODATA; // or EIO
            }
            swear(same); // to trigger breakpoint while debugging
        }
        swear(decoder.rc.error == 0);
    }
    io_close(&out);
    io_close(&in);
    return out.error;
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
        char cwd[1024] = {0};
        if (!getcwd(cwd, sizeof(cwd))) { return ENOENT; }
        if (strcmp(cwd + 1, ":\\") == 0 || strcmp(cwd, "/") == 0) {
            return ENOENT; // at root
        }
        if (file_chdir("..") != 0) { return errno; }
    }
}


int main(int argc, const char* argv[]) {
    (void)argc; (void)argv; // unused
    printf("Window: 2^%d %d sizeof(size_t): %d sizeof(int): %d\n",
            window_bits, 1u << window_bits, sizeof(size_t), sizeof(int));
    errno_t r = locate_test_folder();
#if 0
    if (r == 0) {
        uint8_t d[4 * 1024] = {0};
        r = test(null, d, sizeof(d));
        // lz77 deals with run length encoding when overlapped
        for (size_t i = 0; i < sizeof(d); i += 4) {
            memcpy(d + i, "\x01\x02\x03\x04", 4);
        }
        r = test(null, d, sizeof(d));
    }
    if (r == 0) {
        const char* d = "Hello World Hello.World Hello World";
        size_t bytes = strlen((const char*)d);
        r = test(null, (const uint8_t*)d, bytes);
    }
#endif
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
        "test/silesia.tar"
    };
    for (size_t i = 0; i < sizeof(files)/sizeof(files[0]) && r == 0; i++) {
        if (file_exist(files[i])) { r = test_compression(files[i]); }
    }
    static const char* corpus[] = {
        "test/corpus/dickens.txt",
        "test/corpus/mozilla.tar",
        "test/corpus/mr.dicom",
        "test/corpus/nci.txt",
        "test/corpus/ooffice.dll",
        "test/corpus/os.db",
        "test/corpus/reymont.pdf",
        "test/corpus/samba.tar",
        "test/corpus/sao.bin",
        "test/corpus/webster.html",
        "test/corpus/x-ray.dicom",
        "test/corpus/xml.tar",
    };
    for (size_t i = 0; i < sizeof(corpus)/sizeof(corpus[0]) && r == 0; i++) {
        if (file_exist(corpus[i])) { r = test_compression(corpus[i]); }
    }
    return r;
}

#define rt_implementation
#include "rt/rt.h"
