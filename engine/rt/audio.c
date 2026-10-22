/* Audio: OpenAL (sound effects) is mixed by the host; AudioFile (CAF) and AudioQueue
 * (streamed music) run here, because the game reads their structs directly and its
 * own buffer callbacks refill the queue. IMA4 is decoded here with Apple's ima4 packet
 * layout; the decoded PCM goes to a host stream. */
#include "rt.h"

/* ================================================================== OpenAL -> host mixer */
#define AL0(n) HOSTFN(host_##n) void host_##n
AL0(alGenSources)(s32, u32);   void _alGenSources(int n, u32 *p) { host_alGenSources(n, A(p)); }
AL0(alGenBuffers)(s32, u32);   void _alGenBuffers(int n, u32 *p) { host_alGenBuffers(n, A(p)); }
AL0(alDeleteSources)(s32, u32); void _alDeleteSources(int n, u32 *p) { host_alDeleteSources(n, A(p)); }
AL0(alDeleteBuffers)(s32, u32); void _alDeleteBuffers(int n, u32 *p) { host_alDeleteBuffers(n, A(p)); }
AL0(alSourcei)(u32, s32, s32); void _alSourcei(u32 s, int p, int v) { host_alSourcei(s, p, v); }
AL0(alSourcef)(u32, s32, float); void _alSourcef(u32 s, int p, float v) { host_alSourcef(s, p, v); }
AL0(alSource3f)(u32, s32, float, float, float); void _alSource3f(u32 s, int p, float a, float b, float c) { host_alSource3f(s, p, a, b, c); }
AL0(alListener3f)(s32, float, float, float); void _alListener3f(int p, float a, float b, float c) { host_alListener3f(p, a, b, c); }
AL0(alListenerfv)(s32, u32);   void _alListenerfv(int p, float *v) { host_alListenerfv(p, A(v)); }
AL0(alSourcePlay)(u32);        void _alSourcePlay(u32 s) { host_alSourcePlay(s); }
AL0(alSourceStop)(u32);        void _alSourceStop(u32 s) { host_alSourceStop(s); }
AL0(alSourcePause)(u32);       void _alSourcePause(u32 s) { host_alSourcePause(s); }
AL0(alGetSourcei)(u32, s32, u32); void _alGetSourcei(u32 s, int p, int *v) { host_alGetSourcei(s, p, A(v)); }
AL0(alDistanceModel)(s32);     void _alDistanceModel(int v) { host_alDistanceModel(v); }
AL0(alDopplerFactor)(float);   void _alDopplerFactor(float v) { host_alDopplerFactor(v); }
AL0(alBufferDataStatic)(u32, s32, u32, s32, s32);
HOSTFN(host_alGetError) s32 host_alGetError(void);
int _alGetError(void) { return host_alGetError(); }
/* alBufferDataStatic: the host mixer reads the samples straight from wasm memory */
static void al_buffer_data_static(int buffer, int format, void *data, int size, int freq) { host_alBufferDataStatic((u32)buffer, format, A(data), size, freq); }
void *_alcOpenDevice(char *name) { (void)name; static u32 dev = 0x44455631; return &dev; }
void *_alcCreateContext(void *dev, int *attrs) { (void)dev; (void)attrs; static u32 ctx = 0x43545831; return &ctx; }
char _alcMakeContextCurrent(void *c) { (void)c; return 1; }
void _alcDestroyContext(void *c) { (void)c; }
char _alcCloseDevice(void *d) { (void)d; return 1; }
/* runtime functions the game calls through a pointer live at reserved "code" addresses: the
 * call goes through rt_icall(), which picks the adapter matching the call site's pointer type
 * (the decompiler may type such a pointer differently from the function) */
#define RTFN_ALBUFFERDATASTATIC 0x5a614u
unsigned long long rt_runtime_call(unsigned addr, const unsigned *w) {
    if (addr == RTFN_ALBUFFERDATASTATIC) { al_buffer_data_static((int)w[0], (int)w[1], P(w[2]), (int)w[3], (int)w[4]); return 0; }
    rt_fatal("runtime function 0x%x unknown", addr);
}
void *_alcGetProcAddress(void *dev, char *name) {
    (void)dev;
    if (!strcmp(name, "alBufferDataStatic")) return P(RTFN_ALBUFFERDATASTATIC);
    rt_log("alcGetProcAddress(%s): not available", name); return NULL;
}

/* ================================================================== AudioSession / AudioServices */
int _AudioSessionInitialize(void *rl, void *mode, void *listener, void *data) { (void)rl; (void)mode; (void)listener; (void)data; return 0; }
int _AudioSessionSetActive(char a) { (void)a; return 0; }
int _AudioSessionSetProperty(u32 id, u32 size, void *data) { (void)id; (void)size; (void)data; return 0; }
/* kSystemSoundID_Vibrate: the game sends it when its own VIBRATION option is on (GSoundCtrl::VibrationSet).
 * About 0.4 s on the iPhone. */
void _AudioServicesPlaySystemSound(u32 id) { if (id == 0xfffu) host_vibrate(400); }
void *_CFRunLoopGetCurrent(void) { static u32 rl; return &rl; }

/* ================================================================== CFURL (file paths for AudioFileOpenURL) */
typedef struct { u32 magic; char path[256]; } Url;
void *_CFURLCreateFromFileSystemRepresentation(void *alloc, char *buf, int len, char isdir) {
    (void)alloc; (void)isdir;
    Url *u = calloc(1, sizeof *u); u->magic = 0x4C525543; if (len > 255) len = 255; memcpy(u->path, buf, len); return u;
}

/* ================================================================== AudioFile (CAF) */
typedef struct {
    double rate; u32 fmt, flags, bpp, fpp, bpf, ch, bits, reserved;   /* AudioStreamBasicDescription, 40 bytes */
} ASBD;
typedef struct { u32 magic; int h; ASBD d; s64 data_off, data_size, packets; } AFile;
static u32 be32(const u8 *p) { return (u32)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static u64 be64(const u8 *p) { return (u64)be32(p) << 32 | be32(p + 4); }
int _AudioFileOpenURL(void *url, char perm, u32 hint, void **out) {
    (void)perm; (void)hint;
    Url *u = url; int h = host_fopen(u->path, "rb");
    if (h < 0) { rt_log("AudioFileOpenURL: %s not found", u->path); return -43; }
    u8 hd[8]; host_fread(h, hd, 8);
    if (memcmp(hd, "caff", 4)) { host_fclose(h); rt_log("AudioFileOpenURL: %s is not CAF", u->path); return 0x7479703f; }
    AFile *f = calloc(1, sizeof *f); f->magic = 0x41464C45; f->h = h;
    s64 pos = 8;
    for (;;) {
        u8 ch[12]; if (host_fread(h, ch, 12) != 12) break;
        s64 size = (s64)be64(ch + 4); pos += 12;
        if (!memcmp(ch, "desc", 4)) {
            u8 d[32]; host_fread(h, d, 32);
            f->d.rate = __builtin_bit_cast(double, be64(d)); f->d.fmt = be32(d + 8); f->d.flags = be32(d + 12);
            f->d.bpp = be32(d + 16); f->d.fpp = be32(d + 20); f->d.ch = be32(d + 24); f->d.bits = be32(d + 28);
            if (f->d.fmt == 0x6C70636D) {                 /* 'lpcm': CAF flags bit 1 = little endian */
                f->d.bpf = f->d.bpp;
                f->d.flags = (f->d.flags & 1 ? 1 : 0x4 /* signed int */) | 0x8 /* packed */ | (f->d.flags & 2 ? 0 : 0x2 /* big endian */);
            } else f->d.bpf = 0;
            host_fseek(h, (int)(pos + size), 0);
        } else if (!memcmp(ch, "data", 4)) {
            f->data_off = pos + 4;                        /* skip mEditCount */
            f->data_size = size < 0 ? host_fsize(u->path) - f->data_off : size - 4;
            break;
        } else host_fseek(h, (int)(pos + size), 0);
        pos += size;
    }
    f->packets = f->d.bpp ? f->data_size / f->d.bpp : 0;
    extern int rt_trace_msgs; if (rt_trace_msgs) rt_log("AudioFileOpenURL %s: fmt %x %d Hz %u ch, %lld packets", u->path, f->d.fmt, (int)f->d.rate, f->d.ch, (long long)f->packets);
    *out = f; return 0;
}
static AFile *AF_(void *p) { AFile *f = p; if (!f || f->magic != 0x41464C45) rt_fatal("bad AudioFileID %p", p); return f; }
int _AudioFileClose(void *p) { AFile *f = AF_(p); host_fclose(f->h); f->magic = 0; free(f); return 0; }
#define FOURCC(a, b, c, d) ((u32)(a) << 24 | (u32)(b) << 16 | (u32)(c) << 8 | (u32)(d))
/* AudioFile properties the game asks for. Anything a CAF here does not carry (the bundle has
 * no 'chan' or 'kuki' chunks) is kAudioFileUnsupportedPropertyError with size 0, as on iOS. */
#define kAudioFileUnsupportedPropertyError 0x7074793f   /* 'pty?' */
static u32 prop_size(AFile *f, u32 id) {
    (void)f;
    switch (id) {
    case FOURCC('d','f','m','t'): return 40;
    case FOURCC('p','c','n','t'): case FOURCC('b','c','n','t'): return 8;
    case FOURCC('p','s','z','e'): case FOURCC('p','k','u','b'): return 4;
    default: return 0;
    }
}
int _AudioFileGetPropertyInfo(void *p, u32 id, u32 *size, u32 *wr) {
    AFile *f = AF_(p); u32 n = prop_size(f, id);
    if (size) *size = n; if (wr) *wr = 0;
    return n ? 0 : kAudioFileUnsupportedPropertyError;
}
int _AudioFileGetProperty(void *p, u32 id, u32 *size, void *out) {
    AFile *f = AF_(p);
    switch (id) {
    case FOURCC('d','f','m','t'): memcpy(out, &f->d, 40); *size = 40; return 0;
    case FOURCC('p','c','n','t'): *(s64 *)out = f->packets; *size = 8; return 0;
    case FOURCC('b','c','n','t'): *(s64 *)out = f->data_size; *size = 8; return 0;
    case FOURCC('p','s','z','e'): case FOURCC('p','k','u','b'): *(u32 *)out = f->d.bpp; *size = 4; return 0;
    default:
        rt_log("AudioFileGetProperty: unsupported '%c%c%c%c'", id >> 24, id >> 16 & 255, id >> 8 & 255, id & 255);
        *size = 0; return kAudioFileUnsupportedPropertyError;
    }
}
int _AudioFileReadBytes(void *p, char cache, s64 start, u32 *n, void *out) {
    AFile *f = AF_(p); (void)cache;
    if (start >= f->data_size) { *n = 0; return -39; }
    if (start + *n > f->data_size) *n = (u32)(f->data_size - start);
    host_fseek(f->h, (int)(f->data_off + start), 0);
    *n = (u32)host_fread(f->h, out, (int)*n); return 0;
}
int _AudioFileReadPackets(void *p, char cache, u32 *nbytes, void *descs, s64 start, u32 *npk, void *out) {
    AFile *f = AF_(p); (void)cache; (void)descs;
    if (!f->d.bpp) rt_fatal("AudioFileReadPackets: variable packet size not supported");
    s64 avail = f->packets - start; if (avail < 0) avail = 0;
    if (*npk > avail) *npk = (u32)avail;
    u32 n = *npk * f->d.bpp;
    host_fseek(f->h, (int)(f->data_off + start * f->d.bpp), 0);
    n = (u32)host_fread(f->h, out, (int)n);
    if (nbytes) *nbytes = n;
    *npk = n / f->d.bpp; return 0;
}

/* ================================================================== AudioQueue */
typedef struct { u32 cap, data, size, user, pdcap, pds, pdcount; } AQBuf;   /* AudioQueueBuffer, as the game reads it */
#define AQ_MAXBUF 16
typedef struct { int pred, idx; } ImaState;
typedef struct { u32 magic; ASBD d; u32 cb, user; int stream, running, disposed; u32 bufs[AQ_MAXBUF]; int nbufs; float volume; ImaState ima[2]; } AQ;
static AQ *queues[16]; static int nqueues;
static AQ *Q(void *p) { AQ *q = p; if (!q || q->magic != 0x41515545) rt_fatal("bad AudioQueueRef %p", p); return q; }
int _AudioQueueNewOutput(void *fmt, void *cb, void *user, void *rl, void *mode, u32 flags, void **out) {
    (void)rl; (void)mode; (void)flags;
    AQ *q = calloc(1, sizeof *q); q->magic = 0x41515545; memcpy(&q->d, fmt, 40);
    q->cb = A(cb); q->user = A(user); q->volume = 1;
    q->stream = host_pcm_open((int)q->d.rate, (int)q->d.ch);
    extern int rt_trace_msgs; if (rt_trace_msgs) rt_log("AudioQueueNewOutput: '%c%c%c%c' %d Hz %u ch -> stream %d", q->d.fmt >> 24, q->d.fmt >> 16 & 255, q->d.fmt >> 8 & 255, q->d.fmt & 255, (int)q->d.rate, q->d.ch, q->stream);
    if (nqueues < 16) queues[nqueues++] = q;
    *out = q; return 0;
}
int _AudioQueueAllocateBuffer(void *p, u32 size, void **out) {
    AQ *q = Q(p); AQBuf *b = calloc(1, sizeof *b);
    b->cap = size; b->data = A(calloc(1, size));
    if (q->nbufs < AQ_MAXBUF) q->bufs[q->nbufs++] = A(b);
    *out = b; return 0;
}
int _AudioQueueFreeBuffer(void *p, void *bp) {
    AQ *q = Q(p); AQBuf *b = bp;
    for (int i = 0; i < q->nbufs; i++) if (q->bufs[i] == A(b)) { q->bufs[i] = q->bufs[--q->nbufs]; break; }
    free(P(b->data)); free(b); return 0;
}
/* Apple IMA4: per channel, 34-byte packets of 64 samples: big-endian header (predictor in
 * the top 9 bits, step index in the low 7), then 32 bytes of nibbles, low nibble first */
static const s16 ima_steps[89] = { 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45, 50, 55, 60, 66,
    73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307, 337, 371, 408, 449, 494, 544, 598, 658, 724,
    796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066, 2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871,
    5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899, 15289, 16818, 18500, 20350, 22385, 24623, 27086,
    29794, 32767 };
static const s8 ima_index[16] = { -1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8 };
/* The header carries only the top 9 bits of the predictor. A QuickTime decoder keeps the
 * full-precision predictor from the previous packet when the step index matches and the
 * two agree within 0x7f (the header is then just the truncated running value); this is the
 * behaviour FFmpeg's adpcm_ima_qt reproduces. Bit-exactness against Apple's decoder is a
 * D2 check (PORT_HANDOFF/docs/PLATFORM_API.md). */
static void ima4_packet(const u8 *p, s16 *out, int stride, ImaState *st) {
    int hdr_pred = (s16)(p[0] << 8 | (p[1] & 0x80)), hdr_idx = p[1] & 0x7F;
    if (hdr_idx > 88) hdr_idx = 88;
    int d = hdr_pred - st->pred; if (d < 0) d = -d;
    if (st->idx != hdr_idx || d > 0x7f) { st->pred = hdr_pred; st->idx = hdr_idx; }
    int pred = st->pred, idx = st->idx;
    for (int i = 0; i < 64; i++) {
        int nib = (p[2 + i / 2] >> ((i & 1) * 4)) & 15, step = ima_steps[idx];
        int diff = step >> 3;
        if (nib & 4) diff += step; if (nib & 2) diff += step >> 1; if (nib & 1) diff += step >> 2;
        if (nib & 8) pred -= diff; else pred += diff;
        if (pred > 32767) pred = 32767; if (pred < -32768) pred = -32768;
        idx += ima_index[nib]; if (idx < 0) idx = 0; if (idx > 88) idx = 88;
        out[i * stride] = (s16)pred;
    }
    st->pred = pred; st->idx = idx;
}
static void aq_submit(AQ *q, AQBuf *b, int tag) {
    const u8 *src = P(b->data); u32 n = b->size;
    if (q->d.fmt == FOURCC('i','m','a','4')) {
        int ch = (int)q->d.ch, pk = (int)(n / (34u * ch));
        s16 *pcm = malloc(pk * 64 * ch * 2 + 2);
        for (int k = 0; k < pk; k++) for (int c = 0; c < ch; c++) ima4_packet(src + (k * ch + c) * 34, pcm + k * 64 * ch + c, ch, &q->ima[c & 1]);
        host_pcm_write(q->stream, pcm, pk * 64 * ch * 2, tag); free(pcm);
    } else if (q->d.fmt == FOURCC('l','p','c','m') && q->d.bits == 16) {
        if (q->d.flags & 0x2) {   /* big-endian */
            s16 *pcm = malloc(n + 2); for (u32 i = 0; i + 1 < n; i += 2) pcm[i / 2] = (s16)(src[i] << 8 | src[i + 1]);
            host_pcm_write(q->stream, pcm, (int)n, tag); free(pcm);
        } else host_pcm_write(q->stream, src, (int)n, tag);
    } else { RT_ONCE(rt_log("AudioQueue: unsupported format '%c%c%c%c' %u bits", q->d.fmt >> 24, q->d.fmt >> 16 & 255, q->d.fmt >> 8 & 255, q->d.fmt & 255, q->d.bits)); host_pcm_write(q->stream, src, 0, tag); }
}
int _AudioQueueEnqueueBuffer(void *p, void *bp, u32 npd, void *pd) {
    AQ *q = Q(p); AQBuf *b = bp; (void)npd; (void)pd;
    int tag = -1; for (int i = 0; i < q->nbufs; i++) if (q->bufs[i] == A(b)) tag = i;
    if (tag < 0) rt_fatal("AudioQueueEnqueueBuffer: buffer not from this queue");
    aq_submit(q, b, tag); return 0;
}
int _AudioQueueStart(void *p, void *t) { AQ *q = Q(p); (void)t; q->running = 1; host_pcm_control(q->stream, 0, 0); return 0; }
int _AudioQueuePause(void *p) { AQ *q = Q(p); host_pcm_control(q->stream, 1, 0); return 0; }
int _AudioQueueStop(void *p, char immediate) { AQ *q = Q(p); (void)immediate; q->running = 0; memset(q->ima, 0, sizeof q->ima); host_pcm_control(q->stream, 2, 0); return 0; }
int _AudioQueuePrime(void *p, u32 n, u32 *out) { (void)Q(p); (void)n; if (out) *out = 0; return 0; }
int _AudioQueueDispose(void *p, char immediate) {
    AQ *q = Q(p); (void)immediate;
    host_pcm_control(q->stream, 4, 0);
    while (q->nbufs) _AudioQueueFreeBuffer(q, P(q->bufs[0]));
    for (int i = 0; i < nqueues; i++) if (queues[i] == q) { queues[i] = queues[--nqueues]; break; }
    q->magic = 0; free(q); return 0;
}
int _AudioQueueSetParameter(void *p, u32 id, float v) { AQ *q = Q(p); if (id == 1) { q->volume = v; host_pcm_control(q->stream, 3, v); } return 0; }
int _AudioQueueGetProperty(void *p, u32 id, void *out, u32 *size) {
    AQ *q = Q(p);
    if (id == FOURCC('a','q','r','n')) { *(u32 *)out = (u32)q->running; if (size) *size = 4; return 0; }
    rt_log("AudioQueueGetProperty: unsupported"); return -50;
}
int _AudioQueueSetProperty(void *p, u32 id, void *d, u32 size) { (void)Q(p); (void)id; (void)d; (void)size; return 0; }
int _AudioQueueRemovePropertyListener(void *p, u32 id, void *proc, void *user) { (void)Q(p); (void)id; (void)proc; (void)user; return 0; }
/* buffers the host has finished playing go back to the game's callback: (userData, queue, buffer) */
void aq_service(void) {
    for (int i = 0; i < nqueues; i++) {
        AQ *q = queues[i]; int tag;
        while (q && (tag = host_pcm_done(q->stream)) >= 0) {
            if (tag >= q->nbufs) continue;
            u32 w[3] = { q->user, A(q), q->bufs[tag] };
            pool_push(); invoke_orig(q->cb, w); pool_pop();
            if (i >= nqueues || queues[i] != q) break;   /* disposed inside the callback */
        }
    }
}
