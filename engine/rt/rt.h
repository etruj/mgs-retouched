/* Runtime internals: everything that runs inside the wasm32 module next to the game.
 * Pointers are 32-bit and the original image occupies 0x1000-0x89000, so this code
 * sees exactly the object layouts the game sees. Talking to the OS goes through the
 * host imports declared here (host/ implements them natively). */
#ifndef RT_H
#define RT_H
typedef unsigned char u8; typedef signed char s8;
typedef unsigned short u16; typedef short s16;
typedef unsigned int u32; typedef int s32;
typedef unsigned long long u64; typedef long long s64;
typedef unsigned long size_t;
#define NULL ((void *)0)
#define IMG_LO 0x1000u
#define IMG_HI 0x89000u
#define P(x) ((void *)(unsigned long)(x))          /* u32 address -> pointer */
#define A(x) ((u32)(unsigned long)(x))             /* pointer -> u32 address */
#define RD32(a) (*(u32 *)P(a))
#define WR32(a, v) (*(u32 *)P(a) = (u32)(v))

#define IMPORT(name) __attribute__((import_module("env"), import_name(#name)))
/* HOSTFN: native builds import it from the host; the web build links the same host C into the
 * module (image decoding, text, mixer), so there it is an ordinary function */
#ifdef MGST_WEB
#define HOSTFN(name)
#else
#define HOSTFN(name) IMPORT(name)
#endif
#define EXPORT(name) __attribute__((export_name(#name)))

/* ---- host: process ---- */
IMPORT(host_log) void host_log(const char *s);
IMPORT(host_abort) void host_abort(const char *s);
IMPORT(host_time) double host_time(void);                /* monotonic seconds */
IMPORT(host_wall_time) double host_wall_time(void);      /* seconds since 2001-01-01 (CFAbsoluteTime) */
IMPORT(host_sleep) void host_sleep(double seconds);      /* releases the game lock while sleeping */
IMPORT(host_thread_start) void host_thread_start(u32 target, u32 sel, u32 arg);
IMPORT(host_fmt_double) int host_fmt_double(const char *spec, double v, char *out, int cap);
IMPORT(host_strtod) double host_strtod(const char *s, u32 endp);
IMPORT(host_vibrate) void host_vibrate(int ms);              /* the phone's vibrator (no-op where there is none) */
/* ---- host: math (D1: host libm; D2 replaces with Apple-equivalent code) ---- */
IMPORT(host_sinf) float host_sinf(float); IMPORT(host_cosf) float host_cosf(float);
IMPORT(host_atan2f) float host_atan2f(float, float); IMPORT(host_atan2) double host_atan2(double, double);
IMPORT(host_powf) float host_powf(float, float);
/* ---- host: files. Paths: /app/... = the bundle, /docs/... = writable storage ---- */
HOSTFN(host_fopen) int host_fopen(const char *path, const char *mode);
HOSTFN(host_fread) int host_fread(int h, void *dst, int n);
HOSTFN(host_fwrite) int host_fwrite(int h, const void *src, int n);
HOSTFN(host_fseek) int host_fseek(int h, int off, int whence);
HOSTFN(host_ftell) int host_ftell(int h);
HOSTFN(host_fclose) int host_fclose(int h);
HOSTFN(host_fsize) int host_fsize(const char *path);     /* -1 if absent */
HOSTFN(host_mkdir) int host_mkdir(const char *path);
/* ---- host: images and text ---- */
HOSTFN(host_image_decode) int host_image_decode(const char *path, u32 *w, u32 *h, int *has_alpha); /* -> handle */
HOSTFN(host_image_copy) void host_image_copy(int handle, void *dst_rgba_premul); /* frees the handle */
HOSTFN(host_text_measure) void host_text_measure(const char *utf8, float size, int bold, float max_w, int wrap, float *out_w, float *out_h);
HOSTFN(host_text_draw) void host_text_draw(const char *utf8, float size, int bold, float box_w, float box_h, int align, int wrap,
                                           u8 *dst_a8, int dst_w, int dst_h);
/* ---- host: display ---- */
IMPORT(host_present) void host_present(void);
IMPORT(host_glRenderbufferStorageFromDrawable) void host_glRenderbufferStorageFromDrawable(void);
IMPORT(host_screen) void host_screen(u32 *w, u32 *h);
/* ---- host: audio sink for AudioQueue streams (PCM s16) ---- */
HOSTFN(host_pcm_open) int host_pcm_open(int rate, int channels);
HOSTFN(host_pcm_write) void host_pcm_write(int h, const void *pcm, int bytes, int tag);
HOSTFN(host_pcm_done) int host_pcm_done(int h);          /* next completed tag, or -1 */
HOSTFN(host_pcm_control) void host_pcm_control(int h, int op, float v); /* 0 start 1 pause 2 stop/flush 3 volume 4 close */

/* ---- libc subset (rt/libc.c) ---- */
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strcpy(char *d, const char *s);
char *strchr(const char *s, int c);
char *strstr(const char *h, const char *n);
void *malloc(size_t n); void free(void *p); void *calloc(size_t a, size_t b); void *realloc(void *p, size_t n);
char *rt_strdup(const char *s);
int fmt_words(char *out, int cap, const char *fmt, const u32 *w, int n, int *used);
int rt_snprintf(char *out, int cap, const char *fmt, ...);
void rt_log(const char *fmt, ...);
void rt_fatal(const char *fmt, ...) __attribute__((noreturn));
#define RT_ONCE(stmt) do { static int __once; if (!__once) { __once = 1; stmt; } } while (0)

/* ---- objc runtime (rt/objc.c) ---- */
enum { MSGK_NORMAL = 0, MSGK_STRET = 1, MSGK_SUPER2 = 2 };
u64 objc_msgSend_w(const u32 *w, int n, int kind);
typedef struct Msg { u32 self, sel, stret; const u32 *a; int n; } Msg;
typedef u64 (*ShimFn)(Msg *m);
u32 sel_get(const char *name);                   /* canonical selector for a name */
u32 sel_canon(u32 sel_addr);
const char *sel_name(u32 sel);
u32 class_named(const char *name);               /* class address, or 0 */
u32 rt_class_new(const char *name, const char *super_name, u32 instance_size);
void rt_method(u32 cls, const char *sel, ShimFn fn);
void rt_cmethod(u32 cls, const char *sel, ShimFn fn);
u32 obj_class(u32 obj);
const char *class_name(u32 cls);
int class_is_meta(u32 cls);
u32 class_super(u32 cls);
u32 class_isize(u32 cls);
int obj_is_kind(u32 obj, u32 cls);
int obj_responds(u32 obj, u32 sel);
u32 obj_alloc(u32 cls, u32 size);                /* zeroed, retain count 1 */
u32 obj_retain(u32 obj); void obj_release(u32 obj); u32 obj_autorelease(u32 obj);
u32 obj_retain_count(u32 obj);
void obj_dealloc_memory(u32 obj);
void pool_push(void); void pool_pop(void);
u64 msg_send(u32 self, u32 sel, const u32 *args, int n);   /* runtime-originated message */
u64 msg_sendv(u32 self, const char *sel, int n, ...);       /* convenience: n word args */
#define SEND(obj, sel, ...) msg_sendv((obj), (sel), __VA_ARGS__)
/* argument access inside shims */
#define AU(i) (m->a[i])
#define AI(i) ((s32)m->a[i])
#define AF(i) (__builtin_bit_cast(float, m->a[i]))
#define AD(i) ((double)__builtin_bit_cast(double, ((u64)m->a[(i) + 1] << 32) | m->a[i]))
#define RF(x) ((u64)__builtin_bit_cast(u32, (float)(x)))
#define RD(x) (__builtin_bit_cast(u64, (double)(x)))
#define SHIM(name) static u64 name(Msg *m)
void rt_init_objc(void);
#ifdef MGST_RETOUCHED
/* Re-Touched message hooks (android-retouched/rt/rtx.c). pre: return 1 to replace the call (result
 * in *ret); it may edit the argument words (a private copy). post: may change the result. */
extern int rtx_hooks_on;
int rtx_is_hooked(u32 sel);
int rtx_pre(u32 self, u32 sel, u32 *args, int n, u64 *ret);
u64 rtx_post(u32 self, u32 sel, const u32 *args, int n, u64 ret);
#endif
void rt_init_foundation(void);
void rt_init_uikit(void);
void rt_init_bindings(void);
u32 rt_bind_value(const char *sym);              /* value for a bound external symbol */
unsigned long long invoke_orig(unsigned addr, const unsigned *w);   /* original code address -> recompiled function */

/* ---- Foundation helpers (rt/foundation.c) ---- */
u32 str_new_utf8(const char *s);                 /* autoreleased NSString */
u32 str_new_utf8_retained(const char *s);
u32 str_new_utf16(const u16 *u, int n, int retained);
const char *str_utf8(u32 s);                     /* valid while the string lives */
int str_utf16(u32 s, const u16 **out);
int str_is(u32 s);
u32 num_new_int(s32 v);
u32 arr_new(int retained);
void arr_add(u32 arr, u32 obj);
int arr_count(u32 arr);
u32 arr_get(u32 arr, int i);
u32 dict_new(int retained);
void dict_set(u32 d, u32 key, u32 val);
u32 dict_get(u32 d, u32 key);
u32 set_new_with(const u32 *objs, int n);
u32 data_new(const void *bytes, int n, int retained);
void timers_fire(double now);
void notify_post(const char *name, u32 obj);
#endif
