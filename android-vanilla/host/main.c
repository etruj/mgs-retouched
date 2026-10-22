/* Android shell: NativeActivity + EGL/GLES 1.1 + AAudio + touch + accelerometer + vibrator.
 * The original bundle is read in place from the APK (assets/MGSTUS.app); saves go to
 * the app's internal storage. Portrait-locked, like the iPhone: the game itself draws
 * its landscape scenes rotated, and the player turns the phone.
 * Built with -DMGST_RETOUCHED (android-retouched/build_retouched.sh) it is METAL GEAR SOLID
 * RE-TOUCHED: + the Re-Touched overlay (android-retouched/host), Back key, game controllers,
 * gyroscope, full-screen mode (RetouchedActivity.java) and pause-on-leave. */
#include "host.h"
#include <jni.h>
#ifdef MGST_RETOUCHED
#include "rtx_host.h"
#define PKG "com.mgsport.retouched"
#else
#define PKG "com.mgsport.touch"
#endif
#include <android_native_app_glue.h>
#include <android/log.h>
#include <android/sensor.h>
#include <android/window.h>
#include <aaudio/AAudio.h>
#include <EGL/egl.h>
#include <GLES/gl.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

extern AAssetManager *g_assets;
static ANativeActivity *g_activity;
/* JNI from the game thread (android_main's thread, attached once) */
static JNIEnv *jni(void) {
    JNIEnv *env = NULL;
    if ((*g_activity->vm)->GetEnv(g_activity->vm, (void **)&env, JNI_VERSION_1_6) != JNI_OK)
        (*g_activity->vm)->AttachCurrentThread(g_activity->vm, &env, NULL);
    return env;
}
static void jni_clear(JNIEnv *env) { if ((*env)->ExceptionCheck(env)) { (*env)->ExceptionDescribe(env); (*env)->ExceptionClear(env); } }
/* the original's vibration (AudioServicesPlaySystemSound(kSystemSoundID_Vibrate), sent by the game
 * when its own VIBRATION option is on): Context.getSystemService("vibrator").vibrate(ms) */
static void android_vibrate(int ms) {
    JNIEnv *env = jni(); if (!env) return;
    jobject act = g_activity->clazz;
    jclass ac = (*env)->GetObjectClass(env, act);
    jmethodID gss = (*env)->GetMethodID(env, ac, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    jstring name = (*env)->NewStringUTF(env, "vibrator");
    jobject vib = gss ? (*env)->CallObjectMethod(env, act, gss, name) : NULL;
    jni_clear(env);
    if (vib) {
        jclass vc = (*env)->GetObjectClass(env, vib);
        jmethodID m = (*env)->GetMethodID(env, vc, "vibrate", "(J)V");
        if (m) (*env)->CallVoidMethod(env, vib, m, (jlong)ms);
        jni_clear(env);
        (*env)->DeleteLocalRef(env, vc); (*env)->DeleteLocalRef(env, vib);
    }
    (*env)->DeleteLocalRef(env, name); (*env)->DeleteLocalRef(env, ac);
}
#ifdef MGST_RETOUCHED
/* RetouchedActivity.setImmersive(boolean): hides the status and navigation bars on the UI thread */
void rtx_plat_immersive(int on) {
    JNIEnv *env = jni(); if (!env) return;
    jclass ac = (*env)->GetObjectClass(env, g_activity->clazz);
    jmethodID m = (*env)->GetMethodID(env, ac, "setImmersive", "(Z)V");
    if (m) (*env)->CallVoidMethod(env, g_activity->clazz, m, (jboolean)(on != 0));
    jni_clear(env); (*env)->DeleteLocalRef(env, ac);
}
/* save transfer: RetouchedActivity opens Android's file picker (works with Downloads, Drive, ...)
 * and answers through nativeResult() on its UI thread */
static void call_activity(const char *name, const char *sig, jbyteArray arg) {
    JNIEnv *env = jni(); if (!env) return;
    jclass ac = (*env)->GetObjectClass(env, g_activity->clazz);
    jmethodID m = (*env)->GetMethodID(env, ac, name, sig);
    if (m) { if (arg) (*env)->CallVoidMethod(env, g_activity->clazz, m, arg); else (*env)->CallVoidMethod(env, g_activity->clazz, m); }
    jni_clear(env); (*env)->DeleteLocalRef(env, ac);
}
void rtx_plat_export_save(const uint8_t *d, int n) {
    JNIEnv *env = jni(); if (!env) return;
    jbyteArray a = (*env)->NewByteArray(env, n); (*env)->SetByteArrayRegion(env, a, 0, n, (const jbyte *)d);
    call_activity("exportSave", "([B)V", a); (*env)->DeleteLocalRef(env, a);
}
void rtx_plat_import_save(void) { call_activity("importSave", "()V", NULL); }
void rtx_plat_restart(void) { call_activity("restartGame", "()V", NULL); }
JNIEXPORT void JNICALL Java_com_mgsport_retouched_RetouchedActivity_nativeResult(JNIEnv *env, jobject self, jint kind, jbyteArray data) {
    (void)self; jsize n = data ? (*env)->GetArrayLength(env, data) : 0; jbyte *b = n ? (*env)->GetByteArrayElements(env, data, NULL) : NULL;
    rtx_io_result((int)kind, (const uint8_t *)b, (int)n);
    if (b) (*env)->ReleaseByteArrayElements(env, data, b, JNI_ABORT);
}
#endif
void hlog_platform(const char *line) { __android_log_write(ANDROID_LOG_INFO, "mgst", line); }
void *gfx_getproc(const char *n) { return (void *)eglGetProcAddress(n); }

static EGLDisplay dpy = EGL_NO_DISPLAY; static EGLSurface surf = EGL_NO_SURFACE; static EGLContext ctx = EGL_NO_CONTEXT;
static EGLConfig cfg;
static int started, resumed, have_window;
void platform_swap(void) { if (surf != EGL_NO_SURFACE) eglSwapBuffers(dpy, surf); }

static int egl_init(ANativeWindow *win) {
    if (dpy == EGL_NO_DISPLAY) {
        dpy = eglGetDisplay(EGL_DEFAULT_DISPLAY); eglInitialize(dpy, 0, 0);
        const EGLint ca[] = { EGL_RENDERABLE_TYPE, EGL_OPENGL_ES_BIT, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                              EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_NONE };
        EGLint n; if (!eglChooseConfig(dpy, ca, &cfg, 1, &n) || n < 1) { hlog("EGL: no GLES 1.x config"); return 0; }
        const EGLint xa[] = { EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE };
        ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, xa);
        if (ctx == EGL_NO_CONTEXT) { hlog("EGL: cannot create a GLES 1.1 context"); return 0; }
    }
    surf = eglCreateWindowSurface(dpy, cfg, win, 0);
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) { hlog("EGL: makeCurrent failed"); return 0; }
    EGLint w, h; eglQuerySurface(dpy, surf, EGL_WIDTH, &w); eglQuerySurface(dpy, surf, EGL_HEIGHT, &h);
    gfx_set_window(w, h); hlog("surface %dx%d", w, h);
    return 1;
}
static void egl_surface_lost(void) {
    if (surf != EGL_NO_SURFACE) { eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, ctx); eglDestroySurface(dpy, surf); surf = EGL_NO_SURFACE; }
}

/* ---- audio ---- */
static AAudioStream *astream;
static aaudio_data_callback_result_t audio_cb(AAudioStream *s, void *u, void *data, int32_t frames) {
    (void)s; (void)u; audio_mix((int16_t *)data, frames); return AAUDIO_CALLBACK_RESULT_CONTINUE;
}
static void audio_start(void) {
    if (astream) { AAudioStream_requestStart(astream); return; }
    AAudioStreamBuilder *b; if (AAudio_createStreamBuilder(&b) != AAUDIO_OK) return;
    AAudioStreamBuilder_setFormat(b, AAUDIO_FORMAT_PCM_I16); AAudioStreamBuilder_setChannelCount(b, 2);
    AAudioStreamBuilder_setSampleRate(b, 44100); AAudioStreamBuilder_setPerformanceMode(b, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setDataCallback(b, audio_cb, NULL);
    if (AAudioStreamBuilder_openStream(b, &astream) == AAUDIO_OK) {
        audio_init(AAudioStream_getSampleRate(astream)); AAudioStream_requestStart(astream);
        hlog("audio: %d Hz", AAudioStream_getSampleRate(astream));
    } else hlog("audio: AAudio unavailable");
    AAudioStreamBuilder_delete(b);
}
static void audio_pause(void) { if (astream) AAudioStream_requestPause(astream); }

/* ---- accelerometer ---- */
static ASensorManager *sm; static const ASensor *accel, *gyro; static ASensorEventQueue *sq;
static int focused, gyro_on; static int64_t gyro_ts;
static void sensors_enable(int on) {
    focused = on;
    if (!sq || !accel) return;
    if (on) { ASensorEventQueue_enableSensor(sq, accel); ASensorEventQueue_setEventRate(sq, accel, 16667); }
    else ASensorEventQueue_disableSensor(sq, accel);
}
#ifdef MGST_RETOUCHED
/* the gyroscope runs only while gyro aiming is switched on (battery) */
static void gyro_update(void) {
    int want = focused && gyro && rtx_gyro_wanted();
    if (want == gyro_on || !sq) return;
    gyro_on = want; gyro_ts = 0;
    if (want) { ASensorEventQueue_enableSensor(sq, gyro); ASensorEventQueue_setEventRate(sq, gyro, 10000); }
    else ASensorEventQueue_disableSensor(sq, gyro);
}
/* game controllers: the stick or d-pad moves a cursor, A (or a trigger) is the finger */
static int pad_key(int32_t code, int down) {
    switch (code) {
    case AKEYCODE_BUTTON_A: case AKEYCODE_BUTTON_X: case AKEYCODE_BUTTON_R1: case AKEYCODE_BUTTON_L1:
    case AKEYCODE_BUTTON_R2: case AKEYCODE_BUTTON_L2: case AKEYCODE_DPAD_CENTER:
        rtx_pad_button(down); return 1;
    case AKEYCODE_BUTTON_B: if (!down) rtx_back(); return 1;
    case AKEYCODE_BUTTON_START: case AKEYCODE_MENU: if (!down) rtx_start_button(); return 1;
    case AKEYCODE_DPAD_UP: rtx_pad_axis(0, down ? -1 : 0); return 1;
    case AKEYCODE_DPAD_DOWN: rtx_pad_axis(0, down ? 1 : 0); return 1;
    case AKEYCODE_DPAD_LEFT: rtx_pad_axis(down ? -1 : 0, 0); return 1;
    case AKEYCODE_DPAD_RIGHT: rtx_pad_axis(down ? 1 : 0, 0); return 1;
    }
    return 0;
}
#endif

/* ---- input: Android pointers -> UITouch ---- */
static int32_t on_input(struct android_app *app, AInputEvent *ev) {
    (void)app;
    if (!started) return 0;
#ifdef MGST_RETOUCHED
    if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_KEY) {
        int32_t code = AKeyEvent_getKeyCode(ev), down = AKeyEvent_getAction(ev) == AKEY_EVENT_ACTION_DOWN;
        if (AKeyEvent_getRepeatCount(ev) > 0) return 1;
        if (code == AKEYCODE_BACK) return rtx_back_wanted() ? (down ? 1 : rtx_back()) : 0;   /* else Android's default */
        return pad_key(code, down);
    }
    if (AInputEvent_getType(ev) == AINPUT_EVENT_TYPE_MOTION
        && (AInputEvent_getSource(ev) & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK) {   /* sticks, d-pad hat, triggers */
        float x = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_X, 0), y = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_Y, 0);
        float hx = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HAT_X, 0), hy = AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_HAT_Y, 0);
        rtx_pad_axis(fabsf(hx) > fabsf(x) ? hx : x, fabsf(hy) > fabsf(y) ? hy : y);
        float trig = fmaxf(fmaxf(AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_RTRIGGER, 0), AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_LTRIGGER, 0)),
                           fmaxf(AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_GAS, 0), AMotionEvent_getAxisValue(ev, AMOTION_EVENT_AXIS_BRAKE, 0)));
        static int trig_down; int td = trig > 0.5f;
        if (td != trig_down) { trig_down = td; rtx_pad_button(td); }
        return 1;
    }
#endif
    if (AInputEvent_getType(ev) != AINPUT_EVENT_TYPE_MOTION) return 0;
    int32_t act = AMotionEvent_getAction(ev), kind = act & AMOTION_EVENT_ACTION_MASK;
    size_t idx = (act & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    float vx, vy;
    switch (kind) {
    case AMOTION_EVENT_ACTION_DOWN: case AMOTION_EVENT_ACTION_POINTER_DOWN:
    case AMOTION_EVENT_ACTION_UP: case AMOTION_EVENT_ACTION_POINTER_UP: {
        gfx_window_to_view(AMotionEvent_getX(ev, idx), AMotionEvent_getY(ev, idx), &vx, &vy);
        int down = kind == AMOTION_EVENT_ACTION_DOWN || kind == AMOTION_EVENT_ACTION_POINTER_DOWN;
        host_touch(down ? 0 : 3, AMotionEvent_getPointerId(ev, idx) & 7, vx, vy);
        break;
    }
    case AMOTION_EVENT_ACTION_MOVE:
        for (size_t i = 0; i < AMotionEvent_getPointerCount(ev); i++) {
            gfx_window_to_view(AMotionEvent_getX(ev, i), AMotionEvent_getY(ev, i), &vx, &vy);
            host_touch(1, AMotionEvent_getPointerId(ev, i) & 7, vx, vy);
        }
        break;
    case AMOTION_EVENT_ACTION_CANCEL:
        for (size_t i = 0; i < AMotionEvent_getPointerCount(ev); i++) host_touch(4, AMotionEvent_getPointerId(ev, i) & 7, 0, 0);
        break;
    }
    return 1;
}

static void pick_language(struct android_app *app) {
    char lang[3] = { 0 };
    AConfiguration_getLanguage(app->config, lang);
    const char *map[][2] = { { "ja", "Japanese" }, { "fr", "French" }, { "de", "German" }, { "it", "Italian" }, { "es", "Spanish" } };
    snprintf(g_cfg.lang, sizeof g_cfg.lang, "English");
    for (int i = 0; i < 5; i++) if (!strcmp(lang, map[i][0])) snprintf(g_cfg.lang, sizeof g_cfg.lang, "%s", map[i][1]);
}

static void on_cmd(struct android_app *app, int32_t cmd) {
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        if (!app->window || !egl_init(app->window)) break;
        have_window = 1;
        if (!started) {
            gfx_init(320, 480);
            if (text_init("@app/fonts/LiberationSans-Regular.ttf", "@app/fonts/LiberationSans-Bold.ttf", "@app/fonts/DroidSansFallbackFull.ttf",
#ifdef MGST_RETOUCHED
                          "@app/fonts/BebasNeue-Regular.ttf"))
#else
                          NULL))
#endif
                hlog("text: fonts missing from the APK");
            audio_start();
            started = host_start() == 0;
            if (!started) hlog("boot failed");
        }
        break;
    case APP_CMD_TERM_WINDOW: have_window = 0; egl_surface_lost(); break;
    case APP_CMD_WINDOW_RESIZED: case APP_CMD_CONFIG_CHANGED:
        if (surf != EGL_NO_SURFACE) { EGLint w, h; eglQuerySurface(dpy, surf, EGL_WIDTH, &w); eglQuerySurface(dpy, surf, EGL_HEIGHT, &h); gfx_set_window(w, h); }
        break;
    case APP_CMD_GAINED_FOCUS: sensors_enable(1); break;
    case APP_CMD_LOST_FOCUS: sensors_enable(0); break;
#ifdef MGST_RETOUCHED
    case APP_CMD_RESUME: resumed = 1; if (started) { audio_start(); rtx_app_resume(); } break;
    case APP_CMD_PAUSE: resumed = 0; if (started) { rtx_app_pause(); audio_pause(); } break;
#else
    case APP_CMD_RESUME: resumed = 1; if (started) { audio_start(); host_lifecycle(1); } break;
    case APP_CMD_PAUSE: resumed = 0; if (started) { host_lifecycle(0); audio_pause(); } break;
#endif
    case APP_CMD_DESTROY: if (started) host_lifecycle(2); break;
    }
}

void android_main(struct android_app *app) {
    g_assets = app->activity->assetManager;
    g_activity = app->activity;
    platform_vibrate = android_vibrate;
    snprintf(g_cfg.app_dir, sizeof g_cfg.app_dir, "@app/MGSTUS.app");
    snprintf(g_cfg.overlay_dir, sizeof g_cfg.overlay_dir, "@app/overlay");
    snprintf(g_cfg.docs_dir, sizeof g_cfg.docs_dir, "%s/Documents", app->activity->internalDataPath);
    pick_language(app);
    hlog("MGS Touch port: bundle in APK, saves in %s, language %s", g_cfg.docs_dir, g_cfg.lang);
#ifdef MGST_RETOUCHED
    rtx_init();                                  /* settings (and language) before the game starts */
    rtx_plat_immersive(1);
#endif
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
    app->onAppCmd = on_cmd; app->onInputEvent = on_input;
    sm = ASensorManager_getInstanceForPackage(PKG);
    accel = sm ? ASensorManager_getDefaultSensor(sm, ASENSOR_TYPE_ACCELEROMETER) : NULL;
    gyro = sm ? ASensorManager_getDefaultSensor(sm, ASENSOR_TYPE_GYROSCOPE) : NULL;
    if (sm) sq = ASensorManager_createEventQueue(sm, app->looper, LOOPER_ID_USER, NULL, NULL);
    for (;;) {
        int timeout = -1;
        if (started && resumed && have_window) {
            double wait = host_next_timer() - host_now();
            timeout = wait <= 0 ? 0 : wait > 0.1 ? 100 : (int)(wait * 1000);
        }
        int events, id; struct android_poll_source *src;
        while ((id = ALooper_pollOnce(timeout, NULL, &events, (void **)&src)) >= 0) {
            if (src) src->process(app, src);
            if (id == LOOPER_ID_USER && sq) {
                ASensorEvent se;
                while (ASensorEventQueue_getEvents(sq, &se, 1) > 0) {
                    if (!started) continue;
                    if (se.type == ASENSOR_TYPE_ACCELEROMETER)
                        host_accel(-se.acceleration.x / 9.80665, -se.acceleration.y / 9.80665, -se.acceleration.z / 9.80665);
#ifdef MGST_RETOUCHED
                    else if (se.type == ASENSOR_TYPE_GYROSCOPE) {
                        double dt = gyro_ts ? (se.timestamp - gyro_ts) * 1e-9 : 0; gyro_ts = se.timestamp;
                        if (dt > 0 && dt < 0.1) rtx_gyro(se.vector.x, se.vector.y, dt);
                    }
#endif
                }
            }
            if (app->destroyRequested) { if (astream) AAudioStream_close(astream); return; }
            timeout = 0;
        }
        if (started && resumed && have_window) {
            host_frame(host_now());
#ifdef MGST_RETOUCHED
            rtx_frame(host_now());
            gyro_update();
#endif
        }
    }
}
