/* OpenGL ES 1.1 for the game, on desktop GL (compatibility profile) or GLES 1.1.
 * The game renders into what it believes is the iPhone's 320x480 window renderbuffer;
 * the host backs that renderbuffer with a texture, and on presentRenderbuffer: draws it
 * rotated to landscape and letterboxed into the real window, then restores every bit
 * of GL state it touched (the game's state is shadowed as it is set). */
#include "host.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef HOST_GLES
#include <GLES/gl.h>
#include <GLES/glext.h>
#define GL_FRAMEBUFFER_ GL_FRAMEBUFFER_OES
#define GL_COLOR_ATTACHMENT0_ GL_COLOR_ATTACHMENT0_OES
#define GL_RENDERBUFFER_ GL_RENDERBUFFER_OES
#define glOrtho_(l, r, b, t, n, f) glOrthof(l, r, b, t, n, f)
#define GL_RGBA8_X GL_RGBA8_OES
#else
#define GL_GLEXT_PROTOTYPES 1
#include <GL/gl.h>
#include <GL/glext.h>
#define GL_FRAMEBUFFER_ GL_FRAMEBUFFER
#define GL_COLOR_ATTACHMENT0_ GL_COLOR_ATTACHMENT0
#define GL_RENDERBUFFER_ GL_RENDERBUFFER
#define glOrtho_(l, r, b, t, n, f) glOrtho(l, r, b, t, n, f)
#define GL_RGBA8_X GL_RGBA8
#endif
void *gfx_getproc(const char *name);   /* per platform */

/* framebuffer-object entry points (OES on GLES 1.1, core/ARB on desktop) */
typedef void (*PFgen)(GLsizei, GLuint *); typedef void (*PFbind)(GLenum, GLuint);
typedef void (*PFstore)(GLenum, GLenum, GLsizei, GLsizei); typedef void (*PFattrb)(GLenum, GLenum, GLenum, GLuint);
typedef void (*PFattt)(GLenum, GLenum, GLenum, GLuint, GLint); typedef GLenum (*PFcheck)(GLenum); typedef void (*PFgetrb)(GLenum, GLenum, GLint *);
static PFgen genFB, genRB, delFB, delRB; static PFbind bindFB, bindRB; static PFstore rbStorage; static PFattrb fbRB; static PFattt fbTex; static PFcheck checkFB; static PFgetrb getRB;

static int fb_w = 320, fb_h = 480, win_w, win_h;
static GLuint drawable_rb, drawable_tex, cur_fb, cur_rb, present_fb, game_fb;
static int tex_scale = 1;          /* the drawable texture is (fb_w, fb_h) * tex_scale */
static int presented;

/* ---- shadow of the game's state ---- */
#define NCAPS 16
static const GLenum caps[NCAPS] = { GL_TEXTURE_2D, GL_BLEND, GL_ALPHA_TEST, GL_DEPTH_TEST, GL_SCISSOR_TEST, GL_CLIP_PLANE0, GL_CLIP_PLANE1,
    GL_CLIP_PLANE2, GL_CLIP_PLANE3, GL_CULL_FACE, GL_LIGHTING, GL_FOG, GL_DITHER, GL_STENCIL_TEST, GL_COLOR_LOGIC_OP, GL_POINT_SMOOTH };
static int cap_on[NCAPS] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
static int cl_vertex, cl_tex, cl_color;
static struct { GLint size, stride; GLenum type; const void *p; } vp, tp;
static GLint viewport[4], scissor[4]; static GLenum blend_s = GL_ONE, blend_d = GL_ZERO, mat_mode = GL_MODELVIEW;
static GLuint bound_tex; static GLfloat color[4] = { 1, 1, 1, 1 }; static GLint texenv_mode = GL_MODULATE;
static int cap_index(GLenum c) { for (int i = 0; i < NCAPS; i++) if (caps[i] == c) return i; return -1; }

void gfx_init(int w, int h) {
    fb_w = w; fb_h = h;
#ifdef HOST_GLES
    genFB = (PFgen)gfx_getproc("glGenFramebuffersOES"); genRB = (PFgen)gfx_getproc("glGenRenderbuffersOES");
    delFB = (PFgen)gfx_getproc("glDeleteFramebuffersOES"); delRB = (PFgen)gfx_getproc("glDeleteRenderbuffersOES");
    bindFB = (PFbind)gfx_getproc("glBindFramebufferOES"); bindRB = (PFbind)gfx_getproc("glBindRenderbufferOES");
    rbStorage = (PFstore)gfx_getproc("glRenderbufferStorageOES"); fbRB = (PFattrb)gfx_getproc("glFramebufferRenderbufferOES");
    fbTex = (PFattt)gfx_getproc("glFramebufferTexture2DOES"); checkFB = (PFcheck)gfx_getproc("glCheckFramebufferStatusOES");
    getRB = (PFgetrb)gfx_getproc("glGetRenderbufferParameterivOES");
#else
    genFB = glGenFramebuffers; genRB = glGenRenderbuffers; delFB = (PFgen)glDeleteFramebuffers; delRB = (PFgen)glDeleteRenderbuffers;
    bindFB = glBindFramebuffer; bindRB = glBindRenderbuffer; rbStorage = glRenderbufferStorage; fbRB = glFramebufferRenderbuffer;
    fbTex = glFramebufferTexture2D; checkFB = glCheckFramebufferStatus; getRB = glGetRenderbufferParameteriv;
#endif
    if (!genFB || !fbTex) hlog("GL: framebuffer objects unavailable (OES_framebuffer_object missing)");
    hlog("GL: %s / %s / %s", glGetString(GL_VERSION), glGetString(GL_RENDERER), glGetString(GL_VENDOR));
    genFB(1, &present_fb);
}
void gfx_set_window(int w, int h) { win_w = w; win_h = h; }
int gfx_frame_presented(void) { int p = presented; presented = 0; return p; }

/* the game's viewport and scissor are in 320x480 units; rendering into the window framebuffer at a
 * higher resolution (render_scale) multiplies them, so the game sees nothing different */
static void apply_viewport_scissor(void) {
    int k = cur_fb && cur_fb == game_fb ? tex_scale : 1;
    glViewport(viewport[0] * k, viewport[1] * k, viewport[2] * k, viewport[3] * k);
    glScissor(scissor[0] * k, scissor[1] * k, scissor[2] * k, scissor[3] * k);
}
/* ---- forwarded calls ---- */
#define E struct w2c_env *e
void w2c_env_host_glBindFramebufferOES(E, u32 t, u32 f) {
    (void)e; (void)t; int was = cur_fb == game_fb && game_fb; cur_fb = f; bindFB(GL_FRAMEBUFFER_, f);
    if (tex_scale != 1 && was != (f == game_fb && game_fb)) apply_viewport_scissor();
}
void w2c_env_host_glBindRenderbufferOES(E, u32 t, u32 r) { (void)e; (void)t; cur_rb = r; bindRB(GL_RENDERBUFFER_, r); }
void w2c_env_host_glGenFramebuffersOES(E, u32 n, u32 p) { (void)e; genFB((GLsizei)n, MEM(p)); }
void w2c_env_host_glGenRenderbuffersOES(E, u32 n, u32 p) { (void)e; genRB((GLsizei)n, MEM(p)); }
void w2c_env_host_glDeleteFramebuffersOES(E, u32 n, u32 p) { (void)e; delFB((GLsizei)n, MEM(p)); }
void w2c_env_host_glDeleteRenderbuffersOES(E, u32 n, u32 p) {
    (void)e; GLuint *r = MEM(p);
    for (u32 i = 0; i < n; i++) if (r[i] == drawable_rb) drawable_rb = 0;
    delRB((GLsizei)n, r);
}
/* -[EAGLContext renderbufferStorage:fromDrawable:]: the bound renderbuffer becomes the window */
void w2c_env_host_glRenderbufferStorageFromDrawable(E) {
    (void)e;
    drawable_rb = cur_rb;
    rbStorage(GL_RENDERBUFFER_, GL_RGBA8_X, fb_w, fb_h);
    if (!drawable_tex) {
        GLint prev; glGetIntegerv(GL_TEXTURE_BINDING_2D, &prev);
        glGenTextures(1, &drawable_tex); glBindTexture(GL_TEXTURE_2D, drawable_tex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fb_w, fb_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glBindTexture(GL_TEXTURE_2D, (GLuint)prev);
    }
    hlog("window renderbuffer %u: %dx%d (backed by texture %u)", drawable_rb, fb_w, fb_h, drawable_tex);
}
void w2c_env_host_glFramebufferRenderbufferOES(E, u32 t, u32 att, u32 rbt, u32 rb) {
    (void)e; (void)t; (void)rbt;
    if (rb && rb == drawable_rb && att == GL_COLOR_ATTACHMENT0_) { fbTex(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D, drawable_tex, 0); game_fb = cur_fb; }
    else fbRB(GL_FRAMEBUFFER_, att, GL_RENDERBUFFER_, rb);
}
u32 w2c_env_host_glCheckFramebufferStatusOES(E, u32 t) { (void)e; (void)t; return checkFB(GL_FRAMEBUFFER_); }
void w2c_env_host_glGetRenderbufferParameterivOES(E, u32 t, u32 pn, u32 out) {
    (void)e; (void)t; GLint *o = MEM(out);
    if (cur_rb == drawable_rb && pn == 0x8D42) *o = fb_w;           /* GL_RENDERBUFFER_WIDTH */
    else if (cur_rb == drawable_rb && pn == 0x8D43) *o = fb_h;      /* GL_RENDERBUFFER_HEIGHT */
    else getRB(GL_RENDERBUFFER_, pn, o);
}
void w2c_env_host_glBindTexture(E, u32 t, u32 x) { (void)e; if (t == GL_TEXTURE_2D) bound_tex = x; glBindTexture(t, x); }
void w2c_env_host_glBlendFunc(E, u32 s, u32 d) { (void)e; blend_s = s; blend_d = d; glBlendFunc(s, d); }
void w2c_env_host_glClear(E, u32 m) { (void)e; glClear(m); }
void w2c_env_host_glClipPlanef(E, u32 p, u32 eq) {
    (void)e; float *f = MEM(eq);
#ifdef HOST_GLES
    glClipPlanef(p, f);
#else
    double d[4] = { f[0], f[1], f[2], f[3] }; glClipPlane(p, d);
#endif
}
void w2c_env_host_glColor4f(E, f32 r, f32 g, f32 b, f32 a) { (void)e; color[0] = r; color[1] = g; color[2] = b; color[3] = a; glColor4f(r, g, b, a); }
void w2c_env_host_glDeleteTextures(E, u32 n, u32 p) { (void)e; glDeleteTextures((GLsizei)n, MEM(p)); }
void w2c_env_host_glDisable(E, u32 c) { (void)e; int i = cap_index(c); if (i >= 0) cap_on[i] = 0; glDisable(c); }
void w2c_env_host_glEnable(E, u32 c) { (void)e; int i = cap_index(c); if (i >= 0) cap_on[i] = 1; glEnable(c); }
void w2c_env_host_glEnableClientState(E, u32 a) {
    (void)e; if (a == GL_VERTEX_ARRAY) cl_vertex = 1; else if (a == GL_TEXTURE_COORD_ARRAY) cl_tex = 1; else if (a == GL_COLOR_ARRAY) cl_color = 1;
    glEnableClientState(a);
}
/* RETOUCHED: one alpha-only texture (the main-menu RETOUCHED label) is drawn in a fixed colour; the
 * game colours its text textures at draw time, so the colour is substituted here. */
static GLuint tint_tex; static GLfloat tint_rgb[3];
void w2c_env_host_gl_tint(E, u32 tex, f32 r, f32 g, f32 b) { (void)e; tint_tex = tex; tint_rgb[0] = r; tint_rgb[1] = g; tint_rgb[2] = b; }
void w2c_env_host_glDrawArrays(E, u32 m, u32 f, u32 c) {
    (void)e;
    if (tint_tex && bound_tex == tint_tex) { glColor4f(tint_rgb[0], tint_rgb[1], tint_rgb[2], color[3]); glDrawArrays(m, (GLint)f, (GLsizei)c); glColor4f(color[0], color[1], color[2], color[3]); return; }
    glDrawArrays(m, (GLint)f, (GLsizei)c);
}
void w2c_env_host_glGenTextures(E, u32 n, u32 p) { (void)e; glGenTextures((GLsizei)n, MEM(p)); }
u32 w2c_env_host_glGetError(E) { (void)e; return glGetError(); }
void w2c_env_host_glGetIntegerv(E, u32 pn, u32 out) {
    (void)e; GLint *o = MEM(out);
    if (pn == GL_VIEWPORT) memcpy(o, viewport, 16); else if (pn == 0x8CA6 /* FRAMEBUFFER_BINDING */) *o = (GLint)cur_fb;
    else if (pn == 0x8CA7 /* RENDERBUFFER_BINDING */) *o = (GLint)cur_rb; else glGetIntegerv(pn, o);
}
void w2c_env_host_glLoadIdentity(E) { (void)e; glLoadIdentity(); }
void w2c_env_host_glMatrixMode(E, u32 m) { (void)e; mat_mode = m; glMatrixMode(m); }
void w2c_env_host_glOrthof(E, f32 l, f32 r, f32 b, f32 t, f32 n, f32 f) { (void)e; glOrtho_(l, r, b, t, n, f); }
void w2c_env_host_glScissor(E, u32 x, u32 y, u32 w, u32 h) {
    (void)e; scissor[0] = (GLint)x; scissor[1] = (GLint)y; scissor[2] = (GLint)w; scissor[3] = (GLint)h;
    if (tex_scale != 1) apply_viewport_scissor(); else glScissor((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h);
}
void w2c_env_host_glTexCoordPointer(E, u32 s, u32 t, u32 st, u32 p) { (void)e; tp.size = (GLint)s; tp.type = t; tp.stride = (GLint)st; tp.p = MEM(p); glTexCoordPointer((GLint)s, t, (GLsizei)st, tp.p); }
void w2c_env_host_glVertexPointer(E, u32 s, u32 t, u32 st, u32 p) { (void)e; vp.size = (GLint)s; vp.type = t; vp.stride = (GLint)st; vp.p = MEM(p); glVertexPointer((GLint)s, t, (GLsizei)st, vp.p); }
void w2c_env_host_glTexEnvf(E, u32 t, u32 pn, f32 v) { (void)e; if (pn == GL_TEXTURE_ENV_MODE) texenv_mode = (GLint)v; glTexEnvf(t, pn, v); }
void w2c_env_host_glTexEnvi(E, u32 t, u32 pn, u32 v) { (void)e; if (pn == GL_TEXTURE_ENV_MODE) texenv_mode = (GLint)v; glTexEnvi(t, pn, (GLint)v); }
void w2c_env_host_glTexImage2D(E, u32 t, u32 l, u32 i, u32 w, u32 h, u32 b, u32 f, u32 ty, u32 p) {
    (void)e; glTexImage2D(t, (GLint)l, (GLint)i, (GLsizei)w, (GLsizei)h, (GLint)b, f, ty, p ? MEM(p) : NULL);
}
void w2c_env_host_glTexParameteri(E, u32 t, u32 pn, u32 v) { (void)e; glTexParameteri(t, pn, (GLint)v); }
void w2c_env_host_glViewport(E, u32 x, u32 y, u32 w, u32 h) {
    (void)e; viewport[0] = (GLint)x; viewport[1] = (GLint)y; viewport[2] = (GLint)w; viewport[3] = (GLint)h;
    if (tex_scale != 1) apply_viewport_scissor(); else glViewport((GLint)x, (GLint)y, (GLsizei)w, (GLsizei)h);
}

/* ---- present: portrait framebuffer -> landscape window ---- */
/* the screen as the player sees it: portrait 320x480 (rotate 0) or landscape 480x320 */
static int scr_w(void) { return g_cfg.rotate ? 480 : 320; }
static int scr_h(void) { return g_cfg.rotate ? 320 : 480; }
/* letterboxed area of the window that shows the screen */
static void screen_box(int *x, int *y, int *w, int *h) {
    if (win_w * scr_h() > win_h * scr_w()) { *h = win_h; *w = win_h * scr_w() / scr_h(); } else { *w = win_w; *h = win_w * scr_h() / scr_w(); }
    *x = (win_w - *w) / 2; *y = (win_h - *h) / 2;
}
/* screen point (sx right, sy down, in screen units) shows view point (vx, vy) */
static void land_to_view(float lx, float ly, float *vx, float *vy) {
    if (g_cfg.rotate == 90) { *vx = 320 - ly; *vy = lx; }     /* view's top edge on the screen's left */
    else if (g_cfg.rotate == 270) { *vx = ly; *vy = 480 - lx; } /* view's top edge on the screen's right */
    else { *vx = lx; *vy = ly; }                              /* portrait, as held on the iPhone */
}
void gfx_window_to_view(float wx, float wy, float *vx, float *vy) {
    int x, y, w, h; screen_box(&x, &y, &w, &h);
    land_to_view((wx - x) * (float)scr_w() / w, (wy - y) * (float)scr_h() / h, vx, vy);
}
static void restore_state(void) {
    bindFB(GL_FRAMEBUFFER_, cur_fb); bindRB(GL_RENDERBUFFER_, cur_rb);
    apply_viewport_scissor();
    for (int i = 0; i < NCAPS; i++) { if (cap_on[i]) glEnable(caps[i]); else glDisable(caps[i]); }
    glBlendFunc(blend_s, blend_d);
    glBindTexture(GL_TEXTURE_2D, bound_tex);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, texenv_mode);
    glColor4f(color[0], color[1], color[2], color[3]);
    if (cl_vertex) glEnableClientState(GL_VERTEX_ARRAY); else glDisableClientState(GL_VERTEX_ARRAY);
    if (cl_tex) glEnableClientState(GL_TEXTURE_COORD_ARRAY); else glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    if (cl_color) glEnableClientState(GL_COLOR_ARRAY); else glDisableClientState(GL_COLOR_ARRAY);
    if (vp.p) glVertexPointer(vp.size, vp.type, vp.stride, vp.p);
    if (tp.p) glTexCoordPointer(tp.size, tp.type, tp.stride, tp.p);
    glMatrixMode(GL_TEXTURE); glPopMatrix(); glMatrixMode(GL_MODELVIEW); glPopMatrix(); glMatrixMode(GL_PROJECTION); glPopMatrix();
    glMatrixMode(mat_mode);
}
/* render_scale changed: reallocate the window texture (the framebuffer keeps it attached) */
static void apply_render_scale(void) {
    int k = g_ext.render_scale < 1 ? 1 : g_ext.render_scale > 4 ? 4 : g_ext.render_scale;
    GLint maxt = 0; glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxt);
    while (k > 1 && fb_h * k > maxt) k--;
    if (k == tex_scale || !drawable_tex) return;
    glBindTexture(GL_TEXTURE_2D, drawable_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, fb_w * k, fb_h * k, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glBindTexture(GL_TEXTURE_2D, bound_tex);
    tex_scale = k;
    hlog("game framebuffer: %dx%d (render scale %d)", fb_w * k, fb_h * k, k);
}
/* a blurred, darkened copy of the picture behind the letterbox: downsample twice, stretch */
static GLuint blur_tex[2];
static void quad(float x0, float y0, float x1, float y1, float s0, float t0, float s1, float t1) {
    GLfloat v[8] = { x0, y0, x1, y0, x0, y1, x1, y1 }, t[8] = { s0, t0, s1, t0, s0, t1, s1, t1 };
    glVertexPointer(2, GL_FLOAT, 0, v); glTexCoordPointer(2, GL_FLOAT, 0, t); glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
}
static void draw_blur_fill(void) {
    static const int sz[2] = { 64, 16 };
    if (!blur_tex[0]) {
        glGenTextures(2, blur_tex);
        for (int i = 0; i < 2; i++) {
            glBindTexture(GL_TEXTURE_2D, blur_tex[i]);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, sz[i], sz[i], 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
    }
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho_(0, 1, 0, 1, -1, 1);
    GLuint src = drawable_tex;
    for (int i = 0; i < 2; i++) {
        bindFB(GL_FRAMEBUFFER_, present_fb);
        fbTex(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D, blur_tex[i], 0);
        glViewport(0, 0, sz[i], sz[i]);
        glBindTexture(GL_TEXTURE_2D, src); quad(0, 0, 1, 1, 0, 0, 1, 1);
        src = blur_tex[i];
    }
    bindFB(GL_FRAMEBUFFER_, 0);
    glViewport(0, 0, win_w, win_h);
    /* cover the window, keeping the picture's proportions (portrait 2:3), centred */
    float ww = (float)win_w, wh = (float)win_h, s = ww / 320.0f > wh / 480.0f ? ww / 320.0f : wh / 480.0f;
    float cw = 320.0f * s / ww, ch = 480.0f * s / wh;
    glColor4f(0.38f, 0.38f, 0.38f, 1); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_MODULATE);
    glBindTexture(GL_TEXTURE_2D, blur_tex[1]);
    quad(0.5f - cw / 2, 0.5f - ch / 2, 0.5f + cw / 2, 0.5f + ch / 2, 0, 0, 1, 1);
    glColor4f(1, 1, 1, 1); glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
    glMatrixMode(GL_PROJECTION); glLoadIdentity(); glOrtho_(0, scr_w(), scr_h(), 0, -1, 1);
}
static int present_count;
static char capture_path[512];
void gfx_capture_window(const char *path) { snprintf(capture_path, sizeof capture_path, "%s", path); }
static void present_frame(void) {
    if (g_ext.render_scale != tex_scale) apply_render_scale();
    glMatrixMode(GL_PROJECTION); glPushMatrix(); glLoadIdentity(); glOrtho_(0, scr_w(), scr_h(), 0, -1, 1);
    glMatrixMode(GL_MODELVIEW); glPushMatrix(); glLoadIdentity();
    glMatrixMode(GL_TEXTURE); glPushMatrix(); glLoadIdentity();
    bindFB(GL_FRAMEBUFFER_, 0);
    glViewport(0, 0, win_w, win_h);
    for (int i = 0; i < NCAPS; i++) glDisable(caps[i]);
    if (g_ext.fill == 2) glClearColor(0.07f, 0.075f, 0.08f, 1); else glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glEnable(GL_TEXTURE_2D);
    glTexEnvi(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE); glColor4f(1, 1, 1, 1);
    glDisableClientState(GL_COLOR_ARRAY); glEnableClientState(GL_VERTEX_ARRAY); glEnableClientState(GL_TEXTURE_COORD_ARRAY);
    if (g_ext.fill == 1) draw_blur_fill();
    int x, y, w, h; screen_box(&x, &y, &w, &h);
    glViewport(x, win_h - y - h, w, h);
    glBindTexture(GL_TEXTURE_2D, drawable_tex);
    GLint filt = g_ext.crisp ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filt); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filt);
    /* each landscape corner shows the view point land_to_view() gives; texture t is bottom-up */
    const float corners[4][2] = { { 0, 0 }, { (float)scr_w(), 0 }, { 0, (float)scr_h() }, { (float)scr_w(), (float)scr_h() } };
    GLfloat v[8], t[8];
    for (int i = 0; i < 4; i++) {
        float vx, vy; land_to_view(corners[i][0], corners[i][1], &vx, &vy);
        v[2 * i] = corners[i][0]; v[2 * i + 1] = corners[i][1];
        t[2 * i] = vx / 320.0f; t[2 * i + 1] = 1.0f - vy / 480.0f;
    }
    glVertexPointer(2, GL_FLOAT, 0, v); glTexCoordPointer(2, GL_FLOAT, 0, t);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    if (g_ext.overlay) { glViewport(0, 0, win_w, win_h); g_ext.overlay(x, y, w, h); }
    if (capture_path[0]) {                     /* the window as the player sees it (tests) */
        uint8_t *px = malloc((size_t)win_w * win_h * 4);
        glReadPixels(0, 0, win_w, win_h, GL_RGBA, GL_UNSIGNED_BYTE, px);
        FILE *f = fopen(capture_path, "wb");
        if (f) {
            fprintf(f, "P6\n%d %d\n255\n", win_w, win_h);
            for (int yy = win_h - 1; yy >= 0; yy--) for (int xx = 0; xx < win_w; xx++) fwrite(px + ((size_t)yy * win_w + xx) * 4, 1, 3, f);
            fclose(f);
        }
        free(px); capture_path[0] = 0;
    }
    present_count++;
    platform_swap();
    restore_state();
}
void w2c_env_host_present(E) {
    (void)e;
    if (!drawable_tex) return;
    if (present_count == 0) hlog("first present: game framebuffer status 0x%x", checkFB(GL_FRAMEBUFFER_));
    present_frame();
    presented = 1;
}
void gfx_redraw(void) { if (drawable_tex) present_frame(); }
void gfx_window_size(int *w, int *h) { *w = win_w; *h = win_h; }
/* PPM of the game's own framebuffer, upright in view orientation (portrait) */
int gfx_screenshot(const char *path) {
    if (!drawable_tex) return -1;
    int sw = fb_w * tex_scale, sh = fb_h * tex_scale;
    uint8_t *px = malloc((size_t)sw * sh * 4);
    bindFB(GL_FRAMEBUFFER_, present_fb);
    fbTex(GL_FRAMEBUFFER_, GL_COLOR_ATTACHMENT0_, GL_TEXTURE_2D, drawable_tex, 0);
    glReadPixels(0, 0, sw, sh, GL_RGBA, GL_UNSIGNED_BYTE, px);
    bindFB(GL_FRAMEBUFFER_, cur_fb);
    FILE *f = fopen(path, "wb"); if (!f) { free(px); return -1; }
    fprintf(f, "P6\n%d %d\n255\n", sw, sh);
    for (int y = sh - 1; y >= 0; y--) for (int x = 0; x < sw; x++) fwrite(px + ((size_t)y * sw + x) * 4, 1, 3, f);
    fclose(f); free(px); return 0;
}
