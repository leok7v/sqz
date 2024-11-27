#include "rt/ustd.h"
#include "rt/fileio.h"
#include "sqz/sqz.h"

// TODO: compress to memory not a file

enum { window_bits = 16 };

// window_bits = 16:
// "bible.txt" 4,436,173
// -> 1,346,191 30.35%
// zip: (MS Windows)
// -> 1,398,871 31.5%
// 7z:
// -> 1,052,529 23.72%

// Tests are limited to "size_t" and "int" precision

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

static bool is_ascii(const uint8_t* s, const size_t n) {
    bool ascii = true;
    for (size_t i = 0; i < n && ascii; i++) {
        ascii = 0x20 <= s[i] && s[i] <= 0x7F;
    }
    return ascii;
}

static void print_ascii_or_hex(const uint8_t* s, const size_t n) {
    if (is_ascii(s, n)) {
        size_t mn = min(n, 32);
        if (mn == n) {
            printf("\"%.*s\"", (int)mn, s);
        } else {
            printf("\"%.*s...\"", (int)mn, s);
        }
    } else {
        printf("0x");
        size_t mn = min(n, 16);
        for (size_t i = 0; i < mn; i++) { printf("%02X", s[i]); }
        if (mn != n) { printf("..."); }
    }
    printf("\n");
}

static const char* esc(const void* a, size_t n) {
    static int ix;
    static char text[16][2 * 1024];
    const char* s = (const char*)a;
    char* d = text[ix];
    size_t j = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == '\r') {
            d[j++] = '\\';
            d[j++] = 'r';
        } else if (s[i] == '\n') {
            d[j++] = '\\';
            d[j++] = 'n';
        } else if (0x20 <= s[i] && s[i] <= 0x7F) {
            d[j++] = s[i];
        } else {
            d[j++] = '\\';
            d[j++] = 'x';
            d[j++] = "0123456789ABCDEF"[(((uint8_t)s[i]) / 16) & 0xF];
            d[j++] = "0123456789ABCDEF"[((uint8_t)s[i]) % 16];
        }
    }
    d[j] = 0;
    ix = (ix + 1) % countof(text);
    return d;
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

static size_t pm_n(struct prob_model* pm) {
    uint64_t n = 0;
    for (size_t i = 0; i < countof(pm->freq); i++) { n += pm->freq[i] > 1; }
    return (size_t)n;
}

static uint64_t pm_sum(struct prob_model* pm) {
    uint64_t sum = 0;
    for (size_t i = 0; i < countof(pm->freq); i++) {
        if (pm->freq[i] > 1) { sum += pm->freq[i] - 1; }
    }
    return sum;
}

static double pm_match_percentage(uint64_t sum, uint64_t total) {
    return 100.0 * (double)sum / (double)total;
}

static double pm_stream_percentage(uint64_t sum, double ent,
                                   uint64_t compressed) {
    return 100.0 * sum * ent / (8.0 * (double)compressed);
}

static double pm_entropy(struct prob_model* pm) {
    return entropy(pm->freq, pm_n(pm));
}

static void dump_entropy(struct sqz* s, int64_t bytes, int64_t compressed) {
    // every match and unmatched byte have bit0 before them
    // number of encoded sequences and single bytes
    uint64_t total = pm_sum(&s->pm_bit0);
    printf("of: %s matches %s -> %s\n",
           thousands(total), thousands(bytes), thousands(compressed));
    #pragma push_macro("print_field_entropy")
    #pragma push_macro("print_entropy")
    #define print_field_entropy(name, field) do {                   \
        size_t num = pm_n(&s->field);                               \
        double ent = pm_entropy(&s->field);                         \
        uint64_t sum = pm_sum(&s->field);                           \
        double mp = pm_match_percentage(sum, total);                \
        double sp = pm_stream_percentage(sum, ent, compressed);     \
        printf("%-7s[%3zd]: %.2f bits %6.2f%% %6.2f%% %10.10s\n",   \
               name, num, ent, mp, sp, thousands(sum));             \
    } while (0)
    #define print_entropy(field) print_field_entropy(#field, field)
    print_entropy(pm_bit0);
    print_entropy(pm_byte);
    print_entropy(pm_tag);
    print_entropy(pm_dist);
    print_entropy(pm_rep);
    print_entropy(pm_len);
    print_entropy(pm_lsb);
    print_entropy(pm_msb);
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
                        const uint8_t* data, size_t bytes,
                        uint32_t window, bool quiet) {
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
        sqz_compress(&encoder, data, bytes, window);
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
    if (encoder.rc.error == 0 && !quiet) {
        dump_entropy(&encoder, bytes, out.written);
        char* fn = from == null ? null : strrchr(from, '\\'); // basename
        if (fn == null) { fn = from == null ? null : strrchr(from, '/'); }
        if (fn != null) { fn++; } else { fn = (char*)from; }
        double pc  = out.written * 100.0 / bytes; // percent
        double bps = out.written * 8.0   / bytes; // bits per symbol
        if (from != null) {
            printf("%-11s -> %-11s %6.2f%% bps: %.1f of \"%s\"\n",
                  thousands(bytes), thousands(out.written), pc, bps, fn);
        } else {
            printf("%-11s -> %-11s %6.2f%% bps: %.1f\n",
                  thousands(bytes), thousands(out.written), pc, bps);
        }
        printf("compress   time: %6.3fs ", dt / 1.0e9);
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

static errno_t verify(const char* fn, const uint8_t* input, size_t size,
                      bool quiet) {
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
        assert(out.data); // to avoid warning
        swear(bytes == size);
        uint64_t t = nanoseconds();
        uint64_t decompressed = sqz_decompress(&decoder, out.data, (size_t)bytes);
        t = nanoseconds() - t;
        if (decompressed != bytes) {
            printf("decompressed: %lld bytes: %lld\n", decompressed, bytes);
        }
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
//          printf("%.*s\n", (int)size, input);
//          printf("%.*s\n", (int)size, out.data);
            swear(same); // to trigger breakpoint while debugging
        } else {
            swear(decoder.rc.error == 0);
        }
        swear(decompressed == bytes);
        swear(decoder.rc.error == 0);
        if (!quiet) {
            printf("decompress time: %6.3fs bitrate: %.1f MiB/s\n",
                   t / 1.0e9, size / (t / 1.0e9) / (1024 * 1024));
        }
    }
    io_close(&out);
    io_close(&in);
    return out.error;
}

const char* compressed = "~compressed~.bin";

static uint64_t seed = 1;

static errno_t compress_and_verify(const char* fn, const
        uint8_t* data, size_t bytes, uint32_t window, bool quiet) {
    errno_t r = compress(fn, compressed, data, bytes, window, quiet);
    if (r == 0) {
        r = verify(compressed, data, bytes, quiet);
    }
    (void)remove(compressed);
    return r;
}

static errno_t test(const char* fn, const uint8_t* data, size_t bytes) {
    bool quiet = strstr(fn, "test_0_to_10");
    return compress_and_verify(fn, data, bytes, 1 << window_bits, quiet);
}

static errno_t read_compress_and_verify(const char* fn) {
    uint8_t* data = null;
    size_t bytes = 0;
    errno_t r = file_read_fully(fn, &data, &bytes);
    if (r != 0) { return r; }
    r = test(fn, data, bytes);
    free(data);
    return r;
}

static errno_t test_file(const char* fn) {
    return file_exist(fn) ? read_compress_and_verify(fn) : 0;
}

static void dump_string_input(const char* s) {
    size_t n = strlen(s);
    printf("%s\n", s);
    for (size_t p = 0; p < n; p++) { printf("%d", p % 10); }
    printf("\n");
    for (size_t p = 0; p < n; p++) {
        if (p % 10 == 9) {
            printf("%c", p < 99 ? 0x20 : '0' + ((p + 1) / 100) % 10);
        } else if (p % 10 == 0) {
            printf("%c", p % 10 != 0 ? 0x20 : '0' + (p / 10) % 10);
        } else {
            printf("%c", 0x20);
        }
    }
    printf("\n");
}

static errno_t test_0_to_10(void) {
    const char* s = "0123456789";
    const size_t n = strlen(s);
    errno_t r = 0;
    for (size_t i = 0; i < n && r == 0; i++) {
        char name[64];
        snprintf(name, sizeof(name), "%s[%d]", __func__, (int)i);
        r = test(name, (uint8_t*)s, i);
    }
    return r;
}

static errno_t test_zeros(void) {
    uint8_t d[4 * 1024] = {0};
    return test(__func__, d, sizeof(d));
}

static errno_t test_rle(void) {
    uint8_t d[4 * 1024] = {0};
    // lz77 deals with run length encoding when overlapped
    for (size_t i = 0; i < sizeof(d); i += 4) {
        memcpy(d + i, "\x00\x01\x02\x03", 4);
    }
    return test(__func__, d, sizeof(d));
}

static errno_t test_short(void) {
    const char* s = "0123abcAA_AA_AA_BBB_BBB_CCCC_CCCC_DDDDD_DDDDD_3210";
    const size_t n = strlen(s);
    return test(__func__, (uint8_t*)s, n);
}

static errno_t test_hello(void) {
    const char* s = "Hello World Hello.World Hello World";
    size_t n = strlen(s);
    return  test(__func__, (const uint8_t*)s, n);
}

static errno_t test_long(void) {
    static uint8_t d[128 * 1024];
    memset(d, 0x20, sizeof(d));
    for (size_t k = 0; k < sizeof(d); k ++) {
        uint8_t base = (uint8_t)(rand64(&seed) * 26);
        uint8_t len  = (uint8_t)(rand64(&seed) * 16);
        size_t  start = (size_t)(random64(&seed) % (sizeof(d) - len));
        for (int i = 0; i < len; i++) {
            d[start + i] = 0x20 + (base + i) % (128 - 32);
        }
    }
sqz_debug = true;
    return test(__func__, d, sizeof(d));
}

static errno_t debug_in_window(const char* name, const char* s, uint32_t w) {
    size_t n = strlen(s);
    sqz_debug = true;
    dump_string_input(s);
    const uint8_t* d = (const uint8_t*)s;
    errno_t r = 0;
    r = compress_and_verify(name, d, n, w, !sqz_debug);
    sqz_debug = false;
    return r;
}

static errno_t test_case_1(void) { // good!
//  const char* s = "ABCD1.ABCD2,ABCD3;ABCD4:ABCD5`ABCD6#ABCD7%ABCD8_ABCD9-"
//                  "ABCDo.ABCDn,ABCDk;ABCDj:ABCDi`ABCDh#ABCDg%ABCDf_ABCDe-"
//                  "ABCD1.ABCD2,ABCD1;ABCD2:ABCD3`ABCD2#ABCD1%ABCD0_ABCDx-";
    const char* s =
        "ABCD1,0,ABCD1+1+ABCD1+2_ABCD1_1_abcd1_4-ABCD1+5_1234";
    //   012345678901234567890123456789012345678901234567890123456789012
    //   0         1         2         3         4         5         6
    return debug_in_window(__func__, s, 32);
}

static errno_t test_case_2(void) { // good
    const char* s =
        "ABCD1,0,ABCD1+1+ABCD3+2_ABCD1_1_abcd1_4-ABCD1+5_1234";
    //   012345678901234567890123456789012345678901234567890123456789012
    //   0         1         2         3         4         5         6
    return debug_in_window(__func__, s, 32);
}

static errno_t test_case_3(void) { // good
    const char* s =
        "STUVWJKLMNOPQRRYZQRLMNNNNNSRSTUVWTUVWNOPQRNOPQRNOPQVWXBVWXYZDGH";
    //   012345678901234567890123456789012345678901234567890123456789012
    //   0         1         2         3         4         5         6
    return debug_in_window(__func__, s, 32);
}

static errno_t test_case_4(void) {
    const char* s = // 7 long previous search chain
        "HIJDEFGGGGGGGGGGGEFGHGGGGGGGGGGGIIJTUVWXYZCDECDECTCDECYZAABCDEF";
    //   012345678901234567890123456789012345678901234567890123456789012
    //   0         1         2         3         4         5         6
    return debug_in_window(__func__, s, 32);
}

static errno_t test_case_5(void) {
    const char* s =
        "FGHIJKLMNOTUVWXYZABCKXMXJKLMNOPLMJKLMNWXYTUVJKLMNVWMNOKLMNOPQRD";
    //   012345678901234567890123456789012345678901234567890123456789012
    //   0         1         2         3         4         5         6
    return debug_in_window(__func__, s, 32);
}

static errno_t test_case_6(void) {
    const char* s =
        "VWXHQRSTUVWXGHIJKDEFDEFZLMCDHIHIHICDYQRSTUVWXGHFGHIJKFIJKBCQRST";
    //   012345678901234567890123456789012345678901234567890123456789012
    //   0         1         2         3         4         5         6
    return debug_in_window(__func__, s, 32);
}

static errno_t test_permutations(void) {
    uint8_t d[64]; // [256]
    errno_t r = 0;
    for (int j = 0; j < 1024 && r == 0; j++) {
        memset(d, 0x20, sizeof(d));
        for (size_t k = 0; k < sizeof(d); k ++) {
            uint8_t base = (uint8_t)(rand64(&seed) * 26);
            uint8_t len  = 2 + (uint8_t)(rand64(&seed) * 9);
            size_t  first = (size_t)(random64(&seed) % (sizeof(d) - len));
            for (int i = 0; i < len; i++) {
                d[first + i] = 'A' + (base + i) % 26;
            }
            size_t  second = first + len + (int)(random64(&seed) * 3);
            for (int i = 0; i < len; i++) {
                d[(second + i) % sizeof(d)] = d[(first + i) % sizeof(d)];
            }
            size_t  third = second + len + (int)(random64(&seed) * 3);
            for (int i = 0; i < len; i++) {
                d[(third + i) % sizeof(d)] = d[(first + i) % sizeof(d)];
            }
        }
        d[countof(d) - 1] = 0;
        size_t n = strlen((const char*)d);
        sqz_debug = j == 3;
        if (sqz_debug) { dump_string_input((const char*)d); }
        r = compress_and_verify(__func__, d, n, 32, !sqz_debug);
    }
    return r;
}

static errno_t test_files(void) {
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
    errno_t r = 0;
    for (size_t i = 0; i < countof(files) && r == 0; i++) {
        r = test_file(files[i]);
    }
    return r;
}

static errno_t test_corpus(void) {
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
    errno_t r = 0;
    for (size_t i = 0; i < countof(corpus) && r == 0; i++) {
        r = test_file(corpus[i]);
    }
    return r;
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
    printf("Window: 2^%d %d sizeof(size_t): %d sizeof(int): %d sizeof(long): "
           "%d sizeof(long long): %d\n",
            window_bits, 1u << window_bits, sizeof(size_t), sizeof(int),
            sizeof(long), sizeof(long long));
    errno_t r = locate_test_folder();
#if 0
    if (r == 0) { r = test_case_1(); }
    if (r == 0) { r = test_case_2(); }
    if (r == 0) { r = test_case_3(); }
    if (r == 0) { r = test_case_4(); }
    if (r == 0) { r = test_case_5(); }
    if (r == 0) { r = test_case_6(); }
    if (r == 0) { r = test_permutations(); }
    if (r == 0) { r = test_0_to_10(); }
    if (r == 0) { r = test_zeros(); }
    if (r == 0) { r = test_rle(); }
    if (r == 0) { r = test_hello(); }
    if (r == 0) { r = test_short(); }
    if (r == 0) { r = test_long(); }
    if (r == 0) { r = test_file(__FILE__); } // test.c source code:
    // argv[0] executable filepath (Windows) or possibly name (Unix)
    if (r == 0) { r = test_file(argv[0]); }
    if (r == 0) { r = test_files(); }
    if (r == 0) { r = test_corpus(); }
#else
//  if (r == 0) { r = test_0_to_10(); }
//  if (r == 0) { r = test_case_1(); }
//  if (r == 0) { r = test_case_2(); }
//  if (r == 0) { r = test_case_3(); }
//  if (r == 0) { r = test_case_4(); }
//  if (r == 0) { r = test_case_5(); }
//  if (r == 0) { r = test_case_6(); }
//  if (r == 0) { r = test_permutations(); }
//  if (r == 0) { r = test_zeros(); }
//  if (r == 0) { r = test_rle(); }
//  if (r == 0) { r = test_hello(); }
//  if (r == 0) { r = test_short(); }
//  if (r == 0) { r = test_long(); }
//  if (r == 0) { r = test_file(__FILE__); } // test.c source code:
//  if (r == 0) { r = test_file("test/bible.txt"); }
//  if (r == 0) { r = test_file("test/hhgttg.txt"); }
//  if (r == 0) { r = test_file("test/arm64.elf"); }
//  if (r == 0) { r = test_file("test/corpus/mozilla.tar"); }
    if (r == 0) { r = test_file("test/silesia.tar"); }
#endif
    return r;
}

#define rt_implementation
#include "rt/rt.h"

/*

211,087,360 -> 69,801,693   33.07% of "silesia.tar"
compress   time: 21.834s bitrate: 9.2 MiB/s
decompress time:  5.228s bitrate: 38.5 MiB/s

compare to:
https://github.com/inikep/lzbench/blob/master/lzbench18_sorted.md

*/