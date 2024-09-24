# SQUEEZE_MAP_EXPERIMENT

Using hash map dictionary actually makes compression worse.
The reason is that the dictionary references introduce "far"
relatively large distances and the position encoding for
distance that uses small Huffman (31 terminals tree) becomes
noisy and a lot of extra bits are written as a result.
To make dictionary work, implementation of LZMA like scheme
suites better with minimum length 2 instead of 3 and range
encoding instead of Huffman:
https://en.wikipedia.org/wiki/Range_coding
