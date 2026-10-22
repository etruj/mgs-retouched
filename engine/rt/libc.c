/* libc subset for the wasm module: what the game imports plus what the runtime needs.
 * Darwin semantics where the game can observe them: random()/srandom() are the BSD
 * additive-feedback generator, qsort is BSD's (equal-element order is observable). */
#include "rt.h"
typedef __builtin_va_list va_list;
#define va_start __builtin_va_start
#define va_end __builtin_va_end
#define va_arg __builtin_va_arg

/* ------------------------------------------------------------------ memory */
void *memcpy(void *d, const void *s, size_t n) { u8 *a = d; const u8 *b = s; while (n--) *a++ = *b++; return d; }
void *memmove(void *d, const void *s, size_t n) {
    u8 *a = d; const u8 *b = s;
    if (a < b) while (n--) *a++ = *b++; else { a += n; b += n; while (n--) *--a = *--b; }
    return d;
}
void *memset(void *d, int c, size_t n) { u8 *a = d; while (n--) *a++ = (u8)c; return d; }
int memcmp(const void *x, const void *y, size_t n) {
    const u8 *a = x, *b = y;
    for (; n; n--, a++, b++) if (*a != *b) return *a - *b;
    return 0;
}
size_t strlen(const char *s) { const char *p = s; while (*p) p++; return (size_t)(p - s); }
int strcmp(const char *a, const char *b) { while (*a && *a == *b) a++, b++; return (u8)*a - (u8)*b; }
int strncmp(const char *a, const char *b, size_t n) {
    for (; n; n--, a++, b++) { if (*a != *b) return (u8)*a - (u8)*b; if (!*a) return 0; }
    return 0;
}
char *strcpy(char *d, const char *s) { char *r = d; while ((*d++ = *s++)) continue; return r; }
char *strchr(const char *s, int c) { for (;; s++) { if (*s == (char)c) return (char *)s; if (!*s) return NULL; } }
char *strstr(const char *h, const char *n) {
    size_t k = strlen(n);
    for (; *h; h++) if (!strncmp(h, n, k)) return (char *)h;
    return k ? NULL : (char *)h;
}
char *rt_strdup(const char *s) { size_t n = strlen(s) + 1; char *d = malloc(n); memcpy(d, s, n); return d; }

/* ------------------------------------------------------------------ allocator
 * Boundary-tag allocator over wasm linear memory above __heap_base.
 * chunk: [prev_size][size | INUSE | PINUSE] payload...  (8-byte header, 16-byte aligned payload) */
extern unsigned char __heap_base;
#define INUSE 1u
#define PINUSE 2u
#define HDR 8u
#define MINCHUNK 32u
typedef struct Chunk { u32 prev_size, size; struct Chunk *next, *prev; } Chunk;
#define CSIZE(c) ((c)->size & ~7u)
#define NEXTC(c) ((Chunk *)((u8 *)(c) + CSIZE(c)))
#define PREVC(c) ((Chunk *)((u8 *)(c) - (c)->prev_size))
#define MEM(c) ((void *)((u8 *)(c) + HDR))
#define CHUNK(p) ((Chunk *)((u8 *)(p) - HDR))
#define NBINS 64
static Chunk *bins[NBINS];
static Chunk *top;                 /* wilderness chunk, always free, never in a bin */
static u8 *heap_end;
static u32 heap_bytes_in_use;

/* chunk sizes are multiples of 16: exact bins below 512, then one bin per power of two */
static int bin_index(u32 sz) {
    if (sz < 512) return (int)(sz >> 4);
    int b = 32; u32 s = sz >> 10;
    while (s && b < NBINS - 1) { s >>= 1; b++; }
    return b;
}
static void bin_insert(Chunk *c) {
    int b = bin_index(CSIZE(c));
    c->prev = NULL; c->next = bins[b];
    if (bins[b]) bins[b]->prev = c;
    bins[b] = c;
}
static void bin_remove(Chunk *c) {
    int b = bin_index(CSIZE(c));
    if (c->prev) c->prev->next = c->next; else bins[b] = c->next;
    if (c->next) c->next->prev = c->prev;
}
static void heap_init(void) {
    u8 *base = (u8 *)(((u32)(unsigned long)&__heap_base + 15u) & ~15u);
    heap_end = (u8 *)(unsigned long)(__builtin_wasm_memory_size(0) * 65536u);
    top = (Chunk *)(base + 8);                       /* payload of top is 16-aligned */
    top->prev_size = 0; top->size = (u32)(heap_end - (u8 *)top) & ~15u; top->size |= PINUSE;
}
static int heap_grow(u32 need) {
    u32 pages = (need + 0xFFFFu) >> 16;
    if (pages < 256) pages = 256;                    /* grow in 16 MiB steps */
    if (__builtin_wasm_memory_grow(0, pages) == (unsigned long)-1) return 0;
    heap_end += pages * 65536u; top->size += pages * 65536u;
    return 1;
}
void *malloc(size_t n) {
    if (!top) heap_init();
    u32 need = ((u32)n + HDR + 15u) & ~15u;
    if (need < MINCHUNK) need = MINCHUNK;
    for (int b = bin_index(need); b < NBINS; b++) {
        for (Chunk *c = bins[b]; c; c = c->next) {
            if (CSIZE(c) < need) continue;
            bin_remove(c);
            u32 rest = CSIZE(c) - need;
            if (rest >= MINCHUNK) {
                Chunk *r = (Chunk *)((u8 *)c + need);
                r->size = rest | PINUSE; NEXTC(r)->prev_size = rest;
                c->size = need | (c->size & PINUSE) | INUSE;
                bin_insert(r);
            } else { c->size |= INUSE; NEXTC(c)->size |= PINUSE; }
            heap_bytes_in_use += CSIZE(c);
            return MEM(c);
        }
    }
    while (CSIZE(top) < need + MINCHUNK) if (!heap_grow(need + MINCHUNK)) return NULL;
    Chunk *c = top; u32 rest = CSIZE(top) - need;
    top = (Chunk *)((u8 *)c + need); top->size = rest | PINUSE; top->prev_size = need;
    c->size = need | (c->size & PINUSE) | INUSE;
    heap_bytes_in_use += need;
    return MEM(c);
}
void free(void *p) {
    if (!p) return;
    Chunk *c = CHUNK(p);
    if (!(c->size & INUSE)) rt_fatal("double free of %p", p);
    u32 sz = CSIZE(c); heap_bytes_in_use -= sz;
    c->size &= ~INUSE;
    if (!(c->size & PINUSE)) { Chunk *pc = PREVC(c); bin_remove(pc); sz += CSIZE(pc); c = pc; }
    Chunk *nx = (Chunk *)((u8 *)c + sz);
    if (nx == top) { c->size = (sz + CSIZE(top)) | (c->size & PINUSE); top = c; return; }
    if (!(nx->size & INUSE)) { bin_remove(nx); sz += CSIZE(nx); }
    c->size = sz | (c->size & PINUSE);
    nx = (Chunk *)((u8 *)c + sz); nx->prev_size = sz; nx->size &= ~PINUSE;
    bin_insert(c);
}
void *calloc(size_t a, size_t b) { size_t n = a * b; void *p = malloc(n); if (p) memset(p, 0, n); return p; }
void *realloc(void *p, size_t n) {
    if (!p) return malloc(n);
    u32 have = CSIZE(CHUNK(p)) - HDR;
    if (n <= have) return p;
    void *q = malloc(n); memcpy(q, p, have); free(p); return q;
}
EXPORT(mgst_heap_in_use) u32 mgst_heap_in_use(void) { return heap_bytes_in_use; }
EXPORT(mgst_malloc) u32 mgst_malloc(u32 n) { return A(malloc(n)); }

/* ------------------------------------------------------------------ formatting
 * One formatter for NSLog, printf and +[NSString stringWithFormat:]. Arguments come
 * either as armv6 APCS words (game call sites) or as C varargs (runtime code). */
typedef struct { const u32 *w; int n, i; va_list *ap; } ArgSrc;
static u32 arg32(ArgSrc *s) { if (s->ap) return va_arg(*s->ap, u32); return s->i < s->n ? s->w[s->i++] : 0; }
static u64 arg64(ArgSrc *s) {
    if (s->ap) return va_arg(*s->ap, u64);
    u64 lo = arg32(s), hi = arg32(s); return lo | hi << 32;
}
static double argdbl(ArgSrc *s) { if (s->ap) return va_arg(*s->ap, double); return __builtin_bit_cast(double, arg64(s)); }
int fmt_object_desc(u32 obj, char *buf, int cap);   /* foundation.c: %@ */

typedef struct { char *o; int cap, len; } Out;
static void put(Out *o, char c) { if (o->len + 1 < o->cap) o->o[o->len] = c; o->len++; }
static void puts_n(Out *o, const char *s, int n) { for (int i = 0; i < n; i++) put(o, s[i]); }
static void pad(Out *o, int n, char c) { while (n-- > 0) put(o, c); }

static int fmt_core(char *out, int cap, const char *f, ArgSrc *src) {
    Out o = { out, cap, 0 };
    char tmp[512];
    for (; *f; f++) {
        if (*f != '%') { put(&o, *f); continue; }
        const char *spec_start = f++;
        int left = 0, plus = 0, space = 0, alt = 0, zero = 0, width = -1, prec = -1, lng = 0;
        for (;; f++) {
            if (*f == '-') left = 1; else if (*f == '+') plus = 1; else if (*f == ' ') space = 1;
            else if (*f == '#') alt = 1; else if (*f == '0') zero = 1; else break;
        }
        if (*f == '*') { width = (s32)arg32(src); f++; if (width < 0) { left = 1; width = -width; } }
        else if (*f >= '0' && *f <= '9') { width = 0; while (*f >= '0' && *f <= '9') width = width * 10 + (*f++ - '0'); }
        if (*f == '.') {
            f++; prec = 0;
            if (*f == '*') { prec = (s32)arg32(src); f++; }
            else while (*f >= '0' && *f <= '9') prec = prec * 10 + (*f++ - '0');
        }
        for (;; f++) {
            if (*f == 'l') lng++; else if (*f == 'q' || *f == 'j') lng = 2;
            else if (*f == 'h' || *f == 'z' || *f == 't' || *f == 'L') ; else break;
        }
        char c = *f; int n = 0; const char *s = tmp;
        if (!c) break;
        switch (c) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': case 'p': case 'c': case 'C': {
            u64 v; int neg = 0;
            if (c == 'p') { v = arg32(src); alt = 1; c = 'x'; }
            else if (lng >= 2) v = arg64(src); else v = arg32(src);
            if (c == 'c' || c == 'C') { tmp[0] = (char)v; n = 1; break; }
            if (c == 'd' || c == 'i') {
                s64 sv = lng >= 2 ? (s64)v : (s64)(s32)(u32)v;
                if (sv < 0) { neg = 1; v = (u64)(-sv); } else v = (u64)sv;
            }
            int base = (c == 'x' || c == 'X') ? 16 : c == 'o' ? 8 : 10;
            const char *dig = c == 'X' ? "0123456789ABCDEF" : "0123456789abcdef";
            char rev[32]; int k = 0;
            do { rev[k++] = dig[v % base]; v /= base; } while (v);
            while (k < prec) rev[k++] = '0';
            if (alt && base == 16 && !(k == 1 && rev[0] == '0')) { rev[k++] = c; rev[k++] = '0'; }
            if (neg) rev[k++] = '-'; else if (plus) rev[k++] = '+'; else if (space) rev[k++] = ' ';
            if (zero && !left && prec < 0) {
                int sign = (neg || plus || space) ? 1 : 0;
                while (k < width) { rev[k] = rev[k - 1]; if (sign) { rev[k - 1] = '0'; } else rev[k] = '0'; k++; }
                if (sign) { /* move the sign to the far left */
                    char sg = neg ? '-' : plus ? '+' : ' ';
                    for (int j = 0; j < k; j++) if (rev[j] == sg) { rev[j] = '0'; break; }
                    rev[k - 1] = sg;
                }
            }
            for (int j = 0; j < k; j++) tmp[j] = rev[k - 1 - j];
            n = k; break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            double v = argdbl(src);
            char spec[32]; int sl = (int)(f - spec_start + 1);
            if (sl > 30) sl = 30;
            /* rebuild without length modifiers or '*': the host formats one value */
            int j = 0; spec[j++] = '%';
            if (left) spec[j++] = '-'; if (plus) spec[j++] = '+'; if (space) spec[j++] = ' ';
            if (alt) spec[j++] = '#'; if (zero) spec[j++] = '0';
            j += rt_snprintf(spec + j, 30 - j, width >= 0 ? "%d" : "", width);
            if (prec >= 0) j += rt_snprintf(spec + j, 30 - j, ".%d", prec);
            spec[j++] = c; spec[j] = 0; (void)sl;
            n = host_fmt_double(spec, v, tmp, sizeof tmp);
            width = -1; break;
        }
        case 's': {
            u32 p = arg32(src); s = p ? (const char *)P(p) : "(null)";
            n = (int)strlen(s); if (prec >= 0 && n > prec) n = prec; break;
        }
        case 'S': {
            u32 p = arg32(src); const u16 *u = (const u16 *)P(p);
            for (; u && *u && n < (int)sizeof tmp - 1; u++) tmp[n++] = *u < 128 ? (char)*u : '?';
            break;
        }
        case '@': {
            u32 obj = arg32(src);
            char *big = malloc(4096);
            int k = fmt_object_desc(obj, big, 4096);
            if (prec >= 0 && k > prec) k = prec;
            if (width > k && !left) pad(&o, width - k, ' ');
            puts_n(&o, big, k);
            if (width > k && left) pad(&o, width - k, ' ');
            free(big); continue;
        }
        case '%': tmp[0] = '%'; n = 1; width = -1; break;
        default: puts_n(&o, spec_start, (int)(f - spec_start + 1)); continue;
        }
        if (width > n && !left) pad(&o, width - n, ' ');
        puts_n(&o, s, n);
        if (width > n && left) pad(&o, width - n, ' ');
    }
    if (o.cap) o.o[o.len < o.cap ? o.len : o.cap - 1] = 0;
    return o.len;
}
int fmt_words(char *out, int cap, const char *fmt, const u32 *w, int n, int *used) {
    ArgSrc s = { w, n, 0, NULL };
    int r = fmt_core(out, cap, fmt, &s);
    if (used) *used = s.i;
    return r;
}
int rt_snprintf(char *out, int cap, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    ArgSrc s = { NULL, 0, 0, &ap };
    int r = fmt_core(out, cap, fmt, &s);
    va_end(ap); return r;
}
void rt_log(const char *fmt, ...) {
    char buf[1024]; va_list ap; va_start(ap, fmt);
    ArgSrc s = { NULL, 0, 0, &ap }; fmt_core(buf, sizeof buf, fmt, &s); va_end(ap);
    host_log(buf);
}
void rt_fatal(const char *fmt, ...) {
    char buf[1024]; va_list ap; va_start(ap, fmt);
    ArgSrc s = { NULL, 0, 0, &ap }; fmt_core(buf, sizeof buf, fmt, &s); va_end(ap);
    host_abort(buf); __builtin_trap();
}

/* ------------------------------------------------------------------ game-facing C imports */
void *_malloc(size_t n) { return malloc(n); }
void *_calloc(size_t a, size_t b) { return calloc(a, b); }
void _free(void *p) { free(p); }
void *_memcpy(void *d, void *s, size_t n) { return memcpy(d, s, n); }
void *_memset(void *d, int c, size_t n) { return memset(d, c, n); }
size_t _strlen(char *s) { return strlen(s); }
char *_strcpy(char *d, char *s) { return strcpy(d, s); }
char *_strcat(char *d, char *s) { strcpy(d + strlen(d), s); return d; }
char *_strncat(char *d, char *s, size_t n) {
    char *e = d + strlen(d);
    while (n-- && *s) *e++ = *s++;
    *e = 0; return d;
}
char *_strchr(char *s, int c) { return strchr(s, c); }
int _strncmp(char *a, char *b, size_t n) { return strncmp(a, b, n); }
void *operator_new__5a8c0(u32 n) { return malloc(n ? n : 1); }
void *operator_new__5a8cc(u32 n) { return malloc(n ? n : 1); }
void operator_delete__5a8a8(void *p) { free(p); }
void operator_delete__5a8b4(void *p) { free(p); }
/* SjLj frames (docs/V0_AUDIT.md): nothing throws through them; registration is a no-op */
void __Unwind_SjLj_Register(void *fc) { (void)fc; }
void __Unwind_SjLj_Unregister(void *fc) { (void)fc; }

void _NSLog__w(void *format, const u32 *w, int n);   /* foundation.c */
int _printf__w(char *fmt, const u32 *w, int n) {
    char buf[2048]; int k = fmt_words(buf, sizeof buf, fmt, w, n, NULL);
    host_log(buf); return k;
}
int _puts(char *s) { host_log(s); return 1; }

/* ---- math ---- */
float _sinf(float x) { return host_sinf(x); }
float _cosf(float x) { return host_cosf(x); }
float _sqrtf(float x) { return __builtin_sqrtf(x); }      /* IEEE: exact on every platform */
float _atan2f(float y, float x) { return host_atan2f(y, x); }
double _atan2(double y, double x) { return host_atan2(y, x); }

/* ---- Darwin random(): BSD additive feedback, TYPE_3 (x^31 + x^3 + 1) ---- */
static u32 rnd_state[31];
static int rnd_f = 3, rnd_r = 0;
static u32 good_rand(s32 x) {
    if (x == 0) x = 123459876;
    s32 hi = x / 127773, lo = x % 127773;
    x = 16807 * lo - 2836 * hi;
    if (x < 0) x += 0x7fffffff;
    return (u32)x;
}
static u32 bsd_random(void) {
    rnd_state[rnd_f] += rnd_state[rnd_r];
    u32 i = (rnd_state[rnd_f] >> 1) & 0x7fffffff;
    if (++rnd_f >= 31) rnd_f = 0;
    if (++rnd_r >= 31) rnd_r = 0;
    return i;
}
void _srandom(u32 seed) {
    rnd_state[0] = seed;
    for (int i = 1; i < 31; i++) rnd_state[i] = good_rand((s32)rnd_state[i - 1]);
    rnd_f = 3; rnd_r = 0;
    for (int i = 0; i < 310; i++) bsd_random();
}
static int rnd_seeded;
s32 _random(void) { if (!rnd_seeded) { rnd_seeded = 1; _srandom(1); } return (s32)bsd_random(); }
void rt_srandom_mark(void) { rnd_seeded = 1; }
EXPORT(mgst_test_random) u32 mgst_test_random(u32 seed, u32 n) { _srandom(seed); rnd_seeded = 1; u32 v = 0; while (n--) v = bsd_random(); return v; }

/* ---- time ---- */
unsigned long long _mach_absolute_time(void) { return (unsigned long long)(host_time() * 1e9); }
double _CFAbsoluteTimeGetCurrent(void) { return host_wall_time(); }

/* ---- BSD qsort (FreeBSD qsort.c as in Apple Libc): comparator is an original code address ---- */
static u32 qs_cmp;
static int qcmp(char *a, char *b) { u32 w[2] = { A(a), A(b) }; return (s32)(u32)invoke_orig(qs_cmp, w); }
static void swapfunc(char *a, char *b, int n, int swaptype) {
    if (swaptype <= 1) { long i = n / (int)sizeof(long); long *pi = (long *)a, *pj = (long *)b; do { long t = *pi; *pi++ = *pj; *pj++ = t; } while (--i > 0); }
    else { long i = n; char *pi = a, *pj = b; do { char t = *pi; *pi++ = *pj; *pj++ = t; } while (--i > 0); }
}
#define SWAPINIT(a, es) swaptype = ((A(a)) % sizeof(long) || (es) % sizeof(long)) ? 2 : (es) == sizeof(long) ? 0 : 1;
#define SWAP(a, b) do { if (swaptype == 0) { long t = *(long *)(a); *(long *)(a) = *(long *)(b); *(long *)(b) = t; } else swapfunc(a, b, (int)es, swaptype); } while (0)
#define VECSWAP(a, b, n) do { if ((n) > 0) swapfunc(a, b, (int)(n), swaptype); } while (0)
#define MIN(a, b) ((a) < (b) ? (a) : (b))
static char *med3(char *a, char *b, char *c) {
    return qcmp(a, b) < 0 ? (qcmp(b, c) < 0 ? b : (qcmp(a, c) < 0 ? c : a))
                          : (qcmp(b, c) > 0 ? b : (qcmp(a, c) < 0 ? a : c));
}
static void bsd_qsort(void *a, size_t n, size_t es) {
    char *pa, *pb, *pc, *pd, *pl, *pm, *pn; size_t d, r; int cr, swaptype, swap_cnt;
loop:
    SWAPINIT(a, es);
    swap_cnt = 0;
    if (n < 7) {
        for (pm = (char *)a + es; pm < (char *)a + n * es; pm += es)
            for (pl = pm; pl > (char *)a && qcmp(pl - es, pl) > 0; pl -= es) SWAP(pl, pl - es);
        return;
    }
    pm = (char *)a + (n / 2) * es;
    if (n > 7) {
        pl = a; pn = (char *)a + (n - 1) * es;
        if (n > 40) {
            d = (n / 8) * es;
            pl = med3(pl, pl + d, pl + 2 * d); pm = med3(pm - d, pm, pm + d); pn = med3(pn - 2 * d, pn - d, pn);
        }
        pm = med3(pl, pm, pn);
    }
    SWAP(a, pm);
    pa = pb = (char *)a + es;
    pc = pd = (char *)a + (n - 1) * es;
    for (;;) {
        while (pb <= pc && (cr = qcmp(pb, a)) <= 0) { if (cr == 0) { swap_cnt = 1; SWAP(pa, pb); pa += es; } pb += es; }
        while (pb <= pc && (cr = qcmp(pc, a)) >= 0) { if (cr == 0) { swap_cnt = 1; SWAP(pc, pd); pd -= es; } pc -= es; }
        if (pb > pc) break;
        SWAP(pb, pc); swap_cnt = 1; pb += es; pc -= es;
    }
    if (swap_cnt == 0) {
        for (pm = (char *)a + es; pm < (char *)a + n * es; pm += es)
            for (pl = pm; pl > (char *)a && qcmp(pl - es, pl) > 0; pl -= es) SWAP(pl, pl - es);
        return;
    }
    pn = (char *)a + n * es;
    r = MIN((size_t)(pa - (char *)a), (size_t)(pb - pa)); VECSWAP(a, pb - r, r);
    r = MIN((size_t)(pd - pc), (size_t)(pn - pd) - es); VECSWAP(pb, pn - r, r);
    if ((r = (size_t)(pb - pa)) > es) bsd_qsort(a, r / es, es);
    if ((r = (size_t)(pd - pc)) > es) { a = pn - r; n = r / es; goto loop; }
}
void rt_qsort_orig(void *base, size_t n, size_t es, u32 cmp_addr) {
    u32 saved = qs_cmp; qs_cmp = cmp_addr; bsd_qsort(base, n, es); qs_cmp = saved;
}
void _qsort(void *base, size_t n, size_t es, int *cmp) { rt_qsort_orig(base, n, es, A(cmp)); }

/* ---- stdio over host files ---- */
typedef struct { u32 magic; int h; } RtFile;
#define FILE_MAGIC 0x46494C45u
const char *rt_cwd(void);
void *_fopen(char *path, char *mode) {
    char full[512];
    if (path[0] != '/') { rt_snprintf(full, sizeof full, "%s/%s", rt_cwd(), path); path = full; }   /* relative to -changeCurrentDirectoryPath: */
    int h = host_fopen(path, mode);
    if (h < 0) return NULL;
    RtFile *f = malloc(sizeof *f); f->magic = FILE_MAGIC; f->h = h; return f;
}
/* Darwin's stdio returns EOF for a NULL stream (the game relies on it on first launch:
 * GSaveCtrl applicationDataFromFile closes the result of a failed fopen) */
static RtFile *rtf(void *p) {
    RtFile *f = p;
    if (!f) return NULL;
    if (f->magic != FILE_MAGIC) rt_fatal("bad FILE* %p", p);
    return f;
}
int _fclose(void *p) { RtFile *f = rtf(p); if (!f) return -1; int r = host_fclose(f->h); f->magic = 0; free(f); return r; }
size_t _fread(void *d, size_t sz, size_t cnt, void *p) { RtFile *f = rtf(p); if (!f || !sz) return 0; return (size_t)host_fread(f->h, d, (int)(sz * cnt)) / sz; }
size_t _fwrite(void *s, size_t sz, size_t cnt, void *p) { RtFile *f = rtf(p); if (!f || !sz) return 0; return (size_t)host_fwrite(f->h, s, (int)(sz * cnt)) / sz; }
int _fseek(void *p, long off, int wh) { RtFile *f = rtf(p); return f ? host_fseek(f->h, (int)off, wh) : -1; }
long _ftell(void *p) { RtFile *f = rtf(p); return f ? host_ftell(f->h) : -1; }

/* ---- auto-stub reporting (tools/mk_autostubs.py) ---- */
void rt_stub_hit(const char *name) {
    static const char *seen[256]; static int n;
    for (int i = 0; i < n; i++) if (seen[i] == name) return;
    if (n < 256) seen[n++] = name;
    rt_log("UNIMPLEMENTED C import %s", name);
}
double fmod(double x, double y) { if (y == 0) return 0; double q = x / y; q = q < 0 ? __builtin_ceil(q) : __builtin_floor(q); return x - q * y; }
