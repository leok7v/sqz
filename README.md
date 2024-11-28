# Squeeze

Very simple LZ77 + Range Coder compression

### Based on:

https://en.wikipedia.org/wiki/LZ77_and_LZ78
https://en.wikipedia.org/wiki/Adaptive_Huffman_coding
https://en.wikipedia.org/wiki/Deflate
https://en.wikipedia.org/wiki/Header-only
https://github.com/nothings/single_file_libs

### Goals:

* Simplicity (sqz.h LoC: < 700).
* Ease of build and use (C99/C17/C23).
* Amalgamated into single header file library.
* No external dependencies.

### No goals:

* Performance and bitrate (CPU and memory).
* Existing archivers compatibility.
* Stream to stream encoding decoding.
* 16 bit CPU architectures.
* Beating LZMA (state and XOR delta encoding, prefilters etc)

### Code layout:

* inc/sqz/sqz.h - main header file
* src/sqz.c - implementation
* shl/sqz/sqz.h - amalgamated single header library

### Algorithm Overview:

The `sqz` operates as a map dictionaries optimized LZ77 search with
not matched bytes and length distance backreferences encoded by range 
coder.

### Error Handling:
- The `error` field in the `sqz_type` struct is used to track any 
  issues that arise during compression or decompression. 
  If an error occurs (e.g., out of memory, invalid input), the 
  compression/decompression process is halted.

### Theory of Operation Summary:
The `sqz` interface provides an adaptive compression algorithm 
that dynamically adjusts its probability models based on the input data. 
It uses LZ77 to find repeating patterns in the data and encodes 
them efficiently using backreferences. Range coding is used to 
represent both literal bytes and length/position pairs compactly. 
By updating the probability models as data is processed, the compressor 
adapts to the characteristics of the input data, ensuring that 
commonly occurring symbols are represented with fewer bits.

### Supported Integer models:

| Model     | ILP32 | ILP64 | LP64 | LLP64 |
|-----------|-------|-------|------|-------|
| int       | 32    | 64    | 32   | 32    |
| long      | 32    | 64    | 64   | 32    |
| pointer   | 32    | 64    | 64   | 64    |
| long long | 64    | 64    | 64   | 64    |

### Build targets:

* x86     (Win) 32 bit (ILP32)
* x64     (Win) 64 bit LLP64 
* ARM64EC (Win) same as ARM64 
* ARM64   (Win) 64 bit LLP64 
* ARM64   (Nix) 64 bit LP64 
* ARM     (Win) 32 bit ILP32 cross compilation

size_t could be int32_t / uint32_t or uint64_t on *P64  

### Test materials:

Because Chinese texts are very compact comparing to e.g. the KJV bible
the Guttenberg License wording is stripped from the text files.

* See downloads.bat

### References:

* https://cbloomrants.blogspot.com/2008/10/10-01-08-first-look-at-lzma.html
* https://cbloomrants.blogspot.com/2010/08/08-20-10-deobfuscating-lzma.html
* https://cbloomrants.blogspot.com/2012/10/10-02-12-small-note-on-lzham.html
* https://cbloomrants.blogspot.com/2014/06/06-12-14-some-lzma-notes.html
* https://cbloomrants.blogspot.com/2014/06/06-16-14-rep0-exclusion-in-lzma-like.html
* https://cbloomrants.blogspot.com/2016/06/06-09-16-fundamentals-of-modern-lz-two.html
* https://cbloomrants.blogspot.com/2017/07/09-27-08-2.html
* https://cbloomrants.blogspot.com/2015/01/01-23-15-lza-new-optimal-parse.html
