/* Runtime text (NSString drawInRect:withFont:, sizeWithFont:) with FreeType.
 * Helvetica(-Bold) is substituted by Liberation Sans, which is metric-compatible with
 * Arial (and so near Helvetica); exact iPhone text is a D2 item (captured textures). */
#include "host.h"
#include <ft2build.h>
#include FT_FREETYPE_H
#include <stdlib.h>
#include <string.h>

static FT_Library ft;
static FT_Face faces[4];       /* regular, bold, cjk fallback, display (RETOUCHED only; bold == 2) */
static int bold_idx(int bold) { return bold == 2 && faces[3] ? 3 : bold ? 1 : 0; }

int text_init(const char *regular, const char *bold, const char *cjk, const char *display) {
    if (FT_Init_FreeType(&ft)) return -1;
    const char *paths[4] = { regular, bold, cjk, display };
    for (int i = 0; i < 4; i++) {
        if (!paths[i]) continue;
        long n; uint8_t *data = vfs_read_all(paths[i], &n);   /* kept for the face's lifetime */
        if (!data || FT_New_Memory_Face(ft, data, n, 0, &faces[i])) { hlog("font: cannot load %s", paths[i]); faces[i] = NULL; }
    }
    if (!faces[0]) faces[0] = faces[1]; if (!faces[1]) faces[1] = faces[0];
    return faces[0] ? 0 : -1;
}
static uint32_t next_cp(const char **s) {
    const uint8_t *p = (const uint8_t *)*s; uint32_t c = *p++;
    if (c >= 0xF0) { c = (c & 7) << 18 | (p[0] & 63) << 12 | (p[1] & 63) << 6 | (p[2] & 63); p += 3; }
    else if (c >= 0xE0) { c = (c & 15) << 12 | (p[0] & 63) << 6 | (p[1] & 63); p += 2; }
    else if (c >= 0xC0) { c = (c & 31) << 6 | (p[0] & 63); p += 1; }
    *s = (const char *)p; return c;
}
static FT_Face face_for(uint32_t cp, int bold, float size) {
    FT_Face f = faces[bold_idx(bold)];
    if (!FT_Get_Char_Index(f, cp) && faces[2] && FT_Get_Char_Index(faces[2], cp)) f = faces[2];
    FT_Set_Char_Size(f, 0, (FT_F26Dot6)(size * 64), 72, 72);
    return f;
}
static float advance(uint32_t cp, int bold, float size) {
    FT_Face f = face_for(cp, bold, size);
    if (FT_Load_Char(f, cp, FT_LOAD_DEFAULT)) return 0;
    return f->glyph->advance.x / 64.0f;
}
static float line_height(int bold, float size) {
    FT_Face f = faces[bold_idx(bold)]; FT_Set_Char_Size(f, 0, (FT_F26Dot6)(size * 64), 72, 72);
    return (f->size->metrics.ascender - f->size->metrics.descender) / 64.0f;
}
static float ascent(int bold, float size) {
    FT_Face f = faces[bold_idx(bold)]; FT_Set_Char_Size(f, 0, (FT_F26Dot6)(size * 64), 72, 72);
    return f->size->metrics.ascender / 64.0f;
}

/* layout: hard breaks on '\n', soft breaks at spaces (word wrap) or anywhere for CJK */
typedef struct { const char *start, *end; float width; } Line;
static int layout(const char *s, float size, int bold, float max_w, int wrap, Line *lines, int maxl) {
    int n = 0; const char *ls = s, *p = s, *last_space = NULL; float w = 0, w_at_space = 0;
    while (n < maxl) {
        const char *cs = p; uint32_t cp = *p ? next_cp(&p) : 0;
        if (cp == 0 || cp == '\n') { lines[n++] = (Line){ ls, cs, w }; if (!cp) break; ls = p; w = 0; last_space = NULL; continue; }
        float a = advance(cp, bold, size);
        if (wrap && w + a > max_w && cs > ls) {
            if (last_space && last_space > ls) { lines[n++] = (Line){ ls, last_space, w_at_space }; p = last_space + 1; }
            else { lines[n++] = (Line){ ls, cs, w }; p = cs; }
            ls = p; w = 0; last_space = NULL; continue;
        }
        if (cp == ' ') { last_space = cs; w_at_space = w; }
        w += a;
    }
    return n;
}
void text_measure(const char *s, float size, int bold, float max_w, int wrap, float *ow, float *oh) {
    Line lines[256]; int n = layout(s, size, bold, max_w, wrap, lines, 256); float w = 0;
    for (int i = 0; i < n; i++) if (lines[i].width > w) w = lines[i].width;
    *ow = w; *oh = n * line_height(bold, size);
}
void text_draw(const char *s, float size, int bold, float box_w, float box_h, int align, int wrap, uint8_t *dst, int dw, int dh) {
    Line lines[256]; int n = layout(s, size, bold, box_w, wrap, lines, 256);
    float lh = line_height(bold, size), asc = ascent(bold, size);
    for (int i = 0; i < n; i++) {
        float y = i * lh; if (y >= box_h) break;
        float x = align == 1 ? (box_w - lines[i].width) / 2 : align == 2 ? box_w - lines[i].width : 0;
        int base = (int)(y + asc + 0.5f);
        for (const char *p = lines[i].start; p < lines[i].end;) {
            uint32_t cp = next_cp(&p); FT_Face f = face_for(cp, bold, size);
            if (FT_Load_Char(f, cp, FT_LOAD_RENDER)) continue;
            FT_Bitmap *bm = &f->glyph->bitmap;
            int gx = (int)(x + 0.5f) + f->glyph->bitmap_left, gy = base - f->glyph->bitmap_top;
            for (unsigned r = 0; r < bm->rows; r++) for (unsigned c = 0; c < bm->width; c++) {
                int X = gx + (int)c, Y = gy + (int)r;
                if (X < 0 || Y < 0 || X >= dw || Y >= dh) continue;
                int v = dst[Y * dw + X] + bm->buffer[r * bm->pitch + c]; dst[Y * dw + X] = (uint8_t)(v > 255 ? 255 : v);
            }
            x += f->glyph->advance.x / 64.0f;
        }
    }
}
void w2c_env_host_text_measure(struct w2c_env *e, u32 s, f32 size, u32 bold, f32 max_w, u32 wrap, u32 ow, u32 oh) {
    (void)e; float w, h; text_measure(CSTR(s), size, (int)bold, max_w, (int)wrap, &w, &h);
    memcpy(MEM(ow), &w, 4); memcpy(MEM(oh), &h, 4);
}
void w2c_env_host_text_draw(struct w2c_env *e, u32 s, f32 size, u32 bold, f32 bw, f32 bh, u32 align, u32 wrap, u32 dst, u32 dw, u32 dh) {
    (void)e; text_draw(CSTR(s), size, (int)bold, bw, bh, (int)align, (int)wrap, MEM(dst), (int)dw, (int)dh);
}
