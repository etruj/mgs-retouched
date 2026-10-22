/* OpenGL ES 1.1 + OES_framebuffer_object: forwarded one-to-one to the host. Pointer
 * arguments travel as wasm offsets; the host resolves them against linear memory
 * (which never moves), so client-side vertex arrays stay valid until glDrawArrays. */
#include "rt.h"
#define G0(n) IMPORT(host_##n) void host_##n
G0(glBindFramebufferOES)(u32, u32);       void _glBindFramebufferOES(u32 a, u32 b) { host_glBindFramebufferOES(a, b); }
G0(glBindRenderbufferOES)(u32, u32);      void _glBindRenderbufferOES(u32 a, u32 b) { host_glBindRenderbufferOES(a, b); }
G0(glBindTexture)(u32, u32);              void _glBindTexture(u32 a, u32 b) { host_glBindTexture(a, b); }
G0(glBlendFunc)(u32, u32);                void _glBlendFunc(u32 a, u32 b) { host_glBlendFunc(a, b); }
G0(glClear)(u32);                         void _glClear(u32 a) { host_glClear(a); }
G0(glClipPlanef)(u32, u32);               void _glClipPlanef(u32 a, float *e) { host_glClipPlanef(a, A(e)); }
G0(glColor4f)(float, float, float, float); void _glColor4f(float r, float g, float b, float a) { host_glColor4f(r, g, b, a); }
G0(glDeleteFramebuffersOES)(s32, u32);    void _glDeleteFramebuffersOES(int n, u32 *p) { host_glDeleteFramebuffersOES(n, A(p)); }
G0(glDeleteRenderbuffersOES)(s32, u32);   void _glDeleteRenderbuffersOES(int n, u32 *p) { host_glDeleteRenderbuffersOES(n, A(p)); }
G0(glDeleteTextures)(s32, u32);           void _glDeleteTextures(int n, u32 *p) { host_glDeleteTextures(n, A(p)); }
G0(glDisable)(u32);                       void _glDisable(u32 a) { host_glDisable(a); }
G0(glDrawArrays)(u32, s32, s32);          void _glDrawArrays(u32 m, int f, int c) { host_glDrawArrays(m, f, c); }
G0(glEnable)(u32);                        void _glEnable(u32 a) { host_glEnable(a); }
G0(glEnableClientState)(u32);             void _glEnableClientState(u32 a) { host_glEnableClientState(a); }
G0(glFramebufferRenderbufferOES)(u32, u32, u32, u32);
void _glFramebufferRenderbufferOES(u32 a, u32 b, u32 c, u32 d) { host_glFramebufferRenderbufferOES(a, b, c, d); }
G0(glGenFramebuffersOES)(s32, u32);       void _glGenFramebuffersOES(int n, u32 *p) { host_glGenFramebuffersOES(n, A(p)); }
G0(glGenRenderbuffersOES)(s32, u32);      void _glGenRenderbuffersOES(int n, u32 *p) { host_glGenRenderbuffersOES(n, A(p)); }
G0(glGenTextures)(s32, u32);              void _glGenTextures(int n, u32 *p) { host_glGenTextures(n, A(p)); }
G0(glGetIntegerv)(u32, u32);              void _glGetIntegerv(u32 a, int *p) { host_glGetIntegerv(a, A(p)); }
G0(glGetRenderbufferParameterivOES)(u32, u32, u32);
void _glGetRenderbufferParameterivOES(u32 t, u32 n, int *p) { host_glGetRenderbufferParameterivOES(t, n, A(p)); }
G0(glLoadIdentity)(void);                 void _glLoadIdentity(void) { host_glLoadIdentity(); }
G0(glMatrixMode)(u32);                    void _glMatrixMode(u32 a) { host_glMatrixMode(a); }
G0(glOrthof)(float, float, float, float, float, float);
void _glOrthof(float l, float r, float b, float t, float n, float f) { host_glOrthof(l, r, b, t, n, f); }
G0(glScissor)(s32, s32, s32, s32);        void _glScissor(int x, int y, int w, int h) { host_glScissor(x, y, w, h); }
G0(glTexCoordPointer)(s32, u32, s32, u32); void _glTexCoordPointer(int s, u32 t, int st, void *p) { host_glTexCoordPointer(s, t, st, A(p)); }
G0(glTexEnvf)(u32, u32, float);           void _glTexEnvf(u32 a, u32 b, float c) { host_glTexEnvf(a, b, c); }
G0(glTexEnvi)(u32, u32, s32);             void _glTexEnvi(u32 a, u32 b, int c) { host_glTexEnvi(a, b, c); }
G0(glTexImage2D)(u32, s32, s32, s32, s32, s32, u32, u32, u32);
void _glTexImage2D(u32 t, int l, int i, int w, int h, int b, u32 f, u32 ty, void *p) { host_glTexImage2D(t, l, i, w, h, b, f, ty, A(p)); }
G0(glTexParameteri)(u32, u32, s32);       void _glTexParameteri(u32 a, u32 b, int c) { host_glTexParameteri(a, b, c); }
G0(glVertexPointer)(s32, u32, s32, u32);  void _glVertexPointer(int s, u32 t, int st, void *p) { host_glVertexPointer(s, t, st, A(p)); }
G0(glViewport)(s32, s32, s32, s32);       void _glViewport(int x, int y, int w, int h) { host_glViewport(x, y, w, h); }
IMPORT(host_glCheckFramebufferStatusOES) u32 host_glCheckFramebufferStatusOES(u32);
u32 _glCheckFramebufferStatusOES(u32 t) { return host_glCheckFramebufferStatusOES(t); }
IMPORT(host_glGetError) u32 host_glGetError(void);
u32 _glGetError(void) { return host_glGetError(); }
/* EAGL: renderbuffer storage comes from the drawable (the window) */
