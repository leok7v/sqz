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
    intptr_t      height;  // int8_t would be enough but alignment...
};

struct tree {
    struct tree_node* root;
    struct tree_node  nodes[sqz_max_win];
    size_t pos;
};

static void tree_init(struct tree* t) {
    memset(t, 0, sizeof(*t));
}

static void tree_print(const struct tree* t, const uint8_t* p);

#ifdef DEBUG
static const struct tree* debug_tree;
static const uint8_t*     debug_p;
static const uint8_t*     debug_data;
static       size_t       debug_bytes;
static       int          debug_max_depth;

#define tree_debug_dump() tree_print(debug_tree, debug_p)
#define tree_assert(b, ...) do { if (!(b)) { tree_debug_dump(); assert(b, __VA_ARGS__); } } while (0)
#else
#define tree_debug_dump()
#define tree_assert(...) assert(__VA_ARGS__)
#endif

static size_t tree_node_count(const struct tree_node* n) {
    return !n ? 0 :
        1 + tree_node_count(n->ln) + tree_node_count(n->rn);
}

static inline int tree_node_height(struct tree_node* n) {
    return n ? (int)n->height : 0;
}

static inline int tree_bf(const struct tree_node *n) { // balance factor
    return (int)(!n ? 0 : tree_node_height(n->rn) - tree_node_height(n->ln));
}

static inline int tree_depth(struct tree_node* n) {
    int depth = 0;
    if (n) {
        int ld = tree_depth(n->ln);
        int rd = tree_depth(n->rn);
        depth = 1 + (ld > rd ? ld : rd);
    }
    return depth;
}

static void tree_check_node(struct tree_node* n) {
     // structural consistency paranoia
    if (n) {
        if (n->ln) { tree_assert(n->ln->pn == n, "n: %p n->ln: %p n->ln->pn: %p", n, n->ln, n->ln->pn); }
        if (n->rn) { tree_assert(n->rn->pn == n, "n: %p n->rn: %p n->rn->pn: %p", n, n->rn, n->rn->pn); }
        tree_assert(n->pn != n, "n: %p n->pn: %p", n, n->pn);
        tree_assert(n->ln != n, "n: %p n->ln: %p", n, n->ln);
        tree_assert(n->rn != n, "n: %p n->rn: %p", n, n->rn);
        tree_assert(!n->pn || n->pn != n->ln, "n: %p n->pn: %p n->ln: %p", n, n->pn, n->ln);
        tree_assert(!n->pn || n->pn != n->rn, "n: %p n->pn: %p n->ln: %p", n, n->pn, n->ln);
        tree_check_node(n->ln);
        tree_check_node(n->rn);
    }
}

static inline void tree_verify_height(struct tree_node* n) {
    if (n) {
        int lh = tree_node_height(n->ln);
        int rh = tree_node_height(n->rn);
        int height = 1 + (lh > rh ? lh : rh); (void)height;
        tree_assert(n->height == height, "%p .height: %d expected: %d '%s'",
                    n, n->height, height, n->data);
    }
}

static void tree_verify_tree_heights(struct tree_node* n) {
    if (n) {
        tree_verify_height(n);
        tree_verify_tree_heights(n->ln);
        tree_verify_tree_heights(n->rn);
    }
}

static void tree_verify_node(struct tree_node* pn, struct tree_node* n) {
    if (n) {
        tree_check_node(n);
        tree_check_node(pn);
        tree_assert(n->pn == pn, "n: %p n->pn: %p pn: %p", n, n->pn, pn);
        int bf = tree_bf(n);
        if (!(-1 <= bf && bf <= +1)) { tree_debug_dump(); }
        tree_assert(-1 <= bf && bf <= +1, "%p balance factor: %d '%s'", n, bf, n->data);
        tree_assert(debug_data <= debug_p && debug_p < debug_data + debug_bytes);
        tree_assert(debug_data <= n->data && n->data < debug_data + debug_bytes);
        tree_assert(!pn || debug_data <= pn->data && pn->data < debug_data + debug_bytes);
        tree_verify_node(n, n->ln);
        tree_verify_node(n, n->rn);
    }
}

static void tree_verify(struct tree* t) {
    #ifdef DEBUG
        tree_verify_node((void*)0, t->root);
        int depth = tree_depth(t->root);
        if (depth > debug_max_depth) {
            debug_max_depth = depth;
            printf("max_depth: %d root height: %d\n", (int)depth, (int)t->root->height);
        }
        tree_verify_tree_heights(t->root);
    #else
        (void)t;
    #endif
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

static void tree_print_node(char kind, const struct tree_node* n,
                            size_t indent, const uint8_t* p) {
    if (n) {
        for (size_t i = 0; i < indent; i++) printf(" ");
        assert(p >= n->data); // we can use size_t instead of ptrdiff_t
        const size_t distance = p - n->data; // from current position 'p'
        printf("%c %p %d [%zu]'%s' \n", kind, n, (int)n->height, distance, n->data);
        tree_print_node('L', n->ln, indent + 1, p);
        tree_print_node('R', n->rn, indent + 1, p);
    }
}

static void tree_dump_node(const struct tree_node* n, const uint8_t* p) {
    if (n) {
        tree_dump_node(n->ln, p);
        int bf = tree_bf(n);
        printf("[%2zu] %p [pn:%p ln:%p rn:%p] %d:%+2d %p '%s'\n", p - n->data,
                n, n->pn, n->ln, n->rn, (int)n->height, bf, n->data, n->data);
        tree_dump_node(n->rn, p);
    }
}

static void tree_dump(const struct tree* t, const uint8_t* p) {
    printf("root %p nodes: %zd\n", t->root, tree_node_count(t->root));
    tree_dump_node(t->root, p);
}

static void tree_print(const struct tree* t, const uint8_t* p) {
    printf("\n");
#ifdef DEBUG
    printf("i: %zu\n", debug_p - debug_data);
#endif
    tree_dump(t, p);
    printf("\n");
    tree_print_node(' ', t->root, 0, p);
    printf("\n");
}

// n->height = 1 + max(tree_height(n->ln), tree_height(n->rn));

static inline void tree_fix_height(struct tree_node* n) {
    int lh = tree_node_height(n->ln);
    int rh = tree_node_height(n->rn);
//  int was = (int)n->height;
    n->height = 1 + (lh > rh ? lh : rh);
//  printf("%p ln:%p rn:%p %d := %d\n", n, n->ln, n->rn, was, (int)n->height);
}

/* rotate left/right to root (pp)
 *
 * *pp:y        *pp:x
 *    / \   <==>   / \
 *   x   C        A   y
 *  / \              / \
 * A   B            B   C
 *
 */

static struct tree_node* tree_rotate_left(struct tree_node** pp) {
    struct tree_node* nd = *pp;    // old root
    struct tree_node* rn = nd->rn; // (*pp)->right (new root)
    struct tree_node* ln = rn->ln; // (*pp)->right->left
    nd->rn = ln;
    if (ln) { ln->pn = nd; tree_assert(rn->pn != rn->ln && rn->pn != rn->rn); }
    rn->ln = nd; nd->pn = rn;
tree_assert(nd->pn != nd->ln);
tree_assert(nd->pn != nd->rn);
    *pp = rn;
    tree_fix_height(rn->ln);
    tree_fix_height(rn);
    return rn;
}

static struct tree_node* tree_rotate_right(struct tree_node** pp) {
    struct tree_node* nd = *pp;     // old root
    struct tree_node* ln = nd->ln;  // (*pp)->left (new root)
    struct tree_node* rn = ln->rn;  // (*pp)->left->right
    nd->ln = rn;
    if (rn) { rn->pn = nd; tree_assert(rn->pn != rn->ln && rn->pn != rn->rn); }
    ln->rn = nd; nd->pn = ln;
tree_assert(nd->pn != nd->ln);
tree_assert(nd->pn != nd->rn);
    *pp = ln;
    tree_fix_height(ln->rn);
    tree_fix_height(ln);
    return ln;
}

static void tree_rebalance(struct tree* t, struct tree_node* n) {
    while (n) {
        struct tree_node*  pn = n->pn;  // parent node
        struct tree_node** pp = !pn ? &t->root : // parent reference
                              (n == pn->ln ? &pn->ln : &pn->rn);
        int lh = tree_node_height(n->ln);
        int rh = tree_node_height(n->rn);
        if (lh > rh + 1) {  // Left heavy
            int llh = tree_node_height(n->ln->ln);
            int lrh = tree_node_height(n->ln->rn);
            if (llh >= lrh) {
                // Single Right Rotation
                tree_rotate_right(pp)->pn = pn;
            } else {
                // Double Rotation: Left-Right
                tree_rotate_left(&n->ln)->pn = n;
                tree_rotate_right(pp)->pn = pn;
            }
            tree_check_node(t->root);
        } else if (rh > lh + 1) {  // Right heavy
            int rlh = tree_node_height(n->rn->ln);
            int rrh = tree_node_height(n->rn->rn);
            if (rrh >= rlh) {
                // Single Left Rotation
                tree_rotate_left(pp)->pn = pn;
            } else {
                // Double Rotation: Right-Left
                tree_rotate_right(&n->rn)->pn = n;
                tree_rotate_left(pp)->pn = pn;
            }
            tree_check_node(t->root);
        } else {
            tree_fix_height(n); // absorb
        }
        n = pn;
    }
    #ifdef DEBUG
        tree_verify(t);
    #endif
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
    if (v) { v->pn = u->pn; }
    tree_check_node(u->pn);
    tree_check_node(v);
}

static void tree_delete_node(struct tree* t,
                             struct tree_node* n) {
    struct tree_node* s;
    struct tree_node* sr = n->pn; // node to start rebalance at
    if (!n->ln) {
        s = n->rn;
        tree_shift_nodes(t, n, s);
    } else if (!n->rn) {
        s = n->ln;
        tree_shift_nodes(t, n, s);
    } else {
        s = tree_successor(n->rn);
        if (s->pn != n) { // successor is not a direct child
            sr = s->pn;   // rebalance will start deeper in the tree
            tree_shift_nodes(t, s, s->rn);
            s->rn = n->rn;
            s->rn->pn = s;
        } else {
            sr = s;       // rebalance will start deeper in the tree
        }
        tree_shift_nodes(t, n, s);
        s->ln = n->ln;
        s->ln->pn = s;
    }
//  printf("tree_rebalance(%p) deleted: %p.pn: %p\n", sr, n, n->pn);
    tree_rebalance(t, sr);
}

static struct tree_node* tree_evict(struct tree* t, size_t window) {
    struct tree_node* n = t->nodes + t->pos;
    t->pos = (t->pos + 1) % window;
    if (n->data) {
//      printf("%p delete('%.16s') %p\n", n, n->data, n->data);
        tree_delete_node(t, n);
    }
    memset(n, 0, sizeof(*n));
    return n;
}

static inline void tree_insert(struct tree* t,
                               const uint8_t* p, size_t bytes,
                               size_t window) {
    struct tree_node* f = tree_evict(t, window); // free node
    f->data = p;
    f->height = 1;
    assert(!f->pn && !f->ln && !f->rn);
    struct tree_node* n = t->root;
    struct tree_node* w = (void*)0; // value of 'n' was 'w'
    while (n) {
        w = n;
        int cmp = memcmp(p, n->data, bytes);
        if (cmp < 0) { n = n->ln; } else { n = n->rn; }
    }
    if (!w) {
        tree_check_node(f);
        t->root = f;
        tree_check_node(t->root);
        tree_assert(t->root && !t->root->pn);
    } else {
        int cmp = memcmp(p, w->data, bytes);
        if (cmp < 0) { w->ln = f; } else { w->rn = f; }
        f->pn = w;
        tree_check_node(f); // structural
//      printf("%p inserted('%s')\n", f, f->data);
        tree_rebalance(t, f);
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
            } else {
                assert(false);
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

static void test_compare(const struct tree* t, const size_t i,
        const uint8_t* d, const size_t bytes, const size_t window,
        size_t tree_size, size_t tree_dist) {
    size_t lz77_size = 0;
    size_t lz77_dist = 0;
    lz77_find(d, bytes, i, &lz77_size, &lz77_dist, window);
    const uint8_t* match0 = d + i - tree_dist;
    const uint8_t* match1 = d + i - lz77_dist;
    if (tree_dist != lz77_dist || tree_size != lz77_size) {
        printf("[%zu] '%.*s' %3zu:%zu tree\n",
               i, (int)tree_size, match0, tree_dist, tree_size);
        printf("[%zu] '%.*s' %3zu:%zu lz77\n",
               i, (int)lz77_size, match1, lz77_dist, lz77_size);
        printf("tree_node_count(): %d\n", tree_node_count(t->root));
        tree_print(t, d + i);
    }
    if (lz77_size >= sqz_min_size || tree_size >= sqz_min_size) {
        swear(sqz_min_size <= lz77_size && lz77_size <= sqz_max_size);
        swear(sqz_min_size <= tree_size && tree_size <= sqz_max_size);
        swear(1 <= lz77_dist && lz77_dist <= window);
        swear(1 <= tree_dist && tree_dist <= window);
        assert(memcmp(match0, d + i, tree_size) == 0);
        assert(memcmp(match1, d + i, tree_size) == 0);
    }
    swear(tree_dist == lz77_dist && tree_size == lz77_size);
}

static void tree_test(struct tree* t, const uint8_t* d, const size_t bytes,
                      const size_t window) {
    printf("window: %zd\n", window);
    tree_init(t);
    size_t i = 0;
    #ifdef DEBUG
        debug_data  = d;
        debug_bytes = bytes;
        debug_tree  = t;
        debug_p     = d;
        debug_max_depth = 0;
    #endif
    while (i < bytes) {
        size_t n = bytes - i < sqz_max_size ? bytes - i : sqz_max_size;
        size_t dist = 0;
        size_t size = 0;
        tree_find(t, d + i, n, &size, &dist);
        // compare size, dist with results of lz77 search
        test_compare(t, i, d, n, window, size, dist);
        if (size >= sqz_min_size) {
            const size_t next = i + size;
            while (i < next) {
//              printf("\n[%d] insert('%s')", (int)i, d + i);
                tree_insert(t, d + i, n, window);
//              tree_debug_dump();
                i++;
                n = bytes - i < sqz_max_size ? bytes - i : sqz_max_size;
                #ifdef DEBUG
                    debug_p = d + i;
                #endif
            }
        } else {
//          printf("\n[%d] insert('%s')\n", (int)i, d + i);
            tree_insert(t, d + i, n, window);
//          tree_debug_dump();
            i++;
            #ifdef DEBUG
                debug_p = d + i;
            #endif
        }
    }
    printf("\n");
}

static void test1(void) {
    const char* str[] = {
        "abcabcdabcdeabcdefabcdefgabcdefabcdeabcd",

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
            tree_test(t, d, bytes, window);
        }
    }
}

static void test2(void) {
    static struct tree tree;
    struct tree* t = &tree;
    static uint8_t d[1024];
    memset(d, 0, sizeof(d));
    tree_test(t, d, sizeof(d), 16);
    tree_debug_dump();
}

static void test3(void) {
    // will only work with AVL or Red Black trees
    static struct tree tree;
    struct tree* t = &tree;
    static uint8_t d[128 * 1024];
    memset(d, 0, sizeof(d));
    // this will create very skew deep and will require
    // balanced tree to work:
    tree_test(t, d, sizeof(d), sqz_max_win);
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
    test2();
    test3();
    return 0;
}
