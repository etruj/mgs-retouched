/* Objective-C runtime for the port. Game classes stay where they are in the image
 * (class_t at their original addresses, method lists pointing at original code
 * addresses, which invoke_orig() maps to the recompiled functions). Framework classes
 * are built here with the same class_t layout, so isa/superclass walks are uniform.
 * Selectors are the game's own selector-string addresses, canonicalised by content. */
#include "rt.h"
#include "image_meta.h"

/* ------------------------------------------------------------------ hash maps */
typedef struct { u32 k1, k2; void *v; } Ent;
typedef struct { Ent *e; u32 cap, n; } Map;
static u32 hmix(u32 a, u32 b) { u32 h = a * 0x9E3779B1u ^ (b + 0x7F4A7C15u) * 0x85EBCA77u; h ^= h >> 15; return h * 0xC2B2AE3Du; }
static void *map_get(Map *m, u32 k1, u32 k2) {
    if (!m->cap) return NULL;
    for (u32 i = hmix(k1, k2) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        Ent *e = &m->e[i];
        if (!e->k1 && !e->k2 && !e->v) return NULL;
        if (e->k1 == k1 && e->k2 == k2) return e->v;
    }
}
static void map_put(Map *m, u32 k1, u32 k2, void *v) {
    if ((m->n + 1) * 4 >= m->cap * 3) {
        Map n = { calloc(m->cap ? m->cap * 2 : 64, sizeof(Ent)), m->cap ? m->cap * 2 : 64, 0 };
        for (u32 i = 0; i < m->cap; i++) if (m->e[i].v) map_put(&n, m->e[i].k1, m->e[i].k2, m->e[i].v);
        free(m->e); *m = n;
    }
    for (u32 i = hmix(k1, k2) & (m->cap - 1);; i = (i + 1) & (m->cap - 1)) {
        Ent *e = &m->e[i];
        if (e->k1 == k1 && e->k2 == k2 && e->v) { e->v = v; return; }
        if (!e->v) { e->k1 = k1; e->k2 = k2; e->v = v; m->n++; return; }
    }
}
static u32 strhash(const char *s) { u32 h = 2166136261u; while (*s) h = (h ^ (u8)*s++) * 16777619u; return h; }

/* ------------------------------------------------------------------ selectors */
static Map sel_by_addr;          /* any selector-string address -> canonical */
static Map sel_by_hash;          /* (hash, chain) -> canonical address */
u32 sel_canon(u32 addr) {
    if (!addr) return 0;
    u32 *c = map_get(&sel_by_addr, addr, 0);
    if (c) return (u32)(unsigned long)c;
    const char *name = P(addr);
    u32 h = strhash(name), canon = 0;
    for (u32 k = 1;; k++) {
        void *v = map_get(&sel_by_hash, h, k);
        if (!v) { map_put(&sel_by_hash, h, k, P(addr)); canon = addr; break; }
        if (!strcmp(P(A(v)), name)) { canon = A(v); break; }
    }
    map_put(&sel_by_addr, addr, 0, P(canon));
    return canon;
}
u32 sel_get(const char *name) { return sel_canon(A(name)); }
const char *sel_name(u32 sel) { return sel ? (const char *)P(sel) : "(null)"; }

/* ------------------------------------------------------------------ classes */
typedef struct Method { u32 sel, orig; ShimFn fn; } Method;
typedef struct RtCls { u32 addr; int meta; const char *name; u32 isize; Map methods; u32 partner; int initialized; } RtCls;
static Map classes;              /* class address -> RtCls */
static Map class_by_name;        /* name hash -> class address (non-meta) */
static Map cache;                /* (class, sel) -> Method */
static RtCls *rtcls(u32 addr) { return map_get(&classes, addr, 0); }

static RtCls *cls_register(u32 addr, int meta, const char *name, u32 isize) {
    RtCls *c = calloc(1, sizeof *c);
    c->addr = addr; c->meta = meta; c->name = name; c->isize = isize;
    map_put(&classes, addr, 0, c);
    if (!meta) map_put(&class_by_name, strhash(name), 0x1000000u | (strhash(name) & 0xffff), P(addr));
    return c;
}
u32 class_named(const char *name) {
    void *v = map_get(&class_by_name, strhash(name), 0x1000000u | (strhash(name) & 0xffff));
    return v ? A(v) : 0;
}
u32 class_super(u32 cls) { return cls ? RD32(cls + 4) : 0; }
u32 obj_class(u32 obj) { return obj ? RD32(obj) : 0; }
int class_is_meta(u32 cls) { RtCls *c = rtcls(cls); return c ? c->meta : 0; }
const char *class_name(u32 cls) { RtCls *c = rtcls(cls); return c ? c->name : "?"; }
u32 class_isize(u32 cls) { RtCls *c = rtcls(cls); return c ? c->isize : 0; }

static void add_method(RtCls *c, u32 sel, u32 orig, ShimFn fn) {
    Method *m = malloc(sizeof *m); m->sel = sel; m->orig = orig; m->fn = fn;
    map_put(&c->methods, sel, 0, m);
    cache.n = 0; memset(cache.e, 0, cache.cap * sizeof(Ent));     /* methods changed: drop cache */
}

/* framework class: class_t {isa, superclass, cache, vtable, data} + metaclass, in wasm memory */
u32 rt_class_new(const char *name, const char *super_name, u32 isize) {
    u32 sup = super_name ? class_named(super_name) : 0;
    if (super_name && !sup) rt_fatal("class %s: superclass %s not defined yet", name, super_name);
    u32 cls = A(calloc(1, 20)), meta = A(calloc(1, 20));
    u32 root_meta = sup ? RD32(sup) : meta;
    while (sup && class_super(root_meta)) {                  /* root metaclass: walk to the top */
        u32 s = class_super(root_meta);
        if (class_is_meta(s)) root_meta = s; else break;
    }
    WR32(cls, meta); WR32(cls + 4, sup);
    WR32(meta, sup ? root_meta : meta); WR32(meta + 4, sup ? RD32(sup) : cls);
    if (!isize) isize = sup ? class_isize(sup) : 4;
    RtCls *c = cls_register(cls, 0, name, isize), *mc = cls_register(meta, 1, name, 0);
    c->partner = meta; mc->partner = cls;
    return cls;
}
void rt_method(u32 cls, const char *sel, ShimFn fn) { add_method(rtcls(cls), sel_get(sel), 0, fn); }
void rt_cmethod(u32 cls, const char *sel, ShimFn fn) { add_method(rtcls(RD32(cls)), sel_get(sel), 0, fn); }

static Method *lookup(u32 cls, u32 sel) {
    Method *m = map_get(&cache, cls, sel);
    if (m) return m;
    for (u32 c = cls; c; c = class_super(c)) {
        RtCls *r = rtcls(c);
        if (!r) break;
        if ((m = map_get(&r->methods, sel, 0))) { map_put(&cache, cls, sel, m); return m; }
    }
    return NULL;
}
int obj_responds(u32 obj, u32 sel) { return obj && lookup(obj_class(obj), sel_canon(sel)) != NULL; }
int obj_is_kind(u32 obj, u32 cls) {
    for (u32 c = obj_class(obj); c; c = class_super(c)) if (c == cls) return 1;
    return 0;
}

/* ------------------------------------------------------------------ objects
 * Heap objects carry an 8-byte header: [retain count][magic]. Objects inside the image
 * (constant strings, class objects) and runtime classes are immortal. */
#define OBJ_MAGIC 0x4F424A43u
static int valid_addr(u32 o) { return o >= IMG_LO && (u64)o + 8 <= (u64)__builtin_wasm_memory_size(0) * 65536u && !(o & 3); }
static int is_heap_obj(u32 o) { return valid_addr(o) && o >= IMG_HI && RD32(o - 4) == OBJ_MAGIC; }
u32 obj_alloc(u32 cls, u32 size) {
    if (size < 4) size = 4;
    u8 *p = calloc(1, size + 8);
    u32 o = A(p) + 8;
    WR32(o - 8, 1); WR32(o - 4, OBJ_MAGIC); WR32(o, cls);
    return o;
}
u32 obj_retain(u32 o) {
    if (o && !valid_addr(o)) { rt_log("retain of invalid pointer 0x%x", o); return o; }
    if (is_heap_obj(o)) WR32(o - 8, RD32(o - 8) + 1);
    else if (o >= IMG_HI && valid_addr(o) && RD32(o - 4) == 0xDEADDEADu) rt_log("retain of deallocated object 0x%x", o);
    return o;
}
u32 obj_retain_count(u32 o) { return is_heap_obj(o) ? RD32(o - 8) : 0x7fffffffu; }
void obj_release(u32 o) {
    if (o && !valid_addr(o)) { rt_log("release of invalid pointer 0x%x", o); return; }
    if (o >= IMG_HI && RD32(o - 4) == 0xDEADDEADu) { rt_log("release of deallocated object 0x%x", o); return; }
    if (!is_heap_obj(o)) return;
    u32 rc = RD32(o - 8);
    if (rc == 0) { rt_log("over-release of %x (%s)", o, class_name(obj_class(o))); return; }
    if (rc == 1) { msg_sendv(o, "dealloc", 0); return; }        /* dealloc frees (game may override) */
    WR32(o - 8, rc - 1);
}
void obj_dealloc_memory(u32 o) {
    if (!is_heap_obj(o)) return;
    WR32(o - 4, 0xDEADDEADu); WR32(o - 8, 0);
    free(P(o - 8));
}

/* autorelease pools: a stack of arrays */
typedef struct Pool { u32 *objs; int n, cap; struct Pool *up; } Pool;
static Pool *pools;
void pool_push(void) { Pool *p = calloc(1, sizeof *p); p->up = pools; pools = p; }
void pool_pop(void) {
    Pool *p = pools; if (!p) return;
    pools = p->up;
    for (int i = 0; i < p->n; i++) obj_release(p->objs[i]);   /* in order, like NSAutoreleasePool */
    free(p->objs); free(p);
}
u32 obj_autorelease(u32 o) {
    if (!o) return 0;
    if (!pools) { RT_ONCE(rt_log("autorelease with no pool in place (object leaked, as on iOS)")); return o; }
    if (pools->n == pools->cap) { pools->cap = pools->cap ? pools->cap * 2 : 64; pools->objs = realloc(pools->objs, pools->cap * 4); }
    pools->objs[pools->n++] = o;
    return o;
}

/* ------------------------------------------------------------------ dispatch */
static Map missing;
static u64 not_found(u32 self, u32 cls, u32 sel) {
    if (!map_get(&missing, cls, sel)) {
        map_put(&missing, cls, sel, (void *)1);
        rt_log("UNIMPLEMENTED %c[%s %s]", class_is_meta(cls) ? '+' : '-', class_name(cls), sel_name(sel));
    }
    (void)self; return 0;
}
static u64 call(Method *mt, u32 self, u32 sel, u32 stret, const u32 *w, int n, int args_at) {
    if (mt->orig) {
        if (!mt->fn) return invoke_orig(mt->orig, w);
    }
    Msg m = { self, sel, stret, w + args_at, n - args_at };
    return mt->fn(&m);
}
/* +initialize: sent before the first message to a class, superclasses first. As in Apple's
 * runtime, a subclass without its own +initialize runs the inherited one again. */
static void class_initialize(u32 cls) {
    RtCls *c = rtcls(cls);
    if (!c || c->initialized) return;
    c->initialized = 1;
    u32 sup = class_super(cls);
    if (sup) class_initialize(sup);
    u32 meta = RD32(cls);
    Method *mt = lookup(meta, sel_get("initialize"));
    if (mt) { u32 w[2] = { cls, sel_get("initialize") }; call(mt, cls, w[1], 0, w, 2, 2); }
}
int rt_trace_msgs;
u64 objc_msgSend_w(const u32 *w, int n, int kind) {
    u32 self, sel, stret = 0, cls, buf[48];
    int args_at;
    if (kind == MSGK_STRET) { stret = w[0]; self = w[1]; sel = w[2]; args_at = 3; }
    else { self = w[0]; sel = w[1]; args_at = 2; }
    if (kind == MSGK_SUPER2) {
        u32 sup = self; self = RD32(sup); cls = class_super(RD32(sup + 4));
        memcpy(buf, w, n * 4); buf[0] = self; w = buf;
    } else cls = obj_class(self);
    if (!self) return 0;
    if (!valid_addr(self)) { rt_log("message %s to invalid pointer 0x%x", sel_name(sel_canon(sel)), self); return 0; }
    if (self >= IMG_HI && RD32(self - 4) == 0xDEADDEADu) { rt_log("message %s to deallocated object 0x%x", sel_name(sel_canon(sel)), self); return 0; }
    if (!rtcls(cls)) { rt_log("message %s to 0x%x whose isa 0x%x is not a class", sel_name(sel_canon(sel)), self, cls); return 0; }
    u32 cs = sel_canon(sel);
    if (class_is_meta(cls)) { RtCls *rc = rtcls(self); if (rc && !rc->initialized) class_initialize(self); }
    if (rt_trace_msgs) rt_log("msg %c[%s %s]", class_is_meta(cls) ? '+' : '-', class_name(cls), sel_name(cs));
    Method *mt = lookup(cls, cs);
    if (!mt) return not_found(self, cls, cs);
#ifdef MGST_RETOUCHED
    /* Re-Touched add-ons observe or replace a handful of messages (android-retouched/rt/rtx.c).
     * Never compiled into the original edition. */
    if (rtx_hooks_on && rtx_is_hooked(cs)) {
        u32 cp[48]; u64 r; memcpy(cp, w, (size_t)n * 4);
        if (rtx_pre(self, cs, cp + args_at, n - args_at, &r)) return r;
        r = call(mt, self, sel, stret, cp, n, args_at);
        return rtx_post(self, cs, cp + args_at, n - args_at, r);
    }
#endif
    return call(mt, self, sel, stret, w, n, args_at);
}
u64 msg_send(u32 self, u32 sel, const u32 *args, int n) {
    u32 w[48]; w[0] = self; w[1] = sel;
    for (int i = 0; i < n; i++) w[2 + i] = args[i];
    return objc_msgSend_w(w, n + 2, MSGK_NORMAL);
}
u64 msg_sendv(u32 self, const char *sel, int n, ...) {
    __builtin_va_list ap; __builtin_va_start(ap, n);
    u32 a[16];
    for (int i = 0; i < n; i++) a[i] = __builtin_va_arg(ap, u32);
    __builtin_va_end(ap);
    return msg_send(self, sel_get(sel), a, n);
}
unsigned long long invoke_orig_missing(unsigned addr) { rt_log("invoke_orig: no function at 0x%x", addr); return 0; }

void _objc_enumerationMutation(void *obj) { rt_fatal("collection %p mutated while being enumerated", obj); }
void _objc_setProperty(void *self, char *cmd, int offset, void *nv, char atomic, char copy) {
    u32 slot = A(self) + (u32)offset, old = RD32(slot), v = A(nv);
    (void)cmd; (void)atomic;
    if (copy) v = (u32)msg_sendv(v, "copy", 0); else obj_retain(v);
    WR32(slot, v); obj_release(old);
}

/* ------------------------------------------------------------------ image classes */
static void load_methods(RtCls *c, const ImgMethod *ms) {
    for (; ms && ms->sel; ms++) add_method(c, sel_get(ms->sel), ms->imp, NULL);
}
void rt_init_image_classes(void) {
    for (int i = 0; i < img_nclasses; i++) {
        const ImgClass *ic = &img_classes[i];
        RtCls *c = cls_register(ic->addr, 0, ic->name, ic->isize);
        RtCls *m = cls_register(ic->meta, 1, ic->name, 0);
        c->partner = ic->meta; m->partner = ic->addr;
        load_methods(c, ic->imethods); load_methods(m, ic->cmethods);
    }
    for (int i = 0; i < img_ncategories; i++) {
        const ImgCategory *k = &img_categories[i];
        u32 cls = k->cls ? k->cls : rt_bind_value(k->cls_sym);
        load_methods(rtcls(cls), k->imethods); load_methods(rtcls(RD32(cls)), k->cmethods);
    }
}
/* the method-list names inside the image are the canonical selector strings too */
void rt_init_objc(void) {
    for (int i = 0; i < img_nclasses; i++) (void)0;
}

/* ------------------------------------------------------------------ calls through function pointers */
void *orig_fnptr(unsigned addr);
void *orig_fnptr_typed(unsigned addr, int tid);
void *rt_icall(unsigned target, int tid) {
    if (target >= 0x25bcu && target <= 0x5a614u) {         /* original __text, or a runtime function */
        void *f = tid ? orig_fnptr_typed(target, tid) : 0;
        if (!f) { RT_ONCE(rt_log("indirect call to 0x%x with an untyped pointer: direct", target)); f = orig_fnptr(target); }
        if (!f) rt_fatal("indirect call to 0x%x: no function there", target);
        return f;
    }
    return P(target);
}
