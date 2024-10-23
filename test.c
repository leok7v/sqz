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

enum { window = 512, min_size = 2, max_size = 254 };

static void pretty_print(struct tree_node* node, size_t indent) {
    if (!node) return;
    for (size_t i = 0; i < indent; i++) printf("  ");
    printf("Node '%s':%zu\n", node->data, node->dist);
    pretty_print(node->ln, indent + 1);
    pretty_print(node->rn, indent + 1);
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

static size_t tree_node_count(struct tree_node* n) {
    return n == NULL ? 0 :
        1 + tree_node_count(n->ln) + tree_node_count(n->rn);
}

static inline int tree_height(struct tree_node* n) {
    return n != NULL ? n->height : 0;
}

static inline int tree_balance_factor(struct tree_node* n) {
    return n != NULL ?
        tree_height(n->ln) - tree_height(n->rn) : 0;
}

static inline void tree_update_height(struct tree_node* n) {
    if (n != NULL) {
        n->height =
            1 + (tree_height(n->ln) > tree_height(n->rn) ?
                 tree_height(n->ln) : tree_height(n->rn));
    }
}

static inline struct tree_node* tree_rotate_right(struct tree_node* y) {
    size_t nc = tree_node_count(y);
    struct tree_node* x = y->ln; y->ln = x->rn; x->rn = y;
    tree_update_height(y);
    tree_update_height(x);
    assert(nc == tree_node_count(x));
    return x;
}

static inline struct tree_node* tree_rotate_left(struct tree_node* x) {
    size_t nc = tree_node_count(x);
    struct tree_node* y = x->rn; x->rn = y->ln; y->ln = x;
    tree_update_height(x);
    tree_update_height(y);
    assert(nc == tree_node_count(y));
    return y;
}

static struct tree_node* tree_balance(struct tree_node* n) {
    size_t nc = tree_node_count(n);
    tree_update_height(n);
    int balance = tree_balance_factor(n);
//  printf("balance: %d\n", balance);
    if (balance > 1) {
        if (tree_balance_factor(n->ln) < 0) {
            n->ln = tree_rotate_left(n->ln);
        }
        return tree_rotate_right(n);
    } else if (balance < -1) {
        if (tree_balance_factor(n->rn) > 0) {
            n->rn = tree_rotate_right(n->rn);
        }
        return tree_rotate_left(n);
    }
    assert(nc == tree_node_count(n));
    balance = tree_balance_factor(n);
    assert(-1 <= balance && balance <= 1);
    return n;
}

static size_t evict_count;
static size_t insert_count;

static struct tree_node* leftmost(struct tree_node* n) {
    while (n->ln != NULL) { n = n->ln; }
    return n;
}

static struct tree_node* tree_evict(struct tree* t, struct tree_node* n,
                                    size_t start) {
    if (n != NULL) {
        if (n->dist < start) {
            evict_count++;
            struct tree_node* s = n->rn != NULL ? n->rn : n->ln;
            if (n->rn != NULL) { leftmost(s)->ln = n->ln; }
            tree_free(t, n);
            n = s;
        } else {
            n->ln = tree_evict(t, n->ln, start);
            n->rn = tree_evict(t, n->rn, start);
        }
    }
    return n != NULL ? tree_balance(n) : NULL;
}

static inline struct tree_node* tree_insert(struct tree* t,
        struct tree_node* n, const uint8_t* p,
        size_t maximum, size_t d) {
    if (n == NULL) {
        n = tree_alloc(t);
        insert_count++;
        if (n != NULL) {
            n->data = p;
            n->dist = d;
            n->height = 1;
            n->ln = n->rn = NULL;
        }
    } else {
        int cmp = memcmp(p, n->data, maximum);
        if (cmp < 0) {
            n->ln = tree_insert(t, n->ln, p, maximum, d);
        } else {
            n->rn = tree_insert(t, n->rn, p, maximum, d);
        }
        n = tree_balance(n);
    }
    return n;
}

static bool tree_find_recursive_debug;

static void tree_find_recursive(struct tree_node* node, const uint8_t* p,
                                size_t maximum,
                                size_t* best_size, size_t* best_dist) {
    if (tree_find_recursive_debug) {
        printf("%.4s\n", p);
    }
    if (node != NULL) {
        const uint8_t* s = p;
        const uint8_t* d = node->data;
        const uint8_t* e = d + maximum;
        while (d < e && *d == *s) { d++; s++; }
        const size_t size = d - node->data;
        const size_t dist = p - node->data;
        if (tree_find_recursive_debug) {
            printf("Node: `%.16s...` .dist:%d %d:%d\n", node->data, node->dist, dist, size);
        }
        if (size > *best_size) {
//          printf("New best match dist:size %u:%u := %u:%u\n", (uint32_t)*best_dist,
//                  (uint32_t)*best_size, (uint32_t)size, (uint32_t)dist);
            *best_size = size;
            *best_dist = dist;
        } else if (size > 0 && size == *best_size) {
            if (dist < *best_dist) {
//              printf("Improved match dist:size %u:%u := %u\n", (uint32_t)*best_dist,
//                      (uint32_t)*best_size, (uint32_t)dist);
                *best_dist = dist;
            }
        }
        if (*best_size < max_size && d < e) {
            int compare = ((int8_t)*s) - ((int8_t)*d);
            if (compare < 0) {
                tree_find_recursive(node->ln, p, maximum, best_size, best_dist);
            } else {
                tree_find_recursive(node->rn, p, maximum, best_size, best_dist);
            }
            if (*best_size > 1 &&
                node->rn != NULL &&
                (size_t)(p - node->rn->data) < *best_dist &&
                memcmp(node->rn->data, p, *best_size) == 0) {
                *best_dist = p - node->rn->data;
            }
        }
    }
}

static inline void tree_find(struct tree* t, const uint8_t* p,
                             size_t maximum, size_t* size, size_t* distance) {
    *size = 0;
    *distance = 0;
    tree_find_recursive(t->root, p, maximum, size, distance);
}

static void experiment(void) {
#if 1
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

#elif 0
    const char* s = "abcabcdabcdeabcdefabcdefgabcdefabcdeabcd "
                    "abcabcdabcdeabcdefabcdefgabcdefabcdeabcd";
#else
    const char* s = "0123012301230123012301230123012301230123";
#endif
    evict_count = 0;
    insert_count = 0;
    const size_t bytes = strlen(s);
    const uint8_t* d = (const uint8_t*)s;
    static struct tree t;
    t.root = NULL;
    tree_init(&t);
    size_t i = 0;
    while (i < bytes) {
        const size_t maximum = bytes - i < max_size ? bytes - i : max_size;
        size_t best_dist = 0;
        size_t best_size = 0;
//      tree_find_recursive_debug = i == 55;
//      if (i == 55) { rt_breakpoint(); }
        tree_find(&t, d + i, maximum, &best_size, &best_dist);
        // LZ77:
        size_t lz77_size = 0;
        size_t lz77_dist = 0;
        if (i >= 1) {
            size_t j = i - 1;
            size_t min_j = i >= window ? i - window + 1 : 0;
            for (;;) {
                const size_t n = bytes - i;
                size_t k = 0;
                while (k < n && d[j + k] == d[i + k] && k < max_size) {
                    k++;
                }
                if (k >= min_size && k > lz77_size) {
                    lz77_size = (uint8_t)k;
                    lz77_dist = (uint32_t)(i - j);
                    if (lz77_size == max_size) { break; }
                }
                if (j == min_j) { break; }
                j--;
            }
        }
        if (lz77_size >= min_size || best_size >= min_size) { // ignore single byte matches
            const uint8_t* match0 = d + i - best_dist;
            assert(memcmp(match0, d + i, best_size) == 0);
            const uint8_t* match1 = d + i - lz77_dist;
            if (best_dist != lz77_dist || best_size != lz77_size) {
                printf("[%zu] '%.*s' %3zu:%zu tree\n", i, (int)best_size, match0, best_dist, best_size);
                printf("[%zu] '%.*s' %3zu:%zu lz77\n", i, (int)best_size, match1, lz77_dist, lz77_size);
                printf("tree_node_count(): %d\n", tree_node_count(t.root));
                pretty_print(t.root, 0);
                assert(best_dist == lz77_dist && best_size == lz77_size);
            }
//          if (best_size > min_size || lz77_size >= min_size) {
//              printf("[%zu] '%.*s' %zu:%zu tree\n", i, (int)best_size, match0, best_dist, best_size);
//              printf("[%zu] '%.*s' %zu:%zu lz77\n", i, (int)best_size, match1, lz77_dist, lz77_size);
//          }
            const size_t next = i + best_size;
            while (i < next) {
                size_t nc = tree_node_count(t.root);
                size_t ic = insert_count;
//              printf("tree_insert(distance: %u)\n", i);
                t.root = tree_insert(&t, t.root, d + i, maximum, i);
//              printf("tree_node_count(): %d\n", tree_node_count(t.root));
//              pretty_print(t.root, 0);
                assert(insert_count == ic + 1);
                assert(nc + 1 == tree_node_count(t.root));
                i++;
                if (i < bytes) {
                    size_t start = (i >= window) ? i - window + 1 : 0;
//                  printf("[%u] tree_evict(start: %u)\n", i, start);
                    nc = tree_node_count(t.root);
                    size_t ec = evict_count;
                    t.root = tree_evict(&t, t.root, start);
//                  printf("tree_node_count(): %d\n", tree_node_count(t.root));
//                  pretty_print(t.root, 0);
                    if (start > 0) {
                        assert(evict_count == ec + 1);
                        assert(nc - 1 == tree_node_count(t.root));
                    }
                }
            }
        } else {
            size_t nc = tree_node_count(t.root);
            size_t ic = insert_count;
//          printf("tree_insert(distance: %u)\n", i);
            t.root = tree_insert(&t, t.root, d + i, maximum, i);
//          printf("tree_node_count(): %d\n", tree_node_count(t.root));
//          pretty_print(t.root, 0);
            assert(insert_count == ic + 1);
            assert(nc + 1 == tree_node_count(t.root));
            i++;
            if (i < bytes) {
                size_t start = (i >= window) ? i - window + 1 : 0;
//              printf("[%u] tree_evict(start: %u)\n", i, start);
                size_t ec = evict_count;
                nc = tree_node_count(t.root);
                t.root = tree_evict(&t, t.root, start);
//              printf("tree_node_count(): %d\n", tree_node_count(t.root));
//              pretty_print(t.root, 0);
                if (start > 0) {
                    assert(evict_count == ec + 1);
                    assert(nc - 1 == tree_node_count(t.root));
                }
            }
        }
    }
//  printf("tree_node_count(): %d\n", tree_node_count(t.root));
    if (t.root != NULL) { exit(0); }
}

#else

static void experiment(void) {
}

#endif