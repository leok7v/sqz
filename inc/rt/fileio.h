#ifndef file_header_included
#define file_header_included

#include <errno.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#ifdef _WIN32
#include <direct.h> // chdir
#define chdir(d) _chdir(d)
#else
#include <unistd.h> // chdir
#endif

// input/output to file or memory:

struct io { // either memory or file i/o:
    uint8_t* data;
    size_t   capacity;
    size_t   allocated;
    FILE*    file;
    uint64_t bytes;    // number of bytes read by read_byte()
    uint64_t written;  // number of bytes written by write_byte()
    uint64_t checksum; // FNV hash of put()/get() single byte io
    int32_t  error;    // sticky
    bool     fail_fast;
};

static errno_t file_chdir(const char* name);
static bool    file_exist(const char* filename);
static errno_t file_size(FILE* f, size_t* size);
static errno_t file_read_fully(const char* fn, const uint8_t* *data,
                               size_t *bytes);

static uint64_t checksum_init(void); // FNV-1a offset basis
static uint64_t checksum_append(uint64_t checksum, const uint8_t byte);

static void     io_init(struct io* io);
static void     io_init_with(struct io* io, void* data, size_t bytes);
static void     io_alloc(struct io* io, size_t bytes);
static void     io_open(struct io* io, const char* filename);
static void     io_create(struct io* io, const char* filename);
static void     io_rewind(struct io* io);
static void     io_write(struct io* io, void* data, size_t bytes);
static void     io_read(struct io* io, void* data, size_t bytes);
static void     io_put(struct io* io, uint8_t b); // accumulates checksum
static uint8_t  io_get(struct io* io);            // accumulates checksum
static uint64_t io_get64(struct io* io);
static void     io_put64(struct io* io, uint64_t v);
static void     io_read_fully(struct io* io, const char* fn);
static void     io_write_fully(struct io* io, const char* fn);
static void     io_close(struct io* io);

static bool file_exist(const char* filename) {
    struct stat st = {0};
    return stat(filename, &st) == 0;
}

static errno_t file_size(FILE* f, size_t* size) {
    // on error returns (fpos_t)-1 and sets errno
    fpos_t pos = 0;
    if (fgetpos(f, &pos) != 0) { return errno; }
    if (fseek(f, 0, SEEK_END) != 0) { return errno; }
    fpos_t eof = 0;
    if (fgetpos(f, &eof) != 0) { return errno; }
    if (fseek(f, 0, SEEK_SET) != 0) { return errno; }
    if ((uint64_t)eof > SIZE_MAX) { return E2BIG; }
    *size = (size_t)eof;
    return 0;
}

static errno_t file_read_whole_file(FILE* f, const uint8_t* *data,
                                             size_t *bytes) {
    size_t size = 0;
    errno_t r = file_size(f, &size);
    if (r != 0) { return r; }
    if (size > SIZE_MAX) { return E2BIG; }
    uint8_t* p = (uint8_t*)malloc(size); // does set errno on failure
    if (p == NULL) { return errno; }
    if (fread(p, 1, size, f) != (size_t)size) { free(p); return errno; }
    *data = p;
    *bytes = (size_t)size;
    return 0;
}

static errno_t file_read_fully(const char* fn, const uint8_t* *data,
                                               size_t *bytes) {
    FILE* f = NULL;
    errno_t r = fopen_s(&f, fn, "rb");
    if (r != 0) {
        printf("Failed to open file \"%s\": %s\n", fn, strerror(r));
        return r;
    }
    r = file_read_whole_file(f, data, bytes); // to the heap
    if (r != 0) {
        printf("Failed to read file \"%s\": %s\n", fn, strerror(r));
        fclose(f);
        return r;
    }
    return fclose(f) == 0 ? 0 : errno;
}

static errno_t file_chdir(const char* name) {
    if (chdir(name) != 0) { return errno; }
    return 0;
}

#define io_fail_fast(io) do {       \
    if (io->fail_fast) {            \
        swear(io->error == 0);      \
        exit(io->error);            \
    }                               \
} while (0)

// https://en.wikipedia.org/wiki/Fowler%E2%80%93Noll%E2%80%93Vo_hash_function

static uint64_t checksum_init(void) {
    return 0xCBF29CE484222325uLL; // FNV offset basis
}

static uint64_t checksum_append(uint64_t checksum, const uint8_t byte) {
    checksum ^= byte;
    checksum *= 0x100000001B3; // FNV prime
    checksum ^= (checksum >> 32);
    return (checksum << 7) | (checksum >> (64 - 7));
}

static void io_init(struct io* io) {
    memset(io, 0, sizeof(*io));
    io->checksum = checksum_init();
}

static void io_init_with(struct io* io, void* data, size_t bytes) {
    // caller responsible for [data:bytes] lifetime
    swear(bytes > 0 && data != 0);
    io_init(io);
    io->data = data;
    io->capacity = bytes;
}

static void io_alloc(struct io* io, size_t bytes) {
    io_init(io);
    io->data = calloc(1, bytes);
    io->error = io->data != NULL ? 0 : errno;
    io_fail_fast(io);
    if (io->data != NULL) {
        io->capacity = bytes;
        io->allocated = bytes;
    }
}

static void io_open(struct io* io, const char* filename) {
    io_init(io);
    io->file = fopen(filename, "rb");
    io->error = io->file != NULL ? 0 : errno;
    io_fail_fast(io);
}

static void io_create(struct io* io, const char* filename) {
    io_init(io);
    io->file = fopen(filename, "wb");
    io->error = io->file != NULL ? 0 : errno;
    io_fail_fast(io);
}

static void io_rewind(struct io* io) {
    if (io->file != NULL) {
        io->error = fseek(io->file, 0, SEEK_SET) == 0 ? 0 : errno;
    } else if (io->data != NULL) {
        io->bytes = 0;
    } else {
        io->error = EINVAL;
    }
    io_fail_fast(io);
}

static void io_write(struct io* io, void* data, size_t bytes) {
    if (io->file != NULL) {
        io->error = fwrite(data, bytes, 1, io->file) == 0 ? errno : 0;
    } else if (io->data != NULL) {
        if (io->written + bytes <= io->capacity) {
            memcpy(io->data + io->written, data, bytes);
        } else {
            io->error = E2BIG;
        }
    } else {
        io->error = EINVAL;
    }
    io_fail_fast(io);
    if (io->error == 0) { io->written += bytes; }
}

static void io_read(struct io* io, void* data, size_t bytes) {
    if (io->file != NULL) {
        io->error = fread(data, bytes, 1, io->file) == 0 ? errno : 0;
    } else if (io->data != NULL) {
        if (io->bytes + bytes <= io->written) {
            memcpy(data, io->data + io->bytes, bytes);
        } else {
            io->error = EIO;
        }
    } else {
        io->error = EINVAL;
    }
    io_fail_fast(io);
    if (io->error == 0) { io->bytes += bytes; }
}

static void io_put(struct io* io, uint8_t b) {
    if (io->error == 0) {
        io_write(io, &b, sizeof(b));
    }
    if (io->error == 0) {
        io->checksum = checksum_append(io->checksum, b);
    }
}

static uint8_t io_get(struct io* io) {
    uint8_t b = 0;
    if (io->error == 0) {
        io_read(io, &b, sizeof(b));
    }
    if (io->error == 0) {
        io->checksum = checksum_append(io->checksum, b);
    }
    return b;
}

static uint64_t io_get64(struct io* io) {
    uint64_t v = 0;
    io_read(io, &v, sizeof(v));
    return v;
}

static void io_put64(struct io* io, uint64_t v) {
    io_write(io, &v, sizeof(v));
}

static void io_read_fully(struct io* io, const char* fn) {
    io_init(io);
    io->error = file_read_fully(fn, &io->data, &io->capacity);
    if (io->data != NULL) {
        io->allocated = io->capacity;
    }
    io_fail_fast(io);
}

static void io_write_fully(struct io* io, const char* fn) {
    swear(io->file == NULL && io->data != NULL);
    FILE* f = fopen(fn, "wb");
    if (f != NULL) {
        size_t written = fwrite(&io->data, (size_t)io->written, 1, io->file);
        io->error = written == 1 ? 0 : errno;
        errno_t r = fclose(f) == 0 ? 0 : errno;
        if (io->error == 0) { io->error = r; }
    } else {
        io->error = errno;
    }
    io_fail_fast(io);
}

static void io_close(struct io* io) {
    if (io->file != NULL) {
        io->error = fclose(io->file) == 0 ? 0 : errno;
        io_fail_fast(io);
    } else if (io->allocated > 0) {
        free(io->data);
    } else if (io->data == NULL) {
        io->error = EINVAL;
    }
    io_fail_fast(io);
}

#endif // file_header_included
