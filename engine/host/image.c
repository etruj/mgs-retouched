/* PNG decoding to premultiplied RGBA, including Apple's "iPhone-optimised" CgBI PNGs
 * (CgBI chunk before IHDR, raw deflate without zlib wrapper, BGRA, premultiplied).
 * CgBI pixels are already premultiplied: they are only reordered, never un-premultiplied,
 * so the bytes match what CoreGraphics handed the game. Detection is per file. */
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static int paeth(int a, int b, int c) { int p = a + b - c, pa = abs(p - a), pb = abs(p - b), pc = abs(p - c); return pa <= pb && pa <= pc ? a : pb <= pc ? b : c; }

static int unfilter(uint8_t *raw, size_t rawlen, int w, int h, int bpp_bits, uint8_t *out /* h * stride */) {
    size_t stride = ((size_t)w * bpp_bits + 7) / 8; int bpp = (bpp_bits + 7) / 8;
    if (rawlen < (stride + 1) * h) return -1;
    for (int y = 0; y < h; y++) {
        uint8_t *line = raw + y * (stride + 1), f = line[0], *cur = line + 1, *dst = out + y * stride;
        const uint8_t *prev = y ? out + (y - 1) * stride : NULL;
        for (size_t x = 0; x < stride; x++) {
            int a = x >= (size_t)bpp ? dst[x - bpp] : 0, b = prev ? prev[x] : 0, c = (prev && x >= (size_t)bpp) ? prev[x - bpp] : 0, v = cur[x];
            switch (f) { case 1: v += a; break; case 2: v += b; break; case 3: v += (a + b) / 2; break; case 4: v += paeth(a, b, c); break; case 0: break; default: return -1; }
            dst[x] = (uint8_t)v;
        }
    }
    return 0;
}

int img_decode_file(const char *path, uint32_t *ow, uint32_t *oh, int *oalpha, uint8_t **out) {
    long n; uint8_t *d = vfs_read_all(path, &n); if (!d) return -1;
    if (n < 8 || memcmp(d, "\x89PNG\r\n\x1a\n", 8)) { hlog("image: %s is not PNG", path); free(d); return -1; }
    int cgbi = 0, w = 0, h = 0, depth = 0, ctype = 0, interlace = 0;
    uint8_t pal[256][4]; int npal = 0; memset(pal, 255, sizeof pal);
    int trns_g = -1, trns_r = -1, trns_gr = -1, trns_b = -1;
    uint8_t *idat = NULL; size_t idat_len = 0;
    for (long p = 8; p + 12 <= n;) {
        uint32_t len = be32(d + p); const uint8_t *type = d + p + 4, *c = d + p + 8;
        if (p + 12 + (long)len > n) break;
        if (!memcmp(type, "CgBI", 4)) cgbi = 1;
        else if (!memcmp(type, "IHDR", 4)) { w = be32(c); h = be32(c + 4); depth = c[8]; ctype = c[9]; interlace = c[12]; }
        else if (!memcmp(type, "PLTE", 4)) { npal = len / 3; for (int i = 0; i < npal; i++) { pal[i][0] = c[3*i]; pal[i][1] = c[3*i+1]; pal[i][2] = c[3*i+2]; } }
        else if (!memcmp(type, "tRNS", 4)) {
            if (ctype == 3) for (uint32_t i = 0; i < len && i < 256; i++) pal[i][3] = c[i];
            else if (ctype == 0) trns_g = c[0] << 8 | c[1];
            else if (ctype == 2) { trns_r = c[0] << 8 | c[1]; trns_gr = c[2] << 8 | c[3]; trns_b = c[4] << 8 | c[5]; }
        }
        else if (!memcmp(type, "IDAT", 4)) { idat = realloc(idat, idat_len + len); memcpy(idat + idat_len, c, len); idat_len += len; }
        else if (!memcmp(type, "IEND", 4)) break;
        p += 12 + len;
    }
    free(d);
    if (!w || !h || !idat) { free(idat); hlog("image: %s: no image data", path); return -1; }
    int chans = ctype == 0 ? 1 : ctype == 2 ? 3 : ctype == 3 ? 1 : ctype == 4 ? 2 : 4;
    int bits = chans * depth;
    /* inflate: CgBI is raw deflate, standard PNG is zlib-wrapped */
    size_t rawcap = 0;
    if (!interlace) rawcap = ((size_t)w * bits + 7) / 8 * h + h;
    else for (int pass = 0; pass < 7; pass++) {
        static const int xs[7] = {0,4,0,2,0,1,0}, ys[7] = {0,0,4,0,2,0,1}, dx[7] = {8,8,4,4,2,2,1}, dy[7] = {8,8,8,4,4,2,2};
        int pw = (w - xs[pass] + dx[pass] - 1) / dx[pass], ph = (h - ys[pass] + dy[pass] - 1) / dy[pass];
        if (pw > 0 && ph > 0) rawcap += ((size_t)pw * bits + 7) / 8 * ph + ph;
    }
    uint8_t *raw = malloc(rawcap + 16);
    z_stream zs; memset(&zs, 0, sizeof zs);
    inflateInit2(&zs, cgbi ? -15 : 15);
    zs.next_in = idat; zs.avail_in = (uInt)idat_len; zs.next_out = raw; zs.avail_out = (uInt)rawcap + 16;
    int zr = inflate(&zs, Z_FINISH); size_t rawlen = zs.total_out; inflateEnd(&zs); free(idat);
    if (zr != Z_STREAM_END && rawlen < rawcap) { hlog("image: %s: inflate failed (%d)", path, zr); free(raw); return -1; }
    /* unfilter into a w*h sample image (de-interlacing as needed) */
    size_t stride = ((size_t)w * bits + 7) / 8;
    uint8_t *px = calloc(stride, h);
    if (!interlace) { if (unfilter(raw, rawlen, w, h, bits, px)) { hlog("image: %s: bad filter", path); free(raw); free(px); return -1; } }
    else {
        static const int xs[7] = {0,4,0,2,0,1,0}, ys[7] = {0,0,4,0,2,0,1}, dx[7] = {8,8,4,4,2,2,1}, dy[7] = {8,8,8,4,4,2,2};
        size_t off = 0;
        for (int pass = 0; pass < 7; pass++) {
            int pw = (w - xs[pass] + dx[pass] - 1) / dx[pass], ph = (h - ys[pass] + dy[pass] - 1) / dy[pass];
            if (pw <= 0 || ph <= 0) continue;
            size_t ps = ((size_t)pw * bits + 7) / 8; uint8_t *tmp = malloc(ps * ph);
            unfilter(raw + off, rawlen - off, pw, ph, bits, tmp); off += (ps + 1) * ph;
            for (int y = 0; y < ph; y++) for (int x = 0; x < pw; x++) {
                int X = xs[pass] + x * dx[pass], Y = ys[pass] + y * dy[pass];
                if (bits >= 8) memcpy(px + Y * stride + (size_t)X * (bits / 8), tmp + y * ps + (size_t)x * (bits / 8), bits / 8);
                else { int v = (tmp[y * ps + x * bits / 8] >> (8 - bits - (x * bits) % 8)) & ((1 << bits) - 1);
                       px[Y * stride + X * bits / 8] |= (uint8_t)(v << (8 - bits - (X * bits) % 8)); }
            }
            free(tmp);
        }
    }
    free(raw);
    /* expand to RGBA8, then premultiply (CgBI already is) */
    uint8_t *rgba = malloc((size_t)w * h * 4); int has_alpha = 0;
    for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
        const uint8_t *s = px + y * stride; uint8_t *o = rgba + ((size_t)y * w + x) * 4;
        int r, g, b, a = 255;
        #define S16(i) (depth == 16 ? (s[(x * chans + (i)) * 2] << 8 | s[(x * chans + (i)) * 2 + 1]) : s[x * chans + (i)])
        #define TO8(v) (depth == 16 ? (v) >> 8 : (v))
        if (ctype == 3 || (ctype == 0 && depth < 8)) {
            int v = (s[x * depth / 8] >> (8 - depth - (x * depth) % 8)) & ((1 << depth) - 1);
            if (ctype == 3) { r = pal[v][0]; g = pal[v][1]; b = pal[v][2]; a = pal[v][3]; }
            else { int g8 = v * 255 / ((1 << depth) - 1); r = g = b = g8; if (v == trns_g) a = 0; }
        } else if (ctype == 0) { int v = S16(0); r = g = b = TO8(v); if (v == trns_g) a = 0; }
        else if (ctype == 4) { r = g = b = TO8(S16(0)); a = TO8(S16(1)); }
        else if (ctype == 2) { int R = S16(0), G = S16(1), B = S16(2); r = TO8(R); g = TO8(G); b = TO8(B); if (R == trns_r && G == trns_gr && B == trns_b) a = 0; }
        else { r = TO8(S16(0)); g = TO8(S16(1)); b = TO8(S16(2)); a = TO8(S16(3)); }
        if (cgbi && chans >= 3) { int t = r; r = b; b = t; }       /* BGR(A) -> RGB(A) */
        if (a != 255) has_alpha = 1;
        if (!cgbi && a != 255) { r = (r * a + 127) / 255; g = (g * a + 127) / 255; b = (b * a + 127) / 255; }
        o[0] = (uint8_t)r; o[1] = (uint8_t)g; o[2] = (uint8_t)b; o[3] = (uint8_t)a;
    }
    free(px);
    *ow = (uint32_t)w; *oh = (uint32_t)h; *oalpha = has_alpha || ctype == 4 || ctype == 6 || (ctype == 3 && npal && 0); *out = rgba;
    return 0;
}

/* imports: decode into a host-side handle, then copy into module memory */
#define MAXIMG 64
static uint8_t *pending[MAXIMG]; static uint32_t pend_size[MAXIMG];
u32 w2c_env_host_image_decode(struct w2c_env *e, u32 path, u32 pw, u32 ph, u32 palpha) {
    (void)e; char real[2048]; host_map_path(CSTR(path), real, sizeof real);
    uint32_t w, h; int a; uint8_t *px;
    if (img_decode_file(real, &w, &h, &a, &px)) return (u32)-1;
    for (int i = 0; i < MAXIMG; i++) if (!pending[i]) {
        pending[i] = px; pend_size[i] = w * h * 4;
        *(uint32_t *)MEM(pw) = w; *(uint32_t *)MEM(ph) = h; *(int32_t *)MEM(palpha) = a;
        return (u32)i;
    }
    free(px); return (u32)-1;
}
void w2c_env_host_image_copy(struct w2c_env *e, u32 hd, u32 dst) {
    (void)e; if (hd >= MAXIMG || !pending[hd]) return;
    memcpy(MEM(dst), pending[hd], pend_size[hd]); free(pending[hd]); pending[hd] = NULL;
}
