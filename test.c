#include "rt/ustd.h"
#include "rt/fileio.h"
#include "sqz/sqz.h"

enum { window_bits = 16 };

// window_bits = 16:
// 4,436,173 -> 1,346,191 30.35% "bible.txt"
// zip: (MS Windows)
// 4,436,173 -> 1,398,871 31.5%  "bible.txt"

// Test is limited to "size_t" and "int" precision

static const char* thousands(uint64_t value) {
    static char text[32][64];
    static size_t ix;
    char* s = text[ix];
    ix = (ix + 1) % (sizeof(text) / sizeof(text[0]));
    size_t i = sizeof(text[0]) - 1;
    s[i] = 0;
    int gc = 0; // group count
    do {
        if (gc == 3) { s[--i] = ','; gc = 0; }
        s[--i] = '0' + (value % 10);
        value /= 10;
        gc++;
    } while (value > 0);
    return &s[i];
}

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

static uint64_t pm_n(struct prob_model* pm) {
    uint64_t n = 0;
    for (size_t i = 0; i < countof(pm->freq); i++) { n += pm->freq[i] > 1; }
    return n;
}

static uint64_t pm_sum(struct prob_model* pm) {
    uint64_t sum = 0;
    for (size_t i = 0; i < countof(pm->freq); i++) {
        if (pm->freq[i] > 1) { sum += pm->freq[i] - 1; }
    }
    return sum;
}

static double pm_match_percentage(uint64_t sum, size_t total) {
    return 100.0 * (double)sum / (double)total;
}

static double pm_stream_percentage(uint64_t sum, double ent,
                                   size_t compressed) {
    return 100.0 * sum * ent / (8.0 * (double)compressed);
}

static double pm_entropy(struct prob_model* pm) {
    return entropy(pm->freq, pm_n(pm));
}

static void dump_entropy(struct sqz* s, int64_t bytes, int64_t compressed) {
    uint64_t total = pm_sum(&s->pm_byte) + pm_sum(&s->pm_len); // number of matches
    printf("of: %s matches %s -> %s\n",
           thousands(total), thousands(bytes), thousands(compressed));
    #pragma push_macro("print_field_entropy")
    #pragma push_macro("print_entropy")
    #define print_field_entropy(name, field) do {                   \
        uint64_t num = pm_n(&s->field);                             \
        double   ent = pm_entropy(&s->field);                       \
        uint64_t sum = pm_sum(&s->field);                           \
        double mp =  pm_match_percentage(sum, total);               \
        double sp =  pm_stream_percentage(sum, ent, compressed);    \
        printf("%-7s[%3d]: %.2f bits %6.2f%% %6.2f%% %10.10s\n",    \
               name, num, ent, mp, sp, thousands(sum));             \
    } while (0)
    #define print_entropy(field) print_field_entropy(#field, field)
    print_entropy(pm_bit0);
    print_entropy(pm_byte);
    print_entropy(pm_len);
    print_entropy(pm_lsb);
    print_entropy(pm_msb);
    for (size_t i = 0; i < countof(s->pm_dist); i++) {
        char name[16] = { 0 };
        snprintf(name, sizeof(name), "pm_dist[%d]", (int)i);
        print_field_entropy(name, pm_dist[i]);
    }
    #pragma pop_macro("print_entropy")
    #pragma pop_macro("print_field_entropy")
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
    uint64_t dt = 0; // elapsed time in nanoseconds;
    struct io out = {0}; // compressed file
    io_create(&out, to);
    if (out.error != 0) {
        printf("Failed to create \"%s\": %s\n", to, strerror(out.error));
        return out.error;
    }
    static struct sqz encoder; // static for testing, can be heap malloc()-ed
    encoder.that = &out;
    encoder.rc.write = put;
    sqz_init(&encoder);
    write_header(&out, bytes);
    if (encoder.rc.error != 0) {
        printf("io_create(\"%s\") failed: %s\n", to, strerror(encoder.rc.error));
    } else {
        uint64_t t = nanoseconds();
        sqz_compress(&encoder, data, bytes, 1u << window_bits);
        dt = nanoseconds() - t;
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
        dump_entropy(&encoder, bytes, out.written);
        char* fn = from == null ? null : strrchr(from, '\\'); // basename
        if (fn == null) { fn = from == null ? null : strrchr(from, '/'); }
        if (fn != null) { fn++; } else { fn = (char*)from; }
        double pc  = out.written * 100.0 / bytes; // percent
        double bps = out.written * 8.0   / bytes; // bits per symbol
        printf("time: %.3fs ", dt / 1.0e9);
        if (from != null) {
            printf("%-11s -> %-11s %6.2f%% of \"%s\" ",
                  thousands(bytes), thousands(out.written), pc, fn);
        } else {
            printf("%-11s -> %-11s %6.2f%% ",
                  thousands(bytes), thousands(out.written), pc);
        }
        printf("bps: %.1f ", bps);
        printf("bitrate: %.1f MiB/s\n", bytes / (dt / 1.0e9) / (1024 * 1024));
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
    sqz_init(&decoder);
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
        uint64_t t = nanoseconds();
        uint64_t decompressed = sqz_decompress(&decoder, out.data, (size_t)bytes);
        t = nanoseconds() - t;
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
        swear(decompressed == bytes);
        swear(decoder.rc.error == 0);
        printf("decompress time: %.3fs bitrate: %.1f MiB/s\n",
               t / 1.0e9, size / (t / 1.0e9) / (1024 * 1024));
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

static errno_t test_file(const char* fn) {
    return file_exist(fn) ? test_compression(fn) : 0;
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
#endif
#if 0
    if (r == 0) {
        const char* d = "Hello World Hello.World Hello World";
        size_t bytes = strlen((const char*)d);
        r = test(null, (const uint8_t*)d, bytes);
    }
    if (r == 0) { // test.c source code:
        r = test_file(__FILE__);
    }
    // argv[0] executable filepath (Windows) or possibly name (Unix)
    if (r == 0) {
        r = test_file(argv[0]);
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
    for (size_t i = 0; i < countof(files) && r == 0; i++) {
        r = test_file(files[i]);
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
    for (size_t i = 0; i < countof(corpus) && r == 0; i++) {
        r = test_file(corpus[i]);
    }
#else
    r = test_file("test/silesia.tar");
//  r = test_file("test/bible.txt");
#endif
    return r;
}

#define rt_implementation
#include "rt/rt.h"


/*

211,087,360 -> 72,000,153   34.11% of "silesia.tar"
compress
time: 20.172s bitrate: 10.0 MiB/s
decompress
time:  4.858s bitrate: 41.4 MiB/s

compare to:
https://github.com/inikep/lzbench/blob/master/lzbench18_sorted.md

dump_entropy of: 49,355,075 matches 211,087,360 -> 72,000,153
dump_entropy pm_bit0[  2]: 0.89 bits 100.00%   7.64% 49,355,075
dump_entropy pm_byte[256]: 7.63 bits  69.12%  45.17% 34,112,827
dump_entropy pm_len [254]: 4.21 bits  30.88%  11.14% 15,242,248
dump_entropy pm_lsb [256]: 7.76 bits  27.07%  17.99% 13,359,925
dump_entropy pm_msb [256]: 6.92 bits  27.07%  16.05% 13,359,925
dump_entropy pm_dist[0][  7]: 2.62 bits   0.23%   0.05%    111,973
dump_entropy pm_dist[1][127]: 5.80 bits   2.62%   1.30%  1,292,475
dump_entropy pm_dist[2][255]: 7.34 bits   0.97%   0.61%    477,874

*/