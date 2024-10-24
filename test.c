#include "rt/ustd.h"
#include "rt/fileio.h"
#include "sqz/sqz.h"
#include "rt/rt_generics_test.h"

#undef  SQUEEZE_MAX_WINDOW
#define SQUEEZE_MAX_WINDOW

#ifdef SQUEEZE_MAX_WINDOW // maximum window
enum { window_bits = 16 };
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
    static struct map_entry me[32 * 1024 * 1024];
    encoder.that = &out;
    encoder.rc.write = put;
    sqz_init(&encoder, me, sizeof(me) / sizeof(me[0]));
//  encoder.map.n = 0;
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
        char* fn = from == null ? null : strrchr(from, '\\'); // basename
        if (fn == null) { fn = from == null ? null : strrchr(from, '/'); }
        if (fn != null) { fn++; } else { fn = (char*)from; }
        double pc  = out.written * 100.0 / bytes; // percent
        double bps = out.written * 8.0   / bytes; // bits per symbol
        printf("bps: %4.1f ", bps);
        if (from != null) {
            printf("%7lld -> %7lld %6.2f%% of \"%s\"\n\n",
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
    sqz_init(&decoder, null, 0);
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
        if (file_chdir("..") != 0) { return errno; }
    }
}

static void experiment(void);

int main(int argc, const char* argv[]) {
    (void)argc; (void)argv; // unused
    experiment();
    rt_test_generics();
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
    if (r == 0 && file_exist(__FILE__)) { // test.c source code:
        r = test_compression(__FILE__);
    }
    // argv[0] executable filepath (Windows) or possibly name (Unix)
    if (r == 0 && file_exist(argv[0])) {
        r = test_compression(argv[0]);
    }
#endif
    static const char* files[] = {
        "test/bible.txt",
        "test/hhgttg.txt",
        "test/confucius.txt",
        "test/laozi.txt",
        "test/sqlite3.c",
//      "test/arm64.elf",
//      "test/x64.elf",
//      "test/mandrill.bmp",
//      "test/mandrill.png",
    };
    for (int i = 0; i < sizeof(files)/sizeof(files[0]) && r == 0; i++) {
        if (file_exist(files[i])) {
            r = test_compression(files[i]);
        }
    }
    return r;
}

///
#if 1

enum { window = 8, min_size = 2, max_size = 254 };

static size_t tree_node_count(struct tree_node* n) {
    return n == NULL ? 0 :
        1 + tree_node_count(n->ln) + tree_node_count(n->rn);
}

static void tree_print_node(char kind, struct tree_node* n,
        struct tree_node* parent, size_t indent, const uint8_t* p) {
    if (n != NULL) {
        for (size_t i = 0; i < indent; i++) printf("  ");
        const size_t distance = p - n->data; // from current position 'p'
        if (parent == NULL) {
            printf("%c [%zu]'%s' %p\n", kind, distance, n->data, n->data);
        } else {
            const size_t pd = p - parent->data; // parent distance
            printf("%c p:%zu [%zu]'%s' %p\n", kind, pd, distance, n->data, n->data);
        }
        tree_print_node('L', n->ln, n, indent + 1, p);
        tree_print_node('R', n->rn, n, indent + 1, p);
    }
}

static void tree_dump_node(struct tree_node* n, const uint8_t* p) {
    if (n != NULL) {
        tree_dump_node(n->ln, p);
        printf("[%2zu]'%s'\n", p - n->data, n->data);
        tree_dump_node(n->rn, p);
    }
}

static void tree_dump(struct tree* t, const uint8_t* p) {
    tree_dump_node(t->root, p);
}

static void tree_print(struct tree* t, const uint8_t* p) {
    tree_print_node(' ', t->root, NULL, 0, p);
    printf("\n");
    tree_dump(t, p);
    printf("%zd nodes\n\n", tree_node_count(t->root));
}


static void tree_init(struct tree* t) {
    t->used = 0;
    t->root = NULL;
    t->free_list = NULL;
}

static struct tree_node* tree_alloc(struct tree* t) {
    if (t->free_list == NULL) {
        if (t->used >= sizeof(t->nodes) / sizeof(t->nodes[0])) {
            printf("Tree pool overflow!\n");
            swear(t->used < sizeof(t->nodes) / sizeof(t->nodes[0]));
            return NULL;
        }
        return &t->nodes[t->used++];
    } else {
        struct tree_node* n = t->free_list;
        t->free_list = t->free_list->rn;
        return n;
    }
}

static inline void tree_free(struct tree* t, struct tree_node* n) {
    n->rn = t->free_list;
    t->free_list = n;
}

static inline struct tree_node* tree_successor(struct tree_node* n) {
    while (n->ln != NULL) { n = n->ln; }
    return n;
}

static struct tree_node* tree_delete_node(struct tree* t, struct tree_node* n) {
    if (n->ln == NULL) {
        struct tree_node* rn = n->rn;
        printf("tree_free(%p)\n", n->data);
        tree_free(t, n);
        n = rn;
    } else if (n->rn == NULL) {
        struct tree_node* ln = n->ln;
        printf("tree_free(%p)\n", n->data);
        tree_free(t, n);
        n = ln;
    } else {
        struct tree_node* min = tree_successor(n->rn);
        const uint8_t* tmp = n->data;
        n->data = min->data;
        min->data = tmp;
        n->rn = tree_delete_node(t, n->rn);
    }
    return n;
}

static struct tree_node* tree_evict_node(struct tree* t, struct tree_node* n,
                                         const uint8_t* start) {
    if (n != NULL) {
        if (n->data < start) {
            n = tree_delete_node(t, n);
        } else { // brutally inefficient whole tree walk
            n->ln = tree_evict_node(t, n->ln, start);
            n->rn = tree_evict_node(t, n->rn, start);
        }
    }
    return n;
}

static void tree_evict(struct tree* t, const uint8_t* start) {
    t->root = tree_evict_node(t, t->root, start);
}

static inline void tree_insert(struct tree* t, const uint8_t* p, size_t bytes) {
    struct tree_node* y = NULL;
    struct tree_node* x = t->root;
    struct tree_node* z = tree_alloc(t);
    swear(z != NULL);
    if (z != NULL) {
        z->data = p; z->ln = z->rn = NULL;
        while (x != NULL) {
            y = x;
            int cmp = memcmp(p, x->data, bytes);
            if (cmp < 0) {
                x = x->ln;
            } else {
                x = x->rn;
            }
        }
        if (y == NULL) {
            t->root = z;
        } else {
            int cmp = memcmp(p, y->data, bytes);
            if (cmp < 0) {
                y->ln = z;
            } else {
                y->rn = z;
            }
        }
    }
}

static void tree_max_size(struct tree_node* n, const uint8_t* p,
                          size_t maximum,
                          size_t* best_size, size_t* best_dist) {
    if (n != NULL) {
        const uint8_t* s = p;
        const uint8_t* d = n->data;
        const uint8_t* e = d + maximum;
        while (d < e && *d == *s) { d++; s++; }
        const size_t size = d - n->data;
        if (size >= min_size) {
            const size_t dist = p - n->data;
            if (size > *best_size) {
                *best_size = size;
                *best_dist = dist;
            } else if (size == *best_size && dist < *best_dist) {
                *best_dist = dist;
            }
        }
        if (*best_size < max_size && d < e) {
            int compare = ((int8_t)*s) - ((int8_t)*d);
            if (compare <= 0) {
                tree_max_size(n->ln, p, maximum, best_size, best_dist);
            } else {
                tree_max_size(n->rn, p, maximum, best_size, best_dist);
            }
        }
    }
}

int tree_nodes_walked;

static void tree_min_dist(struct tree_node* n, const uint8_t* p,
                          size_t* best_size, size_t* best_dist) {
    if (n != NULL) {
        tree_nodes_walked++;
        int cmp = memcmp(p, n->data, *best_size);
        size_t dist = p - n->data;
        if (cmp == 0) {
            if (dist < *best_dist) {
                *best_dist = dist;
            }
            tree_min_dist(n->ln, p, best_size, best_dist);
            tree_min_dist(n->rn, p, best_size, best_dist);
        }
        if (cmp < 0) {
            tree_min_dist(n->ln, p, best_size, best_dist);
        } else {
            tree_min_dist(n->rn, p, best_size, best_dist);
        }
    }
}

static void tree_walk(struct tree_node* n, const uint8_t* p,
                      size_t maximum,
                      size_t* best_size, size_t* best_dist) {
    if (n != NULL) {
        tree_nodes_walked++;
        assert(maximum > 0);
        if (*best_size < max_size) {
            int cmp = memcmp(p, n->data, *best_size + 1);
//          printf("memcmp(\"%.8s\", \"%.8s\", %zd): %d\n", p, n->data, *best_size + 1, cmp);
            if (cmp == 0) {
                while (*best_size < max_size && cmp == 0) {
                    (*best_size)++;
                    cmp = (int32_t)p[*best_size] - (int32_t)n->data[*best_size];
                }
                if (*best_dist == 0 || *best_dist > p - n->data) {
                    *best_dist = p - n->data;
                }
                printf("best size: %zd dist: %zd\n", *best_size, *best_dist);
                tree_walk(n->ln, p, maximum, best_size, best_dist);
                tree_walk(n->rn, p, maximum, best_size, best_dist);
            } else if (cmp < 0) {
                tree_walk(n->ln, p, maximum, best_size, best_dist);
            } else if (cmp > 0) {
                tree_walk(n->rn, p, maximum, best_size, best_dist);
            }
        }
    }
}

static inline void tree_find(struct tree* t, const uint8_t* p,
                             size_t maximum, size_t* size, size_t* dist) {
    assert(*size == 0); // callers responsibility
    assert(*dist == 0);
    tree_nodes_walked = 0;
    tree_walk(t->root, p, maximum, size, dist);
    printf("visits: %d\n", tree_nodes_walked);
    if (*size >= min_size) {
        tree_nodes_walked = 0;
        tree_min_dist(t->root, p, size, dist);
        printf("visits: %d\n", tree_nodes_walked);
    }
}

static void lz77_find(const uint8_t d[], size_t bytes, size_t i,
                      size_t* lz77_size, size_t* lz77_dist) {
    size_t size = 0;
    size_t dist = 0;
    if (i >= min_size) {
        size_t j = i - 1;
        size_t min_j = i >= window ? i - window + 1 : 0;
        for (;;) {
            const size_t n = bytes - i > max_size ? max_size : bytes - i;
            size_t k = 0;
            while (k < n && d[j + k] == d[i + k]) { k++; }
            if (k >= min_size && k > size) {
                size = k;
                dist = i - j;
                if (size == max_size) { break; }
            }
            if (j == min_j) { break; }
            j--;
        }
    }
    *lz77_size = size;
    *lz77_dist = dist;
}

static void experiment(void) {
#if 0
    const char* s =
        "The Old Testament of the King James Version of the Bible "
        "The First Book of Moses: Called Genesis"
        "The Second Book of Moses: Called Exodus                  "
        "The Third Book of Moses: Called Leviticus                "
        "The Fourth Book of Moses: Called Numbers                 "
        "The Fifth Book of Moses: Called Deuteronomy              "
        "The Book of Joshua                                       "
        "The Book of Judges                                       "
        "The Book of Ruth                                         "
        "The First Book of Samuel                                 "
        "The Second Book of Samuel                                "
        "The First Book of the Kings                              "
        "The Second Book of the Kings                             "
        "The First Book of the Chronicles                         "
        "The Second Book of the Chronicles                        "
;
#elif 1
    const char* s = "abcabcdabcdeabcdefabcdefgabcdefabcdeabcd";

#elif 1
    const char* s = "abcabcdabcdeabcdefabcdefgabcdefabcdeabcd "
                    "abcabcdabcdeabcdefabcdefgabcdefabcdeabcd";
#else
    const char* s = "0123012301230123012301230123012301230123";
#endif
    const size_t bytes = strlen(s);
    const uint8_t* d = (const uint8_t*)s;
    static struct tree t;
    tree_init(&t);
    size_t i = 0;
    while (i < bytes) {
        const size_t maximum = bytes - i < max_size ? bytes - i : max_size;
        size_t best_dist = 0;
        size_t best_size = 0;
        tree_find(&t, d + i, maximum, &best_size, &best_dist);
        printf("tree_find(\"%.8s\") size:%zd dist:%zd\n", d + i, best_size, best_dist);
        // LZ77:
        size_t lz77_size = 0;
        size_t lz77_dist = 0;
        lz77_find(d, bytes, i, &lz77_size, &lz77_dist);
        if (lz77_size >= min_size || best_size >= min_size) { // ignore single byte matches
            const uint8_t* match0 = d + i - best_dist;
            assert(memcmp(match0, d + i, best_size) == 0);
            const uint8_t* match1 = d + i - lz77_dist;
            if (best_dist != lz77_dist || best_size != lz77_size) {
                printf("[%zu] '%.*s' %3zu:%zu tree\n", i, (int)best_size, match0, best_dist, best_size);
                printf("[%zu] '%.*s' %3zu:%zu lz77\n", i, (int)lz77_size, match1, lz77_dist, lz77_size);
                printf("tree_node_count(): %d\n", tree_node_count(t.root));
                tree_print(&t, d + i);
                assert(best_dist == lz77_dist && best_size == lz77_size);
            }
            const size_t next = i + best_size;
            while (i < next) {
                size_t nc = tree_node_count(t.root);
                tree_insert(&t, d + i, maximum);
                assert(nc + 1 == tree_node_count(t.root));
                i++;
                if (i < bytes) {
                    const uint8_t* start = (i >= window) ? d + i - window + 1 : d;
                    nc = tree_node_count(t.root);
                    printf("%zd start: %p\n", nc, start);
                    tree_evict(&t, start);
                    if (start > d) {
                        tree_print(&t, d + i);
                        printf("%zd %zd start: %p\n", nc, tree_node_count(t.root), start);
                        assert(nc - 1 == tree_node_count(t.root));
                    }
                }
            }
        } else {
            size_t nc = tree_node_count(t.root);
            tree_insert(&t, d + i, maximum);
            assert(nc + 1 == tree_node_count(t.root));
            i++;
            if (i < bytes) {
                const uint8_t* start = (i >= window) ? d + i - window + 1 : d;
                nc = tree_node_count(t.root);
                printf("%zd start: %p\n", nc, start);
                tree_evict(&t, start);
                if (start > d) {
                    tree_print(&t, d + i);
                    printf("%zd %zd start: %p\n", nc, tree_node_count(t.root), start);
                    assert(nc - 1 == tree_node_count(t.root));
                }
            }
        }
    }
    if (t.root != NULL) { exit(0); }
}

#else

static void experiment(void) {
}

#endif


/*

static void experiment(void) {
#if 0
    const char* s =
        "The Old Testament of the King James Version of the Bible "
        "The First Book of Moses: Called Genesis"
//      "The Second Book of Moses: Called Exodus                  "
//      "The Third Book of Moses: Called Leviticus                "
//      "The Fourth Book of Moses: Called Numbers                 "
//      "The Fifth Book of Moses: Called Deuteronomy              "
//      "The Book of Joshua                                       "
//      "The Book of Judges                                       "
//      "The Book of Ruth                                         "
//      "The First Book of Samuel                                 "
//      "The Second Book of Samuel                                "
//      "The First Book of the Kings                              "
//      "The Second Book of the Kings                             "
//      "The First Book of the Chronicles                         "
//      "The Second Book of the Chronicles                        "
;
#elif 1
    const char* s = "abcabcdabcdeabcdefabcdefgabcdefabcdeabcd";

#elif 1
    const char* s = "abcabcdabcdeabcdefabcdefgabcdefabcdeabcd "
                    "abcabcdabcdeabcdefabcdefgabcdefabcdeabcd";
#else
    const char* s = "0123012301230123012301230123012301230123";
#endif



*/