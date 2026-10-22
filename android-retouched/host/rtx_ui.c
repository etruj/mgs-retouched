/* METAL GEAR SOLID RETOUCHED: immediate-mode drawing for the overlay screens, on the same GL
 * context as the game (drawn by the engine's presenter after the game picture; the presenter
 * restores the game's GL state afterwards). UI units are the game's own view units: 320x480 in
 * portrait, 480x320 when a landscape scene is up (rotated the way the game rotates it), drawn at
 * the phone's full resolution so text stays sharp. */
#include "rtx_host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HOST_GLES
#include <GLES/gl.h>
#define glOrtho_(l, r, b, t, n, f) glOrthof(l, r, b, t, n, f)
#else
#include <GL/gl.h>
#define glOrtho_(l, r, b, t, n, f) glOrtho(l, r, b, t, n, f)
#endif

static int land, flip;          /* landscape UI; flip = the other landscape direction */
static float ppu = 1;           /* window pixels per UI unit */
float ui_w(void) { return land ? 480.0f : 320.0f; }
float ui_h(void) { return land ? 320.0f : 480.0f; }

void ui_begin(int bx, int by, int bw, int bh, int l, int f) {
    land = l; flip = f; ppu = bw / 320.0f;
    int ww, wh; gfx_window_size(&ww, &wh);
    glViewport(bx, wh - by - bh, bw, bh);
    glMatrixMode(GL_TEXTURE); glLoadIdentity();
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho_(0, 320, 480, 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glLoadIdentity();
    /* UI (lx, ly) -> view: flip 0: (320 - ly, lx); flip 1: (ly, 480 - lx) */
    if (land && !flip) { glTranslatef(320, 0, 0); glRotatef(90, 0, 0, 1); }
    else if (land) { glTranslatef(0, 480, 0); glRotatef(-90, 0, 0, 1); }
    glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glDisableClientState(GL_COLOR_ARRAY); glEnableClientState(GL_VERTEX_ARRAY);
}
void ui_end(void) { glDisable(GL_BLEND); glDisable(GL_TEXTURE_2D); }
void ui_to_ui(float vx, float vy, float *x, float *y) {
    if (!land) { *x = vx; *y = vy; }
    else if (!flip) { *x = vy; *y = 320 - vx; }
    else { *x = 480 - vy; *y = vx; }
}
static void quad(float x, float y, float w, float h, int tex) {
    GLfloat v[8] = { x, y, x + w, y, x, y + h, x + w, y + h }, t[8] = { 0, 0, 1, 0, 0, 1, 1, 1 };
    glVertexPointer(2, GL_FLOAT, 0, v);
    if (tex) { glEnableClientState(GL_TEXTURE_COORD_ARRAY); glTexCoordPointer(2, GL_FLOAT, 0, t); }
    else glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}
void ui_fill(float x, float y, float w, float h, Col c) {
    glDisable(GL_TEXTURE_2D); glColor4f(c.r, c.g, c.b, c.a); quad(x, y, w, h, 0);
}
void ui_frame(float x, float y, float w, float h, float t, Col c) {
    ui_fill(x, y, w, t, c); ui_fill(x, y + h - t, w, t, c); ui_fill(x, y, t, h, c); ui_fill(x + w - t, y, t, h, c);
}

/* ---- text: rasterised by the port's FreeType text engine at window resolution, cached ---- */
typedef struct { char s[96]; int size10, bold; float ppu; unsigned tex; int pw, ph; unsigned used; } TextTex;
static TextTex cache[128]; static unsigned tick;
static TextTex *text_tex(const char *s, float size, int bold) {
    int s10 = (int)(size * 10 + 0.5f);
    TextTex *best = &cache[0];
    for (int i = 0; i < 128; i++) {
        TextTex *t = &cache[i];
        if (t->tex && t->size10 == s10 && t->bold == bold && t->ppu == ppu && !strncmp(t->s, s, sizeof t->s - 1)) { t->used = ++tick; return t; }
        if (t->used < best->used) best = t;
    }
    if (best->tex) glDeleteTextures(1, &best->tex);
    float px = size * ppu, w, h;
    text_measure(s, px, bold, 1e9f, 0, &w, &h);
    int pw = (int)w + 2, ph = (int)h + 2;
    uint8_t *a8 = calloc((size_t)pw * ph, 1);
    text_draw(s, px, bold, (float)pw, (float)ph, 0, 0, a8, pw, ph);
    glGenTextures(1, &best->tex); glBindTexture(GL_TEXTURE_2D, best->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA, pw, ph, 0, GL_ALPHA, GL_UNSIGNED_BYTE, a8);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(a8);
    snprintf(best->s, sizeof best->s, "%s", s); best->size10 = s10; best->bold = bold; best->ppu = ppu;
    best->pw = pw; best->ph = ph; best->used = ++tick;
    return best;
}
/* align: 0 left, 1 centre, 2 right of x; y = top. Returns the width in UI units. */
float ui_text(const char *s, float x, float y, float size, int bold, int align, Col c, float max_w) {
    if (!s || !*s) return 0;
    TextTex *t = text_tex(s, size, bold);
    float w = t->pw / ppu, h = t->ph / ppu;
    if (max_w > 0 && w > max_w) { h *= max_w / w; w = max_w; }    /* squeeze, never overflow */
    float x0 = align == 1 ? x - w / 2 : align == 2 ? x - w : x;
    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, t->tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glColor4f(c.r, c.g, c.b, c.a);
    quad(x0, y, w, h, 1);
    return w;
}

/* ---- images from the game bundle (premultiplied RGBA) ---- */
unsigned ui_load_image(const char *path, int *w, int *h) {
    uint32_t iw, ih; int alpha; uint8_t *px;
    char real[2048]; host_map_path(path, real, sizeof real);
    if (img_decode_file(real, &iw, &ih, &alpha, &px)) return 0;
    GLuint t; glGenTextures(1, &t); glBindTexture(GL_TEXTURE_2D, t);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, (GLsizei)iw, (GLsizei)ih, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(px); *w = (int)iw; *h = (int)ih;
    return t;
}
void ui_free_image(unsigned tex) { GLuint t = tex; if (t) glDeleteTextures(1, &t); }
void ui_image(unsigned tex, float x, float y, float w, float h, float alpha) {
    glEnable(GL_TEXTURE_2D); glBindTexture(GL_TEXTURE_2D, tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);                  /* premultiplied */
    glColor4f(alpha, alpha, alpha, alpha);
    quad(x, y, w, h, 1);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
}
