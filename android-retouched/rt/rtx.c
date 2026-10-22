/* METAL GEAR SOLID RETOUCHED: the add-ons that live inside the game module.
 *
 * Only the Re-Touched edition contains this file (engine/tools/build_wasm.sh, MGST_EDITION=retouched);
 * the original edition is built without it and without the hook point in engine/rt/objc.c.
 * Nothing here edits game code: it observes or replaces a few Objective-C messages (every message
 * passes through the port's runtime) and calls the game's own functions.
 *   - the RETOUCHED main-menu entry: a sixth row in menu_0.data (label slot 53, no action for the
 *     game), its label drawn with the game's own text style, and the tap detected the way
 *     -[CrashLandingAppDelegate Menu_Touch:] detects taps;
 *   - pause: the game's own interruption pause (applicationWillResignActive: sets SleepOn, which
 *     stops GameScene/DrawGame and suspends the sound), resumed with applicationDidBecomeActive:;
 *   - skip intro logos, sound test, unlocks, and the hidden modifiers. */
#include "rt.h"

IMPORT(host_rtx_event) void host_rtx_event(int ev, int arg);
IMPORT(host_gl_tint) void host_gl_tint(unsigned tex, float r, float g, float b);   /* draw one texture in a fixed colour */
enum { EV_OPEN_OPTIONS = 1 };

/* typed accessors (rt/game/rtx_fields.c) */
int *rtxf_scene(void *d); int *rtxf_case(void *d); int *rtxf_timecount(void *d);
u8 *rtxf_sleep_on(void *d); u8 *rtxf_start(void *d); u8 *rtxf_inclination(void *d);
int rtxf_menu_blocked(void *d); float rtxf_menu_scroll_y(void *d); void *rtxf_menu_bar(void *d);
void **rtxf_menu_label(void *d, int i); void rtxf_bar_anim(void *d, int row); int rtxf_anim_busy(void *d);
void *rtxf_view(void *d); void *rtxf_font_name(void *d); void rtxf_quit_mission(void *d);
/* the game's own functions (same module) */
void GSoundCtrl__SeSet(int id);
void GSoundCtrl__BgmSet(int id, u8 loop);
void GSoundCtrl__BgmStop(void);
u32 GSaveCtrl__writeApplicationData(char *name);
void GResult__unlockDrebinWallPaper(int a, int b);

/* fixed addresses in the original image (MGSTUS v2.3.0, checked by engine/tools/check_handoff.py) */
#define SAVE_DATA (*(char **)P(0x75778u))          /* GSaveCtrl::SaveData */
#define PLAYER_HP (*(int *)P(0x84b44u))            /* Player_s_hp */
#define PLAYER_MAX_HP (*(int *)P(0x84b48u))        /* Player_s_maxHp */

#define LABEL_SLOT 53          /* tex_menu_str[53]: used by no menu data file and no code (66 is SURVIVAL, put in by title_loadMenuData) */
#define ENTRY_ROW 5            /* the sixth main-menu row */

int rtx_hooks_on;
static int opt_skip_logos, opt_inf_life, opt_one_hit;
static double last_mission;    /* host_time() of the last -[sh_gameMain] */
static int paused;

enum { H_DATAREAD, H_DRAWBAR, H_MENUTOUCH, H_TOUCHOUT, H_TOUCHIN, H_BACK, H_HELP, H_GAMESCENE, H_SHMAIN,
       H_SETDAMAGE, H_FRIENDFIRE, H_SUBDAMAGE, H_COUNT };
static const char *hook_names[H_COUNT] = { "DataRead::", "title_drawBar", "Menu_Touch:", "getTouchOut:", "getTouchIn:",
    "Get_TitleTouch_Back", "Get_TitleTouch_Help", "GameScene", "sh_gameMain", "setDamage:::", "setDamage_FriendFire:",
    "subDamage:" };
static u32 hook_sel[H_COUNT];
int rtx_is_hooked(u32 sel) {
    if (!hook_sel[0]) for (int i = 0; i < H_COUNT; i++) hook_sel[i] = sel_get(hook_names[i]);
    for (int i = 0; i < H_COUNT; i++) if (hook_sel[i] == sel) return 1;
    return 0;
}
static int which(u32 sel) { for (int i = 0; i < H_COUNT; i++) if (hook_sel[i] == sel) return i; return -1; }
static float fbits(u64 r) { return __builtin_bit_cast(float, (u32)r); }

static u32 delegate(void) {
    u32 app = (u32)msg_sendv(class_named("UIApplication"), "sharedApplication", 0);
    return app ? (u32)msg_sendv(app, "delegate", 0) : 0;
}

/* ---- the RETOUCHED label: made like the game's own -createTexWithText:fontSize:, drawn in green ---- */
static u32 label_tex, label_saved;
static void label_swap_in(u32 d) {
    if (!label_tex) {
        pool_push();
        /* the game's own label text style (what -createTexWithText:fontSize: makes), but green */
        float bw = fbits(msg_sendv(A(rtxf_menu_bar(P(d))), "getImgW", 0)) - 24.0f;
        u32 tex = (u32)msg_sendv(class_named("Texture2D"), "alloc", 0);
        label_tex = (u32)msg_sendv(tex, "initWithString:dimensions:alignment:fontName:fontSize:a:r:g:b:", 10,
                                   str_new_utf8("RETOUCHED"), __builtin_bit_cast(u32, bw), __builtin_bit_cast(u32, 14.0f), 1,
                                   A(rtxf_font_name(P(d))), __builtin_bit_cast(u32, 14.0f), __builtin_bit_cast(u32, 1.0f),
                                   __builtin_bit_cast(u32, 0.0f), __builtin_bit_cast(u32, 1.0f), __builtin_bit_cast(u32, 0.1f));
        pool_pop();
        if (label_tex) host_gl_tint(RD32(label_tex + 4), 0.0f, 1.0f, 0.1f);   /* Texture2D._name: the logo green */
        if (label_tex) obj_retain(label_tex);        /* ours for the life of the app */
    }
    void **slot = rtxf_menu_label(P(d), LABEL_SLOT);
    label_saved = A(*slot); *slot = P(label_tex);
}
static void label_swap_out(u32 d) { *rtxf_menu_label(P(d), LABEL_SLOT) = P(label_saved); }

/* ---- menu_0.data with a sixth entry: label 53, no action (-1), flag 0 ---- */
static u32 menu0_with_entry(u32 data) {
    const char *src = P((u32)msg_sendv(data, "bytes", 0)); int n = (int)msg_sendv(data, "length", 0);
    char in[512], out[600]; if (n <= 0 || n >= (int)sizeof in) return data;
    memcpy(in, src, n); in[n] = 0;
    char *line[8]; int nl = 0; line[nl++] = in;
    for (char *p = in; *p && nl < 8; p++) if (*p == '\n') { *p = 0; if (p[1]) line[nl++] = p + 1; }
    if (nl < 5) return data;
    int count = 0; for (const char *p = line[0]; *p >= '0' && *p <= '9'; p++) count = count * 10 + (*p - '0');
    int k = rt_snprintf(out, sizeof out, "%d\n%s,%d\n%s,-1\n%s\n%s,0\n", count + 1, line[1], LABEL_SLOT, line[2], line[3], line[4]);
    return data_new(out, k, 0);
}

/* ---- tap on the sixth row, judged exactly as Menu_Touch: judges taps ---- */
static int in_menu_touch, menu_tap, menu_backhelp, menu_blocked;
static void menu_touch_done(u32 d) {
    if (!menu_tap || menu_backhelp || menu_blocked) return;
    u32 view = A(rtxf_view(P(d)));
    float y = *(float *)P(view + 0x48);                       /* -[MyEAGLView getTouchPos:0].y */
    float h = fbits(msg_sendv(A(rtxf_menu_bar(P(d))), "getImgH", 0));
    if (!(y > 284.0f && y < (h + 8.0f) * 4.0f + 284.0f)) return;
    int row = (int)(((y - 284.0f) - rtxf_menu_scroll_y(P(d))) / (h + 8.0f));
    if (row != ENTRY_ROW) return;
    GSoundCtrl__SeSet(1);
    rtxf_bar_anim(P(d), row);
    host_rtx_event(EV_OPEN_OPTIONS, 0);
}

int rtx_pre(u32 self, u32 sel, u32 *a, int n, u64 *ret) {
    (void)n;
    switch (which(sel)) {
    case H_DRAWBAR: label_swap_in(self); return 0;
    case H_MENUTOUCH:
        in_menu_touch = (int)a[0] == 0;                       /* menu 0: the main menu */
        menu_tap = menu_backhelp = 0;
        menu_blocked = in_menu_touch ? rtxf_menu_blocked(P(self)) : 1;
        return 0;
    case H_GAMESCENE:
        if (opt_skip_logos && (*rtxf_scene(P(self)) == 0 || *rtxf_scene(P(self)) == 0xb) && *rtxf_case(P(self)) == 1
            && *rtxf_timecount(P(self)) < 38)
            *rtxf_timecount(P(self)) = 38;                    /* KONAMI / KOJIMA PRODUCTIONS: straight to their end */
        if (opt_inf_life && PLAYER_MAX_HP > 0 && PLAYER_HP < PLAYER_MAX_HP) PLAYER_HP = PLAYER_MAX_HP;
        return 0;
    case H_SETDAMAGE: case H_FRIENDFIRE:
        if (opt_inf_life) { RT_ONCE(rt_log("re-touched: damage to the player ignored")); *ret = 0; return 1; }  /* +[Player setDamage:::] */
        return 0;
    case H_SUBDAMAGE:
        if (opt_one_hit && (int)a[0] > 0) { RT_ONCE(rt_log("re-touched: enemy damage %d -> 1000000", (int)a[0])); a[0] = 1000000; }
        return 0;
    }
    return 0;
}
u64 rtx_post(u32 self, u32 sel, const u32 *a, int n, u64 r) {
    (void)n;
    switch (which(sel)) {
    case H_DATAREAD: {
        u32 name = a[0];
        if (r && name && str_is(name) && !strcmp(str_utf8(name), "menu_0")) return menu0_with_entry((u32)r);
        return r;
    }
    case H_DRAWBAR: label_swap_out(self); return r;
    case H_TOUCHOUT: case H_TOUCHIN: if (in_menu_touch && (r & 0xff)) menu_tap = 1; return r;
    case H_BACK: case H_HELP: if (in_menu_touch && (r & 0xff)) menu_backhelp = 1; return r;
    case H_MENUTOUCH: if (in_menu_touch) menu_touch_done(self); in_menu_touch = 0; return r;
    case H_SHMAIN: last_mission = host_time(); return r;
    }
    return r;
}

/* ---- host -> module ---- */
enum { RTX_ENABLE = 0, RTX_SKIP_LOGOS = 1, RTX_INF_LIFE = 2, RTX_ONE_HIT = 3 };
EXPORT(mgst_rtx_set) void mgst_rtx_set(int key, int v) {
    switch (key) {
    case RTX_ENABLE: rtx_hooks_on = v; break;
    case RTX_SKIP_LOGOS: opt_skip_logos = v; break;
    case RTX_INF_LIFE: opt_inf_life = v; break;
    case RTX_ONE_HIT: opt_one_hit = v; break;
    }
}
enum { CMD_PAUSE = 1, CMD_RESUME, CMD_UNLOCK_MISSIONS, CMD_UNLOCK_EXTRAS, CMD_BGM_PLAY, CMD_BGM_STOP, CMD_SE, CMD_STATE, CMD_QUIT_MISSION };
enum { ST_MISSION = 1, ST_PAUSED = 2, ST_INCLINATION = 4, ST_MAIN_MENU = 8, ST_BGM_ON = 16, ST_STARTED = 32 };
static void lifecycle(const char *sel) {
    u32 d = delegate(), app = (u32)msg_sendv(class_named("UIApplication"), "sharedApplication", 0);
    if (!d) return;
    pool_push(); msg_sendv(d, sel, 1, app); pool_pop();
}
EXPORT(mgst_rtx_cmd) int mgst_rtx_cmd(int cmd, int arg) {
    u32 d = delegate();
    char *sd = SAVE_DATA;
    switch (cmd) {
    case CMD_PAUSE: if (!paused) { lifecycle("applicationWillResignActive:"); paused = 1; } return 0;
    case CMD_RESUME: if (paused) { lifecycle("applicationDidBecomeActive:"); paused = 0; } return 0;
    case CMD_UNLOCK_MISSIONS:
        if (!sd) return -1;
        for (int i = 0; i < 25; i++) if (sd[0x15 + i] == 0) sd[0x15 + i] = 2;      /* 2 = unlocked, marked NEW */
        if (sd[0x10] == 0) sd[0x10] = 2;                                         /* Survival */
        GSaveCtrl__writeApplicationData("SAVE.data");
        if (d && *rtxf_scene(P(d)) == 1) { pool_push(); msg_sendv(d, "title_loadMenuData", 0); pool_pop(); }
        return 0;
    case CMD_UNLOCK_EXTRAS:
        if (!sd) return -1;
        GResult__unlockDrebinWallPaper(0, 3); GResult__unlockDrebinWallPaper(1, 3); GResult__unlockDrebinWallPaper(2, 3);
        GSaveCtrl__writeApplicationData("SAVE.data");
        return 0;
    case CMD_QUIT_MISSION:
        if (!d || last_mission <= 0 || (!paused && host_time() - last_mission > 1.0)) return -1;   /* only from inside a mission (a paused game stops calling sh_gameMain) */
        if (paused) { lifecycle("applicationDidBecomeActive:"); paused = 0; }
        rtxf_quit_mission(P(d));
        last_mission = 0;
        return 0;
    case CMD_BGM_PLAY: GSoundCtrl__BgmSet(arg, 1); return 0;
    case CMD_BGM_STOP: GSoundCtrl__BgmStop(); return 0;
    case CMD_SE: GSoundCtrl__SeSet(arg); return 0;
    case CMD_STATE: {
        int s = 0;
        if (last_mission > 0 && host_time() - last_mission < 0.5) s |= ST_MISSION;
        if (paused) s |= ST_PAUSED;
        if (d && *rtxf_inclination(P(d))) s |= ST_INCLINATION;
        if (d && *rtxf_scene(P(d)) == 1) s |= ST_MAIN_MENU;
        if (sd && sd[5] == 1) s |= ST_BGM_ON;
        if (d && *rtxf_start(P(d))) s |= ST_STARTED;
        return s;
    }
    }
    return -1;
}
