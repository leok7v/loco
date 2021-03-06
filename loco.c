/* Copyright 2020 "Leo" Dmitry Kuznetsov https://leok7v.github.io/
   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at
       http://www.apache.org/licenses/LICENSE-2.0
   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License. */
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>

#ifdef FOLDERS
#include "folders.h"
#endif

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#ifdef _MSC_VER // 1200, 1300, ...
#pragma warning(disable: 4189) // local variable is initialized but not referenced
#pragma warning(disable: 4505) // unreferenced local function has been removed
#endif

#define null NULL // beautification of code
#define byte uint8_t
#define countof(a) (sizeof(a) / sizeof((a)[0]))
#define minimum(a, b) ((a) < (b) ? (a) : (b))
#define maximum(a, b) ((a) > (b) ? (a) : (b))

#ifdef __cplusplus
extern "C" {
#endif

enum {
    limit = 6, // unary encoding bit limit (6 is optimal for thermofoil)
    start_with_bits = 3 // must be the same in encode and decode
};

// consider limit = 10 and start_with_bits = 2 for IR images

static int log2n(int v) { // undefined log2(0) assumed to be 1
    assert(v >= 0);
    if (v == 0) { return 1; }
    int k = 0;
    while ((1 << k) <= v) { k++; }
    return k;
}

typedef struct { char text[256]; } str256_t;

static str256_t binary(int v, int p) { // "v" value, "p" precision
    assert(v >= 0);
    str256_t str;
    int i = 0;
    int k = log2n(v);
    p = p - k;
    while (p > 0) { str.text[i] = '0'; i++; p--; }
    i = i + k;
    str.text[i] = 0;
    while (k > 0) { i--; str.text[i] = v % 2 ? '1' : '0'; v >>= 1; k--; }
    return str;
}

#define b2s(v, p) (binary(v, p).text)

static void hexdump(byte* data, int bytes) {
    for (int i = 0; i < bytes; i++) { printf("%02X", data[i]); }
    printf("\n");
}

static double rms(byte* a, byte* b, int n) {
    double s = 0;
    for (int i = 0; i < n; i++) {
        double e = (double)a[i] - (double)b[i];
        s += e * e;
    }
    return sqrt(s) / n;
}

static int push_bits(byte* output, int bytes, int pos, int v, int bits) {
    assert(0 <= v);
    assert(bits <= 31);
    for (int i = 0; i < bits; i++) {
        const int bit_pos = (pos + i) % 8;
        const int byte_pos = (pos + i) / 8;
        assert(0 <= byte_pos && byte_pos < bytes);
        const byte b = output[(pos + i) / 8];
        const int bv = ((1 << i) & v) != 0; // bit value
        // clear bit at bit_pos and set it to the bit value:
        output[byte_pos] = (byte)((b & ~(1 << bit_pos)) | (bv << bit_pos));
    }
    return pos + bits;
}

static int pull_bits(byte* input, int bytes, int* bp, int bits) {
    assert(bits < 31);
    int pos = *bp;
    int v = 0;
    for (int i = 0; i < bits; i++) {
        const int bit_pos = (pos + i) % 8;
        const int byte_pos = (pos + i) / 8;
        assert(0 <= byte_pos && byte_pos < bytes);
        const byte bv = (input[byte_pos] & (1 << bit_pos)) != 0; // bit value
        v |= (bv << i);
    }
// printf("@%d,%d=%d\n", pos, bits, v);
    *bp = pos + bits;
    return v;
}

static int encode_unary(byte* output, int count, int pos, int q) { // encode q as unary
    if (q >= limit) { // 24 bits versus possible 511 bits is a win? TODO: verify
        assert(q <= 0xFF);
        assert(limit <= 31);
        pos = push_bits(output, count, pos, (1 << limit) - 1, limit);
        pos = push_bits(output, count, pos, 0, 1);
        pos = push_bits(output, count, pos, q, 8);
    } else {
        while (q > 0) { pos = push_bits(output, count, pos, 1, 1); q--; }
        pos = push_bits(output, count, pos, 0, 1);
    }
    return pos;
}

static int k_freq[128];
static int bits_freq[128];
static int quo_freq[256];

static int encode_entropy(byte* output, int count, int pos, int v, int bits) {
    // simple entropy encoding https://en.wikipedia.org/wiki/Golomb_coding for now
    // can be improved to https://en.wikipedia.org/wiki/Asymmetric_numeral_systems
    assert(0 <= v && v <= 0xFF);
    const int m = 1 << bits;
k_freq[bits]++;    
    int q = v >> bits; // v / m quotient
int at = pos;
quo_freq[q]++;
    pos = encode_unary(output, count, pos, q);
    const int r = v & (m - 1); // v % m reminder (bits)
    pos = push_bits(output, count, pos, r, bits);
bits_freq[pos - at]++;
if (pos - at == 1) { assert(bits == 0 && v == 0); }
if (pos - at == 2 && v > 1) { printf("v=%d bits=%d\n", v, bits); }
if (pos - at == 3 && v > 3) { printf("v=%d bits=%d\n", v, bits); }
    return pos;
}

static int decode_unary(byte* input, int bytes, int* pos) {
    int q = 0;
    for (;;) {
        int bit = pull_bits(input, bytes, pos, 1);
        if (bit == 0) { break; }
        q++;
    }
    assert(q <= limit);
    if (q == limit) {
        return pull_bits(input, bytes, pos, 8);
    } else {
        return q;
    }
}

static int decode_entropy(byte* input, int bytes, int* pos, int bits) {
    int q = decode_unary(input, bytes, pos);
    int r = pull_bits(input, bytes, pos, bits);
    int v = (q << bits) | r;
    return v;
}

typedef struct rle_s { // RLE
    int count; // number of samples in a `run'
    int val;   // first value the a `run'
    int x;     // start x of last `run'
    int pos;   // start bit position of a `run'
    int bits;  // bit-width of Golomb entropy encoding at the beginning of a `run'
} rle_t;

typedef struct neighbors_s { // RLE
    int a; //  c b d
    int b; //  a v
    int c;
    int d;
    int d1; // d - b
    int d2; // b - c
    int d3; // c - a
} neighbors_t;

typedef struct encoder_context_s {
    int w;
    int h;
    bool  rle;
    byte* data;
    byte* output;
    int   max_bytes; // number of bytes available in output
    byte* prev; // previous line or null for y == 0
    byte* line; // current line
    int   bits; // bit-width of Golomb entropy encoding
    int   last; // last pixel value
    int   lossy;
    int   lossy2p1; // lossy * 2 + 1
    int   pos; // output bit position
    int   x;
    int   y;
    int   v; // current pixel value at [x,y]
    neighbors_t neighbors;
} encoder_context_t;

static int prediction(int x, int y, int a, int b, int c) {
    if (y == 0) {
        return x == 0 ? 0 : a;
    }
    if (x == 0) { return b; }
    if (c >= maximum(a, b)) {
        return minimum(a, b);
    } else if (c <= minimum(a, b)) {
        return maximum(a, b);
    } else {
        return a + b - c;
    }
}

static void neighbors(int x, int y, int w, byte* prev, byte* line, neighbors_t* neighbors) {
    #define ns (*neighbors)
    //  c b d
    //  a v
    ns.a = x == 0 ? 0 : line[x - 1];
    ns.c = y == 0 || x == 0 ? ns.a : prev[x - 1];
    ns.b = y == 0 ? ns.a : prev[x];
    ns.d = y == 0 || x == w - 1 ? ns.b : prev[x + 1];
    // gradients:
    ns.d1  = ns.d - ns.b;
    ns.d2  = ns.b - ns.c;
    ns.d3  = ns.c - ns.a;
    #undef ns
}

static void encode_delta(encoder_context_t* context, int v);

static void encode_run(encoder_context_t* context, int count) {
    #define ctx (*context)
    if (count == 1) { // run == 1 encoded 2 bits as 0xb10
        ctx.pos = push_bits(ctx.output, ctx.max_bytes, ctx.pos, 1, 1);
        ctx.pos = push_bits(ctx.output, ctx.max_bytes, ctx.pos, 0, 1);
    } else if (count <= 5) {
        count -= 2; // 1 already encoded above 2 -> 0, 3 -> 1, 4 -> 2, 5 -> 3
        assert(limit > 3);
        ctx.pos = push_bits(ctx.output, ctx.max_bytes, ctx.pos, 1, 1); // unary(3)
        ctx.pos = push_bits(ctx.output, ctx.max_bytes, ctx.pos, 1, 1);
        ctx.pos = push_bits(ctx.output, ctx.max_bytes, ctx.pos, 0, 1);
        ctx.pos = push_bits(ctx.output, ctx.max_bytes, ctx.pos, count, 2);
        // 5 bits 0xb110cc
    } else {
        count -= 6;
        int lb = log2n(count);
//      printf("@%d unary(%d) and %d bits of %d=0b%s\n", ctx.pos, lb + 2, lb, count, b2s(count, lb));
        // deliberately not using encode_unary here because limit may be < lb
        // need to add 2x"1" before unary lb (see above)
        ctx.pos = push_bits(ctx.output, ctx.max_bytes, ctx.pos, (1 << (lb + 2)) - 1, (lb + 2));
        ctx.pos = push_bits(ctx.output, ctx.max_bytes, ctx.pos, 0, 1);
        ctx.pos = push_bits(ctx.output, ctx.max_bytes, ctx.pos, count, lb);
        // count 6 -> 0, 7 -> 1                     log2(count) == 1
        //       8 -> 2, 9 -> 3,                    log2(count) == 2
        //      10 -> 4, 11 -> 5, 12 -> 6, 13 -> 7  log2(count) == 3
        // [10..13] encoded as 0b111110ccc (9 bit)
        // ...
        // 256-6 = 250 is encoded as 0b11,1111,1111,0,cccc,cccc (10 + 8 = 18 bit)
    }
//  printf("encode rle run count=%d @%d [%d,%d] last=%d\n", ctx.run, ctx.pos, ctx.x, ctx.y, ctx.last);
    #undef ctx
}

static void encode_rle(encoder_context_t* context) {
    #define ctx (*context)
    int count = 0; // rle number of pixels in a run
    while (ctx.x < ctx.w && abs(ctx.line[ctx.x] - ctx.last) <= ctx.lossy) {
        ctx.line[ctx.x] = (byte)ctx.last; // corrected value
        ctx.x++;
        count++;
    }
    if (count > 0) {
        encode_run(context, count);
        ctx.x--; // will be incremented by for(x) loop
    } else {
        ctx.pos = push_bits(ctx.output, ctx.max_bytes,ctx.pos, 0, 1);
        encode_delta(context, ctx.line[ctx.x]);
    }
    #undef ctx
}

static inline bool rle_mode(neighbors_t* neighbors, int lossy) {
    #define ns (*neighbors)
    return abs(ns.d1) <= lossy && abs(ns.d2) <= lossy && abs(ns.d3) <= lossy;
    #undef ns
}

static int verify[1024][1024];

static void encode_delta(encoder_context_t* context, int v) {
    #define ctx (*context)
    ctx.v = v;
    int predicted = prediction(ctx.x, ctx.y, ctx.neighbors.a, ctx.neighbors.b, ctx.neighbors.c);
    int delta = (byte)ctx.v - (byte)predicted;
    assert((byte)(predicted + delta) == (byte)ctx.v);
    if (ctx.lossy > 0) {
        // lossy adjustment
        if (delta >= 0) {
            delta = (ctx.lossy + delta) / ctx.lossy2p1;
        } else {
            delta = -(ctx.lossy - delta) / ctx.lossy2p1;
        }
        ctx.v = (byte)(predicted + delta * ctx.lossy2p1); // save reconstructed value
        ctx.line[ctx.x] = (byte)ctx.v;  // need to write back resulting value
    }
    delta = delta < 0 ? delta + 256 : delta;
    delta = delta >= 128 ? delta - 256 : delta;
    // this folds abs(deltas) > 128 to much smaller numbers which is OK
    assert(-128 <= delta && delta <= 127);
    // delta:    -128 ... -2, -1, 0, +1, +2 ... + 127
    // positive:                  0,  2,  4       254
    // negative:  255      3   1
    int rice = delta >= 0 ? delta * 2 : -delta * 2 - 1;
    assert(0 <= rice && rice <= 0xFF);
    int at = ctx.pos;
verify[ctx.y][ctx.x] = at;
//if (ctx.y <= 2 && ctx.x < 20) { printf("verify[%d][%d]=%d\n", ctx.y, ctx.x, verify[ctx.y][ctx.x]); }
    ctx.pos = encode_entropy(ctx.output, ctx.max_bytes, ctx.pos, rice, ctx.bits);
//  printf("[%3d,%-3d] predicted=%3d v=%3d rice=%4d delta=%4d bits=%d @%d\n",
//         ctx.x, ctx.y, predicted, ctx.v, rice, delta, ctx.bits, at);
    ctx.bits = 0;
    while ((1 << ctx.bits) < rice) { ctx.bits++; }
//if (ctx.bits > 1 ) { ctx.bits = log2n(rice); }
    ctx.last = ctx.v;
    #undef ctx
}

static int encode_context(encoder_context_t* context) {
    #define ctx (*context)
    for (ctx.y = 0; ctx.y < ctx.h; ctx.y++) {
        for (ctx.x = 0; ctx.x < ctx.w; ctx.x++) {
            neighbors(ctx.x, ctx.y, ctx.w, ctx.prev, ctx.line, &ctx.neighbors);
            // last == -1 at the beginning of line
            if (ctx.rle && ctx.last >= 0 && rle_mode(&ctx.neighbors, ctx.lossy)) {
                encode_rle(context);
            } else {
                encode_delta(context, ctx.line[ctx.x]);
            }
        }
        ctx.prev = ctx.line;
        ctx.line += ctx.w;
        ctx.last = -1;
        ctx.bits = start_with_bits;
    }
    return (ctx.pos + 7) / 8;
    #undef ctx
}

int encode(byte* data, int w, int h, bool rle, int lossy, byte* output, int max_bytes) {
    assert(lossy >= 0);
    encoder_context_t ctx = {};
    ctx.rle = rle;
    ctx.lossy = lossy;
    ctx.lossy2p1 = ctx.lossy * 2 + 1;
    ctx.data = data;
    ctx.w = w;
    ctx.h = h;
    ctx.output = output;
    ctx.max_bytes = max_bytes;
    ctx.last = -1;
    ctx.bits = start_with_bits; // m = (1 << bits)
    ctx.line = data;
    // shared knowledge between encoder and decoder:
    // does not have to be encoded in the stream, may as well be simply known by both
    ctx.pos = push_bits(ctx.output, ctx.max_bytes, ctx.pos, w, 16);
    ctx.pos = push_bits(ctx.output, ctx.max_bytes, ctx.pos, h, 16);
    ctx.pos = push_bits(ctx.output, ctx.max_bytes, ctx.pos, lossy, 8);
    return encode_context(&ctx);
}

static int decode_run(byte* input, int bytes, int *pos) { // return run count
int at = *pos;
    int bit = pull_bits(input, bytes, pos, 1);
    if (bit == 0) { return 1; }
    bit = pull_bits(input, bytes, pos, 1);
    if (bit == 0) {
        return pull_bits(input, bytes, pos, 2) + 2;
    } else {
        int lb = 3; // because we alread read two "1" "1" above
        // deliberately not using decode_unary because limit may be < lb
        for (;;) {
            if (pull_bits(input, bytes, pos, 1) == 0) { break; }
            lb++;
        }
        int count = pull_bits(input, bytes, pos, lb - 2) + 6;
//      printf("@%d unary(%d) and %d bits of %d=0b%s\n", at, lb + 3, lb, count, b2s(count, lb));
        return count;
    }
}

int decode(byte* input, int bytes, bool rle, byte* output, int width, int height, int loss) {
    byte* prev = null;
    byte* line = output;
    int pos = 0; // input bit position
    int bits  = start_with_bits;
    int last = -1;
    const int w = pull_bits(input, bytes, &pos, 16);
    const int h = pull_bits(input, bytes, &pos, 16);
    const int lossy = pull_bits(input, bytes, &pos, 8);
    const int lossy2p1 = lossy * 2 + 1;
    assert(w == width && h == height && lossy == loss);
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            neighbors_t nei;
            neighbors(x, y, w, prev, line, &nei);
            bool run_mode = rle && last >= 0 && rle_mode(&nei, lossy);
            if (run_mode) {
                run_mode = pull_bits(input, bytes, &pos, 1) != 0;
            }
            if (run_mode) {
                int count = decode_run(input, bytes, &pos);
//              printf("decode rle run count=%d @%d [%d,%d] last=%d\n", count, pos, x + count, y, last);
                while (count > 0) { line[x] = (byte)last; x++; count--; }
                assert(x <= w);
                x--; // because it will be incremented by for loop above
            } else {
//if (x == 6 && y == 3) { __debugbreak(); }
                int predicted = prediction(x, y, nei.a, nei.b, nei.c);
                int at = pos; // only for printf below
                int rice = decode_entropy(input, bytes, &pos, bits);
//printf("[%d,%d] %d rice=%d bits=%d predicted=%d\n", x, y, pos, rice, bits, predicted);
                assert(0 <= rice && rice <= 0xFF);
                int delta = rice % 2 == 0 ? rice / 2 : -(rice / 2) - 1;
                if (lossy > 0) {
                    delta *= lossy2p1;
                }
                int v = (byte)(predicted + delta);
                assert(0 <= v && v <= 0xFF);
                line[x] = (byte)v;
                last = v;
//              printf("[%3d,%-3d] predicted=%3d v=%3d rice=%4d delta=%4d bits=%d @%d\n",
//                      x, y, predicted, v, rice, delta, bits, at);
assert(at == verify[y][x]);
                bits = 0;
                while ((1 << bits) < rice) { bits++; }
//if (bits > 1) { bits = log2n(rice); }
            }
        }
        last = -1;
        prev = line;
        line += w;
        bits = start_with_bits;
    }
    return w * h;
}

static void d8x4_test(bool rle, int lossy) {
    enum { w = 8, h = 4, bytes = w * h };
/*
    byte data[w * h] = {
        127, 128, 127, 128, 127, 128, 127, 128,
        127, 128, 127, 128, 127, 128, 127, 128,
        127, 128, 127, 128, 127, 128, 127, 128,
        127, 128, 127, 128, 127, 128, 127, 128
    };
*/
    byte data[bytes] = {
        63, 64, 63, 64, 63, 64, 63, 64,
        63, 64, 63, 64, 63, 64, 63, 64,
        63, 63, 63, 64, 64, 64, 65, 65,
        65, 65, 65, 65, 65, 65, 65, 64
    };
    byte copy[bytes];
    memcpy(copy, data, bytes);
    byte encoded[countof(data) * 2] = {};
    int k = encode(data, w, h, rle, lossy, encoded, countof(encoded));
hexdump(encoded, k);
    byte decoded[countof(data)] = {};
    int n = decode(encoded, k, rle, decoded, w, h, lossy);
    assert(n == countof(data));
    if (lossy == 0) {
        assert(memcmp(decoded, data, n) == 0);
    } else {
        printf("error(rms) = %.1f%c\n", rms(decoded, copy, n) * 100, '%');
    }
    const int wh = w * h;
    const double bpp = k * 8 / (double)wh;
    const double percent = 100.0 * k / wh;
    printf("%dx%d %d->%d bytes %.3f bpp %.1f%c lossy(%d)%s\n",
            w, h, wh, k, bpp, percent, '%', lossy, rle ? " RLE" : "");
}

static bool option_output;
static int  option_lossy;
static int  option_threshold;

static void image_compress(const char* fn, bool rle, int lossy, bool write) {
    int w = 0;
    int h = 0;
    int c = 0;
    byte* data = stbi_load(fn, &w, &h, &c, 0);
    assert(c == 1);
    int bytes = w * h;
    if (option_threshold != 0) {
        assert(0 < option_threshold && option_threshold <= 0xFF);
        for (int i = 0; i < bytes; i++) {
            if (data[i] < option_threshold) { data[i] = 0; }
        }
    }
    byte* encoded = (byte*)malloc(bytes * 3);
    byte* decoded = (byte*)malloc(bytes);
    byte* copy    = (byte*)malloc(bytes);
    memcpy(copy, data, bytes);
    int k = encode(data, w, h, rle, lossy, encoded, bytes * 3);
    int n = decode(encoded, k, rle, decoded, w, h, lossy);
    assert(n == bytes);
    if (lossy == 0) {
        assert(memcmp(decoded, copy, n) == 0);
    }
    char filename[128];
    const char* p = strrchr(fn, '.');
    int len = (int)(p - fn);
    if (lossy != 0) {
        sprintf(filename, "%.*s.lossy(%d)%s.png", len, fn, lossy, rle ? ".rle" : "");
    } else {
        sprintf(filename, "%.*s.loco%s.png", len, fn, rle ? ".rle" : "");
    }
    const char* file = strrchr(filename, '/');
    if (file == null) { file = filename; } else { file++; }
    if (write) {
        stbi_write_png(file, w, h, 1, decoded, 0);
    }
    const int wh = w * h;
    const double bpp = k * 8 / (double)wh;
    const double percent = 100.0 * k / wh;
    if (lossy == 0) {
        printf("%s %dx%d %d->%d bytes %.3f bpp %.1f%c lossy(%d)%s\n",
                file, w, h, wh, k, bpp, percent, '%', lossy, rle ? " RLE" : "");
    } else {
        printf("%s %dx%d %d->%d bytes %.3f bpp %.1f%c lossy(%d)%s rms(err) = %.1f%c\n",
                file, w, h, wh, k, bpp, percent, '%', lossy, rle ? " RLE" : "",
                rms(decoded, copy, n) * 100, '%');
    }
    free(copy);
    free(encoded);
    free(decoded);
    stbi_image_free(data);
}

static void delta_modulo_folding(int step, bool verbose) {
    for (int p = 0; p <= 0xFF; p += step) {
        for (int v = 0; v <= 0xFF; v += step) {
            int d1 = p - v;
            assert(-255 <= d1 && d1 <= +255);
            // because: for any byte x (x + delta) == (x + delta + 256)
            assert((byte)v == (byte)(p - d1));
            int d2 = d1 < 0 ? d1 + 256 : d1;
            int d3 = d2 >= 128 ? d2 - 256 : d2;
            // this folds abs(deltas) > 128 to much smaller numbers which is OK
            assert(-128 <= d3 && d3 <= 127);
            int rice = d3 >= 0 ? d3 * 2 : -d3 * 2 - 1;
            int log2 = 0;
            while ((1 << log2) < rice) { log2++; }
            int ice = rice % 2 == 0 ? rice / 2 : -(rice / 2) - 1;
            assert(ice == d3);
            int x = (byte)(p - ice);
            if (verbose) {
                printf("p=%4d v=%4d d1=%4d d2=%4d d3=%4d rice=%4d log2=%d x=%4d\n",
                        p, v, d1, d2, d3, rice, log2, x);
            }
            assert(x == v);
        }
    }
}

static void straighten(char* pathname) {
    while (strchr(pathname, '\\') != null) { *strchr(pathname, '\\') = '/'; }
}

#ifdef FOLDERS

static void compress_folder(const char* folder_name) {
    folder_t folders = folder_open();
    int r = folder_enumerate(folders, folder_name);
    if (r != 0) { perror("failed to open folder"); exit(1); }
    const char* folder = folder_foldername(folders);
    const int n = folder_count(folders);
    for (int i = 0; i < n; i++) {
        const char* name = folder_filename(folders, i);
        int pathname_length = (int)(strlen(folder) + strlen(name) + 3);
        char* pathname = (char*)malloc(pathname_length);
        if (pathname == null) { break; }
        snprintf(pathname, pathname_length, "%s/%s", folder, name);
        straighten(pathname);
        const char* suffix = "";
        if (folder_is_folder(folders, i)) { suffix = "/"; }
        if (folder_is_symlink(folders, i)) { suffix = "->"; }
//      printf("%s%s\n", pathname, suffix);
        image_compress(pathname, false, 0, true);
        image_compress(pathname, true,  0, true);
        image_compress(pathname, false, 1, true);
        image_compress(pathname, true,  1, true);
        free(pathname);
    }
    folder_close(folders);
}

#endif

static int option_int(int argc, const char* argv[], const char* opt, int *n) {
    for (int i = 0; i < argc; i++) {
        int t = 0;
        if (sscanf(argv[i], opt, &t) == 1) {
            *n = t;
            for (int j = i + 1; j < argc; j++) { argv[i] = argv[j]; }
            argc--;
            break;
        }
    }
    return argc;
}

static int option_bool(int argc, const char* argv[], const char* opt, bool *b) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], opt) == 0) {
            *b = true;
            for (int j = i + 1; j < argc; j++) { argv[i] = argv[j]; }
            argc--;
            break;
        }
    }
    return argc;
}

static int sum(int a[], int n) {
    int s = 0;
    for (int i = 0; i < n; i++) { s += a[i]; }
    return s;
}


int main(int argc, const char* argv[]) {
    setbuf(stdout, null);
    argc = option_bool(argc, argv, "-o", &option_output);
    argc = option_int(argc, argv, "-n=%d", &option_lossy);
    argc = option_int(argc, argv, "-t=%d", &option_threshold);
    delta_modulo_folding(1, false);
//  delta_modulo_folding(63, true);
    image_compress("thermo-foil.png", false, 0, option_output);

    printf("k freq:\n");
    int k_sum = sum(k_freq, countof(k_freq));
    for (int i = 0; i < countof(k_freq); i++) {
        if (k_freq[i] != 0) {
            printf("%2d %7d %5.1f%% %5.1f%%\n", i, k_freq[i], k_freq[i] * 100.0 / k_sum, sum(k_freq, i + 1) * 100.0 / k_sum);
        }
    }

    printf("bits per symbol:\n");
    int bits_sum = sum(bits_freq, countof(bits_freq));
    for (int i = 0; i < countof(bits_freq); i++) {
        if (bits_freq[i] != 0) {
            printf("%2d %7d %5.1f%% %5.1f%%\n", i, bits_freq[i], bits_freq[i] * 100.0 / bits_sum, sum(bits_freq, i + 1) * 100.0 / bits_sum);
        }
    }

    printf("quontient:\n");
    int quo_sum = sum(quo_freq, countof(quo_freq));
    for (int i = 0; i < countof(quo_freq); i++) {
        if (quo_freq[i] != 0) {
            printf("%2d %7d %5.1f%% %5.1f%%\n", i, quo_freq[i], quo_freq[i] * 100.0 / quo_sum, sum(quo_freq, i + 1) * 100.0 / quo_sum);
        }
    }
if (1) exit(0);    
    d8x4_test(false, 0); // w/o RLE
    d8x4_test(true, 0);  // with RLE lossless
    d8x4_test(true, 1);  // with RLE lossy
    image_compress("greyscale.128x128.pgm", false, 0, option_output);
    image_compress("greyscale.128x128.pgm", true,  0, option_output);
    image_compress("greyscale.128x128.pgm", true,  1, option_output);
    image_compress("greyscale.640x480.pgm", false, 0, option_output);
    image_compress("greyscale.640x480.pgm", true,  0, option_output);
    image_compress("greyscale.640x480.pgm", false, 1, option_output);
    image_compress("greyscale.640x480.pgm", true,  1, option_output);
    image_compress("greyscale.640x480.pgm", true,  2, option_output);
    image_compress("greyscale.640x480.pgm", true,  3, option_output);
    image_compress("greyscale.640x480.pgm", true,  4, option_output);
    image_compress("thermo-foil.png", true, 1, option_output);
#ifdef FOLDERS
    while (argc > 1 && is_folder(argv[1])) {
        compress_folder(argv[1]);
        memmove(&argv[1], &argv[2], (argc - 2) * sizeof(argv[1]));
        argc--;
    }
#endif    
    return 0;
}

#ifdef __cplusplus
} // extern "C"
#endif
