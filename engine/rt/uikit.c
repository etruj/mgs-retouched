/* UIKit / EAGL / MediaPlayer subset, the app lifecycle, binding of the image's external
 * symbols, and the entry points the host drives (boot, tick, touch, accelerometer). */
#include "rt.h"
#include "image_meta.h"

static u32 C_UIApplication, C_UIScreen, C_UIDevice, C_UIResponder, C_UIView, C_UIWindow, C_CALayer, C_CAEAGLLayer,
    C_EAGLContext, C_UITouch, C_UIEvent, C_UIAccelerometer, C_UIAcceleration, C_UIAlertView, C_UIWebView,
    C_MPMoviePlayerController;

/* ---- deferred actions, run at the start of the next tick ---- */
typedef struct Deferred { void (*fn)(u32); u32 arg; struct Deferred *next; } Deferred;
static Deferred *deferred;
static void defer(void (*fn)(u32), u32 arg) {
    Deferred *d = calloc(1, sizeof *d); d->fn = fn; d->arg = arg;
    Deferred **pp = &deferred; while (*pp) pp = &(*pp)->next; *pp = d;
}

/* ---- view side data (UIView's own ivars are opaque to the game; subclasses start after them) ---- */
typedef struct View { u32 obj; float frame[4]; u32 layer, superview, subviews, multitouch, opaque; struct View *next; } View;
static View *views[256];
static View *view(u32 o) {
    View **b = &views[(o >> 4) & 255];
    for (View *v = *b; v; v = v->next) if (v->obj == o) return v;
    View *v = calloc(1, sizeof *v); v->obj = o; v->next = *b; *b = v; return v;
}
static void rect_out(u32 stret, const float *r) { for (int i = 0; i < 4; i++) WR32(stret + 4 * i, __builtin_bit_cast(u32, r[i])); }
static const float screen_rect[4] = { 0, 0, 320, 480 };

/* ---- UIApplication ---- */
static u32 the_app, app_delegate, key_window, game_view;
SHIM(app_shared) { return the_app; }
SHIM(app_delegate_get) { return app_delegate; }
SHIM(app_setDelegate) { app_delegate = AU(0); return 0; }
SHIM(app_noop) { return 0; }
SHIM(app_openURL) { u32 u = AU(0); rt_log("openURL: %s (not opened: online features are offline in this port)", u ? str_utf8(RD32(u + 4)) : "nil"); return 0; }
SHIM(app_keyWindow) { return key_window; }
/* ---- UIScreen / UIDevice ---- */
SHIM(scr_main) { static u32 s; if (!s) s = obj_alloc(C_UIScreen, 8); return s; }
SHIM(scr_bounds) { rect_out(m->stret, screen_rect); return 0; }
SHIM(dev_current) { static u32 d; if (!d) d = obj_alloc(C_UIDevice, 8); return d; }
SHIM(dev_model) { return str_new_utf8("iPhone"); }
SHIM(dev_systemVersion) { return str_new_utf8("3.1.3"); }
SHIM(dev_idiom) { return 0; }                     /* UIUserInterfaceIdiomPhone: the iPhone build path */
SHIM(dev_uid) { return str_new_utf8("0000000000000000000000000000000000000000"); }
/* ---- UIView / UIWindow ---- */
SHIM(v_initWithFrame) { View *v = view(m->self); for (int i = 0; i < 4; i++) v->frame[i] = AF(i); return m->self; }
SHIM(v_init) { return m->self; }
SHIM(v_frame) { rect_out(m->stret, view(m->self)->frame); return 0; }
SHIM(v_bounds) { View *v = view(m->self); float r[4] = { 0, 0, v->frame[2], v->frame[3] }; rect_out(m->stret, r); return 0; }
SHIM(v_setFrame) { View *v = view(m->self); for (int i = 0; i < 4; i++) v->frame[i] = AF(i); return 0; }
SHIM(v_layer) {
    View *v = view(m->self);
    if (!v->layer) {
        u32 cls = obj_class(m->self), lc = (u32)msg_sendv(cls, "layerClass", 0);
        v->layer = (u32)msg_sendv((u32)msg_sendv(lc ? lc : C_CALayer, "alloc", 0), "init", 0);
    }
    return v->layer;
}
SHIM(v_layerClass) { return C_CALayer; }
SHIM(v_addSubview) {
    View *v = view(m->self); u32 sub = AU(0);
    if (!v->subviews) v->subviews = arr_new(1);
    arr_add(v->subviews, sub); view(sub)->superview = m->self;
    if (m->self == key_window || obj_is_kind(m->self, C_UIWindow)) {
        if (!obj_is_kind(sub, C_UIWebView) && !game_view) { game_view = sub; rt_log("touch target view: %s", class_name(obj_class(sub))); }
    }
    return 0;
}
SHIM(v_removeFromSuperview) {
    View *v = view(m->self); if (!v->superview) return 0;
    View *s = view(v->superview); v->superview = 0;
    if (s->subviews) { u32 a[1] = { m->self }; msg_send(s->subviews, sel_get("removeObject:"), a, 1); }
    if (game_view == m->self) game_view = 0;
    return 0;
}
SHIM(v_superview) { return view(m->self)->superview; }
SHIM(v_setMultipleTouchEnabled) { view(m->self)->multitouch = AU(0) & 0xff; return 0; }
SHIM(v_noop) { return 0; }
SHIM(v_yes) { return 1; }
SHIM(w_makeKeyAndVisible) { key_window = m->self; return 0; }
SHIM(v_touches) { return 0; }                     /* UIResponder default: ignore */
/* ---- CALayer / CAEAGLLayer {isa, props} ---- */
SHIM(l_init) { return m->self; }
SHIM(l_setDrawableProperties) { WR32(m->self + 4, obj_retain(AU(0))); return 0; }
SHIM(l_drawableProperties) { return RD32(m->self + 4); }
/* ---- EAGLContext {isa, api} ---- */
static u32 cur_context;
SHIM(ctx_initWithAPI) { WR32(m->self + 4, AU(0)); return AU(0) == 1 ? m->self : 0; }   /* kEAGLRenderingAPIOpenGLES1 only */
SHIM(ctx_setCurrent) { cur_context = AU(0); return 1; }
SHIM(ctx_current) { return cur_context; }
SHIM(ctx_renderbufferStorage) { host_glRenderbufferStorageFromDrawable(); return 1; }
SHIM(ctx_present) { host_present(); return 1; }
/* ---- UITouch {isa, x, y, px, py, phase, taps, view, ts(d)} / UIEvent {isa, touches} ---- */
typedef struct { u32 isa; float x, y, px, py; u32 phase, taps, view; double ts; } Touch;
SHIM(t_locationInView) { Touch *t = P(m->self); WR32(m->stret, __builtin_bit_cast(u32, t->x)); WR32(m->stret + 4, __builtin_bit_cast(u32, t->y)); return 0; }
SHIM(t_prevLocationInView) { Touch *t = P(m->self); WR32(m->stret, __builtin_bit_cast(u32, t->px)); WR32(m->stret + 4, __builtin_bit_cast(u32, t->py)); return 0; }
SHIM(t_phase) { return ((Touch *)P(m->self))->phase; }
SHIM(t_tapCount) { return ((Touch *)P(m->self))->taps; }
SHIM(t_view) { return ((Touch *)P(m->self))->view; }
SHIM(t_timestamp) { return RD(((Touch *)P(m->self))->ts); }
SHIM(ev_allTouches) { return RD32(m->self + 4); }
SHIM(ev_touchesForView) { return RD32(m->self + 4); }
/* ---- UIAccelerometer {isa, delegate, interval(d)} / UIAcceleration {isa, ts, x, y, z (doubles)} ---- */
static u32 the_accel; static double accel_interval = 0.1, accel_last;
SHIM(acc_shared) { if (!the_accel) the_accel = obj_alloc(C_UIAccelerometer, 16); return the_accel; }
SHIM(acc_setDelegate) { WR32(m->self + 4, AU(0)); return 0; }
SHIM(acc_delegate) { return RD32(m->self + 4); }
SHIM(acc_setUpdateInterval) { accel_interval = AD(0); return 0; }
typedef struct { u32 isa, pad; double ts, x, y, z; } Accel;
SHIM(a_x) { return RD(((Accel *)P(m->self))->x); }
SHIM(a_y) { return RD(((Accel *)P(m->self))->y); }
SHIM(a_z) { return RD(((Accel *)P(m->self))->z); }
SHIM(a_timestamp) { return RD(((Accel *)P(m->self))->ts); }
/* ---- UIAlertView: shown in the log; the cancel button is "pressed" on the next tick ---- */
static void alert_dismiss(u32 alert) {
    u32 del = RD32(alert + 4);
    if (del && obj_responds(del, sel_get("alertView:clickedButtonAtIndex:"))) msg_sendv(del, "alertView:clickedButtonAtIndex:", 2, alert, 0);
    if (del && obj_responds(del, sel_get("alertView:didDismissWithButtonIndex:"))) msg_sendv(del, "alertView:didDismissWithButtonIndex:", 2, alert, 0);
    obj_release(alert);
}
SHIM(al_init) {
    WR32(m->self + 4, AU(2)); WR32(m->self + 8, AU(0) ? obj_retain(AU(0)) : 0); WR32(m->self + 12, AU(1) ? obj_retain(AU(1)) : 0);
    return m->self;
}
SHIM(al_show) {
    rt_log("UIAlertView: %s / %s", RD32(m->self + 8) ? str_utf8(RD32(m->self + 8)) : "", RD32(m->self + 12) ? str_utf8(RD32(m->self + 12)) : "");
    defer(alert_dismiss, obj_retain(m->self)); return 0;
}
/* ---- UIWebView: the online news panel. Offline: a view that never loads anything ---- */
SHIM(wv_load) { RT_ONCE(rt_log("UIWebView load ignored (online news is offline in this port)")); return 0; }
/* ---- MPMoviePlayerController: no movie files ship in the bundle; finish immediately ---- */
static void movie_finished(u32 mp) { notify_post("MPMoviePlayerPlaybackDidFinishNotification", mp); obj_release(mp); }
SHIM(mp_init) { WR32(m->self + 4, AU(0) ? obj_retain(AU(0)) : 0); return m->self; }
SHIM(mp_play) { rt_log("MPMoviePlayerController play: no movie in the bundle, finishing"); defer(movie_finished, obj_retain(m->self)); return 0; }

/* ================================================================== bindings */
static u32 cell(u32 v) { u32 *c = malloc(4); *c = v; return A(c); }
static u32 const_block(const float *v, int n) { float *b = malloc(n * 4); memcpy(b, v, n * 4); return A(b); }
u32 rt_bind_value(const char *sym) {
    if (!strncmp(sym, "_OBJC_CLASS_$_", 14) || !strncmp(sym, "_OBJC_METACLASS_$_", 18)) {
        int meta = sym[6] == 'M'; const char *n = sym + (meta ? 18 : 14);
        u32 c = class_named(n);
        if (!c) { rt_log("binding: framework class %s not implemented; created empty", n); c = rt_class_new(n, "NSObject", 0); }
        return meta ? RD32(c) : c;
    }
    if (!strcmp(sym, "___CFConstantStringClassReference")) return class_named("__NSCFConstantString");
    static const float zero[6] = { 0 }, ident[6] = { 1, 0, 0, 1, 0, 0 };
    if (!strcmp(sym, "_CGRectZero") || !strcmp(sym, "_CGPointZero") || !strcmp(sym, "_CGSizeZero")) return const_block(zero, 4);
    if (!strcmp(sym, "_CGAffineTransformIdentity")) return const_block(ident, 6);
    static const char *strs[] = { "_kEAGLDrawablePropertyRetainedBacking", "kEAGLDrawablePropertyRetainedBacking",
        "_kEAGLDrawablePropertyColorFormat", "kEAGLDrawablePropertyColorFormat", "_kEAGLColorFormatRGBA8", "EAGLColorFormatRGBA8",
        "_kEAGLColorFormatRGB565", "EAGLColorFormat565", "_NSInternalInconsistencyException", "NSInternalInconsistencyException",
        "_MPMoviePlayerPlaybackDidFinishNotification", "MPMoviePlayerPlaybackDidFinishNotification",
        "_kCFRunLoopCommonModes", "kCFRunLoopCommonModes", "_kCFRunLoopDefaultMode", "kCFRunLoopDefaultMode",
        "_NSDefaultRunLoopMode", "kCFRunLoopDefaultMode", 0 };
    for (int i = 0; strs[i]; i += 2) if (!strcmp(sym, strs[i])) return cell(str_new_utf8_retained(strs[i + 1]));
    if (!strcmp(sym, "_kCFAllocatorDefault")) return cell(0);
    if (!strcmp(sym, "__objc_empty_cache") || !strcmp(sym, "__objc_empty_vtable")) return 0;
    if (!strncmp(sym, "__ZTV", 5) || !strncmp(sym, "_OBJC_EHTYPE_", 13) || strstr(sym, "personality") || !strcmp(sym, "_exit")) return 0;
    rt_log("binding: no value for %s", sym);
    return 0;
}
void rt_init_bindings(void) {
    for (int i = 0; i < img_nbinds; i++) WR32(img_binds[i].addr, rt_bind_value(img_binds[i].sym) + img_binds[i].addend);
}

/* ================================================================== lifecycle and host entry points */
void rt_init_image_classes(void);
extern const unsigned char img_data[]; extern const unsigned img_data_size;
static int booted;
EXPORT(mgst_boot) int mgst_boot(void) {
    memcpy(P(IMG_LO), img_data, img_data_size);              /* the original data image, at its addresses */
    rt_init_foundation(); void rt_init_cg(void); rt_init_cg(); rt_init_uikit();
    rt_init_bindings();
    rt_init_image_classes();
    pool_push();                                              /* _main's NSAutoreleasePool */
    the_app = obj_alloc(C_UIApplication, 16);
    u32 dc = class_named("CrashLandingAppDelegate");
    if (!dc) rt_fatal("CrashLandingAppDelegate not found in the image");
    app_delegate = (u32)msg_sendv((u32)msg_sendv(dc, "alloc", 0), "init", 0);
    rt_log("UIApplicationMain: delegate %s at 0x%x", class_name(dc), app_delegate);
    pool_push();
    msg_sendv(app_delegate, "applicationDidFinishLaunching:", 1, the_app);
    pool_pop();
    booted = 1;
    return 0;
}
int _UIApplicationMain(int argc, char **argv, void *p, void *d) { (void)argc; (void)argv; (void)p; (void)d; return mgst_boot(); }
void aq_service(void);
void accel_deliver(double now);
EXPORT(mgst_tick) void mgst_tick(double now) {
    if (!booted) return;
    while (deferred) { Deferred *d = deferred; deferred = d->next; pool_push(); d->fn(d->arg); pool_pop(); free(d); }
    aq_service();
    accel_deliver(now);
    timers_fire(now);
}
/* touches arrive in view coordinates (320x480 portrait, the iPhone's view space) */
static u32 touch_objs[8];
EXPORT(mgst_touch) void mgst_touch(int phase, int id, float x, float y, double ts) {
    if (!booted || !game_view || id < 0 || id >= 8) return;
    if (id > 0 && !view(game_view)->multitouch) return;
    pool_push();
    u32 t = touch_objs[id];
    if (!t || phase == 0) { if (t) obj_release(t); t = touch_objs[id] = obj_alloc(C_UITouch, sizeof(Touch)); ((Touch *)P(t))->px = x; ((Touch *)P(t))->py = y; }
    Touch *tc = P(t);
    if (phase != 0) { tc->px = tc->x; tc->py = tc->y; }
    tc->x = x; tc->y = y; tc->phase = (u32)phase; tc->taps = 1; tc->view = game_view; tc->ts = ts;
    u32 set = set_new_with(&t, 1);
    u32 ev = obj_autorelease(obj_alloc(C_UIEvent, 8)); WR32(ev + 4, obj_retain(set));
    static const char *sels[] = { "touchesBegan:withEvent:", "touchesMoved:withEvent:", "touchesMoved:withEvent:", "touchesEnded:withEvent:", "touchesCancelled:withEvent:" };
    if (phase == 2) { pool_pop(); return; }                                   /* UITouchPhaseStationary: no message */
    msg_sendv(game_view, sels[phase], 2, set, ev);
    if (phase >= 3) { obj_release(t); touch_objs[id] = 0; }
    pool_pop();
}
static double acc_x, acc_y, acc_z = -1;
EXPORT(mgst_accel) void mgst_accel(double x, double y, double z) { acc_x = x; acc_y = y; acc_z = z; }
void accel_deliver(double now) {
    if (!the_accel || !RD32(the_accel + 4) || now - accel_last < accel_interval) return;
    accel_last = now;
    pool_push();
    u32 a = obj_autorelease(obj_alloc(C_UIAcceleration, sizeof(Accel)));
    Accel *ac = P(a); ac->ts = now; ac->x = acc_x; ac->y = acc_y; ac->z = acc_z;
    msg_sendv(RD32(the_accel + 4), "accelerometer:didAccelerate:", 2, the_accel, a);
    pool_pop();
}
EXPORT(mgst_lifecycle) void mgst_lifecycle(int ev) {
    static const char *sels[] = { "applicationWillResignActive:", "applicationDidBecomeActive:", "applicationWillTerminate:" };
    if (!booted || ev < 0 || ev > 2) return;
    if (obj_responds(app_delegate, sel_get(sels[ev]))) { pool_push(); msg_sendv(app_delegate, sels[ev], 1, the_app); pool_pop(); }
}
extern int rt_trace_msgs;
EXPORT(mgst_trace) void mgst_trace(int on) { rt_trace_msgs = on; }

void rt_init_uikit(void) {
    u32 c;
    c = C_UIResponder = rt_class_new("UIResponder", "NSObject", 4);
    rt_method(c, "touchesBegan:withEvent:", v_touches); rt_method(c, "touchesMoved:withEvent:", v_touches);
    rt_method(c, "touchesEnded:withEvent:", v_touches); rt_method(c, "touchesCancelled:withEvent:", v_touches);
    c = C_UIApplication = rt_class_new("UIApplication", "UIResponder", 16);
    rt_cmethod(c, "sharedApplication", app_shared); rt_method(c, "delegate", app_delegate_get); rt_method(c, "setDelegate:", app_setDelegate);
    rt_method(c, "setNetworkActivityIndicatorVisible:", app_noop); rt_method(c, "setStatusBarHidden:", app_noop);
    rt_method(c, "setStatusBarHidden:animated:", app_noop); rt_method(c, "setIdleTimerDisabled:", app_noop);
    rt_method(c, "setStatusBarOrientation:animated:", app_noop); rt_method(c, "openURL:", app_openURL); rt_method(c, "keyWindow", app_keyWindow);
    c = C_UIScreen = rt_class_new("UIScreen", "NSObject", 8);
    rt_cmethod(c, "mainScreen", scr_main); rt_method(c, "bounds", scr_bounds); rt_method(c, "applicationFrame", scr_bounds);
    c = C_UIDevice = rt_class_new("UIDevice", "NSObject", 8);
    rt_cmethod(c, "currentDevice", dev_current); rt_method(c, "model", dev_model); rt_method(c, "systemVersion", dev_systemVersion);
    rt_method(c, "userInterfaceIdiom", dev_idiom); rt_method(c, "uniqueIdentifier", dev_uid);
    c = C_UIView = rt_class_new("UIView", "UIResponder", 0);
    /* UIView's ivars: the game's UIView subclasses start their ivars at their own instanceStart */
    rt_cmethod(c, "layerClass", v_layerClass);
    rt_method(c, "initWithFrame:", v_initWithFrame); rt_method(c, "init", v_init); rt_method(c, "frame", v_frame);
    rt_method(c, "bounds", v_bounds); rt_method(c, "setFrame:", v_setFrame); rt_method(c, "layer", v_layer);
    rt_method(c, "addSubview:", v_addSubview); rt_method(c, "removeFromSuperview", v_removeFromSuperview); rt_method(c, "superview", v_superview);
    rt_method(c, "setMultipleTouchEnabled:", v_setMultipleTouchEnabled); rt_method(c, "setOpaque:", v_noop);
    rt_method(c, "setAutoresizesSubviews:", v_noop); rt_method(c, "setAutoresizingMask:", v_noop); rt_method(c, "setBackgroundColor:", v_noop);
    rt_method(c, "setUserInteractionEnabled:", v_noop); rt_method(c, "setHidden:", v_noop); rt_method(c, "setAlpha:", v_noop);
    rt_method(c, "setNeedsDisplay", v_noop); rt_method(c, "isOpaque", v_yes); rt_method(c, "setExclusiveTouch:", v_noop);
    rt_method(c, "setTransform:", v_noop); rt_method(c, "setCenter:", v_noop); rt_method(c, "bringSubviewToFront:", v_noop);
    c = C_UIWindow = rt_class_new("UIWindow", "UIView", 0);
    rt_method(c, "makeKeyAndVisible", w_makeKeyAndVisible); rt_method(c, "makeKeyWindow", w_makeKeyAndVisible);
    c = C_CALayer = rt_class_new("CALayer", "NSObject", 16);
    rt_method(c, "init", l_init); rt_method(c, "setOpaque:", v_noop); rt_method(c, "setDrawableProperties:", l_setDrawableProperties);
    rt_method(c, "drawableProperties", l_drawableProperties);
    c = C_CAEAGLLayer = rt_class_new("CAEAGLLayer", "CALayer", 16);
    c = C_EAGLContext = rt_class_new("EAGLContext", "NSObject", 8);
    rt_method(c, "initWithAPI:", ctx_initWithAPI); rt_cmethod(c, "setCurrentContext:", ctx_setCurrent); rt_cmethod(c, "currentContext", ctx_current);
    rt_method(c, "renderbufferStorage:fromDrawable:", ctx_renderbufferStorage); rt_method(c, "presentRenderbuffer:", ctx_present);
    c = C_UITouch = rt_class_new("UITouch", "NSObject", sizeof(Touch));
    rt_method(c, "locationInView:", t_locationInView); rt_method(c, "previousLocationInView:", t_prevLocationInView);
    rt_method(c, "phase", t_phase); rt_method(c, "tapCount", t_tapCount); rt_method(c, "view", t_view); rt_method(c, "timestamp", t_timestamp);
    c = C_UIEvent = rt_class_new("UIEvent", "NSObject", 8);
    rt_method(c, "allTouches", ev_allTouches); rt_method(c, "touchesForView:", ev_touchesForView);
    c = C_UIAccelerometer = rt_class_new("UIAccelerometer", "NSObject", 16);
    rt_cmethod(c, "sharedAccelerometer", acc_shared); rt_method(c, "setDelegate:", acc_setDelegate); rt_method(c, "delegate", acc_delegate);
    rt_method(c, "setUpdateInterval:", acc_setUpdateInterval);
    c = C_UIAcceleration = rt_class_new("UIAcceleration", "NSObject", sizeof(Accel));
    rt_method(c, "x", a_x); rt_method(c, "y", a_y); rt_method(c, "z", a_z); rt_method(c, "timestamp", a_timestamp);
    c = C_UIAlertView = rt_class_new("UIAlertView", "UIView", 16);
    rt_method(c, "initWithTitle:message:delegate:cancelButtonTitle:otherButtonTitles:", al_init); rt_method(c, "show", al_show);
    c = C_UIWebView = rt_class_new("UIWebView", "UIView", 0);
    rt_method(c, "loadRequest:", wv_load); rt_method(c, "loadHTMLString:baseURL:", wv_load); rt_method(c, "setDelegate:", v_noop);
    rt_method(c, "setScalesPageToFit:", v_noop); rt_method(c, "stopLoading", v_noop);
    c = C_MPMoviePlayerController = rt_class_new("MPMoviePlayerController", "NSObject", 8);
    rt_method(c, "initWithContentURL:", mp_init); rt_method(c, "play", mp_play); rt_method(c, "stop", v_noop);
    rt_method(c, "setScalingMode:", v_noop); rt_method(c, "setMovieControlMode:", v_noop); rt_method(c, "setBackgroundColor:", v_noop);
}
