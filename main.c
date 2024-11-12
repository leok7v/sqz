#define rt_implementation
#include "rt/rt.h"
#include "rt/fileio.h"

int maps_test(void);
int lz_maps_test(void);

static errno_t locate_test_folder(void) {
    for (;;) {
        if (file_exist("test/bible.txt")) { return 0; }
        char cwd[1024] = {0};
        if (!getcwd(cwd, sizeof(cwd))) { return ENOENT; }
        if (strcmp(cwd + 1, ":\\") == 0 || strcmp(cwd, "/") == 0) {
            return ENOENT;
        }
        if (file_chdir("..") != 0) { return errno; }
    }
}

int main(int argc, const char* argv[]) {
    errno_t r = locate_test_folder();
    if (r != 0) { return r; }
    if (argc >= 2 && strcmp(argv[1], "maps") == 0) { r = maps_test(); }
    if (argc >= 2 && strcmp(argv[1], "lzm") == 0)  { r = lz_maps_test(); }
    return r;
}

