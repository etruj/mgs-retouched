/* Native host for the wasm2c-compiled game module: everything the game needs from the
 * operating system. Shared by the desktop test build (SDL2) and Android. */
#ifndef HOST_H
#define HOST_H
#include <stdint.h>
#include <stddef.h>
#include "mgst_w2c.h"

extern w2c_mgst g_inst;
extern struct w2c_env { int unused; } g_env;
#define MEMBASE (g_inst.w2c_memory.data)
#define MEM(off) ((void *)(MEMBASE + (uint32_t)(off)))
#define CSTR(off) ((const char *)MEM(off))

/* configuration */
typedef struct {
    char app_dir[1024];     /* the original MGSTUS.app bundle */
    char overlay_dir[1024]; /* RETOUCHED: replacement files that win over the bundle ("" = none) */
    char docs_dir[1024];    /* writable storage */
    char lang[32];          /* English, Japanese, French, German, Italian, Spanish */
    int rotate;             /* 90 or 270: how the 320x480 portrait framebuffer maps to landscape */
    int trace;
    int audio_stats;        /* log mixer activity every 5 s */
    int raw_touch;          /* diagnostics: deliver touches unfiltered */
} HostConfig;
extern HostConfig g_cfg;

/* Optional extensions, used by METAL GEAR SOLID RE-TOUCHED (android-retouched/host). The original
 * edition leaves them at their defaults, which are exactly the original behaviour. */
typedef struct {
    int render_scale;       /* game framebuffer resolution: 1 = 320x480 (original), k = k times that */
    int fill;               /* outside the game picture: 0 black, 1 blurred game picture, 2 dark frame */
    int crisp;              /* 1: nearest-neighbour presentation instead of smooth */
    void (*overlay)(int bx, int by, int bw, int bh);    /* drawn over the game; the screen box in window pixels */
    int (*touch)(int phase, int id, float vx, float vy); /* sees every touch first; 1 = consumed */
    float touch_slop;       /* points before a touch counts as moving (original: 3) */
    float aim_gain;         /* movement gain of a sliding finger (original: 1) */
    float music_gain, effects_gain;  /* mixer volumes (original: 1) */
    int hq_resample;        /* 1: cubic resampling of effects and music */
    void (*pre_boot)(void); /* after the module is instantiated, before the game starts (lock held) */
} HostExt;
extern HostExt g_ext;
void gfx_redraw(void);      /* show the last game frame again (paused game: the overlay changes, the game doesn't) */
void gfx_window_size(int *w, int *h);
void gfx_capture_window(const char *path);   /* save the next presented window (game + overlay) as PPM */

/* logging */
void hlog(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void hlog_platform(const char *line);   /* per platform: stdout / logcat */

/* the game lock: all wasm code runs while holding it */
typedef struct { uint32_t sp; int has_sp; } GameThread;
void game_lock(GameThread *t);
void game_unlock(GameThread *t);
extern GameThread g_main_thread;

/* lifecycle */
int host_start(void);                   /* instantiate + boot; returns 0 on success */
void host_frame(double now);            /* timers, deferred work, audio callbacks */
void host_touch(int phase, int id, float vx, float vy);   /* view coordinates (320x480 portrait); host/touch.c */
void host_touch_deliver(int phase, int id, float x, float y);   /* per platform: hand a filtered touch to the game */
void host_accel(double x, double y, double z);
void host_lifecycle(int ev);
extern void (*platform_vibrate)(int ms);   /* optional, per platform */
double host_now(void);
double host_next_timer(void);          /* when the next NSTimer is due */
int host_is_crashed(void);              /* a trap stopped the game */

/* graphics: per-platform GL loader + present target */
void gfx_init(int fb_w, int fb_h);
void gfx_set_window(int w, int h);
int gfx_frame_presented(void);          /* nonzero once since last call */
void gfx_window_to_view(float wx, float wy, float *vx, float *vy);
int gfx_screenshot(const char *path);   /* writes a PPM of the game framebuffer (portrait) */
void platform_swap(void);               /* per platform */

/* images */
int img_decode_file(const char *path, uint32_t *w, uint32_t *h, int *alpha, uint8_t **rgba_premul);

/* text */
int text_init(const char *regular, const char *bold, const char *cjk, const char *display);   /* display may be NULL */   /* font paths (vfs) */
void text_measure(const char *utf8, float size, int bold, float max_w, int wrap, float *w, float *h);
void text_draw(const char *utf8, float size, int bold, float box_w, float box_h, int align, int wrap, uint8_t *dst, int dw, int dh);

/* audio */
void audio_init(int out_rate);
void audio_mix(int16_t *out, int frames);   /* called by the platform audio thread, interleaved stereo */

/* paths and files: /app = the original bundle, /docs = writable storage */
int host_map_path(const char *game_path, char *out, size_t cap);
typedef struct VFile VFile;
VFile *vfs_open(const char *path, const char *mode);
size_t vfs_read(VFile *v, void *d, size_t n);
size_t vfs_write(VFile *v, const void *s, size_t n);
int vfs_seek(VFile *v, long off, int wh);
long vfs_tell(VFile *v);
int vfs_close(VFile *v);
long vfs_size(const char *path);
uint8_t *vfs_read_all(const char *path, long *size);
#endif
