#include "rt/ustd.h"

// https://en.wikipedia.org/wiki/Binary_search_tree (1960)
// https://en.wikipedia.org/wiki/AVL_tree (1962)

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

static void tree_init(struct tree* t) {
    memset(t, 0, sizeof(*t));
}

static inline struct tree_node* tree_successor(struct tree_node* n) {
    while (n->ln) { n = n->ln; }
    return n;
}
static inline const struct tree_node* tree_prev(const struct tree_node* n) {
    // in-order predecessor
    if (n->ln) {
        n = n->ln;
        while (n->rn) { n = n->rn; }
    } else {
        while (n->pn && n == n->pn->ln) { n = n->pn; }
        n = n->pn;
    }
    return n;
}

static inline const struct tree_node* tree_next(const struct tree_node* n) {
    // in-order successor:
    if (n->rn) {
        n = n->rn;
        while (n->ln) { n = n->ln; }
    } else {
        while (n->pn && n == n->pn->rn) { n = n->pn; }
        n = n->pn;
    }
    return n;
}

static size_t tree_node_count(const struct tree_node* n) {
    return !n ? 0 :
        1 + tree_node_count(n->ln) + tree_node_count(n->rn);
}

static void tree_print_node(char kind, const struct tree_node* n,
        const struct tree_node* parent, size_t indent, const uint8_t* p) {
    if (n) {
        for (size_t i = 0; i < indent; i++) printf(" ");
        assert(p > n->data); // we can use size_t instead of ptrdiff_t
        const size_t distance = p - n->data; // from current position 'p'
        if (!parent) {
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
    if (n) {
        tree_dump_node(n->ln, p);
        printf("[%2zu]'%s'\n", p - n->data, n->data);
        tree_dump_node(n->rn, p);
    }
}

static void tree_dump(const struct tree* t, const uint8_t* p) {
    tree_dump_node(t->root, p);
}

static void tree_print(const struct tree* t, const uint8_t* p) {
    tree_print_node(' ', t->root, (void*)0, 0, p);
    printf("\n");
    tree_dump(t, p);
    printf("%zd nodes\n\n", tree_node_count(t->root));
}

static inline void tree_shift_nodes(struct tree* t, struct tree_node* u,
                                                    struct tree_node* v) {
    if (!u->pn) {
        t->root = v;
    } else if (u == u->pn->ln) {
        u->pn->ln = v;
    } else {
        u->pn->rn = v;
    }
    if (v) {
        v->pn = u->pn;
    }
}

static void tree_delete_node(struct tree* t, struct tree_node* n) {
    if (!n->ln) {
        tree_shift_nodes(t, n, n->rn);
    } else if (!n->rn) {
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

static struct tree_node* tree_evict(struct tree* t, size_t window) {
    struct tree_node* n = t->nodes + t->pos;
    t->pos = (t->pos + 1) % window;
    if (n->data) { tree_delete_node(t, n); }
    memset(n, 0, sizeof(*n));
    return n;
}

static inline void tree_insert(struct tree* t,
                               const uint8_t* p, size_t bytes,
                               size_t window) {
    struct tree_node* f = tree_evict(t, window); // free node
    f->data = p;
    struct tree_node* n = t->root;
    struct tree_node* w = (void*)0; // value of 'n' was 'w'
    while (n) {
        w = n;
        int cmp = memcmp(p, n->data, bytes);
        if (cmp <= 0) { n = n->ln; } else { n = n->rn; }
    }
    if (!w) {
        t->root = f;
    } else {
        int cmp = memcmp(p, w->data, bytes);
        if (cmp <= 0) { w->ln = f; } else { w->rn = f; }
        f->pn = w;
    }
}

static const struct tree_node* tree_max_size(
        const struct tree_node* n, const struct tree_node* best,
        const uint8_t* p, size_t bytes, size_t* size, size_t* dist) {
    const struct tree_node* r = best;
    if (n) {
        assert(bytes > 0);
        if (*size < sqz_max_size) {
            size_t len = *size;
            int cmp = memcmp(p, n->data, len + 1);
            if (cmp == 0) {
                const size_t k = bytes < sqz_max_size ? bytes : sqz_max_size;
                while (len < k && cmp == 0) {
                    len++;
                    cmp = (int32_t)p[len] - (int32_t)n->data[len];
                }
                const size_t dst = p - n->data;
                if (len > *size) {
                    *size = len;
                    *dist = dst;
                    r = n;
                } else if (len == *size && dst < *dist) {
                    *dist = dst;
                    r = n;
                }
                r = tree_max_size(n->ln, r, p, bytes, size, dist);
                r = tree_max_size(n->rn, r, p, bytes, size, dist);
            } else if (cmp < 0) {
                r = tree_max_size(n->ln, r, p, bytes, size, dist);
            } else if (cmp > 0) {
                r = tree_max_size(n->rn, r, p, bytes, size, dist);
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
    while (node && memcmp(p, node->data, *size) == 0) {
        size_t dst = p - node->data;
        if (dst < min_dist) { min_dist  = dst; }
        node = tree_prev(node);
    }
    node = tree_next(n);
    while (node && memcmp(p, node->data, *size) == 0) {
        size_t dst = p - node->data;
        if (dst < min_dist) { min_dist  = dst; }
        node = tree_next(node);
    }
    *dist = min_dist;
}

// returns the size of the longest match and the shortest distance
// size: [sqz_min_size..sqz_max_size]  dist: [1..window]
// or size: 0 dist: 0

static inline void tree_find(const struct tree* t, const uint8_t* p,
                             size_t bytes, size_t* size, size_t* dist) {
    assert(*size == 0); // callers responsibility
    assert(*dist == 0);
    const struct tree_node* n =
        tree_max_size(t->root, (void*)0, p, bytes, size, dist);
    if (*size >= sqz_min_size) {
        tree_min_dist(n, p, size, dist);
    } else {
        *size = 0;
        *dist = 0;
    }
}

// returns the size of the longest match and the distance to it
// size: [sqz_min_size..sqz_max_size]  dst: [1..window]

static void lz77_find(const uint8_t d[], const size_t bytes, const size_t i,
                      size_t* size, size_t* dist, const size_t window) {
    size_t len = 0;
    size_t dst = 0;
    if (i > 0) {
        size_t j = i - 1;
        size_t min_j = i >= window ? i - window : 0;
        for (;;) {
            size_t k = 0;
            while (k < bytes && d[j + k] == d[i + k]) { k++; }
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

static void verify(const struct tree* t, const size_t i,
                   const uint8_t* d, const size_t bytes, const size_t window,
                   size_t tree_size, size_t tree_dist) {
    size_t lz77_size = 0;
    size_t lz77_dist = 0;
    lz77_find(d, bytes, i, &lz77_size, &lz77_dist, window);
    const uint8_t* match0 = d + i - tree_dist;
    const uint8_t* match1 = d + i - lz77_dist;
    if (lz77_size >= sqz_min_size || tree_size >= sqz_min_size) {
        swear(sqz_min_size <= lz77_size && lz77_size <= sqz_max_size);
        swear(sqz_min_size <= tree_size && tree_size <= sqz_max_size);
        swear(1 <= lz77_dist && lz77_dist <= window);
        swear(1 <= tree_dist && tree_dist <= window);
        assert(memcmp(match0, d + i, tree_size) == 0);
        assert(memcmp(match1, d + i, tree_size) == 0);
    }
    if (tree_dist != lz77_dist || tree_size != lz77_size) {
        printf("[%zu] '%.*s' %3zu:%zu tree\n", i, (int)tree_size, match0, tree_dist, tree_size);
        printf("[%zu] '%.*s' %3zu:%zu lz77\n", i, (int)lz77_size, match1, lz77_dist, lz77_size);
        printf("tree_node_count(): %d\n", tree_node_count(t->root));
        tree_print(t, d + i);
    }
    swear(tree_dist == lz77_dist && tree_size == lz77_size);
}

static void tree_lz77(struct tree* t, const uint8_t* d, const size_t bytes,
                const size_t window) { // compare bst/lva search to lz77
    printf("window: %zd\n", window);
    tree_init(t);
    size_t i = 0;
    while (i < bytes) {
        const size_t n = bytes - i < sqz_max_size ? bytes - i : sqz_max_size;
        size_t dist = 0;
        size_t size = 0;
        tree_find(t, d + i, n, &size, &dist);
        verify(t, i, d, bytes, window, size, dist); // compare with lz77 search
        if (size >= sqz_min_size) {
            const size_t next = i + size;
            while (i < next) {
                tree_insert(t, d + i, n, window);
                i++;
            }
        } else {
            tree_insert(t, d + i, n, window);
            i++;
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
    static struct tree tree;
    struct tree* t = &tree;
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
            tree_lz77(t, d, bytes, window);
        }
    }
}

static void test2(void) {
    // will only work with AVL or Red Black trees
    static struct tree tree;
    struct tree* t = &tree;
    static uint8_t d[128 * 1024];
    memset(d, 0, sizeof(d));
    // this will create very skew deep and will require
    // balanced tree to work:
    tree_lz77(t, d, sizeof(d), sqz_max_win);
    // possible additional optimization:
    // Do not to add equal nodes to the tree.
    // When same value node found (e.g. all zeros to sqz_max_size)
    // replace it in the tree by new node (will have smaller distance).
    // The large distance node .ln/.rn/.pn set to NULL and it will be
    // skipped on node deletion when window shifts.
    // It makes the tree more shallow and speed up searches in
    // special case of big (> sqz_max_size) same value filled arrays.
}

int main(int argc, const char* argv[]) {
    (void)argc; (void)argv; // unused
    test1();
//  test2();
    return 0;
}
