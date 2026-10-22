/* CoreGraphics subset + UIImage/UIFont/UIColor + NSString drawing.
 * Device space of a bitmap context: origin bottom-left, y up; memory row 0 is the top.
 * Images are premultiplied RGBA, which is what CoreGraphics hands the game. */
#include "rt.h"
extern int rt_trace_msgs;
static int getenv_dbg(void) { return rt_trace_msgs; }

typedef struct { u32 magic, w, h, data, alpha_info, rc; } Img;
typedef struct { u32 magic, data, w, h, bpr, bpc, ncomp, alpha_only, owns; float ctm[6]; float fill[4]; } Ctx;
#define IMG_MAGIC 0x43474D49u
#define CTX_MAGIC 0x43475843u
static u32 cs_rgb = 1, cs_gray = 2;      /* colour spaces are just tokens */

void *_CGColorSpaceCreateDeviceRGB(void) { return &cs_rgb; }
void *_CGColorSpaceCreateDeviceGray(void) { return &cs_gray; }
void _CGColorSpaceRelease(void *s) { (void)s; }
void _CFRelease(void *cf) { (void)cf; }

/* ---- CGImage ---- */
u32 cg_image_from_file(const char *path) {
    u32 w, h; int alpha;
    int hd = host_image_decode(path, &w, &h, &alpha);
    if (hd < 0) return 0;
    Img *im = calloc(1, sizeof *im);
    im->magic = IMG_MAGIC; im->w = w; im->h = h; im->rc = 1;
    im->alpha_info = alpha ? 1 /* kCGImageAlphaPremultipliedLast */ : 5 /* kCGImageAlphaNoneSkipLast */;
    u8 *px = malloc(w * h * 4); host_image_copy(hd, px); im->data = A(px);
    if (getenv_dbg()) rt_log("CGImage 0x%x %ux%u data 0x%x-0x%x (%s)", A(im), w, h, A(px), A(px) + w * h * 4, path);
    return A(im);
}
static Img *IM(void *p) { Img *i = p; if (!i || i->magic != IMG_MAGIC) rt_fatal("bad CGImageRef %p", p); return i; }
u32 _CGImageGetWidth(void *i) { return IM(i)->w; }
u32 _CGImageGetHeight(void *i) { return IM(i)->h; }
u32 _CGImageGetAlphaInfo(void *i) { return IM(i)->alpha_info; }
void *_CGImageGetColorSpace(void *i) { (void)IM(i); return &cs_rgb; }
void cg_image_release(u32 i) { Img *im = P(i); if (!i) return; free(P(im->data)); im->magic = 0; free(im); }

/* ---- CGBitmapContext ---- */
void *_CGBitmapContextCreate(void *data, u32 w, u32 h, u32 bpc, u32 bpr, void *space, u32 info) {
    Ctx *c = calloc(1, sizeof *c);
    if (getenv_dbg()) rt_log("CGBitmapContextCreate data 0x%x %ux%u bpr %u info %x", A(data), w, h, bpr, info);
    c->magic = CTX_MAGIC; c->w = w; c->h = h; c->bpc = bpc;
    u32 ai = info & 0x1f;
    c->alpha_only = (ai == 7);                                 /* kCGImageAlphaOnly */
    c->ncomp = (space == &cs_gray || c->alpha_only) ? 1 : 4;
    if (bpc != 8) rt_log("CGBitmapContextCreate: %u bits per component not supported", bpc);
    c->bpr = bpr ? bpr : w * c->ncomp;
    if (!data) { data = calloc(c->bpr, h); c->owns = 1; }
    c->data = A(data);
    c->ctm[0] = 1; c->ctm[3] = 1;
    c->fill[3] = 1;
    return c;
}
static Ctx *CX(void *p) { Ctx *c = p; if (!c || c->magic != CTX_MAGIC) rt_fatal("bad CGContextRef %p", p); return c; }
void _CGContextRelease(void *p) { if (!p) return; Ctx *c = CX(p); if (c->owns) free(P(c->data)); c->magic = 0; free(c); }
static void concat(Ctx *c, float a, float b, float cc, float d, float tx, float ty) {
    float *m = c->ctm, n[6];
    n[0] = a * m[0] + b * m[2];      n[1] = a * m[1] + b * m[3];
    n[2] = cc * m[0] + d * m[2];     n[3] = cc * m[1] + d * m[3];
    n[4] = tx * m[0] + ty * m[2] + m[4]; n[5] = tx * m[1] + ty * m[3] + m[5];
    memcpy(m, n, sizeof n);
}
void _CGContextTranslateCTM(void *p, float tx, float ty) { concat(CX(p), 1, 0, 0, 1, tx, ty); }
void _CGContextScaleCTM(void *p, float sx, float sy) { concat(CX(p), sx, 0, 0, sy, 0, 0); }
void _CGContextConcatCTM(void *p, float a, float b, float c, float d, float tx, float ty) { concat(CX(p), a, b, c, d, tx, ty); }
void _CGContextSetRGBFillColor(void *p, float r, float g, float b, float a) { Ctx *c = CX(p); c->fill[0] = r * a; c->fill[1] = g * a; c->fill[2] = b * a; c->fill[3] = a; }
void _CGContextSetGrayFillColor(void *p, float g, float a) { Ctx *c = CX(p); c->fill[0] = c->fill[1] = c->fill[2] = g * a; c->fill[3] = a; }
char _CGAffineTransformIsIdentity(float a, float b, float c, float d, float tx, float ty) { return a == 1 && b == 0 && c == 0 && d == 1 && tx == 0 && ty == 0; }
void _CGAffineTransformScale(float *out, float a, float b, float c, float d, float tx, float ty, float sx, float sy) {
    out[0] = a * sx; out[1] = b * sx; out[2] = c * sy; out[3] = d * sy; out[4] = tx; out[5] = ty;
}

static void apply(const float *m, float x, float y, float *ox, float *oy) { *ox = m[0] * x + m[2] * y + m[4]; *oy = m[1] * x + m[3] * y + m[5]; }
static int invert(const float *m, float *r) {
    float det = m[0] * m[3] - m[1] * m[2];
    if (det == 0) return 0;
    r[0] = m[3] / det; r[1] = -m[1] / det; r[2] = -m[2] / det; r[3] = m[0] / det;
    r[4] = -(r[0] * m[4] + r[2] * m[5]); r[5] = -(r[1] * m[4] + r[3] * m[5]);
    return 1;
}
/* device bounding box of a user rect, clipped to the context */
static void dev_bbox(Ctx *c, float x, float y, float w, float h, int *x0, int *y0, int *x1, int *y1) {
    float px[4], py[4];
    apply(c->ctm, x, y, &px[0], &py[0]); apply(c->ctm, x + w, y, &px[1], &py[1]);
    apply(c->ctm, x, y + h, &px[2], &py[2]); apply(c->ctm, x + w, y + h, &px[3], &py[3]);
    float a = px[0], b = py[0], e = px[0], f = py[0];
    for (int i = 1; i < 4; i++) { if (px[i] < a) a = px[i]; if (px[i] > e) e = px[i]; if (py[i] < b) b = py[i]; if (py[i] > f) f = py[i]; }
    *x0 = (int)__builtin_floorf(a); *y0 = (int)__builtin_floorf(b); *x1 = (int)__builtin_ceilf(e); *y1 = (int)__builtin_ceilf(f);
    if (*x0 < 0) *x0 = 0; if (*y0 < 0) *y0 = 0; if (*x1 > (int)c->w) *x1 = c->w; if (*y1 > (int)c->h) *y1 = c->h;
}
/* premultiplied source-over of (r,g,b,a in 0..255) at device pixel (dx, dy-up) */
static void blend(Ctx *c, int dx, int dy, float r, float g, float b, float a) {
    u8 *p = (u8 *)P(c->data) + (c->h - 1 - dy) * c->bpr + dx * c->ncomp;
    float k = 1.0f - a / 255.0f;
    if (c->ncomp == 4) {
        p[0] = (u8)(r + p[0] * k + 0.5f); p[1] = (u8)(g + p[1] * k + 0.5f);
        p[2] = (u8)(b + p[2] * k + 0.5f); p[3] = (u8)(a + p[3] * k + 0.5f);
    } else if (c->alpha_only) p[0] = (u8)(a + p[0] * k + 0.5f);
    else p[0] = (u8)((r * 0.299f + g * 0.587f + b * 0.114f) + p[0] * k + 0.5f);
}
void _CGContextClearRect(void *p, float x, float y, float w, float h) {
    Ctx *c = CX(p); int x0, y0, x1, y1; dev_bbox(c, x, y, w, h, &x0, &y0, &x1, &y1);
    for (int dy = y0; dy < y1; dy++) memset((u8 *)P(c->data) + (c->h - 1 - dy) * c->bpr + x0 * c->ncomp, 0, (x1 - x0) * c->ncomp);
}
void _CGContextFillRect(void *p, float x, float y, float w, float h) {
    Ctx *c = CX(p); int x0, y0, x1, y1; dev_bbox(c, x, y, w, h, &x0, &y0, &x1, &y1);
    for (int dy = y0; dy < y1; dy++) for (int dx = x0; dx < x1; dx++)
        blend(c, dx, dy, c->fill[0] * 255, c->fill[1] * 255, c->fill[2] * 255, c->fill[3] * 255);
}
void _CGContextFillEllipseInRect(void *p, float x, float y, float w, float h) {
    Ctx *c = CX(p); float inv[6]; if (!invert(c->ctm, inv)) return;
    int x0, y0, x1, y1; dev_bbox(c, x, y, w, h, &x0, &y0, &x1, &y1);
    float cx = x + w / 2, cy = y + h / 2, rx = w / 2, ry = h / 2;
    for (int dy = y0; dy < y1; dy++) for (int dx = x0; dx < x1; dx++) {
        /* 4x4 supersampling for antialiased edges, as CG antialiases fills */
        int in = 0;
        for (int sy = 0; sy < 4; sy++) for (int sx = 0; sx < 4; sx++) {
            float ux, uy; apply(inv, dx + (sx + 0.5f) / 4, dy + (sy + 0.5f) / 4, &ux, &uy);
            float nx = (ux - cx) / rx, ny = (uy - cy) / ry; in += nx * nx + ny * ny <= 1.0f;
        }
        if (in) { float cov = in / 16.0f; blend(c, dx, dy, c->fill[0] * 255 * cov, c->fill[1] * 255 * cov, c->fill[2] * 255 * cov, c->fill[3] * 255 * cov); }
    }
}
void _CGContextDrawImage(void *p, float x, float y, float w, float h, void *ip) {
    Ctx *c = CX(p); Img *im = IM(ip); float inv[6];
    if (!invert(c->ctm, inv) || w == 0 || h == 0) return;
    int x0, y0, x1, y1; dev_bbox(c, x, y, w, h, &x0, &y0, &x1, &y1);
    const u8 *src = P(im->data);
    for (int dy = y0; dy < y1; dy++) for (int dx = x0; dx < x1; dx++) {
        float ux, uy; apply(inv, dx + 0.5f, dy + 0.5f, &ux, &uy);
        float u = (ux - x) / w * im->w, v = (y + h - uy) / h * im->h;   /* v = 0 at the image's top row */
        if (u < 0 || v < 0 || u >= im->w || v >= im->h) continue;
        const u8 *s = src + ((u32)v * im->w + (u32)u) * 4;
        if (c->alpha_only) { u8 *d = (u8 *)P(c->data) + (c->h - 1 - dy) * c->bpr + dx; float k = 1 - s[3] / 255.0f; *d = (u8)(s[3] + *d * k + 0.5f); }
        else blend(c, dx, dy, s[0], s[1], s[2], s[3]);
    }
}

/* ---- UIGraphics context stack ---- */
static Ctx *gstack[16]; static int gsp;
void _UIGraphicsPushContext(void *c) { if (gsp < 16) gstack[gsp++] = CX(c); }
void _UIGraphicsPopContext(void) { if (gsp) gsp--; }
static Ctx *gcur(void) { return gsp ? gstack[gsp - 1] : NULL; }

/* ---- UIFont {isa, name, size} ---- */
static u32 C_UIFont, C_UIImage, C_UIColor;
static float font_size(u32 f) { return f ? __builtin_bit_cast(float, RD32(f + 8)) : 12.0f; }
static int font_bold(u32 f) { if (!f || !RD32(f + 4)) return 0; const char *n = str_utf8(RD32(f + 4)); return strstr(n, "Bold") || strstr(n, "W6") || strstr(n, "W8"); }
static u32 font_new(u32 name, float size) {
    u32 f = obj_autorelease(obj_alloc(C_UIFont, 12)); WR32(f + 4, obj_retain(name)); WR32(f + 8, __builtin_bit_cast(u32, size)); return f;
}
SHIM(f_fontWithName) { return font_new(AU(0), AF(1)); }
SHIM(f_systemFont) { return font_new(str_new_utf8("Helvetica"), AF(0)); }
SHIM(f_boldSystemFont) { return font_new(str_new_utf8("Helvetica-Bold"), AF(0)); }
SHIM(f_pointSize) { return RF(font_size(m->self)); }
SHIM(f_fontName) { return RD32(m->self + 4); }

/* ---- NSString drawing (UIKit coordinates: origin top-left, through the context CTM) ---- */
#define UILineBreakModeWordWrap 0
static void text_box(Ctx *c, const char *s, u32 font, float x, float y, float w, float h, int align, int wrap) {
    int bw = (int)__builtin_ceilf(w), bh = (int)__builtin_ceilf(h);
    if (bw <= 0 || bh <= 0 || bw > 4096 || bh > 4096) return;
    u8 *cov = calloc(bw, bh);
    host_text_draw(s, font_size(font), font_bold(font), w, h, align, wrap, cov, bw, bh);
    for (int ty = 0; ty < bh; ty++) for (int tx = 0; tx < bw; tx++) {
        u8 a = cov[ty * bw + tx]; if (!a) continue;
        float dx, dy; apply(c->ctm, x + tx + 0.5f, y + ty + 0.5f, &dx, &dy);
        int ix = (int)__builtin_floorf(dx), iy = (int)__builtin_floorf(dy);
        if (ix < 0 || iy < 0 || ix >= (int)c->w || iy >= (int)c->h) continue;
        float k = a / 255.0f;
        blend(c, ix, iy, c->fill[0] * 255 * k, c->fill[1] * 255 * k, c->fill[2] * 255 * k, c->fill[3] * 255 * k);
    }
    free(cov);
}
/* -drawInRect:withFont:lineBreakMode:alignment: (stret CGSize) */
u64 str_drawInRect(Msg *m) {
    float x = AF(0), y = AF(1), w = AF(2), h = AF(3); u32 font = AU(4); int lbm = AI(5), align = AI(6);
    const char *s = str_utf8(m->self); float ow, oh;
    host_text_measure(s, font_size(font), font_bold(font), w, lbm == UILineBreakModeWordWrap || lbm == 1, &ow, &oh);
    if (oh > h) oh = h;
    Ctx *c = gcur();
    if (c) text_box(c, s, font, x, y, w, h, align, lbm == UILineBreakModeWordWrap || lbm == 1);
    else RT_ONCE(rt_log("drawInRect: with no current UIGraphics context"));
    if (m->stret) { WR32(m->stret, __builtin_bit_cast(u32, ow)); WR32(m->stret + 4, __builtin_bit_cast(u32, oh)); }
    return 0;
}
u64 str_drawAtPoint(Msg *m) {
    float x = AF(0), y = AF(1); u32 font = AU(2); const char *s = str_utf8(m->self); float ow, oh;
    host_text_measure(s, font_size(font), font_bold(font), 1e6f, 0, &ow, &oh);
    Ctx *c = gcur(); if (c) text_box(c, s, font, x, y, ow + 1, oh, 0, 0);
    if (m->stret) { WR32(m->stret, __builtin_bit_cast(u32, ow)); WR32(m->stret + 4, __builtin_bit_cast(u32, oh)); }
    return 0;
}
u64 str_sizeWithFont(Msg *m) {
    u32 font = AU(0); float ow, oh;
    host_text_measure(str_utf8(m->self), font_size(font), font_bold(font), 1e6f, 0, &ow, &oh);
    WR32(m->stret, __builtin_bit_cast(u32, ow)); WR32(m->stret + 4, __builtin_bit_cast(u32, oh)); return 0;
}
u64 str_sizeWithFontConstrained(Msg *m) {
    u32 font = AU(0); float ow, oh;
    host_text_measure(str_utf8(m->self), font_size(font), font_bold(font), AF(1), 1, &ow, &oh);
    if (oh > AF(2)) oh = AF(2);
    WR32(m->stret, __builtin_bit_cast(u32, ow)); WR32(m->stret + 4, __builtin_bit_cast(u32, oh)); return 0;
}

/* ---- UIImage {isa, cgimage} ---- */
SHIM(im_imageWithContentsOfFile) {
    if (!AU(0)) return 0;
    u32 cg = cg_image_from_file(str_utf8(AU(0)));
    if (!cg) return 0;
    u32 o = obj_autorelease(obj_alloc(C_UIImage, 8)); WR32(o + 4, cg); return o;
}
SHIM(im_imageNamed) {
    char path[300]; rt_snprintf(path, sizeof path, "/app/%s", str_utf8(AU(0)));
    u32 cg = cg_image_from_file(path); if (!cg) return 0;
    u32 o = obj_autorelease(obj_alloc(C_UIImage, 8)); WR32(o + 4, cg); return o;
}
SHIM(im_CGImage) { return RD32(m->self + 4); }
SHIM(im_size) { Img *i = P(RD32(m->self + 4)); WR32(m->stret, __builtin_bit_cast(u32, (float)i->w)); WR32(m->stret + 4, __builtin_bit_cast(u32, (float)i->h)); return 0; }
SHIM(im_dealloc) {
    if (getenv_dbg()) rt_log("UIImage 0x%x dealloc, CGImage 0x%x", m->self, RD32(m->self + 4)); cg_image_release(RD32(m->self + 4)); obj_dealloc_memory(m->self); return 0; }

/* ---- UIColor {isa, r, g, b, a} ---- */
static u32 color_new(float r, float g, float b, float a) {
    u32 o = obj_autorelease(obj_alloc(C_UIColor, 20));
    WR32(o + 4, __builtin_bit_cast(u32, r)); WR32(o + 8, __builtin_bit_cast(u32, g)); WR32(o + 12, __builtin_bit_cast(u32, b)); WR32(o + 16, __builtin_bit_cast(u32, a));
    return o;
}
SHIM(c_white) { return color_new(1, 1, 1, 1); }
SHIM(c_black) { return color_new(0, 0, 0, 1); }
SHIM(c_clear) { return color_new(0, 0, 0, 0); }
SHIM(c_rgba) { return color_new(AF(0), AF(1), AF(2), AF(3)); }
SHIM(c_set) { Ctx *c = gcur(); if (c) _CGContextSetRGBFillColor(c, __builtin_bit_cast(float, RD32(m->self + 4)), __builtin_bit_cast(float, RD32(m->self + 8)), __builtin_bit_cast(float, RD32(m->self + 12)), __builtin_bit_cast(float, RD32(m->self + 16))); return 0; }

void rt_init_cg(void) {
    u32 c;
    c = C_UIFont = rt_class_new("UIFont", "NSObject", 12);
    rt_cmethod(c, "fontWithName:size:", f_fontWithName); rt_cmethod(c, "systemFontOfSize:", f_systemFont);
    rt_cmethod(c, "boldSystemFontOfSize:", f_boldSystemFont); rt_method(c, "pointSize", f_pointSize); rt_method(c, "fontName", f_fontName);
    c = C_UIImage = rt_class_new("UIImage", "NSObject", 8);
    rt_cmethod(c, "imageWithContentsOfFile:", im_imageWithContentsOfFile); rt_cmethod(c, "imageNamed:", im_imageNamed);
    rt_method(c, "CGImage", im_CGImage); rt_method(c, "size", im_size); rt_method(c, "dealloc", im_dealloc);
    c = C_UIColor = rt_class_new("UIColor", "NSObject", 20);
    rt_cmethod(c, "whiteColor", c_white); rt_cmethod(c, "blackColor", c_black); rt_cmethod(c, "clearColor", c_clear);
    rt_cmethod(c, "colorWithRed:green:blue:alpha:", c_rgba); rt_method(c, "set", c_set); rt_method(c, "setFill", c_set);
}
