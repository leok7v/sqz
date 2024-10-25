#include "rt/ustd.h"

enum { sqz_min_size = 2, sqz_max_size = 254 };
enum { sqz_max_win_bits = 16, sqz_max_win = 1u << sqz_max_win_bits };

struct tree_node {
    const  uint8_t*   data;
    struct tree_node* ln;  // left   node
    struct tree_node* rn;  // right  node
    struct tree_node* pn;  // parent node
};

struct tree {
    struct tree_node* root;
    struct tree_node  nodes[sqz_max_win];
    size_t pos;
};

struct sqz {
    size_t             window;
    struct tree        tree;
};

static void tree_init(struct tree* t) {
    t->pos = 0;
    t->root = NULL;
    memset(t->nodes, 0, sizeof(t->nodes));
}

static inline struct tree_node* tree_successor(struct tree_node* n) {
    while (n->ln != NULL) { n = n->ln; }
    return n;
}
static inline const struct tree_node* tree_prev(const struct tree_node* n) {
    // in-order predecessor
    if (n->ln != NULL) {
        n = n->ln;
        while (n->rn != NULL) { n = n->rn; }
    } else {
        while (n->pn != NULL && n == n->pn->ln) { n = n->pn; }
        n = n->pn;
    }
    return n;
}

static inline const struct tree_node* tree_next(const struct tree_node* n) {
    // in-order successor:
    if (n->rn != NULL) {
        n = n->rn;
        while (n->ln != NULL) { n = n->ln; }
    } else {
        while (n->pn != NULL && n == n->pn->rn) { n = n->pn; }
        n = n->pn;
    }
    return n;
}

static size_t tree_node_count(const struct tree_node* n) {
    return n == NULL ? 0 :
        1 + tree_node_count(n->ln) + tree_node_count(n->rn);
}

static void tree_print_node(char kind, const struct tree_node* n,
        const struct tree_node* parent, size_t indent, const uint8_t* p) {
    if (n != NULL) {
        for (size_t i = 0; i < indent; i++) printf(" ");
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

static void tree_dump_node(const struct tree_node* n, const uint8_t* p) {
    if (n != NULL) {
        tree_dump_node(n->ln, p);
        printf("[%2zu]'%s'\n", p - n->data, n->data);
        tree_dump_node(n->rn, p);
    }
}

static void tree_dump(const struct tree* t, const uint8_t* p) {
    tree_dump_node(t->root, p);
}

static void tree_print(const struct tree* t, const uint8_t* p) {
    tree_print_node(' ', t->root, NULL, 0, p);
    printf("\n");
    tree_dump(t, p);
    printf("%zd nodes\n\n", tree_node_count(t->root));
}

static inline void tree_shift_nodes(struct tree* t, struct tree_node* u,
                                                    struct tree_node* v) {
    if (u->pn == NULL) {
        t->root = v;
    } else if (u == u->pn->ln) {
        u->pn->ln = v;
    } else {
        u->pn->rn = v;
    }
    if (v != NULL) {
        v->pn = u->pn;
    }
}

static void tree_delete_node(struct tree* t, struct tree_node* n) {
    if (n->ln == NULL) {
        tree_shift_nodes(t, n, n->rn);
    } else if (n->rn == NULL) {
        tree_shift_nodes(t, n, n->ln);
    } else {
        struct tree_node* s = tree_successor(n->rn);
        if (s->pn != n) {
            tree_shift_nodes(t, s, s->rn);
            s->rn = n->rn;
            s->rn->pn = s;
        }
        tree_shift_nodes(t, n, s);
        s->ln = n->ln;
        s->ln->pn = s;
    }
}

static struct tree_node* tree_evict(struct sqz* s) {
    struct tree* t = &s->tree;
    struct tree_node* n = t->nodes + t->pos;
    t->pos = (t->pos + 1) % s->window;
    if (n->data != NULL) { tree_delete_node(t, n); }
    memset(n, 0, sizeof(*n));
    return n;
}

static inline void tree_insert(struct sqz* s, const uint8_t* p, size_t bytes) {
    struct tree* t = &s->tree;
    struct tree_node* z = tree_evict(s);
    z->data = p;
    struct tree_node* x = t->root;
    struct tree_node* y = NULL;
    while (x != NULL) {
        y = x;
        int cmp = memcmp(p, x->data, bytes);
        if (cmp <= 0) {
            x = x->ln;
        } else {
            x = x->rn;
        }
    }
    if (y == NULL) {
        t->root = z;
    } else {
        int cmp = memcmp(p, y->data, bytes);
        if (cmp <= 0) {
            y->ln = z;
        } else {
            y->rn = z;
        }
        z->pn = y;
    }
}

static int tree_nodes_walked;

static const struct tree_node* tree_max_size(const struct sqz* s,
                          const struct tree_node* n,
                          const struct tree_node* best,
                          const uint8_t* p, size_t bytes,
                          size_t* size, size_t* dist) {
    const struct tree_node* r = best;
    if (n != NULL) {
        tree_nodes_walked++;
        assert(bytes > 0);
        if (*size < sqz_max_size) {
            tree_nodes_walked++;
            size_t len = *size;
            int cmp = memcmp(p, n->data, len + 1);
//          printf("memcmp(\"%.8s\", \"%.8s\", %zd): %d\n", p, n->data, *size + 1, cmp);
            if (cmp == 0) {
                const size_t dst = p - n->data;
                const size_t max_size = bytes < sqz_max_size ? bytes : sqz_max_size;
                while (len < max_size && cmp == 0) {
                    len++;
                    cmp = (int32_t)p[len] - (int32_t)n->data[len];
                }
                assert(memcmp(p, p - dst, len) == 0);
                if (len > *size) {
                    *size = len;
                    *dist = dst;
                    r = n;
                } else if (len == *size && dst < *dist) {
                    *dist = dst;
                    r = n;
                }
//              printf("best size: %zd dst: %zd\n", *size, *dst);
                r = tree_max_size(s, n->ln, r, p, bytes, size, dist);
                r = tree_max_size(s, n->rn, r, p, bytes, size, dist);
            } else if (cmp < 0) {
                r = tree_max_size(s, n->ln, r, p, bytes, size, dist);
            } else if (cmp > 0) {
                r = tree_max_size(s, n->rn, r, p, bytes, size, dist);
            }
        }
    }
    return r;
}

static inline void tree_min_dist(const struct tree_node* n,
                                 const uint8_t* p,
                                 size_t* size, size_t* dist) {
    size_t min_dist = *dist;
    const struct tree_node* node = tree_prev(n);
    while (node != NULL && memcmp(p, node->data, *size) == 0) {
        tree_nodes_walked++;
        size_t dst = p - node->data;
        if (dst < min_dist) { min_dist  = dst; }
        node = tree_prev(node);
    }
    node = tree_next(n);
    while (node != NULL && memcmp(p, node->data, *size) == 0) {
        tree_nodes_walked++;
        size_t dst = p - node->data;
        if (dst < min_dist) { min_dist  = dst; }
        node = tree_next(node);
    }
    *dist = min_dist;
}

// returns the size of the longest match and the distance to it
// size: [sqz_min_size..sqz_max_size]  dst: [1..window]

static inline void tree_find(const struct sqz* s, const uint8_t* p,
                             size_t bytes, size_t* size, size_t* dist) {
    assert(*size == 0); // callers responsibility
    assert(*dist == 0);
    const struct tree* t = &s->tree;
    tree_nodes_walked = 0;
    const struct tree_node* n = tree_max_size(s, t->root, NULL, p, bytes, size, dist);
//  size_t walked = tree_nodes_walked;
    if (sqz_min_size <= *size) {
        assert(*size <= sqz_max_size);
        tree_nodes_walked = 0;
        tree_min_dist(n, p, size, dist);
//      printf("tree_max_size(): %d tree_min_dist(): %d nodes\n",
//              walked, tree_nodes_walked);
    } else {
//      printf("tree_max_size(): %d\n", walked);
    }
}

// returns the size of the longest match and the distance to it
// size: [sqz_min_size..sqz_max_size]  dst: [1..window]

static void lz77_find(const struct sqz* s, const uint8_t d[], size_t bytes, size_t i,
                      size_t* size, size_t* dist) {
    size_t len = 0;
    size_t dst = 0;
    if (i > 0) {
        size_t j = i - 1;
        size_t min_j = i >= s->window ? i - s->window : 0;
        for (;;) {
            const size_t n = bytes - i > sqz_max_size ? sqz_max_size : bytes - i;
            size_t k = 0;
            while (k < n && d[j + k] == d[i + k]) { k++; }
            if (k >= sqz_min_size && k > len) {
                len = k;
                dst = i - j;
                if (len == sqz_max_size) { break; }
            }
            if (j == min_j) { break; }
            j--;
        }
    }
    *size = len;
    *dist = dst;
}

static void bst(size_t window, const uint8_t* d, size_t bytes) {
    static struct sqz sqz;
    struct sqz*  s = &sqz;
    struct tree* t = &s->tree;
    s->window = window;
    printf("window: %zd\n", window);
    tree_init(t);
    size_t i = 0;
    while (i < bytes) {
        const size_t maximum = bytes - i < sqz_max_size ? bytes - i : sqz_max_size;
        size_t best_dist = 0;
        size_t best_size = 0;
        tree_find(s, d + i, maximum, &best_size, &best_dist);
//      printf("tree_find(\"%.16s\") size:%zd dst:%zd \"%.16s\"\n",
//              d + i, size, dst, d + i - dst);
        // LZ77:
        size_t lz77_size = 0;
        size_t lz77_dist = 0;
        lz77_find(s, d, bytes, i, &lz77_size, &lz77_dist);
        if (lz77_size >= sqz_min_size || best_size >= sqz_min_size) { // ignore single byte matches
            const uint8_t* match0 = d + i - best_dist;
            assert(memcmp(match0, d + i, best_size) == 0);
            const uint8_t* match1 = d + i - lz77_dist;
            if (best_dist != lz77_dist || best_size != lz77_size) {
                printf("[%zu] '%.*s' %3zu:%zu tree\n", i, (int)best_size, match0, best_dist, best_size);
                printf("[%zu] '%.*s' %3zu:%zu lz77\n", i, (int)lz77_size, match1, lz77_dist, lz77_size);
                printf("tree_node_count(): %d\n", tree_node_count(t->root));
                tree_print(t, d + i);
                assert(best_dist == lz77_dist && best_size == lz77_size);
            }
            const size_t next = i + best_size;
            while (i < next) {
                tree_insert(s, d + i, maximum);
                i++;
                if (i < s->window && i != tree_node_count(t->root)) {
                    printf("[%2zu] tree_node_count(t->root): %zu\n", i, tree_node_count(t->root));
                }
                if (i < s->window) {
                    assert(i == tree_node_count(t->root));
                } else {
                    assert(s->window == tree_node_count(t->root));
                }
            }
        } else {
            tree_insert(s, d + i, maximum);
            i++;
            if (i < s->window) {
                assert(i == tree_node_count(t->root));
            } else {
                assert(s->window == tree_node_count(t->root));
            }
        }
    }
    printf("\n");
}

static void test1(void) {
    const char* str[] = {
        "abcabcdabcdeabcdefabcdefgabcdefabcdeabcd "
        "abcabcdabcdeabcdefabcdefgabcdefabcdeabcd",

        "0123012301230123012301230123012301230123",

        "abcabcdabcdeabcdefabcdefgabcdefabcdeabcd",

        "The Old Testament of the King James Version of the Bible "
        "The First Book of Moses: Called Genesis "
        "The Second Book of Moses: Called Exodus "
        "The Third Book of Moses: Called Leviticus "
        "The Fourth Book of Moses: Called Numbers "
        "The Fifth Book of Moses: Called Deuteronomy "
        "The Book of Joshua "
        "The Book of Judges "
        "The Book of Ruth "
        "The First Book of Samuel "
        "The Second Book of Samuel "
        "The First Book of the Kings "
        "The Second Book of the Kings "
        "The First Book of the Chronicles "
        "The Second Book of the Chronicles "
    };
    for (size_t k = 0; k < sizeof(str) / sizeof(str[0]); k++) {
        const size_t bytes = strlen(str[k]);
        const uint8_t* d = (const uint8_t*)str[k];

        printf("%.*s: %zu\n", (int)bytes, str[k], bytes);
        for (size_t i = 0; i < bytes; i++) {
            printf("%d", i % 10);
        }
        printf("\n");
        for (size_t i = 0; i < bytes; i++) {
            printf("%c", i % 10 == 0 ? '0' + (i / 10) % 10 : 0x20);
        }
        printf("\n");
        for (size_t window = 8; window <= sqz_max_win; window <<= 1) {
            bst(window, d, bytes);
        }
    }
}

static void test2(void) {
    static uint8_t d[128 * 1024];
    memset(d, 0, sizeof(d));
    // this will create very skew deep and will require
    // balanced tree to work:
    bst(sqz_max_win, d, sizeof(d));
}

int main(int argc, const char* argv[]) {
    (void)argc; (void)argv; // unused
    test1();
//  test2();
    return 0;
}
