/* Host core: instance + game lock + threads, logging, time, files, math, formatting. */
#include "host.h"
#include "wasm-rt-exceptions.h"
#include "wasm-rt-impl.h"
#include <math.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <errno.h>

w2c_mgst g_inst;
struct w2c_env g_env;
HostConfig g_cfg = { .lang = "English", .rotate = 0 };
GameThread g_main_thread;

/* ------------------------------------------------------------------ logging */
void hlog(const char *fmt, ...) {
    char b[4096]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    hlog_platform(b);
}
void w2c_env_host_log(struct w2c_env *e, u32 s) { (void)e; hlog_platform(CSTR(s)); }
void w2c_env_host_abort(struct w2c_env *e, u32 s) { (void)e; hlog("FATAL (game runtime): %s", CSTR(s)); wasm_rt_trap(WASM_RT_TRAP_UNREACHABLE); }

/* ------------------------------------------------------------------ time */
static double t0;
double host_now(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9 - t0; }
f64 w2c_env_host_time(struct w2c_env *e) { (void)e; return host_now(); }
f64 w2c_env_host_wall_time(struct w2c_env *e) {
    (void)e; struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9 - 978307200.0;          /* CFAbsoluteTime epoch: 2001-01-01 */
}

/* ------------------------------------------------------------------ traps */
static int host_crashed;
int host_is_crashed(void) { return host_crashed; }
#define GUARDED(what, call) do {                                              \
    wasm_rt_trap_t _tr = wasm_rt_impl_try();                                  \
    if (_tr) { hlog("TRAP in %s: %s", what, wasm_rt_strerror(_tr)); host_crashed = 1; } \
    else { call; }                                                            \
} while (0)

/* ------------------------------------------------------------------ game lock + threads
 * One lock serialises all wasm execution (the module has one shadow stack pointer, so
 * every thread brings its own shadow stack and swaps it in while holding the lock). */
static pthread_mutex_t glock = PTHREAD_MUTEX_INITIALIZER;
void game_lock(GameThread *t) {
    pthread_mutex_lock(&glock);
    if (t->has_sp) { uint32_t *sp = w2c_mgst_0x5F_stack_pointer(&g_inst); uint32_t mine = t->sp; t->sp = *sp; *sp = mine; }
}
void game_unlock(GameThread *t) {
    if (t->has_sp) { uint32_t *sp = w2c_mgst_0x5F_stack_pointer(&g_inst); uint32_t theirs = t->sp; t->sp = *sp; *sp = theirs; }
    pthread_mutex_unlock(&glock);
}
static __thread GameThread *cur_thread;
void w2c_env_host_sleep(struct w2c_env *e, f64 s) {
    (void)e; GameThread *t = cur_thread ? cur_thread : &g_main_thread;
    game_unlock(t);
    if (s > 0) { struct timespec ts = { (time_t)s, (long)((s - (time_t)s) * 1e9) }; nanosleep(&ts, NULL); }
    else sched_yield();
    game_lock(t);
}
typedef struct { uint32_t target, sel, arg; } ThreadArgs;
static void *thread_main(void *p) {
    ThreadArgs a = *(ThreadArgs *)p; free(p);
    wasm_rt_init_thread();
    GameThread t = { 0 }; cur_thread = &t;
    /* this thread's shadow stack: 512 KiB from the module heap (allocated under the lock) */
    pthread_mutex_lock(&glock);
    uint32_t base = w2c_mgst_mgst_malloc(&g_inst, 512 * 1024);
    pthread_mutex_unlock(&glock);
    t.sp = (base + 512 * 1024) & ~15u; t.has_sp = 1;
    game_lock(&t);
    GUARDED("game thread", w2c_mgst_mgst_thread_entry(&g_inst, a.target, a.sel, a.arg));
    game_unlock(&t);
    hlog("game thread finished");
    wasm_rt_free_thread();
    return NULL;
}
void w2c_env_host_thread_start(struct w2c_env *e, u32 target, u32 sel, u32 arg) {
    (void)e; ThreadArgs *a = malloc(sizeof *a); a->target = target; a->sel = sel; a->arg = arg;
    pthread_t th; pthread_attr_t at; pthread_attr_init(&at); pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    pthread_attr_setstacksize(&at, 8 << 20);    /* wasm2c code runs on the native stack */
    if (pthread_create(&th, &at, thread_main, a)) hlog("thread start failed");
    hlog("NSThread started (game thread)");
}

/* ------------------------------------------------------------------ files */
#ifdef MGST_RETOUCHED
/* RETOUCHED ships replacement files (the title wordmark) in g_cfg.overlay_dir; a file found there
 * wins over the same name in the original bundle. */
static int overlay_path(const char *rel, char *out, size_t cap) {
    if (!g_cfg.overlay_dir[0] || strchr(rel, '/')) return 0;
    snprintf(out, cap, "%s/%s", g_cfg.overlay_dir, rel);
    return vfs_size(out) >= 0;
}
#endif
int host_map_path(const char *p, char *out, size_t cap) {
#ifdef MGST_RETOUCHED
    const char *rel = !strncmp(p, "/app/", 5) ? p + 5 : p[0] != '/' ? p : NULL;
    if (rel && overlay_path(rel, out, cap)) return 1;
#endif
    if (!strncmp(p, "/app/", 5) || !strcmp(p, "/app")) { snprintf(out, cap, "%s%s", g_cfg.app_dir, p + 4); return 1; }
    if (!strncmp(p, "/docs", 5)) { snprintf(out, cap, "%s%s", g_cfg.docs_dir, p + 5); return 1; }
    if (p[0] != '/') { snprintf(out, cap, "%s/%s", g_cfg.app_dir, p); return 1; }   /* cwd is the bundle */
    snprintf(out, cap, "%s", p);
    hlog("file access outside /app and /docs: %s", p);
    return 0;
}
#define MAXF 256
static VFile *files[MAXF];
static void mkdirs_for(const char *real) {
    char dir[2048]; snprintf(dir, sizeof dir, "%s", real);
    for (char *p = dir + 1; *p; p++) if (*p == '/') { *p = 0; mkdir(dir, 0755); *p = '/'; }
}
u32 w2c_env_host_fopen(struct w2c_env *e, u32 path, u32 mode) {
    (void)e; char real[2048]; host_map_path(CSTR(path), real, sizeof real);
    const char *m = CSTR(mode);
    if (strchr(m, 'w') || strchr(m, 'a')) mkdirs_for(real);    /* the save directory may not exist yet */
    VFile *f = vfs_open(real, m);
    if (!f) { if (g_cfg.trace) hlog("fopen(%s, %s) failed", real, m); return (u32)-1; }
    for (int i = 0; i < MAXF; i++) if (!files[i]) { files[i] = f; return (u32)i; }
    vfs_close(f); return (u32)-1;
}
static VFile *hf(u32 h) { return h < MAXF ? files[h] : NULL; }
u32 w2c_env_host_fread(struct w2c_env *e, u32 h, u32 dst, u32 n) { (void)e; VFile *f = hf(h); return f ? (u32)vfs_read(f, MEM(dst), n) : 0; }
u32 w2c_env_host_fwrite(struct w2c_env *e, u32 h, u32 src, u32 n) { (void)e; VFile *f = hf(h); return f ? (u32)vfs_write(f, MEM(src), n) : 0; }
u32 w2c_env_host_fseek(struct w2c_env *e, u32 h, u32 off, u32 wh) { (void)e; VFile *f = hf(h); return f ? (u32)vfs_seek(f, (long)(int32_t)off, (int)wh) : (u32)-1; }
u32 w2c_env_host_ftell(struct w2c_env *e, u32 h) { (void)e; VFile *f = hf(h); return f ? (u32)vfs_tell(f) : (u32)-1; }
u32 w2c_env_host_fclose(struct w2c_env *e, u32 h) { (void)e; VFile *f = hf(h); if (!f) return (u32)-1; files[h] = NULL; return (u32)vfs_close(f); }
u32 w2c_env_host_fsize(struct w2c_env *e, u32 path) {
    (void)e; char real[2048]; host_map_path(CSTR(path), real, sizeof real);
    return (u32)vfs_size(real);
}
u32 w2c_env_host_mkdir(struct w2c_env *e, u32 path) { (void)e; char real[2048]; host_map_path(CSTR(path), real, sizeof real); return (u32)mkdir(real, 0755); }

/* ------------------------------------------------------------------ math, formatting */
f32 w2c_env_host_sinf(struct w2c_env *e, f32 x) { (void)e; return sinf(x); }
f32 w2c_env_host_cosf(struct w2c_env *e, f32 x) { (void)e; return cosf(x); }
f32 w2c_env_host_atan2f(struct w2c_env *e, f32 y, f32 x) { (void)e; return atan2f(y, x); }
f64 w2c_env_host_atan2(struct w2c_env *e, f64 y, f64 x) { (void)e; return atan2(y, x); }
u32 w2c_env_host_fmt_double(struct w2c_env *e, u32 spec, f64 v, u32 out, u32 cap) {
    (void)e; const char *s = CSTR(spec);
    if (strpbrk(s, "%") != s || strpbrk(s + 1, "%*n")) return 0;     /* exactly one conversion, nothing exotic */
    int n = snprintf((char *)MEM(out), cap, s, v);
    return (u32)(n < (int)cap ? n : (int)cap - 1);
}
f64 w2c_env_host_strtod(struct w2c_env *e, u32 s, u32 endp) { (void)e; (void)endp; return strtod(CSTR(s), NULL); }
void (*platform_vibrate)(int ms);   /* set by platforms with a vibrator */
void w2c_env_host_vibrate(struct w2c_env *e, u32 ms) { (void)e; if (platform_vibrate) platform_vibrate((int)ms); else if (g_cfg.trace) hlog("vibrate %u ms", ms); }

/* ------------------------------------------------------------------ lifecycle */
int host_start(void) {
    t0 = 0; t0 = host_now();
    wasm_rt_init();
    wasm2c_mgst_instantiate(&g_inst, &g_env);
    hlog("module instantiated: %llu MiB linear memory", (unsigned long long)(g_inst.w2c_memory.size >> 20));
    game_lock(&g_main_thread);
    {   /* pass the language through the module heap */
        uint32_t p = w2c_mgst_mgst_malloc(&g_inst, 32); strcpy((char *)MEM(p), g_cfg.lang);
        w2c_mgst_mgst_set_language(&g_inst, p);
        if (g_cfg.trace) w2c_mgst_mgst_trace(&g_inst, 1);
        if (g_ext.pre_boot) g_ext.pre_boot();
    }
    int ok = 0;
    GUARDED("boot", ok = 1; w2c_mgst_mgst_boot(&g_inst));
    game_unlock(&g_main_thread);
    return ok && !host_crashed ? 0 : -1;
}
void host_frame(double now) {
    if (host_crashed) return;
    game_lock(&g_main_thread);
    GUARDED("frame", w2c_mgst_mgst_tick(&g_inst, now));
    game_unlock(&g_main_thread);
}
/* touches: filtered in host/touch.c, delivered to the game under the lock */
void host_touch_deliver(int phase, int id, float x, float y) {
    if (host_crashed) return;
    game_lock(&g_main_thread);
    GUARDED("touch", w2c_mgst_mgst_touch(&g_inst, (u32)phase, (u32)id, x, y, host_now()));
    game_unlock(&g_main_thread);
}
void host_accel(double x, double y, double z) {
    game_lock(&g_main_thread); w2c_mgst_mgst_accel(&g_inst, x, y, z); game_unlock(&g_main_thread);
}
void host_lifecycle(int ev) {
    if (host_crashed) return;
    game_lock(&g_main_thread); GUARDED("lifecycle", w2c_mgst_mgst_lifecycle(&g_inst, (u32)ev)); game_unlock(&g_main_thread);
}
double host_next_timer(void) {
    game_lock(&g_main_thread); double t = w2c_mgst_mgst_next_timer(&g_inst); game_unlock(&g_main_thread); return t;
}
