/* Included first by every transformed game file (tools/xform.py). Provides the
 * argument-word packing the generated message thunks use and the runtime entry
 * points they call. Words follow armv6 APCS softfp: floats as their bits, 64-bit
 * values as two consecutive words (low first), small integers widened. */
#ifndef PORT_GAME_H
#define PORT_GAME_H
#include "mgst_types.fixed.h"

enum { MSGK_NORMAL = 0, MSGK_STRET = 1, MSGK_SUPER2 = 2 };
unsigned long long objc_msgSend_w(const unsigned *w, int n, int kind);
unsigned long long invoke_orig(unsigned addr, const unsigned *w);
unsigned long long invoke_orig_missing(unsigned addr);
void *rt_icall(unsigned target, int tid);
/* calls through function pointers: an original code address (C++ vtable entry) becomes an
 * adapter with this call site's signature (tools/xform.py, typed indirect calls); genuine
 * function pointers pass through */
#include "icall_types.h"
#define ICALL(p) ((__typeof__(p))rt_icall((unsigned)(unsigned long)(p), ICALL_TID(p)))

#define MSG_PUT(w, n, x) do { __typeof__(x) __t = (x);                               \
    if (sizeof __t < 4) { int __e = __builtin_choose_expr(sizeof(__t) < 4, (int)__t, 0); \
        __builtin_memcpy(&(w)[n], &__e, 4); (n)++; }                                   \
    else { __builtin_memcpy(&(w)[n], &__t, sizeof __t); (n) += (int)((sizeof __t + 3) / 4); } } while (0)
#define MSG_GET(w, k, a) do { __builtin_memcpy(&(a), &(w)[k], sizeof (a)); (k) += (int)((sizeof (a) + 3) / 4); } while (0)

/* variadic extras: C default argument promotions, then words */
static inline void msg_push_w(unsigned *w, int *n, unsigned v) { w[(*n)++] = v; }
static inline void msg_push_d(unsigned *w, int *n, double v) { __builtin_memcpy(&w[*n], &v, 8); *n += 2; }
static inline void msg_push_q(unsigned *w, int *n, unsigned long long v) { __builtin_memcpy(&w[*n], &v, 8); *n += 2; }
#define MSG_PUSHV(w, n, x) _Generic((x) + 0, float: msg_push_d, double: msg_push_d,        \
    long long: msg_push_q, unsigned long long: msg_push_q, default: msg_push_w)(w, &(n), (x))

/* SjLj frames in the V0 functions (docs/V0_AUDIT.md): never unwound through */
#undef __ENTRY_STACK
#define __ENTRY_STACK(o) (*(undefined1 *)((unsigned char *)__builtin_frame_address(0) + (o)))

#include "msg_thunks.h"
#include "variadic_imports.h"
#endif
