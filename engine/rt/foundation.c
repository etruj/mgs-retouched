/* Foundation subset: exactly what MGS Touch sends (docs/PLATFORM_API.md and the
 * runtime-observed `dyn:` receivers in the verified C). Objects are plain structs in
 * wasm memory with isa first. NSString uses the CFString constant layout
 * {isa, flags, ptr, length} so the game's constant strings are ordinary instances. */
#include "rt.h"

static u32 C_NSObject, C_NSString, C_NSMutableString, C_ConstStr, C_NSArray, C_NSMutableArray,
    C_NSDictionary, C_NSMutableDictionary, C_NSNumber, C_NSData, C_NSMutableData, C_NSSet,
    C_NSBundle, C_NSFileManager, C_NSUserDefaults, C_NSNotificationCenter, C_NSNotification,
    C_NSTimer, C_NSThread, C_NSLock, C_NSAutoreleasePool, C_NSException, C_NSURL, C_NSURLRequest,
    C_RevEnum, C_NSError;

/* ================================================================== NSObject */
SHIM(o_alloc) { return obj_alloc(m->self, class_isize(m->self)); }
SHIM(o_new) { u32 o = obj_alloc(m->self, class_isize(m->self)); return msg_sendv(o, "init", 0); }
SHIM(o_self) { return m->self; }
SHIM(o_dealloc) { obj_dealloc_memory(m->self); return 0; }
SHIM(o_retain) { return obj_retain(m->self); }
SHIM(o_release) { obj_release(m->self); return 0; }
SHIM(o_autorelease) { return obj_autorelease(m->self); }
SHIM(o_retainCount) { return obj_retain_count(m->self); }
SHIM(o_class) { return class_is_meta(obj_class(m->self)) ? m->self : obj_class(m->self); }
SHIM(o_superclass) { return class_super(class_is_meta(obj_class(m->self)) ? m->self : obj_class(m->self)); }
SHIM(o_isKindOfClass) { return obj_is_kind(m->self, AU(0)); }
SHIM(o_isMemberOfClass) { return obj_class(m->self) == AU(0); }
SHIM(o_respondsToSelector) { return obj_responds(m->self, AU(0)); }
SHIM(o_instancesRespond) { u32 tmp = obj_alloc(m->self, 4); int r = obj_responds(tmp, AU(0)); obj_dealloc_memory(tmp); return r; }
SHIM(o_perform0) { return msg_send(m->self, AU(0), NULL, 0); }
SHIM(o_perform1) { u32 a[1] = { AU(1) }; return msg_send(m->self, AU(0), a, 1); }
SHIM(o_perform2) { u32 a[2] = { AU(1), AU(2) }; return msg_send(m->self, AU(0), a, 2); }
SHIM(o_hash) { return m->self; }
SHIM(o_isEqual) { return m->self == AU(0); }
SHIM(o_copy) { return obj_retain(m->self); }
SHIM(o_description) {
    char b[96]; u32 c = obj_class(m->self);
    rt_snprintf(b, sizeof b, "<%s: 0x%x>", class_name(class_is_meta(c) ? m->self : c), m->self);
    return str_new_utf8(b);
}
SHIM(o_cls_description) { return str_new_utf8(class_name(m->self)); }

/* ================================================================== NSString */
typedef struct { u32 isa, flags, ptr, len, u8cache, cap; } Str;
#define STR_UNI 0x10u
#define STR_HEAP 0x100000u
#define S(o) ((Str *)P(o))
static void *side_get(u32 key);
static void side_put(u32 key, void *v);

int str_is(u32 o) { return o && obj_is_kind(o, C_NSString); }
static u32 str_alloc(u32 cls) { u32 o = obj_alloc(cls, sizeof(Str)); S(o)->flags = STR_UNI | STR_HEAP; return o; }
static void str_set_utf16(u32 o, const u16 *u, int n) {
    Str *s = S(o);
    if (s->flags & STR_HEAP) { free(P(s->ptr)); free(P(s->u8cache)); }
    u16 *b = malloc((n + 1) * 2); memcpy(b, u, n * 2); b[n] = 0;
    s->ptr = A(b); s->len = n; s->cap = n; s->u8cache = 0; s->flags = STR_UNI | STR_HEAP;
}
static int utf8_to_utf16(const char *s, int n, u16 *out) {   /* out may be NULL: count only */
    int k = 0;
    for (int i = 0; i < n;) {
        u32 c = (u8)s[i], len = 1;
        if (c >= 0xF0 && i + 3 < n) { c = (c & 7) << 18 | (s[i+1] & 0x3F) << 12 | (s[i+2] & 0x3F) << 6 | (s[i+3] & 0x3F); len = 4; }
        else if (c >= 0xE0 && i + 2 < n) { c = (c & 15) << 12 | (s[i+1] & 0x3F) << 6 | (s[i+2] & 0x3F); len = 3; }
        else if (c >= 0xC0 && i + 1 < n) { c = (c & 31) << 6 | (s[i+1] & 0x3F); len = 2; }
        i += len;
        if (c >= 0x10000) { if (out) { out[k] = 0xD800 | ((c - 0x10000) >> 10); out[k+1] = 0xDC00 | ((c - 0x10000) & 0x3FF); } k += 2; }
        else { if (out) out[k] = (u16)c; k++; }
    }
    return k;
}
static char *utf16_to_utf8(const u16 *u, int n) {
    char *o = malloc(n * 3 + 1); int k = 0;
    for (int i = 0; i < n; i++) {
        u32 c = u[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) { c = 0x10000 + ((c - 0xD800) << 10) + (u[i+1] - 0xDC00); i++; o = realloc(o, n * 3 + 2 + k); }
        if (c < 0x80) o[k++] = (char)c;
        else if (c < 0x800) { o[k++] = (char)(0xC0 | c >> 6); o[k++] = (char)(0x80 | (c & 0x3F)); }
        else if (c < 0x10000) { o[k++] = (char)(0xE0 | c >> 12); o[k++] = (char)(0x80 | (c >> 6 & 0x3F)); o[k++] = (char)(0x80 | (c & 0x3F)); }
        else { o[k++] = (char)(0xF0 | c >> 18); o[k++] = (char)(0x80 | (c >> 12 & 0x3F)); o[k++] = (char)(0x80 | (c >> 6 & 0x3F)); o[k++] = (char)(0x80 | (c & 0x3F)); }
    }
    o[k] = 0; return o;
}
/* UTF-16 view of any string (constant 8-bit strings are widened once and cached) */
int str_utf16(u32 o, const u16 **out) {
    Str *s = S(o);
    if (s->flags & STR_UNI) { *out = P(s->ptr); return (int)s->len; }
    u16 *c = side_get(o);
    if (!c) { c = malloc((s->len + 1) * 2); for (u32 i = 0; i < s->len; i++) c[i] = ((u8 *)P(s->ptr))[i]; c[s->len] = 0; side_put(o, c); }
    *out = c; return (int)s->len;
}
const char *str_utf8(u32 o) {
    if (!o) return "";
    Str *s = S(o);
    if (!(s->flags & STR_UNI)) {                 /* constant 8-bit: ASCII is already UTF-8 */
        const u8 *p = P(s->ptr); u32 i = 0;
        while (i < s->len && p[i] < 0x80) i++;
        if (i == s->len) return (const char *)p;
    }
    if (s->flags & STR_HEAP) {
        if (!s->u8cache) s->u8cache = A(utf16_to_utf8(P(s->ptr), (int)s->len));
        return P(s->u8cache);
    }
    char *c = side_get(o ^ 0x80000000u);
    if (!c) { const u16 *u; int n = str_utf16(o, &u); c = utf16_to_utf8(u, n); side_put(o ^ 0x80000000u, c); }
    return c;
}
u32 str_new_utf16(const u16 *u, int n, int retained) {
    u32 o = str_alloc(C_NSString); str_set_utf16(o, u, n);
    return retained ? o : obj_autorelease(o);
}
static u32 str_from_utf8(u32 cls, const char *s, int n) {
    u32 o = str_alloc(cls); int k = utf8_to_utf16(s, n, NULL);
    u16 *b = malloc((k + 1) * 2); utf8_to_utf16(s, n, b); str_set_utf16(o, b, k); free(b);
    return o;
}
u32 str_new_utf8_retained(const char *s) { return str_from_utf8(C_NSString, s, (int)strlen(s)); }
u32 str_new_utf8(const char *s) { return obj_autorelease(str_new_utf8_retained(s)); }
static int str_eq(u32 a, u32 b) {
    if (a == b) return 1;
    if (!str_is(a) || !str_is(b)) return 0;
    const u16 *x, *y; int n = str_utf16(a, &x), k = str_utf16(b, &y);
    return n == k && !memcmp(x, y, n * 2);
}
static u32 str_hash(u32 a) { const u16 *x; int n = str_utf16(a, &x); u32 h = 5381; for (int i = 0; i < n; i++) h = h * 33 + x[i]; return h; }
/* decode bytes in an NSStringEncoding into a new retained string */
static u32 str_from_bytes(u32 cls, const u8 *b, int n, u32 enc) {
    switch (enc) {
    case 4: case 1: case 8: case 5: case 30: case 12: {   /* UTF-8, ASCII, Shift-JIS (ASCII only in the bundle), Latin-1, MacRoman, Windows-1252 */
        int ascii = 1; for (int i = 0; i < n; i++) if (b[i] >= 0x80) { ascii = 0; break; }
        if (enc == 4 || ascii) return str_from_utf8(cls, (const char *)b, n);
        if (enc == 8) RT_ONCE(rt_log("NSShiftJISStringEncoding with non-ASCII bytes: decoded as Latin-1"));
        u16 *u = malloc((n + 1) * 2); for (int i = 0; i < n; i++) u[i] = b[i];
        u32 o = str_alloc(cls); str_set_utf16(o, u, n); free(u); return o;
    }
    case 10: case 0x90000100u: case 0x94000100u: {   /* UTF-16 (BOM honoured), UTF-16BE, UTF-16LE */
        int be = enc == 0x90000100u, off = 0;
        if (enc == 10 && n >= 2) { if (b[0] == 0xFE && b[1] == 0xFF) { be = 1; off = 2; } else if (b[0] == 0xFF && b[1] == 0xFE) off = 2; }
        int k = (n - off) / 2; u16 *u = malloc((k + 1) * 2);
        for (int i = 0; i < k; i++) u[i] = be ? (u16)(b[off + 2*i] << 8 | b[off + 2*i + 1]) : (u16)(b[off + 2*i] | b[off + 2*i + 1] << 8);
        u32 o = str_alloc(cls); str_set_utf16(o, u, k); free(u); return o;
    }
    default:
        rt_log("unsupported NSStringEncoding %u", enc);
        return str_from_utf8(cls, (const char *)b, n);
    }
}
static int file_read_all(const char *path, u8 **out) {
    int sz = host_fsize(path); if (sz < 0) return -1;
    int h = host_fopen(path, "rb"); if (h < 0) return -1;
    u8 *b = malloc(sz + 1); int got = host_fread(h, b, sz); host_fclose(h); b[got] = 0;
    *out = b; return got;
}

SHIM(s_alloc) { return str_alloc(m->self); }
SHIM(s_init) { str_set_utf16(m->self, (const u16 *)"\0\0", 0); return m->self; }
SHIM(s_initWithCapacity) { str_set_utf16(m->self, (const u16 *)"\0\0", 0); return m->self; }
SHIM(s_initWithBytes) { u32 o = str_from_bytes(obj_class(m->self), P(AU(0)), AI(1), AU(2)); obj_dealloc_memory(m->self); return o; }
SHIM(s_initWithData) {
    struct { u32 isa, len, bytes; } *d = P(AU(0));
    u32 o = str_from_bytes(obj_class(m->self), P(d->bytes), (int)d->len, AU(1)); obj_dealloc_memory(m->self); return o;
}
SHIM(s_initWithString) { const u16 *u; int n = str_utf16(AU(0), &u); str_set_utf16(m->self, u, n); return m->self; }
SHIM(s_initWithUTF8String) { u32 o = str_from_utf8(obj_class(m->self), P(AU(0)), (int)strlen(P(AU(0)))); obj_dealloc_memory(m->self); return o; }
static u32 format_words(const char *fmt, const u32 *w, int n) {
    int cap = 4096; char *b = malloc(cap);
    int k = fmt_words(b, cap, fmt, w, n, NULL);
    if (k >= cap) { free(b); cap = k + 1; b = malloc(cap); fmt_words(b, cap, fmt, w, n, NULL); }
    u32 o = str_new_utf8(b); free(b); return o;
}
SHIM(s_stringWithFormat) { return format_words(str_utf8(AU(0)), m->a + 1, m->n - 1); }
SHIM(s_initWithFormat) { u32 o = obj_retain(format_words(str_utf8(AU(0)), m->a + 1, m->n - 1)); obj_dealloc_memory(m->self); return o; }
SHIM(s_stringWithCString) { const char *c = P(AU(0)); return obj_autorelease(str_from_bytes(C_NSString, (const u8 *)c, (int)strlen(c), m->n > 1 ? AU(1) : 4)); }
SHIM(s_stringWithUTF8String) { return str_new_utf8(P(AU(0))); }
SHIM(s_stringWithString) { const u16 *u; int n = str_utf16(AU(0), &u); return str_new_utf16(u, n, 0); }
SHIM(s_string) { return str_new_utf8(""); }
SHIM(s_stringWithContentsOfFile) {
    u8 *b; int n = file_read_all(str_utf8(AU(0)), &b);
    if (n < 0) { if (m->n > 2 && AU(2)) WR32(AU(2), 0); return 0; }
    u32 o = str_from_bytes(C_NSString, b, n, AU(1)); free(b); return obj_autorelease(o);
}
SHIM(s_UTF8String) { return A(str_utf8(m->self)); }
SHIM(s_cStringUsingEncoding) { return A(str_utf8(m->self)); }
SHIM(s_length) { const u16 *u; return (u32)str_utf16(m->self, &u); }
SHIM(s_characterAtIndex) { const u16 *u; int n = str_utf16(m->self, &u); return AI(0) < n ? u[AI(0)] : 0; }
SHIM(s_isEqualToString) { return str_eq(m->self, AU(0)); }
SHIM(s_isEqual) { return str_is(AU(0)) && str_eq(m->self, AU(0)); }
SHIM(s_hash) { return str_hash(m->self); }
SHIM(s_copy) { if (S(m->self)->flags & STR_HEAP && obj_class(m->self) == C_NSMutableString) { const u16 *u; int n = str_utf16(m->self, &u); return str_new_utf16(u, n, 1); } return obj_retain(m->self); }
SHIM(s_mutableCopy) { const u16 *u; int n = str_utf16(m->self, &u); u32 o = str_alloc(C_NSMutableString); str_set_utf16(o, u, n); return o; }
SHIM(s_description) { return m->self; }
static double parse_double(const char *s) { return host_strtod(s, 0); }
static s32 parse_int(const char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
    int neg = 0; if (*s == '-' || *s == '+') neg = *s++ == '-';
    s64 v = 0; while (*s >= '0' && *s <= '9') { v = v * 10 + (*s++ - '0'); if (v > 0x7fffffffLL + neg) { v = 0x7fffffffLL + neg; break; } }
    return (s32)(neg ? -v : v);
}
SHIM(s_intValue) { return (u32)parse_int(str_utf8(m->self)); }
SHIM(s_floatValue) { return RF((float)parse_double(str_utf8(m->self))); }
SHIM(s_doubleValue) { return RD(parse_double(str_utf8(m->self))); }
SHIM(s_boolValue) { const char *s = str_utf8(m->self); while (*s == ' ') s++; return *s == 'Y' || *s == 'y' || *s == 'T' || *s == 't' || (*s >= '1' && *s <= '9'); }
SHIM(s_substringWithRange) {
    const u16 *u; int n = str_utf16(m->self, &u); u32 loc = AU(0), len = AU(1);
    if (loc > (u32)n || loc + len > (u32)n) rt_fatal("substringWithRange: {%u,%u} out of bounds (length %d)", loc, len, n);
    return str_new_utf16(u + loc, (int)len, 0);
}
SHIM(s_substringFromIndex) { const u16 *u; int n = str_utf16(m->self, &u); int i = AI(0); if (i > n) i = n; return str_new_utf16(u + i, n - i, 0); }
SHIM(s_substringToIndex) { const u16 *u; int n = str_utf16(m->self, &u); int i = AI(0); if (i > n) i = n; return str_new_utf16(u, i, 0); }
SHIM(s_stringByAppendingString) {
    const u16 *a, *b; int n = str_utf16(m->self, &a), k = str_utf16(AU(0), &b);
    u16 *t = malloc((n + k + 1) * 2); memcpy(t, a, n * 2); memcpy(t + n, b, k * 2);
    u32 o = str_new_utf16(t, n + k, 0); free(t); return o;
}
SHIM(s_appendString) {
    const u16 *a, *b; int n = str_utf16(m->self, &a), k = str_utf16(AU(0), &b);
    u16 *t = malloc((n + k + 1) * 2); memcpy(t, a, n * 2); memcpy(t + n, b, k * 2);
    str_set_utf16(m->self, t, n + k); free(t); return 0;
}
SHIM(s_appendFormat) {
    u32 s = format_words(str_utf8(AU(0)), m->a + 1, m->n - 1);
    u32 a[1] = { s }; Msg mm = { m->self, 0, 0, a, 1 }; return s_appendString(&mm);
}
SHIM(s_setString) { const u16 *u; int n = str_utf16(AU(0), &u); str_set_utf16(m->self, u, n); return 0; }
SHIM(s_components) {
    const u16 *u, *sep; int n = str_utf16(m->self, &u), k = str_utf16(AU(0), &sep);
    u32 arr = arr_new(0); int start = 0;
    for (int i = 0; k && i + k <= n;) {
        if (!memcmp(u + i, sep, k * 2)) { arr_add(arr, str_new_utf16(u + start, i - start, 0)); i += k; start = i; }
        else i++;
    }
    arr_add(arr, str_new_utf16(u + start, n - start, 0));
    return arr;
}
/* -lineRangeForRange: (stret NSRange): the full lines containing the range */
SHIM(s_lineRangeForRange) {
    const u16 *u; int n = str_utf16(m->self, &u); u32 loc = AU(0), len = AU(1);
    u32 s = loc; while (s > 0 && u[s - 1] != '\n' && u[s - 1] != '\r' && u[s - 1] != 0x2028) s--;
    u32 e = loc + len; if (len) e--;
    while ((int)e < n && u[e] != '\n' && u[e] != '\r' && u[e] != 0x2028) e++;
    if ((int)e < n) { if (u[e] == '\r' && (int)e + 1 < n && u[e + 1] == '\n') e += 2; else e++; }
    WR32(m->stret, s); WR32(m->stret + 4, e - s); return 0;
}
SHIM(s_rangeOfString) {
    const u16 *u, *f; int n = str_utf16(m->self, &u), k = str_utf16(AU(0), &f);
    for (int i = 0; k && i + k <= n; i++) if (!memcmp(u + i, f, k * 2)) { WR32(m->stret, i); WR32(m->stret + 4, k); return 0; }
    WR32(m->stret, 0x7fffffff); WR32(m->stret + 4, 0); return 0;
}
SHIM(s_hasPrefix) { const u16 *u, *f; int n = str_utf16(m->self, &u), k = str_utf16(AU(0), &f); return k <= n && !memcmp(u, f, k * 2); }
SHIM(s_hasSuffix) { const u16 *u, *f; int n = str_utf16(m->self, &u), k = str_utf16(AU(0), &f); return k <= n && !memcmp(u + n - k, f, k * 2); }
SHIM(s_dataUsingEncoding) {
    if (AU(0) == 10) { const u16 *u; int n = str_utf16(m->self, &u); return data_new(u, n * 2, 0); }
    const char *s = str_utf8(m->self); return data_new(s, (int)strlen(s), 0);
}
SHIM(s_pathExtension) { const char *s = str_utf8(m->self); const char *d = 0; for (const char *p = s; *p; p++) { if (*p == '.') d = p; if (*p == '/') d = 0; } return str_new_utf8(d ? d + 1 : ""); }
SHIM(s_lastPathComponent) { const char *s = str_utf8(m->self); const char *d = s; for (const char *p = s; *p; p++) if (*p == '/' && p[1]) d = p + 1; return str_new_utf8(d); }
SHIM(s_stringByAppendingPathComponent) {
    const char *a = str_utf8(m->self), *b = str_utf8(AU(0)); int n = (int)strlen(a);
    char *t = malloc(n + strlen(b) + 2); strcpy(t, a);
    if (n && t[n - 1] != '/') t[n++] = '/';
    strcpy(t + n, b); u32 o = str_new_utf8(t); free(t); return o;
}
SHIM(s_dealloc) {
    Str *s = S(m->self);
    if (s->flags & STR_HEAP) { free(P(s->ptr)); free(P(s->u8cache)); }
    obj_dealloc_memory(m->self); return 0;
}
/* drawing and measuring live in cg.c (they need the current UIGraphics context) */
u64 str_drawInRect(Msg *m); u64 str_sizeWithFont(Msg *m); u64 str_drawAtPoint(Msg *m);
u64 str_sizeWithFontConstrained(Msg *m);

/* ================================================================== NSArray */
typedef struct { u32 isa, count, cap, items, mut; } Arr;
#define AR(o) ((Arr *)P(o))
u32 arr_new(int retained) { u32 o = obj_alloc(C_NSMutableArray, sizeof(Arr)); return retained ? o : obj_autorelease(o); }
void arr_add(u32 a, u32 x) {
    Arr *r = AR(a);
    if (!x) rt_fatal("-[NSMutableArray addObject:]: nil");
    if (r->count == r->cap) { r->cap = r->cap ? r->cap * 2 : 8; r->items = A(realloc(P(r->items), r->cap * 4)); }
    ((u32 *)P(r->items))[r->count++] = obj_retain(x); r->mut++;
}
int arr_count(u32 a) { return a ? (int)AR(a)->count : 0; }
u32 arr_get(u32 a, int i) {
    if (i < 0 || (u32)i >= AR(a)->count) rt_fatal("-[NSArray objectAtIndex:]: index %d beyond bounds [0 .. %d]", i, (int)AR(a)->count - 1);
    return ((u32 *)P(AR(a)->items))[i];
}
SHIM(a_alloc) { return obj_alloc(m->self, sizeof(Arr)); }
SHIM(a_init) { return m->self; }
SHIM(a_array) { return obj_autorelease(obj_alloc(m->self == C_NSArray ? C_NSArray : m->self, sizeof(Arr))); }
SHIM(a_arrayWithObjects) { u32 a = obj_autorelease(obj_alloc(m->self, sizeof(Arr))); for (int i = 0; i < m->n && AU(i); i++) arr_add(a, AU(i)); return a; }
SHIM(a_initWithObjects) { for (int i = 0; i < m->n && AU(i); i++) arr_add(m->self, AU(i)); return m->self; }
SHIM(a_arrayWithArray) { u32 a = obj_autorelease(obj_alloc(m->self, sizeof(Arr))); for (int i = 0; i < arr_count(AU(0)); i++) arr_add(a, arr_get(AU(0), i)); return a; }
SHIM(a_initWithCapacity) { return m->self; }
SHIM(a_count) { return AR(m->self)->count; }
SHIM(a_objectAtIndex) { return arr_get(m->self, AI(0)); }
SHIM(a_lastObject) { int n = arr_count(m->self); return n ? arr_get(m->self, n - 1) : 0; }
SHIM(a_addObject) { arr_add(m->self, AU(0)); return 0; }
SHIM(a_insertObject) {
    Arr *r = AR(m->self); u32 x = AU(0), at = AU(1);
    arr_add(m->self, x); u32 *it = P(r->items);
    memmove(it + at + 1, it + at, (r->count - 1 - at) * 4); it[at] = x; return 0;
}
SHIM(a_removeObjectAtIndex) {
    Arr *r = AR(m->self); u32 i = AU(0); u32 *it = P(r->items);
    if (i >= r->count) rt_fatal("removeObjectAtIndex: %u beyond bounds", i);
    u32 x = it[i]; memmove(it + i, it + i + 1, (r->count - i - 1) * 4); r->count--; r->mut++; obj_release(x); return 0;
}
SHIM(a_removeObject) {
    Arr *r = AR(m->self);
    for (int i = (int)r->count - 1; i >= 0; i--) {
        u32 x = ((u32 *)P(r->items))[i];
        if (x == AU(0) || (str_is(x) && str_eq(x, AU(0)))) { u32 a[1] = { (u32)i }; Msg mm = { m->self, 0, 0, a, 1 }; a_removeObjectAtIndex(&mm); }
    }
    return 0;
}
SHIM(a_removeLastObject) { Arr *r = AR(m->self); if (r->count) { u32 a[1] = { r->count - 1 }; Msg mm = { m->self, 0, 0, a, 1 }; a_removeObjectAtIndex(&mm); } return 0; }
SHIM(a_removeAllObjects) {
    Arr *r = AR(m->self); u32 n = r->count; r->count = 0; r->mut++;
    for (u32 i = 0; i < n; i++) obj_release(((u32 *)P(r->items))[i]);
    return 0;
}
SHIM(a_containsObject) { for (int i = 0; i < arr_count(m->self); i++) if (arr_get(m->self, i) == AU(0)) return 1; return 0; }
SHIM(a_indexOfObject) { for (int i = 0; i < arr_count(m->self); i++) if (arr_get(m->self, i) == AU(0)) return (u32)i; return 0x7fffffff; }
/* NSFastEnumerationState {state, itemsPtr, mutationsPtr, extra[5]} */
SHIM(a_countByEnumerating) {
    u32 st = AU(0);
    if (RD32(st)) return 0;
    WR32(st, 1); WR32(st + 4, AR(m->self)->items); WR32(st + 8, m->self + 16);
    return AR(m->self)->count;
}
SHIM(a_dealloc) {
    Arr *r = AR(m->self);
    for (u32 i = 0; i < r->count; i++) obj_release(((u32 *)P(r->items))[i]);
    free(P(r->items)); obj_dealloc_memory(m->self); return 0;
}
SHIM(a_reverseObjectEnumerator) {
    u32 e = obj_autorelease(obj_alloc(C_RevEnum, sizeof(Arr)));
    for (int i = arr_count(m->self) - 1; i >= 0; i--) arr_add(e, arr_get(m->self, i));
    return e;
}
SHIM(a_objectEnumerator) {
    u32 e = obj_autorelease(obj_alloc(C_RevEnum, sizeof(Arr)));
    for (int i = 0; i < arr_count(m->self); i++) arr_add(e, arr_get(m->self, i));
    return e;
}
/* -sortUsingFunction:context: comparator is an original code address: NSInteger (*)(id, id, void *).
 * Foundation on iPhone OS 3 used a merge sort (stable); equal elements keep their order. */
static u32 sort_fn, sort_ctx;
static int sort_cmp(u32 a, u32 b) { u32 w[3] = { a, b, sort_ctx }; return (s32)(u32)invoke_orig(sort_fn, w); }
static void merge_sort(u32 *v, u32 *tmp, int n) {
    if (n < 2) return;
    int h = n / 2; merge_sort(v, tmp, h); merge_sort(v + h, tmp, n - h);
    int i = 0, j = h, k = 0;
    while (i < h && j < n) tmp[k++] = sort_cmp(v[j], v[i]) < 0 ? v[j++] : v[i++];
    while (i < h) tmp[k++] = v[i++];
    while (j < n) tmp[k++] = v[j++];
    memcpy(v, tmp, n * 4);
}
SHIM(a_sortUsingFunction) {
    Arr *r = AR(m->self); u32 sf = sort_fn, sc = sort_ctx;
    sort_fn = AU(0); sort_ctx = AU(1);
    u32 *tmp = malloc(r->count * 4 + 4); merge_sort(P(r->items), tmp, (int)r->count); free(tmp);
    sort_fn = sf; sort_ctx = sc; r->mut++; return 0;
}

/* enumerator objects reuse the array layout; the cursor lives in the side table */
SHIM(en_nextObject) {
    Arr *r = AR(m->self); u32 *cursor = side_get(m->self);
    if (!cursor) { cursor = calloc(1, 4); side_put(m->self, cursor); }
    if (*cursor >= r->count) return 0;
    return ((u32 *)P(r->items))[(*cursor)++];
}
SHIM(en_allObjects) { return m->self; }

/* ================================================================== NSDictionary */
typedef struct { u32 isa, count, cap, keys, vals, mut; } Dict;
#define DI(o) ((Dict *)P(o))
static int key_eq(u32 a, u32 b) {
    if (a == b) return 1;
    if (str_is(a) && str_is(b)) return str_eq(a, b);
    if (obj_is_kind(a, C_NSNumber) && obj_is_kind(b, C_NSNumber)) return RD32(a + 8) == RD32(b + 8) && RD32(a + 12) == RD32(b + 12);
    return 0;
}
static int dict_find(u32 d, u32 key) {
    Dict *r = DI(d); u32 *k = P(r->keys);
    for (u32 i = 0; i < r->count; i++) if (key_eq(k[i], key)) return (int)i;
    return -1;
}
u32 dict_new(int retained) { u32 o = obj_alloc(C_NSMutableDictionary, sizeof(Dict)); return retained ? o : obj_autorelease(o); }
void dict_set(u32 d, u32 key, u32 val) {
    if (!key || !val) rt_fatal("-[NSMutableDictionary setObject:forKey:]: nil %s", key ? "object" : "key");
    Dict *r = DI(d); int i = dict_find(d, key);
    obj_retain(val);
    if (i >= 0) { u32 *v = P(r->vals); obj_release(v[i]); v[i] = val; return; }
    if (r->count == r->cap) { r->cap = r->cap ? r->cap * 2 : 8; r->keys = A(realloc(P(r->keys), r->cap * 4)); r->vals = A(realloc(P(r->vals), r->cap * 4)); }
    u32 kc = str_is(key) ? (u32)msg_sendv(key, "copy", 0) : obj_retain(key);
    ((u32 *)P(r->keys))[r->count] = kc; ((u32 *)P(r->vals))[r->count] = val; r->count++; r->mut++;
}
u32 dict_get(u32 d, u32 key) { int i = d ? dict_find(d, key) : -1; return i < 0 ? 0 : ((u32 *)P(DI(d)->vals))[i]; }
SHIM(d_alloc) { return obj_alloc(m->self, sizeof(Dict)); }
SHIM(d_dictionary) { return obj_autorelease(obj_alloc(m->self, sizeof(Dict))); }
SHIM(d_init) { return m->self; }
SHIM(d_dictionaryWithObjectForKey) { u32 d = obj_autorelease(obj_alloc(m->self, sizeof(Dict))); dict_set(d, AU(1), AU(0)); return d; }
static void fill_obj_key_pairs(u32 d, const u32 *a, int n) { for (int i = 0; i + 1 < n && a[i]; i += 2) dict_set(d, a[i + 1], a[i]); }
SHIM(d_dictionaryWithObjectsAndKeys) { u32 d = obj_autorelease(obj_alloc(m->self, sizeof(Dict))); fill_obj_key_pairs(d, m->a, m->n); return d; }
SHIM(d_initWithObjectsAndKeys) { fill_obj_key_pairs(m->self, m->a, m->n); return m->self; }
SHIM(d_objectForKey) { return dict_get(m->self, AU(0)); }
SHIM(d_setObjectForKey) { dict_set(m->self, AU(1), AU(0)); return 0; }
SHIM(d_removeObjectForKey) {
    Dict *r = DI(m->self); int i = dict_find(m->self, AU(0)); if (i < 0) return 0;
    u32 *k = P(r->keys), *v = P(r->vals); obj_release(k[i]); obj_release(v[i]);
    memmove(k + i, k + i + 1, (r->count - i - 1) * 4); memmove(v + i, v + i + 1, (r->count - i - 1) * 4); r->count--; r->mut++;
    return 0;
}
SHIM(d_count) { return DI(m->self)->count; }
SHIM(d_allKeys) { u32 a = arr_new(0); for (u32 i = 0; i < DI(m->self)->count; i++) arr_add(a, ((u32 *)P(DI(m->self)->keys))[i]); return a; }
SHIM(d_allValues) { u32 a = arr_new(0); for (u32 i = 0; i < DI(m->self)->count; i++) arr_add(a, ((u32 *)P(DI(m->self)->vals))[i]); return a; }
SHIM(d_dealloc) {
    Dict *r = DI(m->self);
    for (u32 i = 0; i < r->count; i++) { obj_release(((u32 *)P(r->keys))[i]); obj_release(((u32 *)P(r->vals))[i]); }
    free(P(r->keys)); free(P(r->vals)); obj_dealloc_memory(m->self); return 0;
}
SHIM(d_countByEnumerating) {
    u32 st = AU(0); if (RD32(st)) return 0;
    WR32(st, 1); WR32(st + 4, DI(m->self)->keys); WR32(st + 8, m->self + 20); return DI(m->self)->count;
}

/* ================================================================== NSNumber {isa, kind, lo, hi} */
enum { NK_INT, NK_BOOL, NK_FLOAT, NK_DOUBLE };
u32 num_new_int(s32 v) { u32 o = obj_alloc(C_NSNumber, 16); WR32(o + 4, NK_INT); WR32(o + 8, (u32)v); WR32(o + 12, v < 0 ? ~0u : 0); return obj_autorelease(o); }
static double num_d(u32 o) {
    switch (RD32(o + 4)) {
    case NK_FLOAT: return __builtin_bit_cast(float, RD32(o + 8));
    case NK_DOUBLE: return __builtin_bit_cast(double, (u64)RD32(o + 12) << 32 | RD32(o + 8));
    default: return (double)(s32)RD32(o + 8);
    }
}
SHIM(n_numberWithInt) { return num_new_int(AI(0)); }
SHIM(n_numberWithBool) { u32 o = num_new_int(AU(0) & 0xff ? 1 : 0); WR32(o + 4, NK_BOOL); return o; }
SHIM(n_numberWithFloat) { u32 o = obj_autorelease(obj_alloc(C_NSNumber, 16)); WR32(o + 4, NK_FLOAT); WR32(o + 8, AU(0)); return o; }
SHIM(n_numberWithDouble) { u32 o = obj_autorelease(obj_alloc(C_NSNumber, 16)); WR32(o + 4, NK_DOUBLE); WR32(o + 8, AU(0)); WR32(o + 12, AU(1)); return o; }
SHIM(n_intValue) { u32 k = RD32(m->self + 4); return (k == NK_INT || k == NK_BOOL) ? RD32(m->self + 8) : (u32)(s32)num_d(m->self); }
SHIM(n_boolValue) { return num_d(m->self) != 0; }
SHIM(n_floatValue) { return RF((float)num_d(m->self)); }
SHIM(n_doubleValue) { return RD(num_d(m->self)); }
SHIM(n_isEqual) { return AU(0) && obj_is_kind(AU(0), C_NSNumber) && key_eq(m->self, AU(0)); }
SHIM(n_description) {
    char b[64]; u32 k = RD32(m->self + 4);
    if (k == NK_INT || k == NK_BOOL) rt_snprintf(b, sizeof b, "%d", (s32)RD32(m->self + 8)); else rt_snprintf(b, sizeof b, "%g", num_d(m->self));
    return str_new_utf8(b);
}

/* ================================================================== NSData {isa, len, bytes, owned} */
u32 data_new(const void *bytes, int n, int retained) {
    u32 o = obj_alloc(C_NSMutableData, 16); u8 *b = malloc(n + 1); memcpy(b, bytes, n); b[n] = 0;
    WR32(o + 4, n); WR32(o + 8, A(b)); WR32(o + 12, 1);
    return retained ? o : obj_autorelease(o);
}
SHIM(da_dataWithContentsOfFile) {
    u8 *b; int n = file_read_all(str_utf8(AU(0)), &b); if (n < 0) return 0;
    u32 o = obj_alloc(C_NSData, 16); WR32(o + 4, n); WR32(o + 8, A(b)); WR32(o + 12, 1); return obj_autorelease(o);
}
SHIM(da_dataWithBytes) { return data_new(P(AU(0)), AI(1), 0); }
SHIM(da_bytes) { return RD32(m->self + 8); }
SHIM(da_length) { return RD32(m->self + 4); }
SHIM(da_getBytes) { memcpy(P(AU(0)), P(RD32(m->self + 8)), RD32(m->self + 4)); return 0; }
SHIM(da_getBytesLength) { u32 n = AU(1) < RD32(m->self + 4) ? AU(1) : RD32(m->self + 4); memcpy(P(AU(0)), P(RD32(m->self + 8)), n); return 0; }
SHIM(da_dealloc) { if (RD32(m->self + 12)) free(P(RD32(m->self + 8))); obj_dealloc_memory(m->self); return 0; }

/* ================================================================== NSSet (touch sets) {isa, count, items} */
u32 set_new_with(const u32 *objs, int n) {
    u32 o = obj_alloc(C_NSSet, 16); u32 *it = malloc(n * 4 + 4);
    for (int i = 0; i < n; i++) it[i] = obj_retain(objs[i]);
    WR32(o + 4, n); WR32(o + 8, A(it)); WR32(o + 12, 0); return obj_autorelease(o);
}
SHIM(set_count) { return RD32(m->self + 4); }
SHIM(set_anyObject) { return RD32(m->self + 4) ? RD32(RD32(m->self + 8)) : 0; }
SHIM(set_allObjects) { u32 a = arr_new(0); for (u32 i = 0; i < RD32(m->self + 4); i++) arr_add(a, RD32(RD32(m->self + 8) + 4 * i)); return a; }
SHIM(set_countByEnumerating) {
    u32 st = AU(0); if (RD32(st)) return 0;
    WR32(st, 1); WR32(st + 4, RD32(m->self + 8)); WR32(st + 8, m->self + 12); return RD32(m->self + 4);
}
SHIM(set_containsObject) { for (u32 i = 0; i < RD32(m->self + 4); i++) if (RD32(RD32(m->self + 8) + 4 * i) == AU(0)) return 1; return 0; }
SHIM(set_dealloc) { for (u32 i = 0; i < RD32(m->self + 4); i++) obj_release(RD32(RD32(m->self + 8) + 4 * i)); free(P(RD32(m->self + 8))); obj_dealloc_memory(m->self); return 0; }

/* ================================================================== side table (caches keyed by object) */
typedef struct SideEnt { u32 key; void *v; struct SideEnt *next; } SideEnt;
static SideEnt *side_tab[1024];
static void *side_get(u32 key) { for (SideEnt *e = side_tab[(key >> 3) & 1023]; e; e = e->next) if (e->key == key) return e->v; return NULL; }
static void side_put(u32 key, void *v) {
    SideEnt *e = malloc(sizeof *e); e->key = key; e->v = v; e->next = side_tab[(key >> 3) & 1023]; side_tab[(key >> 3) & 1023] = e;
}

/* ================================================================== NSBundle, localisation */
static u32 the_bundle;
static char rt_lang[32] = "English";
EXPORT(mgst_set_language) void mgst_set_language(const char *l) { strcpy(rt_lang, l); }
SHIM(b_mainBundle) { if (!the_bundle) the_bundle = obj_alloc(C_NSBundle, 8); return the_bundle; }
static u32 find_resource(const char *name, const char *type, const char *dir) {
    char path[512], file[256];
    if (type && *type) rt_snprintf(file, sizeof file, "%s.%s", name, type); else rt_snprintf(file, sizeof file, "%s", name);
    const char *subs[3] = { "", rt_lang, "English" };
    for (int i = 0; i < 3; i++) {
        if (i == 0) rt_snprintf(path, sizeof path, dir && *dir ? "/app/%s/%s" : "/app/%s%s", dir && *dir ? dir : "", file);
        else rt_snprintf(path, sizeof path, dir && *dir ? "/app/%s.lproj/%s/%s" : "/app/%s.lproj/%s%s", subs[i], dir && *dir ? dir : "", file);
        if (host_fsize(path) >= 0) return str_new_utf8(path);
    }
    return 0;
}
SHIM(b_pathForResource) { return find_resource(str_utf8(AU(0)), AU(1) ? str_utf8(AU(1)) : NULL, NULL); }
SHIM(b_pathForResourceInDir) { return find_resource(str_utf8(AU(0)), AU(1) ? str_utf8(AU(1)) : NULL, AU(2) ? str_utf8(AU(2)) : NULL); }
SHIM(b_bundlePath) { return str_new_utf8("/app"); }
SHIM(b_infoDictionary) { return dict_new(0); }

/* binary plist (Localizable.strings): dict of string -> string */
typedef struct { const u8 *b; int n; int osz, rsz; u32 nobj, top, ot; } BPlist;
static u64 be(const u8 *p, int k) { u64 v = 0; while (k--) v = v << 8 | *p++; return v; }
static u32 bp_off(BPlist *p, u32 i) { return (u32)be(p->b + p->ot + i * p->osz, p->osz); }
static u32 bp_string(BPlist *p, u32 ref) {
    const u8 *o = p->b + bp_off(p, ref); int t = o[0] >> 4; u32 len = o[0] & 15; int h = 1;
    if (len == 15) { int ib = 1 << (o[1] & 15); len = (u32)be(o + 2, ib); h = 2 + ib; }
    if (t == 5) return obj_autorelease(str_from_bytes(C_NSString, o + h, (int)len, 1));
    if (t == 6) { u16 *u = malloc(len * 2 + 2); for (u32 i = 0; i < len; i++) u[i] = (u16)be(o + h + 2 * i, 2); u32 s = str_new_utf16(u, (int)len, 0); free(u); return s; }
    return 0;
}
static u32 strings_table;
static void load_strings(void) {
    char path[256]; rt_snprintf(path, sizeof path, "/app/%s.lproj/Localizable.strings", rt_lang);
    u8 *b; int n = file_read_all(path, &b);
    strings_table = dict_new(1);
    if (n < 40 || memcmp(b, "bplist00", 8)) { rt_log("no binary Localizable.strings for %s", rt_lang); return; }
    BPlist p = { b, n, b[n - 26], b[n - 25], (u32)be(b + n - 24, 8), (u32)be(b + n - 16, 8), (u32)be(b + n - 8, 8) };
    const u8 *o = b + bp_off(&p, p.top);
    if (o[0] >> 4 != 0xD) { rt_log("Localizable.strings: top object is not a dict"); return; }
    u32 cnt = o[0] & 15; int h = 1;
    if (cnt == 15) { int ib = 1 << (o[1] & 15); cnt = (u32)be(o + 2, ib); h = 2 + ib; }
    pool_push();
    for (u32 i = 0; i < cnt; i++) {
        u32 kr = (u32)be(o + h + i * p.rsz, p.rsz), vr = (u32)be(o + h + (cnt + i) * p.rsz, p.rsz);
        u32 k = bp_string(&p, kr), v = bp_string(&p, vr);
        if (k && v) dict_set(strings_table, k, v);
    }
    pool_pop();
    rt_log("Localizable.strings (%s): %d entries", rt_lang, (int)DI(strings_table)->count);
}
SHIM(b_localizedString) {
    if (!strings_table) load_strings();
    u32 v = dict_get(strings_table, AU(0));
    return v ? v : (AU(1) && S(AU(1))->len ? AU(1) : AU(0));
}

/* ================================================================== NSFileManager, search paths */
static char cwd[256] = "/app";
const char *rt_cwd(void) { return cwd; }
SHIM(fm_defaultManager) { static u32 fm; if (!fm) fm = obj_alloc(C_NSFileManager, 8); return fm; }
SHIM(fm_changeCwd) { const char *p = str_utf8(AU(0)); if (strlen(p) < sizeof cwd) strcpy(cwd, p); return 1; }
SHIM(fm_fileExists) { return host_fsize(str_utf8(AU(0))) >= 0; }
SHIM(fm_createDirectory) { return host_mkdir(str_utf8(AU(0))) == 0; }
SHIM(fm_removeItem) { return 1; }
void *_NSSearchPathForDirectoriesInDomains(u32 dir, u32 mask, char tilde) {
    (void)mask; (void)tilde;
    u32 a = arr_new(0);
    arr_add(a, str_new_utf8(dir == 9 ? "/docs" : dir == 13 ? "/docs/Caches" : "/docs/Library"));
    return P(a);
}

/* ================================================================== NSUserDefaults */
static u32 defaults_reg, defaults_vals, the_defaults;
SHIM(ud_standard) { if (!the_defaults) { the_defaults = obj_alloc(C_NSUserDefaults, 8); defaults_reg = dict_new(1); defaults_vals = dict_new(1); } return the_defaults; }
SHIM(ud_registerDefaults) { u32 d = AU(0); for (u32 i = 0; i < DI(d)->count; i++) dict_set(defaults_reg, ((u32 *)P(DI(d)->keys))[i], ((u32 *)P(DI(d)->vals))[i]); return 0; }
static u32 ud_lookup(u32 k) { u32 v = dict_get(defaults_vals, k); return v ? v : dict_get(defaults_reg, k); }
SHIM(ud_objectForKey) { return ud_lookup(AU(0)); }
SHIM(ud_integerForKey) { u32 v = ud_lookup(AU(0)); return v ? msg_sendv(v, "intValue", 0) : 0; }
SHIM(ud_boolForKey) { u32 v = ud_lookup(AU(0)); return v ? (msg_sendv(v, "intValue", 0) != 0) : 0; }
SHIM(ud_floatForKey) { u32 v = ud_lookup(AU(0)); return v ? msg_sendv(v, "floatValue", 0) : 0; }
SHIM(ud_setObject) { dict_set(defaults_vals, AU(1), AU(0)); return 0; }
SHIM(ud_setInteger) { dict_set(defaults_vals, AU(1), num_new_int(AI(0))); return 0; }
SHIM(ud_setBool) { dict_set(defaults_vals, AU(1), num_new_int(AU(0) & 0xff ? 1 : 0)); return 0; }
SHIM(ud_synchronize) { return 1; }

/* ================================================================== NSNotificationCenter */
typedef struct Obs { u32 obs, sel, name, obj; struct Obs *next; } Obs;
static Obs *observers;
SHIM(nc_default) { static u32 nc; if (!nc) nc = obj_alloc(C_NSNotificationCenter, 8); return nc; }
SHIM(nc_addObserver) {
    Obs *o = calloc(1, sizeof *o); o->obs = AU(0); o->sel = AU(1); o->name = AU(2) ? obj_retain(AU(2)) : 0; o->obj = AU(3);
    o->next = observers; observers = o; return 0;
}
SHIM(nc_removeObserver) {
    for (Obs **pp = &observers; *pp;) {
        Obs *o = *pp;
        if (o->obs == AU(0) && (m->n < 2 || !AU(1) || (o->name && str_eq(o->name, AU(1))))) { *pp = o->next; free(o); }
        else pp = &o->next;
    }
    return 0;
}
void notify_post(const char *name, u32 obj) {
    pool_push();
    u32 n = obj_autorelease(obj_alloc(C_NSNotification, 16));
    WR32(n + 4, obj_retain(str_new_utf8(name))); WR32(n + 8, obj);
    for (Obs *o = observers; o; o = o->next)
        if ((!o->name || !strcmp(str_utf8(o->name), name)) && (!o->obj || o->obj == obj)) { u32 a[1] = { n }; msg_send(o->obs, o->sel, a, 1); }
    pool_pop();
}
SHIM(nc_post) { notify_post(str_utf8(AU(0)), m->n > 1 ? AU(1) : 0); return 0; }
SHIM(nt_name) { return RD32(m->self + 4); }
SHIM(nt_object) { return RD32(m->self + 8); }

/* ================================================================== NSTimer {isa, target, sel, userInfo, repeats, interval(d), fire(d), valid} */
typedef struct { u32 isa, target, sel, info, repeats, valid; double interval, fire; } Timer;
static u32 timers[64]; static int ntimers;
SHIM(t_scheduled) {
    u32 t = obj_alloc(C_NSTimer, sizeof(Timer)); Timer *tm = P(t);
    tm->interval = AD(0); if (tm->interval < 0.0001) tm->interval = 0.0001;
    tm->target = obj_retain(AU(2)); tm->sel = AU(3); tm->info = obj_retain(AU(4)); tm->repeats = AU(5) & 0xff; tm->valid = 1;
    tm->fire = host_time() + tm->interval;
    if (ntimers < 64) timers[ntimers++] = obj_retain(t);
    rt_log("NSTimer scheduled: every %.4f s -> [%s %s]", tm->interval, class_name(obj_class(tm->target)), sel_name(sel_canon(tm->sel)));
    return obj_autorelease(t);
}
SHIM(t_invalidate) { Timer *tm = P(m->self); tm->valid = 0; return 0; }
SHIM(t_isValid) { return ((Timer *)P(m->self))->valid; }
SHIM(t_userInfo) { return ((Timer *)P(m->self))->info; }
SHIM(t_dealloc) { Timer *tm = P(m->self); obj_release(tm->target); obj_release(tm->info); obj_dealloc_memory(m->self); return 0; }
/* fire every due timer once; repeating timers keep their phase and skip missed fires, as NSRunLoop does */
void timers_fire(double now) {
    for (int i = 0; i < ntimers; i++) {
        u32 t = timers[i]; Timer *tm = P(t);
        if (tm->valid && now >= tm->fire) {
            pool_push();
            u32 a[1] = { t }; msg_send(tm->target, tm->sel, a, 1);
            pool_pop();
            if (tm->repeats) { tm->fire += tm->interval; if (tm->fire <= now) tm->fire = now + tm->interval - __builtin_fmod(now - tm->fire, tm->interval); }
            else tm->valid = 0;
        }
        if (!tm->valid) { timers[i] = timers[--ntimers]; obj_release(t); i--; }
    }
}
EXPORT(mgst_next_timer) double mgst_next_timer(void) {
    double t = 1e30; for (int i = 0; i < ntimers; i++) { Timer *tm = P(timers[i]); if (tm->valid && tm->fire < t) t = tm->fire; }
    return t;
}

/* ================================================================== NSThread, NSLock, pools */
SHIM(th_detach) { obj_retain(AU(1)); obj_retain(AU(2)); host_thread_start(AU(1), AU(0), AU(2)); return 0; }
SHIM(th_sleep) { host_sleep(AD(0)); return 0; }
SHIM(th_exit) { return 0; }
SHIM(th_isMainThread) { return 1; }
EXPORT(mgst_thread_entry) void mgst_thread_entry(u32 target, u32 sel, u32 arg) {
    pool_push(); u32 a[1] = { arg }; msg_send(target, sel, a, 1); pool_pop();
}
/* all game code runs under the host's single game lock, released only in host_sleep;
 * NSLock therefore never contends and only needs to keep a balanced count */
SHIM(lk_lock) { WR32(m->self + 4, RD32(m->self + 4) + 1); return 0; }
SHIM(lk_unlock) { WR32(m->self + 4, RD32(m->self + 4) - 1); return 0; }
SHIM(lk_tryLock) { if (RD32(m->self + 4)) return 0; WR32(m->self + 4, 1); return 1; }
SHIM(ap_init) { pool_push(); return m->self; }
SHIM(ap_release) { pool_pop(); obj_dealloc_memory(m->self); return 0; }
SHIM(ap_retain) { return m->self; }
SHIM(ap_autorelease) { return m->self; }
SHIM(ex_raise) {
    char b[1024]; fmt_words(b, sizeof b, str_utf8(AU(1)), m->a + 2, m->n - 2, NULL);
    rt_fatal("NSException %s: %s", str_utf8(AU(0)), b);
}
SHIM(err_localizedDescription) { return str_new_utf8("error"); }

/* NSURL / NSURLRequest: only ever built for the dead online features */
SHIM(url_with) { u32 o = obj_autorelease(obj_alloc(C_NSURL, 12)); WR32(o + 4, obj_retain(AU(0))); return o; }
SHIM(url_path) { return RD32(m->self + 4); }
SHIM(req_with) { u32 o = obj_autorelease(obj_alloc(C_NSURLRequest, 12)); WR32(o + 4, obj_retain(AU(0))); return o; }

/* ================================================================== NSLog */
int fmt_object_desc(u32 obj, char *buf, int cap) {
    if (!obj) { strcpy(buf, "(null)"); return 6; }
    u32 d = (u32)msg_sendv(obj, "description", 0);
    const char *s = d ? str_utf8(d) : "(null)"; int n = (int)strlen(s);
    if (n >= cap) n = cap - 1;
    memcpy(buf, s, n); buf[n] = 0; return n;
}
void _NSLog__w(void *format, const u32 *w, int n) {
    pool_push();
    char *b = malloc(4096); fmt_words(b, 4096, str_utf8(A(format)), w, n, NULL);
    host_log(b); free(b);
    pool_pop();
}

/* ================================================================== registration */
#define M(c, s, f) rt_method(c, s, f)
#define CM(c, s, f) rt_cmethod(c, s, f)
void rt_init_foundation(void) {
    u32 c;
    c = C_NSObject = rt_class_new("NSObject", NULL, 4);
    CM(c, "alloc", o_alloc); CM(c, "allocWithZone:", o_alloc); CM(c, "new", o_new); CM(c, "class", o_self);
    CM(c, "self", o_self); CM(c, "retain", o_self); CM(c, "release", o_self); CM(c, "autorelease", o_self);
    CM(c, "respondsToSelector:", o_respondsToSelector); CM(c, "instancesRespondToSelector:", o_instancesRespond);
    CM(c, "description", o_cls_description); CM(c, "superclass", o_superclass); CM(c, "isKindOfClass:", o_isKindOfClass);
    CM(c, "initialize", o_self);
    M(c, "init", o_self); M(c, "self", o_self); M(c, "dealloc", o_dealloc); M(c, "retain", o_retain); M(c, "release", o_release);
    M(c, "autorelease", o_autorelease); M(c, "retainCount", o_retainCount); M(c, "class", o_class); M(c, "superclass", o_superclass);
    M(c, "isKindOfClass:", o_isKindOfClass); M(c, "isMemberOfClass:", o_isMemberOfClass); M(c, "respondsToSelector:", o_respondsToSelector);
    M(c, "performSelector:", o_perform0); M(c, "performSelector:withObject:", o_perform1); M(c, "performSelector:withObject:withObject:", o_perform2);
    M(c, "hash", o_hash); M(c, "isEqual:", o_isEqual); M(c, "copy", o_copy); M(c, "description", o_description);

    c = C_NSString = rt_class_new("NSString", "NSObject", sizeof(Str));
    CM(c, "alloc", s_alloc); CM(c, "stringWithFormat:", s_stringWithFormat); CM(c, "stringWithCString:encoding:", s_stringWithCString);
    CM(c, "stringWithCString:", s_stringWithCString); CM(c, "stringWithUTF8String:", s_stringWithUTF8String); CM(c, "string", s_string);
    CM(c, "stringWithString:", s_stringWithString); CM(c, "stringWithContentsOfFile:encoding:error:", s_stringWithContentsOfFile);
    M(c, "init", s_init); M(c, "initWithBytes:length:encoding:", s_initWithBytes); M(c, "initWithData:encoding:", s_initWithData);
    M(c, "initWithString:", s_initWithString); M(c, "initWithUTF8String:", s_initWithUTF8String); M(c, "initWithFormat:", s_initWithFormat);
    M(c, "initWithCapacity:", s_initWithCapacity);
    M(c, "UTF8String", s_UTF8String); M(c, "cStringUsingEncoding:", s_cStringUsingEncoding); M(c, "cString", s_UTF8String);
    M(c, "length", s_length); M(c, "characterAtIndex:", s_characterAtIndex); M(c, "isEqualToString:", s_isEqualToString);
    M(c, "isEqual:", s_isEqual); M(c, "hash", s_hash); M(c, "copy", s_copy); M(c, "mutableCopy", s_mutableCopy); M(c, "description", s_description);
    M(c, "intValue", s_intValue); M(c, "integerValue", s_intValue); M(c, "floatValue", s_floatValue); M(c, "doubleValue", s_doubleValue); M(c, "boolValue", s_boolValue);
    M(c, "substringWithRange:", s_substringWithRange); M(c, "substringFromIndex:", s_substringFromIndex); M(c, "substringToIndex:", s_substringToIndex);
    M(c, "stringByAppendingString:", s_stringByAppendingString); M(c, "componentsSeparatedByString:", s_components);
    M(c, "lineRangeForRange:", s_lineRangeForRange); M(c, "rangeOfString:", s_rangeOfString); M(c, "hasPrefix:", s_hasPrefix); M(c, "hasSuffix:", s_hasSuffix);
    M(c, "dataUsingEncoding:", s_dataUsingEncoding); M(c, "pathExtension", s_pathExtension); M(c, "lastPathComponent", s_lastPathComponent);
    M(c, "stringByAppendingPathComponent:", s_stringByAppendingPathComponent); M(c, "dealloc", s_dealloc);
    M(c, "drawInRect:withFont:lineBreakMode:alignment:", str_drawInRect); M(c, "sizeWithFont:", str_sizeWithFont);
    M(c, "drawAtPoint:withFont:", str_drawAtPoint); M(c, "sizeWithFont:constrainedToSize:lineBreakMode:", str_sizeWithFontConstrained);
    c = C_NSMutableString = rt_class_new("NSMutableString", "NSString", sizeof(Str));
    M(c, "appendString:", s_appendString); M(c, "appendFormat:", s_appendFormat); M(c, "setString:", s_setString);
    CM(c, "string", s_string); CM(c, "stringWithCapacity:", s_string);
    c = C_ConstStr = rt_class_new("__NSCFConstantString", "NSString", 16);
    M(c, "retain", o_self); M(c, "release", o_self); M(c, "autorelease", o_self); M(c, "dealloc", o_self); M(c, "copy", o_self);

    c = C_NSArray = rt_class_new("NSArray", "NSObject", sizeof(Arr));
    CM(c, "alloc", a_alloc); CM(c, "array", a_array); CM(c, "arrayWithObjects:", a_arrayWithObjects); CM(c, "arrayWithArray:", a_arrayWithArray);
    M(c, "init", a_init); M(c, "initWithObjects:", a_initWithObjects); M(c, "count", a_count); M(c, "objectAtIndex:", a_objectAtIndex);
    M(c, "lastObject", a_lastObject); M(c, "containsObject:", a_containsObject); M(c, "indexOfObject:", a_indexOfObject);
    M(c, "countByEnumeratingWithState:objects:count:", a_countByEnumerating); M(c, "reverseObjectEnumerator", a_reverseObjectEnumerator);
    M(c, "objectEnumerator", a_objectEnumerator); M(c, "dealloc", a_dealloc);
    c = C_NSMutableArray = rt_class_new("NSMutableArray", "NSArray", sizeof(Arr));
    CM(c, "arrayWithCapacity:", a_array); M(c, "initWithCapacity:", a_initWithCapacity);
    M(c, "addObject:", a_addObject); M(c, "insertObject:atIndex:", a_insertObject); M(c, "removeObjectAtIndex:", a_removeObjectAtIndex);
    M(c, "removeObject:", a_removeObject); M(c, "removeLastObject", a_removeLastObject); M(c, "removeAllObjects", a_removeAllObjects);
    M(c, "sortUsingFunction:context:", a_sortUsingFunction);
    c = C_RevEnum = rt_class_new("__NSArrayEnumerator", "NSArray", sizeof(Arr));
    M(c, "nextObject", en_nextObject); M(c, "allObjects", en_allObjects);

    c = C_NSDictionary = rt_class_new("NSDictionary", "NSObject", sizeof(Dict));
    CM(c, "alloc", d_alloc); CM(c, "dictionary", d_dictionary); CM(c, "dictionaryWithObject:forKey:", d_dictionaryWithObjectForKey);
    CM(c, "dictionaryWithObjectsAndKeys:", d_dictionaryWithObjectsAndKeys);
    M(c, "init", d_init); M(c, "initWithObjectsAndKeys:", d_initWithObjectsAndKeys); M(c, "objectForKey:", d_objectForKey);
    M(c, "count", d_count); M(c, "allKeys", d_allKeys); M(c, "allValues", d_allValues); M(c, "dealloc", d_dealloc);
    M(c, "countByEnumeratingWithState:objects:count:", d_countByEnumerating); M(c, "valueForKey:", d_objectForKey);
    c = C_NSMutableDictionary = rt_class_new("NSMutableDictionary", "NSDictionary", sizeof(Dict));
    CM(c, "dictionaryWithCapacity:", d_dictionary);
    M(c, "setObject:forKey:", d_setObjectForKey); M(c, "setValue:forKey:", d_setObjectForKey); M(c, "removeObjectForKey:", d_removeObjectForKey);

    c = C_NSNumber = rt_class_new("NSNumber", "NSObject", 16);
    CM(c, "numberWithInt:", n_numberWithInt); CM(c, "numberWithInteger:", n_numberWithInt); CM(c, "numberWithBool:", n_numberWithBool);
    CM(c, "numberWithFloat:", n_numberWithFloat); CM(c, "numberWithDouble:", n_numberWithDouble);
    M(c, "intValue", n_intValue); M(c, "integerValue", n_intValue); M(c, "boolValue", n_boolValue); M(c, "floatValue", n_floatValue);
    M(c, "doubleValue", n_doubleValue); M(c, "isEqual:", n_isEqual); M(c, "isEqualToNumber:", n_isEqual); M(c, "description", n_description);
    M(c, "stringValue", n_description);

    c = C_NSData = rt_class_new("NSData", "NSObject", 16);
    CM(c, "dataWithContentsOfFile:", da_dataWithContentsOfFile); CM(c, "dataWithBytes:length:", da_dataWithBytes);
    M(c, "bytes", da_bytes); M(c, "length", da_length); M(c, "getBytes:", da_getBytes); M(c, "getBytes:length:", da_getBytesLength); M(c, "dealloc", da_dealloc);
    C_NSMutableData = rt_class_new("NSMutableData", "NSData", 16);
    M(C_NSMutableData, "mutableBytes", da_bytes);

    c = C_NSSet = rt_class_new("NSSet", "NSObject", 16);
    M(c, "count", set_count); M(c, "anyObject", set_anyObject); M(c, "allObjects", set_allObjects);
    M(c, "countByEnumeratingWithState:objects:count:", set_countByEnumerating); M(c, "containsObject:", set_containsObject); M(c, "dealloc", set_dealloc);

    c = C_NSBundle = rt_class_new("NSBundle", "NSObject", 8);
    CM(c, "mainBundle", b_mainBundle); M(c, "pathForResource:ofType:", b_pathForResource);
    M(c, "pathForResource:ofType:inDirectory:", b_pathForResourceInDir); M(c, "bundlePath", b_bundlePath); M(c, "resourcePath", b_bundlePath);
    M(c, "localizedStringForKey:value:table:", b_localizedString); M(c, "infoDictionary", b_infoDictionary);
    c = C_NSFileManager = rt_class_new("NSFileManager", "NSObject", 8);
    CM(c, "defaultManager", fm_defaultManager); M(c, "changeCurrentDirectoryPath:", fm_changeCwd); M(c, "fileExistsAtPath:", fm_fileExists);
    M(c, "createDirectoryAtPath:attributes:", fm_createDirectory); M(c, "removeItemAtPath:error:", fm_removeItem);
    c = C_NSUserDefaults = rt_class_new("NSUserDefaults", "NSObject", 8);
    CM(c, "standardUserDefaults", ud_standard); M(c, "registerDefaults:", ud_registerDefaults); M(c, "objectForKey:", ud_objectForKey);
    M(c, "integerForKey:", ud_integerForKey); M(c, "boolForKey:", ud_boolForKey); M(c, "floatForKey:", ud_floatForKey);
    M(c, "setObject:forKey:", ud_setObject); M(c, "setInteger:forKey:", ud_setInteger); M(c, "setBool:forKey:", ud_setBool); M(c, "synchronize", ud_synchronize);
    c = C_NSNotificationCenter = rt_class_new("NSNotificationCenter", "NSObject", 8);
    CM(c, "defaultCenter", nc_default); M(c, "addObserver:selector:name:object:", nc_addObserver);
    M(c, "removeObserver:name:object:", nc_removeObserver); M(c, "removeObserver:", nc_removeObserver);
    M(c, "postNotificationName:object:", nc_post);
    c = C_NSNotification = rt_class_new("NSNotification", "NSObject", 16);
    M(c, "name", nt_name); M(c, "object", nt_object);
    c = C_NSTimer = rt_class_new("NSTimer", "NSObject", sizeof(Timer));
    CM(c, "scheduledTimerWithTimeInterval:target:selector:userInfo:repeats:", t_scheduled);
    M(c, "invalidate", t_invalidate); M(c, "isValid", t_isValid); M(c, "userInfo", t_userInfo); M(c, "dealloc", t_dealloc);
    c = C_NSThread = rt_class_new("NSThread", "NSObject", 8);
    CM(c, "detachNewThreadSelector:toTarget:withObject:", th_detach); CM(c, "sleepForTimeInterval:", th_sleep);
    CM(c, "exit", th_exit); CM(c, "isMainThread", th_isMainThread);
    c = C_NSLock = rt_class_new("NSLock", "NSObject", 8);
    M(c, "lock", lk_lock); M(c, "unlock", lk_unlock); M(c, "tryLock", lk_tryLock);
    c = C_NSAutoreleasePool = rt_class_new("NSAutoreleasePool", "NSObject", 8);
    M(c, "init", ap_init); M(c, "release", ap_release); M(c, "drain", ap_release); M(c, "retain", ap_retain); M(c, "autorelease", ap_autorelease);
    c = C_NSException = rt_class_new("NSException", "NSObject", 8);
    CM(c, "raise:format:", ex_raise);
    c = C_NSError = rt_class_new("NSError", "NSObject", 8);
    M(c, "localizedDescription", err_localizedDescription);
    c = C_NSURL = rt_class_new("NSURL", "NSObject", 12);
    CM(c, "URLWithString:", url_with); CM(c, "fileURLWithPath:", url_with); M(c, "path", url_path); M(c, "absoluteString", url_path);
    c = C_NSURLRequest = rt_class_new("NSURLRequest", "NSObject", 12);
    CM(c, "requestWithURL:", req_with);
}
