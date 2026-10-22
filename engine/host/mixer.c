/* Audio: the OpenAL subset the game uses, mixed in software, plus PCM streams for the
 * AudioQueue (music). The platform calls audio_mix() from its audio thread. Sample data
 * of OpenAL buffers is read straight from module memory (alBufferDataStatic). */
#include "host.h"
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#define AL_NONE 0
#define AL_SOURCE_RELATIVE 0x202
#define AL_LOOPING 0x1007
#define AL_BUFFER 0x1009
#define AL_GAIN 0x100A
#define AL_POSITION 0x1004
#define AL_VELOCITY 0x1006
#define AL_PITCH 0x1003
#define AL_ORIENTATION 0x100F
#define AL_SOURCE_STATE 0x1010
#define AL_INITIAL 0x1011
#define AL_PLAYING 0x1012
#define AL_PAUSED 0x1013
#define AL_STOPPED 0x1014
#define AL_REFERENCE_DISTANCE 0x1020
#define AL_ROLLOFF_FACTOR 0x1021
#define AL_MAX_DISTANCE 0x1023
#define AL_MIN_GAIN 0x100D
#define AL_MAX_GAIN 0x100E
#define AL_FORMAT_MONO8 0x1100
#define AL_FORMAT_MONO16 0x1101
#define AL_FORMAT_STEREO8 0x1102
#define AL_FORMAT_STEREO16 0x1103
#define AL_INVERSE_DISTANCE 0xD001
#define AL_INVERSE_DISTANCE_CLAMPED 0xD002
#define AL_LINEAR_DISTANCE_CLAMPED 0xD004

static pthread_mutex_t mx = PTHREAD_MUTEX_INITIALIZER;
static int out_rate = 44100;

typedef struct { int used; uint32_t data, size, freq; int fmt; } Buf;
typedef struct { int used, state, looping, relative; uint32_t buffer; float gain, pitch, pos[3], ref, rolloff, maxd, ming, maxg; double cursor; } Src;
#define NBUF 1024
#define NSRC 64
static Buf bufs[NBUF]; static Src srcs[NSRC];
static float listener_pos[3], listener_gain = 1, listener_ori[6] = { 0, 0, -1, 0, 1, 0 };
static int dist_model = AL_INVERSE_DISTANCE_CLAMPED;

static void gen(int n, uint32_t out, int is_src) {
    uint32_t *o = MEM(out);
    pthread_mutex_lock(&mx);
    for (int k = 0; k < n; k++) {
        o[k] = 0;
        if (is_src) { for (int i = 1; i < NSRC; i++) if (!srcs[i].used) { memset(&srcs[i], 0, sizeof srcs[i]); srcs[i].used = 1; srcs[i].state = AL_INITIAL; srcs[i].gain = srcs[i].pitch = 1; srcs[i].ref = 1; srcs[i].rolloff = 1; srcs[i].maxd = 1e30f; srcs[i].maxg = 1; o[k] = i; break; } }
        else { for (int i = 1; i < NBUF; i++) if (!bufs[i].used) { memset(&bufs[i], 0, sizeof bufs[i]); bufs[i].used = 1; o[k] = i; break; } }
        if (!o[k]) hlog("OpenAL: out of %s", is_src ? "sources" : "buffers");
    }
    pthread_mutex_unlock(&mx);
}
void w2c_env_host_alGenSources(struct w2c_env *e, u32 n, u32 p) { (void)e; gen((int)n, p, 1); }
void w2c_env_host_alGenBuffers(struct w2c_env *e, u32 n, u32 p) { (void)e; gen((int)n, p, 0); }
void w2c_env_host_alDeleteSources(struct w2c_env *e, u32 n, u32 p) { (void)e; uint32_t *o = MEM(p); pthread_mutex_lock(&mx); for (u32 i = 0; i < n; i++) if (o[i] < NSRC) srcs[o[i]].used = 0; pthread_mutex_unlock(&mx); }
void w2c_env_host_alDeleteBuffers(struct w2c_env *e, u32 n, u32 p) { (void)e; uint32_t *o = MEM(p); pthread_mutex_lock(&mx); for (u32 i = 0; i < n; i++) if (o[i] < NBUF) bufs[o[i]].used = 0; pthread_mutex_unlock(&mx); }
static Src *S(u32 s) { return s && s < NSRC && srcs[s].used ? &srcs[s] : NULL; }
void w2c_env_host_alBufferDataStatic(struct w2c_env *e, u32 b, u32 fmt, u32 data, u32 size, u32 freq) {
    (void)e; if (!b || b >= NBUF) return;
    pthread_mutex_lock(&mx); bufs[b].data = data; bufs[b].size = size; bufs[b].freq = freq; bufs[b].fmt = (int)fmt; pthread_mutex_unlock(&mx);
}
void w2c_env_host_alSourcei(struct w2c_env *e, u32 s, u32 p, u32 v) {
    (void)e; pthread_mutex_lock(&mx); Src *x = S(s);
    if (x) switch (p) {
    case AL_BUFFER: x->buffer = v; x->cursor = 0; break;
    case AL_LOOPING: x->looping = (int)v; break;
    case AL_SOURCE_RELATIVE: x->relative = (int)v; break;
    default: break;
    }
    pthread_mutex_unlock(&mx);
}
void w2c_env_host_alSourcef(struct w2c_env *e, u32 s, u32 p, f32 v) {
    (void)e; pthread_mutex_lock(&mx); Src *x = S(s);
    if (x) switch (p) {
    case AL_GAIN: x->gain = v; break; case AL_PITCH: x->pitch = v; break;
    case AL_REFERENCE_DISTANCE: x->ref = v; break; case AL_ROLLOFF_FACTOR: x->rolloff = v; break;
    case AL_MAX_DISTANCE: x->maxd = v; break; case AL_MIN_GAIN: x->ming = v; break; case AL_MAX_GAIN: x->maxg = v; break;
    default: break;
    }
    pthread_mutex_unlock(&mx);
}
void w2c_env_host_alSource3f(struct w2c_env *e, u32 s, u32 p, f32 a, f32 b, f32 c) {
    (void)e; pthread_mutex_lock(&mx); Src *x = S(s); if (x && p == AL_POSITION) { x->pos[0] = a; x->pos[1] = b; x->pos[2] = c; } pthread_mutex_unlock(&mx);
}
void w2c_env_host_alListener3f(struct w2c_env *e, u32 p, f32 a, f32 b, f32 c) { (void)e; if (p == AL_POSITION) { listener_pos[0] = a; listener_pos[1] = b; listener_pos[2] = c; } }
void w2c_env_host_alListenerfv(struct w2c_env *e, u32 p, u32 v) {
    (void)e; float *f = MEM(v);
    if (p == AL_ORIENTATION) memcpy(listener_ori, f, 24); else if (p == AL_POSITION) memcpy(listener_pos, f, 12); else if (p == AL_GAIN) listener_gain = f[0];
}
void w2c_env_host_alSourcePlay(struct w2c_env *e, u32 s) { (void)e; pthread_mutex_lock(&mx); Src *x = S(s); if (x) { if (x->state != AL_PAUSED) x->cursor = 0; x->state = AL_PLAYING; } pthread_mutex_unlock(&mx); }
void w2c_env_host_alSourceStop(struct w2c_env *e, u32 s) { (void)e; pthread_mutex_lock(&mx); Src *x = S(s); if (x) { x->state = AL_STOPPED; x->cursor = 0; } pthread_mutex_unlock(&mx); }
void w2c_env_host_alSourcePause(struct w2c_env *e, u32 s) { (void)e; pthread_mutex_lock(&mx); Src *x = S(s); if (x && x->state == AL_PLAYING) x->state = AL_PAUSED; pthread_mutex_unlock(&mx); }
void w2c_env_host_alGetSourcei(struct w2c_env *e, u32 s, u32 p, u32 out) {
    (void)e; pthread_mutex_lock(&mx); Src *x = S(s); int32_t v = 0;
    if (x) { if (p == AL_SOURCE_STATE) v = x->state; else if (p == AL_BUFFER) v = (int32_t)x->buffer; else if (p == AL_LOOPING) v = x->looping; }
    pthread_mutex_unlock(&mx); *(int32_t *)MEM(out) = v;
}
void w2c_env_host_alDistanceModel(struct w2c_env *e, u32 m) { (void)e; dist_model = (int)m; }
void w2c_env_host_alDopplerFactor(struct w2c_env *e, f32 v) { (void)e; (void)v; }
u32 w2c_env_host_alGetError(struct w2c_env *e) { (void)e; return 0; }

/* ------------------------------------------------------------------ PCM streams (AudioQueue) */
typedef struct Chunk { int16_t *pcm; int frames, pos, tag; struct Chunk *next; } Chunk;
typedef struct { int used, rate, ch, playing; float volume; double frac; Chunk *head, *tail; int done[64], ndone; } Stream;
#define NSTREAM 8
static Stream streams[NSTREAM];
u32 w2c_env_host_pcm_open(struct w2c_env *e, u32 rate, u32 ch) {
    (void)e; pthread_mutex_lock(&mx);
    for (int i = 0; i < NSTREAM; i++) if (!streams[i].used) { memset(&streams[i], 0, sizeof streams[i]); streams[i].used = 1; streams[i].rate = (int)rate; streams[i].ch = (int)ch; streams[i].volume = 1; pthread_mutex_unlock(&mx); return (u32)i; }
    pthread_mutex_unlock(&mx); hlog("audio: out of streams"); return 0;
}
void w2c_env_host_pcm_write(struct w2c_env *e, u32 h, u32 pcm, u32 bytes, u32 tag) {
    (void)e; if (h >= NSTREAM) return;
    Chunk *c = calloc(1, sizeof *c); Stream *s = &streams[h];
    c->frames = (int)bytes / (2 * s->ch); c->pcm = malloc(bytes + 4); memcpy(c->pcm, MEM(pcm), bytes); c->tag = (int)tag;
    pthread_mutex_lock(&mx);
    if (s->tail) s->tail->next = c; else s->head = c;
    s->tail = c;
    pthread_mutex_unlock(&mx);
}
u32 w2c_env_host_pcm_done(struct w2c_env *e, u32 h) {
    (void)e; if (h >= NSTREAM) return (u32)-1;
    pthread_mutex_lock(&mx); Stream *s = &streams[h]; int t = -1;
    if (s->ndone) { t = s->done[0]; memmove(s->done, s->done + 1, (--s->ndone) * sizeof(int)); }
    pthread_mutex_unlock(&mx); return (u32)t;
}
void w2c_env_host_pcm_control(struct w2c_env *e, u32 h, u32 op, f32 v) {
    (void)e; if (h >= NSTREAM) return;
    pthread_mutex_lock(&mx); Stream *s = &streams[h];
    switch (op) {
    case 0: s->playing = 1; break;
    case 1: s->playing = 0; break;
    case 2: case 4:   /* stop: drop queued audio (its buffers are handed back), close */
        for (Chunk *c = s->head; c;) { Chunk *n = c->next; if (op == 2 && s->ndone < 64) s->done[s->ndone++] = c->tag; free(c->pcm); free(c); c = n; }
        s->head = s->tail = NULL; s->playing = 0; if (op == 4) s->used = 0; break;
    case 3: s->volume = v; break;
    }
    pthread_mutex_unlock(&mx);
}

/* ------------------------------------------------------------------ mixing */
void audio_init(int rate) { out_rate = rate; }
/* Re-Touched "high-quality resampling": 4-point cubic (Catmull-Rom) instead of linear (effects) and
 * sample-and-hold (music streams). Off in the original edition. */
static float cubic(float y0, float y1, float y2, float y3, float t) {
    return y1 + 0.5f * t * (y2 - y0 + t * (2 * y0 - 5 * y1 + 4 * y2 - y3 + t * (3 * (y1 - y2) + y3 - y0)));
}
static float src_sample(const Buf *b, long i, int ch) {
    const uint8_t *d = MEM(b->data);
    switch (b->fmt) {
    case AL_FORMAT_MONO16: return ((const int16_t *)d)[i] / 32768.0f;
    case AL_FORMAT_STEREO16: return ((const int16_t *)d)[i * 2 + ch] / 32768.0f;
    case AL_FORMAT_MONO8: return (d[i] - 128) / 128.0f;
    case AL_FORMAT_STEREO8: return (d[i * 2 + ch] - 128) / 128.0f;
    }
    return 0;
}
static long buf_frames(const Buf *b) { int bpf = b->fmt == AL_FORMAT_STEREO16 ? 4 : (b->fmt == AL_FORMAT_MONO16 || b->fmt == AL_FORMAT_STEREO8) ? 2 : 1; return b->size / bpf; }
void audio_mix(int16_t *out, int frames) {
    float *acc = calloc((size_t)frames * 2, sizeof(float));
    const int hq = g_ext.hq_resample;
    pthread_mutex_lock(&mx);
    for (int i = 1; i < NSRC; i++) {
        Src *x = &srcs[i];
        if (!x->used || x->state != AL_PLAYING || !x->buffer || x->buffer >= NBUF || !bufs[x->buffer].used || !bufs[x->buffer].size) continue;
        Buf *b = &bufs[x->buffer]; long n = buf_frames(b);
        int stereo = b->fmt == AL_FORMAT_STEREO16 || b->fmt == AL_FORMAT_STEREO8;
        /* distance attenuation and pan (mono sources are positioned) */
        float g = x->gain * listener_gain * g_ext.effects_gain, pl = 1, pr = 1;
        if (!stereo) {
            float dx = x->pos[0] - (x->relative ? 0 : listener_pos[0]), dy = x->pos[1] - (x->relative ? 0 : listener_pos[1]), dz = x->pos[2] - (x->relative ? 0 : listener_pos[2]);
            float d = sqrtf(dx * dx + dy * dy + dz * dz);
            if (dist_model == AL_INVERSE_DISTANCE_CLAMPED || dist_model == AL_INVERSE_DISTANCE) {
                if (dist_model == AL_INVERSE_DISTANCE_CLAMPED) { if (d < x->ref) d = x->ref; if (d > x->maxd) d = x->maxd; }
                float den = x->ref + x->rolloff * (d - x->ref); if (den > 0) g *= x->ref / den;
            } else if (dist_model == AL_LINEAR_DISTANCE_CLAMPED) {
                if (d < x->ref) d = x->ref; if (d > x->maxd) d = x->maxd;
                if (x->maxd > x->ref) g *= 1 - x->rolloff * (d - x->ref) / (x->maxd - x->ref);
            }
            if (d > 1e-4f) {   /* azimuth relative to the listener's right vector (at x up) */
                float *at = listener_ori, *up = listener_ori + 3;
                float rx = at[1] * up[2] - at[2] * up[1], ry = at[2] * up[0] - at[0] * up[2], rz = at[0] * up[1] - at[1] * up[0];
                float pan = (dx * rx + dy * ry + dz * rz) / d;        /* -1 left .. 1 right */
                float a = (pan + 1) * 0.25f * 3.14159265f; pl = cosf(a) * 1.41421356f; pr = sinf(a) * 1.41421356f;
            }
        }
        if (x->maxg > 0 && g > x->maxg) g = x->maxg;
        if (!isfinite(g) || !isfinite(pl) || !isfinite(pr)) { static int warned; if (!warned++) hlog("audio: source %d has a non-finite gain or pan (skipped)", i); continue; }
        if (!isfinite(x->pitch) || x->pitch <= 0) continue;
        double step = (double)b->freq * x->pitch / out_rate;
        for (int f = 0; f < frames; f++) {
            long i0 = (long)x->cursor; if (i0 >= n) { if (x->looping) { x->cursor -= n; i0 = (long)x->cursor; } else { x->state = AL_STOPPED; x->cursor = 0; break; } }
            long i1 = i0 + 1 < n ? i0 + 1 : (x->looping ? 0 : i0); float t = (float)(x->cursor - i0);
            float l, r;
            if (hq) {
                long im = i0 > 0 ? i0 - 1 : (x->looping ? n - 1 : 0), i2 = i1 + 1 < n ? i1 + 1 : (x->looping ? (i1 + 1) % n : i1);
                l = cubic(src_sample(b, im, 0), src_sample(b, i0, 0), src_sample(b, i1, 0), src_sample(b, i2, 0), t);
                r = stereo ? cubic(src_sample(b, im, 1), src_sample(b, i0, 1), src_sample(b, i1, 1), src_sample(b, i2, 1), t) : l;
            } else {
                l = src_sample(b, i0, 0) * (1 - t) + src_sample(b, i1, 0) * t;
                r = stereo ? src_sample(b, i0, 1) * (1 - t) + src_sample(b, i1, 1) * t : l;
            }
            acc[f * 2] += l * g * pl; acc[f * 2 + 1] += r * g * pr;
            x->cursor += step;
        }
    }
    for (int k = 0; k < NSTREAM; k++) {
        Stream *s = &streams[k]; if (!s->used || !s->playing) continue;
        double step = (double)s->rate / out_rate;
        for (int f = 0; f < frames && s->head; f++) {
            Chunk *c = s->head;
            int16_t *p = c->pcm + c->pos * s->ch;
            float l = p[0] / 32768.0f, r = s->ch > 1 ? p[1] / 32768.0f : l;
            if (hq) {   /* neighbours within this chunk (the edges of a chunk fall back to its own samples) */
                int pm = c->pos > 0 ? c->pos - 1 : c->pos, p1 = c->pos + 1 < c->frames ? c->pos + 1 : c->pos, p2 = p1 + 1 < c->frames ? p1 + 1 : p1;
                float t = (float)s->frac;
                for (int ch = 0; ch < (s->ch > 1 ? 2 : 1); ch++) {
                    float v = cubic(c->pcm[pm * s->ch + ch] / 32768.0f, c->pcm[c->pos * s->ch + ch] / 32768.0f,
                                    c->pcm[p1 * s->ch + ch] / 32768.0f, c->pcm[p2 * s->ch + ch] / 32768.0f, t);
                    if (ch == 0) l = v; else r = v;
                }
                if (s->ch == 1) r = l;
            }
            float mv = s->volume * g_ext.music_gain;
            acc[f * 2] += l * mv; acc[f * 2 + 1] += r * mv;
            s->frac += step;
            while (s->frac >= 1 && s->head) {
                s->frac -= 1; c->pos++;
                if (c->pos >= c->frames) { if (s->ndone < 64) s->done[s->ndone++] = c->tag; s->head = c->next; if (!s->head) s->tail = NULL; free(c->pcm); free(c); c = s->head; }
            }
        }
    }
    static long mixed; static double peak; mixed += frames;
    for (int i = 0; i < frames * 2; i++) { double a = acc[i] < 0 ? -acc[i] : acc[i]; if (a > peak) peak = a; }
    if (g_cfg.audio_stats && mixed >= out_rate * 5L) {
        int ns = 0, nq = 0; for (int i = 1; i < NSRC; i++) ns += srcs[i].used && srcs[i].state == AL_PLAYING;
        for (int k = 0; k < NSTREAM; k++) for (Chunk *c = streams[k].head; c; c = c->next) nq++;
        hlog("audio: %d sources playing, %d music chunks queued, peak %.3f over the last 5 s", ns, nq, peak);
        mixed = 0; peak = 0;
    }
    pthread_mutex_unlock(&mx);
    for (int i = 0; i < frames * 2; i++) { float v = acc[i] * 32767.0f; out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v); }
    free(acc);
}
