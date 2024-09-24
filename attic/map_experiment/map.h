#ifndef map_header_included
#define map_header_included

#include <stdint.h>
#include <string.h>

#ifndef assert
#include <assert.h>
#endif

// Very simple hash map for lz77 compression dictionary
// Only supports d from 2 to 255 b because
// 255 to ~2 b compression is about 1% of source and is good enough.

typedef struct map_entry {
    const uint8_t* data;
    uint64_t hash;
    uint32_t bytes;
} map_entry;

typedef struct {
    map_entry* entry;
    uint32_t n;
    uint32_t entries;
    uint32_t max_chain;
    uint32_t max_bytes;
} map_type;

static void        map_init(map_type* m, map_entry entry[], size_t n);
static int32_t     map_get(const map_type* m, const void* data, uint32_t bytes);
static int32_t     map_put(map_type* m, const void* data, uint32_t bytes);
static int32_t     map_best(const map_type* m, const void* data, size_t bytes);
static void        map_clear(map_type *m);

// map_put()  is no operation if map is filled to 75% or more
// map_get()  returns index of matching entry or -1
// map_best() returns index of longest matching entry or -1

// FNV Fowler–Noll–Vo hash function
// https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function

// FNV offset basis for 64-bit:
static const uint64_t map_hash_init = 0xCBF29CE484222325;

// FNV prime for 64-bit
static const uint64_t map_prime64   = 0x100000001B3;

static inline uint64_t map_hash64_byte(uint64_t hash, const uint32_t byte) {
    return (hash ^ (uint64_t)byte) * map_prime64;
}

static inline uint64_t map_hash64(const uint8_t* data, size_t bytes) {
    assert(3 <= bytes && bytes <= UINT32_MAX);
    uint64_t hash = map_hash_init;
    for (size_t i = 0; i < bytes; i++) {
        hash = map_hash64_byte(hash, data[i]);
    }
    return hash;
}

static void map_init(map_type* m, map_entry entry[], size_t n) {
    assert(16 < n && n < UINT32_MAX);
    m->entry = entry;
    m->n = (int32_t)n;
    memset(m->entry, 0x00, n * sizeof(m->entry[0]));
    m->entries = 0;
    m->max_chain = 0;
    m->max_bytes = 0;
}

static int32_t map_get_hashed(const map_type* m, uint64_t hash,
                              const void* d, uint32_t b) {
    assert(2 <= b);
    const map_entry* entries = m->entry;
    size_t i = (size_t)hash % m->n;
    // Because map is filled to 3/4 only there will always be
    // an empty slot at the end of the chain.
    while (entries[i].bytes > 0) {
        if (entries[i].bytes == b && entries[i].hash == hash &&
            memcmp(entries[i].data, d, b) == 0) {
            return (int32_t)i;
        }
        i = (i + 1) % m->n;
    }
    return -1;
}

static int32_t map_get(const map_type* m, const void* d, uint32_t b) {
    return map_get_hashed(m, map_hash64(d, b), d, b);
}

static int32_t map_put(map_type* m, const void* d, uint32_t b) {
    enum { max_bytes = sizeof(m->entry[0]) - 1 };
    assert(3 <= b && b <= UINT32_MAX);
    if (m->entries < m->n * 3 / 4) {
        map_entry* entries = m->entry;
        uint64_t hash = map_hash64(d, b);
        size_t i = (size_t)hash % m->n;
        uint32_t chain = 0; // max chain length
        while (entries[i].bytes > 0) {
            if (entries[i].bytes == b && entries[i].hash == hash &&
                memcmp(entries[i].data, d, b) == 0) {
                assert((const uint8_t*)d > (const uint8_t*)entries[i].data);
                entries[i].data = d; // update to shorter distance
                return (int32_t)i; // found match with existing entry
            }
            chain++;
            i = (i + 1) % m->n;
            assert(chain < m->n); // looping endlessly?
        }
        if (chain > m->max_chain) { m->max_chain = chain; }
        if (b > m->max_bytes) { m->max_bytes = b; }
        entries[i].data = d;
        entries[i].hash = hash;
        entries[i].bytes = b;
        m->entries++;
        return (int32_t)i;
    }
    return -1;
}

static int32_t map_best(const map_type* m, const void* data, size_t bytes) {
    int32_t best = -1; // best (longest) result
    if (bytes >= 3) {
        const uint32_t b = (uint32_t)(bytes < UINT32_MAX ? bytes : UINT32_MAX);
        const uint8_t* d = (uint8_t*)data;
        uint64_t hash = map_hash64_byte(map_hash_init, d[0]);
        hash = map_hash64_byte(hash, d[1]);
        for (uint8_t i = 2; i < b - 1; i++) {
            hash = map_hash64_byte(hash, d[i]);
            int32_t r = map_get_hashed(m, hash, data, i + 1);
            if (r != -1) {
                best = r;
            } else {
                break; // will return longest matching entry index
            }
        }
    }
    return best;
}

static void map_clear(map_type *m) {
    memset(m->entry, 0x00, m->n * sizeof(m->entry[0]));
    m->entries = 0;
    m->max_chain = 0;
    m->max_bytes = 0;
}

#endif


