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

static errno_t file_chdir(const char* name);
static bool    file_exist(const char* filename);
static errno_t file_size(FILE* f, size_t* size);
static errno_t file_read_fully(const char* fn, const uint8_t* *data, size_t *bytes);

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

#endif // file_header_included
