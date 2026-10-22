/* METAL GEAR SOLID RETOUCHED: settings, the RETOUCHED options screen, the in-mission pause,
 * extras (art gallery, sound test, save backup), controller and gyro input, and the modifiers.
 * Shared by Android and the desktop test build; the platform glue is in rtx_host.h.
 * Faithful by default: with the default settings the game plays like the original edition (the
 * pause button and pausing when the app is left are the only defaults that add anything).
 * Save transfer: SAVE.data (the game's own 1 KB save) is exported to / imported from a file the
 * player picks (Android: the system file picker), so progress moves between devices. */
#include "rtx_host.h"
#include "wasm-rt-exceptions.h"
#include "wasm-rt-impl.h"
#include <pthread.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* ================================================================== settings (Documents/retouched.cfg) */
static struct {
    int sharp, fill, crisp, immersive, stats;
    int music, effects, hq_audio;
    int vibration, aim, slop, gyro, gyro_invert, pad_speed;
    int lang, skip_logos, pause_button, auto_pause;
    int inf_life, one_hit, open_missions, open_walls;   /* CHEATS tab */
} S;
typedef struct { const char *key; int *v; int def, lo, hi; } Key;
static const Key keys[] = {
    { "sharp_rendering", &S.sharp, 0, 0, 1 }, { "screen_fill", &S.fill, 0, 0, 2 }, { "crisp_pixels", &S.crisp, 0, 0, 1 },
    { "full_screen", &S.immersive, 1, 0, 1 }, { "show_fps", &S.stats, 0, 0, 1 },
    { "music_volume", &S.music, 100, 0, 100 }, { "effects_volume", &S.effects, 100, 0, 100 }, { "hq_resampling", &S.hq_audio, 0, 0, 1 },
    { "vibration", &S.vibration, 1, 0, 1 }, { "aim_sensitivity", &S.aim, 100, 50, 200 }, { "touch_tolerance", &S.slop, 3, 1, 8 },
    { "gyro_aiming", &S.gyro, 0, 0, 3 }, { "gyro_invert", &S.gyro_invert, 0, 0, 1 }, { "controller_speed", &S.pad_speed, 1, 0, 2 },
    { "language", &S.lang, 0, 0, 6 }, { "skip_intro", &S.skip_logos, 0, 0, 1 }, { "pause_button", &S.pause_button, 1, 0, 1 },
    { "pause_on_leave", &S.auto_pause, 1, 0, 1 }, { "cheat_infinite_life", &S.inf_life, 0, 0, 1 }, { "cheat_one_hit_kills", &S.one_hit, 0, 0, 1 },
    { "cheat_open_all_missions", &S.open_missions, 0, 0, 1 }, { "cheat_open_all_wallpapers", &S.open_walls, 0, 0, 1 },
};
#define NKEYS (int)(sizeof keys / sizeof keys[0])
static const char *const LANGS[] = { "English", "Japanese", "French", "German", "Italian", "Spanish" };

static void cfg_path(char *out, size_t cap) { snprintf(out, cap, "%s/retouched.cfg", g_cfg.docs_dir); }
static void settings_load(void) {
    for (int i = 0; i < NKEYS; i++) *keys[i].v = keys[i].def;
    char p[1100]; cfg_path(p, sizeof p);
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[160];
    while (fgets(line, sizeof line, f)) {
        char k[64]; int v;
        if (sscanf(line, "%63[^=]=%d", k, &v) != 2) continue;
        for (int i = 0; i < NKEYS; i++) if (!strcmp(k, keys[i].key)) *keys[i].v = v < keys[i].lo ? keys[i].lo : v > keys[i].hi ? keys[i].hi : v;
    }
    fclose(f);
}
static void settings_save(void) {
    char p[1100]; cfg_path(p, sizeof p);
    mkdir(g_cfg.docs_dir, 0755);
    FILE *f = fopen(p, "w"); if (!f) { hlog("re-touched: cannot write %s", p); return; }
    fprintf(f, "# METAL GEAR SOLID RETOUCHED settings\n");
    for (int i = 0; i < NKEYS; i++) fprintf(f, "%s=%d\n", keys[i].key, *keys[i].v);
    fclose(f);
}

/* ================================================================== the game module (Re-Touched exports) */
enum { RTX_ENABLE = 0, RTX_SKIP_LOGOS = 1, RTX_INF_LIFE = 2, RTX_ONE_HIT = 3 };
enum { CMD_PAUSE = 1, CMD_RESUME, CMD_UNLOCK_MISSIONS, CMD_UNLOCK_EXTRAS, CMD_BGM_PLAY, CMD_BGM_STOP, CMD_SE, CMD_STATE, CMD_QUIT_MISSION };
enum { ST_MISSION = 1, ST_PAUSED = 2, ST_INCLINATION = 4, ST_MAIN_MENU = 8, ST_BGM_ON = 16, ST_STARTED = 32 };
static int booted;
/* called outside the game lock only (never from the overlay, which runs inside a game call) */
static int mcmd(int cmd, int arg) {
    if (!booted || host_is_crashed()) return -1;
    int r = -1;
    game_lock(&g_main_thread);
    wasm_rt_trap_t tr = wasm_rt_impl_try();
    if (tr) hlog("re-touched: trap in command %d: %s", cmd, wasm_rt_strerror(tr));
    else r = (int)w2c_mgst_mgst_rtx_cmd(&g_inst, (u32)cmd, (u32)arg);
    game_unlock(&g_main_thread);
    return r;
}
static void mset_locked(int key, int v) { w2c_mgst_mgst_rtx_set(&g_inst, (u32)key, (u32)v); }
static void mset(int key, int v) { if (!booted) return; game_lock(&g_main_thread); mset_locked(key, v); game_unlock(&g_main_thread); }

/* ================================================================== state */
enum { SCR_NONE, SCR_OPTIONS, SCR_PAUSE, SCR_GALLERY, SCR_SOUND };
static int screen, opt_from_pause, tab, st, paused, bg_suspended;
static char msg[256];
static double now_t, last_redraw;
static void vibrate_gate(int ms);

static void apply(void) {
    g_ext.fill = S.fill; g_ext.crisp = S.crisp;
    g_ext.touch_slop = (float)S.slop; g_ext.aim_gain = S.aim / 100.0f;
    g_ext.music_gain = S.music / 100.0f; g_ext.effects_gain = S.effects / 100.0f; g_ext.hq_resample = S.hq_audio;
    platform_vibrate = vibrate_gate;
    if (booted) { mset(RTX_SKIP_LOGOS, S.skip_logos); mset(RTX_INF_LIFE, S.inf_life); mset(RTX_ONE_HIT, S.one_hit); }
}
static void changed(void) { apply(); settings_save(); }

/* the game's vibration (its own VIBRATION option) passes only when Re-Touched's switch is on too */
static void (*plat_vibrate)(int);
static void vibrate_gate(int ms) { if (S.vibration && plat_vibrate) plat_vibrate(ms); }

/* ================================================================== pause (the game's own interruption pause) */
static int quit_confirm;                             /* the pause screen's "Are you sure?" box is up */
static void do_pause(void) { if (!paused) { mcmd(CMD_PAUSE, 0); paused = 1; screen = SCR_PAUSE; } }
static void do_resume(void) { if (paused) { mcmd(CMD_RESUME, 0); paused = 0; } screen = SCR_NONE; msg[0] = 0; quit_confirm = 0; }
void rtx_app_pause(void) {
    if (paused) return;                                        /* already suspended by our pause */
    if (S.auto_pause && (st & ST_MISSION)) { do_pause(); return; }
    host_lifecycle(0); bg_suspended = 1;
}
void rtx_app_resume(void) {
    if (bg_suspended) { host_lifecycle(1); bg_suspended = 0; }  /* our pause stays until RESUME */
}

/* ================================================================== save transfer (export / import SAVE.data) */
#define SAVE_SIZE 1024                     /* GSaveCtrl's SAVE.data is always 1 KB */
static pthread_mutex_t io_mx = PTHREAD_MUTEX_INITIALIZER;
static int io_kind; static uint8_t io_data[SAVE_SIZE]; static int io_len;   /* set by the platform, any thread */
static int restart_pending;
void rtx_io_result(int kind, const uint8_t *d, int n) {
    pthread_mutex_lock(&io_mx);
    io_kind = kind; io_len = n > SAVE_SIZE ? SAVE_SIZE + 1 : n;
    if (d && n > 0) memcpy(io_data, d, n > SAVE_SIZE ? SAVE_SIZE : n);
    pthread_mutex_unlock(&io_mx);
}
static void save_path(char *out, size_t cap) { snprintf(out, cap, "%s/SAVE.data", g_cfg.docs_dir); }
static void act_export(void) {
    char p[1100]; save_path(p, sizeof p);
    uint8_t buf[SAVE_SIZE + 1]; FILE *f = fopen(p, "rb"); int n = f ? (int)fread(buf, 1, sizeof buf, f) : 0; if (f) fclose(f);
    if (n != SAVE_SIZE) { snprintf(msg, sizeof msg, "No save file yet:\nplay a little first."); return; }
    rtx_plat_export_save(buf, n);          /* the platform asks where to put it */
}
static void act_import(void) {
    if (opt_from_pause || (st & ST_MISSION)) { snprintf(msg, sizeof msg, "Import a save from the main menu,\nnot during a mission."); return; }
    rtx_plat_import_save();
}
static void io_poll(void) {
    pthread_mutex_lock(&io_mx);
    int kind = io_kind, n = io_len; uint8_t d[SAVE_SIZE]; memcpy(d, io_data, sizeof d); io_kind = 0;
    pthread_mutex_unlock(&io_mx);
    switch (kind) {
    case RTX_IO_EXPORTED: snprintf(msg, sizeof msg, "Save exported.\nImport it on another device to continue there."); break;
    case RTX_IO_FAILED: snprintf(msg, sizeof msg, "The file could not be written or read."); break;
    case RTX_IO_IMPORTED: {
        if (n != SAVE_SIZE) { snprintf(msg, sizeof msg, "That is not an MGS Touch save\n(SAVE.data is exactly 1 KB)."); break; }
        char p[1100], bak[1200]; save_path(p, sizeof p); snprintf(bak, sizeof bak, "%s.before-import", p);
        rename(p, bak);                    /* the replaced save is kept next to it */
        FILE *f = fopen(p, "wb");
        if (!f || fwrite(d, 1, n, f) != (size_t)n) { if (f) fclose(f); rename(bak, p); snprintf(msg, sizeof msg, "Could not write the save."); break; }
        fclose(f);
        snprintf(msg, sizeof msg, "Save imported.\nThe game restarts now to load it.");
        restart_pending = 1;
        break;
    }
    }
}

/* ================================================================== the options screen */
typedef enum { R_TOGGLE, R_CHOICE, R_SLIDER, R_ACTION } RowType;
typedef struct { RowType t; const char *label, *hint; int *v; int lo, hi; const char *const *names; void (*act)(void); const char *unit; } Row;
static const char *const FILL_NAMES[] = { "Black bars", "Blurred", "Dark frame" };
static const char *const GYRO_NAMES[] = { "Off", "Low", "Medium", "High" };
static const char *const PAD_NAMES[] = { "Slow", "Normal", "Fast" };
static const char *const LANG_NAMES[] = { "Phone", "English", "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e", "Fran\xc3\xa7" "ais", "Deutsch", "Italiano", "Espa\xc3\xb1ol" };
static void act_hd(void) {
    snprintf(msg, sizeof msg, "Enhanced graphics (HD) is coming:\nthe HD art pack is being made with the upscale kit.\nThis switch turns on once the pack is installed.");
}
static void act_gallery(void);
static void act_sound(void);
/* "Open all ..." toggles: ON opens them in the save now and again at every start of the game (the
 * game keeps what is opened, so turning it OFF stops reopening but does not close them again). */
static int unlocked_missions, unlocked_walls;
static void act_unlock_missions(void) {
    if (!S.open_missions) return;
    int r = mcmd(CMD_UNLOCK_MISSIONS, 0); if (!r) unlocked_missions = 1;
    if (r) snprintf(msg, sizeof msg, "Not available right now: it applies at the main menu.");
}
static void act_unlock_extras(void) {
    if (!S.open_walls) return;
    int r = mcmd(CMD_UNLOCK_EXTRAS, 0); if (!r) unlocked_walls = 1;
    if (r) snprintf(msg, sizeof msg, "Not available right now: it applies at the main menu.");
}
static void act_lang(void) { snprintf(msg, sizeof msg, "The language changes the next time the game starts."); }
static const Row TAB_GFX[] = {
    { R_ACTION, "Enhanced graphics (HD)", "Same size, HD resolution", NULL, 0, 0, NULL, act_hd, "SOON" },
    { R_TOGGLE, "Sharp rendering", "Draw at the phone's resolution", &S.sharp, 0, 1, NULL, NULL, NULL },
    { R_CHOICE, "Screen fill", "Around the game picture", &S.fill, 0, 2, FILL_NAMES, NULL, NULL },
    { R_TOGGLE, "Crisp pixels", "No smoothing when enlarged", &S.crisp, 0, 1, NULL, NULL, NULL },
    { R_TOGGLE, "Full screen", "Hide Android's system bars", &S.immersive, 0, 1, NULL, NULL, NULL },
    { R_TOGGLE, "Show frame rate", NULL, &S.stats, 0, 1, NULL, NULL, NULL },
};
static const Row TAB_SND[] = {
    { R_SLIDER, "Music volume", NULL, &S.music, 0, 100, NULL, NULL, "%" },
    { R_SLIDER, "Effects volume", NULL, &S.effects, 0, 100, NULL, NULL, "%" },
    { R_TOGGLE, "High-quality resampling", "Cleaner pitch and music", &S.hq_audio, 0, 1, NULL, NULL, NULL },
    { R_ACTION, "Sound test", "Play any music track", NULL, 0, 0, NULL, act_sound, NULL },
};
static const Row TAB_CTL[] = {
    { R_TOGGLE, "Vibration", "Also needs VIBRATION on in OPTIONS", &S.vibration, 0, 1, NULL, NULL, NULL },
    { R_SLIDER, "Aim sensitivity", "Finger movement while dragging", &S.aim, 50, 200, NULL, NULL, "%" },
    { R_SLIDER, "Touch tolerance", "Wobble allowed in a tap", &S.slop, 1, 8, NULL, NULL, " pt" },
    { R_CHOICE, "Gyro aiming", "Tilt to fine-tune while aiming", &S.gyro, 0, 3, GYRO_NAMES, NULL, NULL },
    { R_TOGGLE, "Invert gyro", NULL, &S.gyro_invert, 0, 1, NULL, NULL, NULL },
    { R_CHOICE, "Controller cursor", "Stick moves, A presses", &S.pad_speed, 0, 2, PAD_NAMES, NULL, NULL },
};
static const Row TAB_GAME[] = {
    { R_CHOICE, "Language", NULL, &S.lang, 0, 6, LANG_NAMES, act_lang, NULL },
    { R_TOGGLE, "Skip intro logos", NULL, &S.skip_logos, 0, 1, NULL, NULL, NULL },
    { R_TOGGLE, "Pause button", "In missions, top corner", &S.pause_button, 0, 1, NULL, NULL, NULL },
    { R_TOGGLE, "Pause when leaving", "Missions stay paused on return", &S.auto_pause, 0, 1, NULL, NULL, NULL },
    { R_ACTION, "Export save", "Copy your progress to a file", NULL, 0, 0, NULL, act_export, NULL },
    { R_ACTION, "Import save", "Continue progress from another device", NULL, 0, 0, NULL, act_import, NULL },
};
static const Row TAB_EXTRA[] = {
    { R_ACTION, "Art gallery", "Story, history and wallpapers", NULL, 0, 0, NULL, act_gallery, NULL },
    { R_ACTION, "Sound test", "Play any music track", NULL, 0, 0, NULL, act_sound, NULL },
};
static const Row TAB_CHEATS[] = {
    { R_TOGGLE, "Infinite life", NULL, &S.inf_life, 0, 1, NULL, NULL, NULL },
    { R_TOGGLE, "One-hit kills", NULL, &S.one_hit, 0, 1, NULL, NULL, NULL },
    { R_TOGGLE, "Open all missions", NULL, &S.open_missions, 0, 1, NULL, act_unlock_missions, NULL },
    { R_TOGGLE, "Open all wallpapers", NULL, &S.open_walls, 0, 1, NULL, act_unlock_extras, NULL },
};
static const struct { const char *name; const Row *rows; int n; } TABS[] = {
    { "GRAPHICS", TAB_GFX, 6 }, { "SOUND", TAB_SND, 4 }, { "CONTROLS", TAB_CTL, 6 }, { "GAME", TAB_GAME, 6 }, { "EXTRAS", TAB_EXTRA, 2 },
    { "CHEATS", TAB_CHEATS, 4 },
};
#define NTABS 6

static const Col WHITE = { 1, 1, 1, 1 }, DIM = { 0.62f, 0.64f, 0.62f, 1 }, ACCENT = { 0.55f, 0.88f, 0.35f, 1 },
                 PANEL = { 0.02f, 0.03f, 0.02f, 0.88f }, LINE = { 1, 1, 1, 0.10f }, BTN = { 0.16f, 0.18f, 0.16f, 0.95f };

/* layout (UI units), portrait or landscape */
static int L(void) { return ui_w() > ui_h(); }
static float title_y(void) { return L() ? 6 : 14; }
static float tabs_y(void) { return L() ? 32 : 50; }
static float rows_y(void) { return L() ? 60 : 88; }
static float row_h(void) { return L() ? 34 : 48; }
static float back_y(void) { return ui_h() - (L() ? 40 : 56); }
static const Row *cur_rows(int *n) { *n = TABS[tab].n; return TABS[tab].rows; }

static unsigned bar_tex; static int bar_tw, bar_th, bar_tried;
static void button(float x, float y, float w, float h, const char *label, int lit) {
    if (!bar_tried) { bar_tried = 1; bar_tex = ui_load_image("/app/mainmenu_bar_bg.png", &bar_tw, &bar_th); }
    if (bar_tex) ui_image(bar_tex, x, y, w, h, 1); else ui_fill(x, y, w, h, BTN);
    if (lit) ui_frame(x, y, w, h, 1.5f, ACCENT);
    ui_text(label, x + w / 2, y + h / 2 - 7.5f, 12.5f, 1, 1, WHITE, w - 8);
}
static void value_text(const Row *r, char *out, size_t cap) {
    switch (r->t) {
    case R_TOGGLE: snprintf(out, cap, "%s", *r->v ? "ON" : "OFF"); break;
    case R_CHOICE: snprintf(out, cap, "<  %s  >", r->names[*r->v - r->lo]); break;
    case R_SLIDER: snprintf(out, cap, "%d%s", *r->v, r->unit ? r->unit : ""); break;
    case R_ACTION: snprintf(out, cap, "%s", r->unit ? r->unit : ">"); break;
    }
}
static void draw_options(void) {
    float W = ui_w(), H = ui_h();
    ui_fill(0, 0, W, H, PANEL);
    ui_text("RETOUCHED", W / 2, title_y(), L() ? 19 : 24, 2, 1, ACCENT, 0);   /* bold 2: the wordmark's display face */
    float tw = (W - 16) / NTABS, ty = tabs_y();
    for (int i = 0; i < NTABS; i++) {
        int sel = tab == i;
        ui_fill(8 + i * tw + 1, ty, tw - 2, 24, sel ? (Col){ 0.25f, 0.42f, 0.18f, 1 } : BTN);
        ui_text(TABS[i].name, 8 + i * tw + tw / 2, ty + (L() ? 6 : 7), L() ? 9.5f : 8.5f, 1, 1, sel ? WHITE : DIM, tw - 6);
    }
    int n; const Row *rows = cur_rows(&n);
    float y = rows_y(), rh = row_h();
    for (int i = 0; i < n; i++, y += rh) {
        const Row *r = &rows[i]; char v[96]; value_text(r, v, sizeof v);
        ui_fill(12, y + rh - 1, W - 24, 1, LINE);
        float ly = r->hint ? y + rh / 2 - 13 : y + rh / 2 - 8;
        ui_text(r->label, 16, ly, 12.5f, 0, 0, WHITE, W * 0.55f);
        if (r->hint) ui_text(r->hint, 16, ly + 16, 9, 0, 0, DIM, W * 0.6f);
        Col vc = r->t == R_TOGGLE ? (*r->v ? ACCENT : DIM) : r->t == R_ACTION && r->unit ? DIM : WHITE;
        if (r->t == R_SLIDER) {
            float sx = W * 0.56f, sw = W * 0.25f, sy = y + rh / 2, f = (float)(*r->v - r->lo) / (r->hi - r->lo);
            ui_fill(sx, sy - 2, sw, 4, (Col){ 1, 1, 1, 0.18f });
            ui_fill(sx, sy - 2, sw * f, 4, ACCENT);
            ui_fill(sx + sw * f - 5, sy - 9, 10, 18, WHITE);
            ui_text(v, W - 14, sy - 8, 11.5f, 1, 2, WHITE, 0);
        } else ui_text(v, W - 16, y + rh / 2 - 8, 12.5f, 1, 2, vc, W * 0.42f);
    }
    button(W / 2 - 60, back_y(), 120, L() ? 32 : 38, "BACK", 0);
}
/* touch on the options screen (UI coordinates) */
static int drag_row = -1, down_row = -1, down_tab = -1, down_back;
static void slider_set(const Row *r, float x) {
    float W = ui_w(), sx = W * 0.56f, sw = W * 0.25f, f = (x - sx) / sw;
    f = f < 0 ? 0 : f > 1 ? 1 : f;
    int v = r->lo + (int)lroundf(f * (r->hi - r->lo));
    if (v != *r->v) { *r->v = v; apply(); }
}
static int row_at(float x, float y) {
    (void)x; int n; cur_rows(&n);
    if (y < rows_y()) return -1;
    int i = (int)((y - rows_y()) / row_h());
    return i < n ? i : -1;
}
static int tab_at(float x, float y) {
    if (y < tabs_y() || y > tabs_y() + 24) return -1;
    int i = (int)((x - 8) / ((ui_w() - 16) / NTABS));
    return i >= 0 && i < NTABS ? i : -1;
}
static int in_back(float x, float y) { return fabsf(x - ui_w() / 2) < 64 && y > back_y() - 4 && y < back_y() + 42; }
static void options_close(void) {
    settings_save();
   
    screen = opt_from_pause ? SCR_PAUSE : SCR_NONE;
}
static void options_touch(int phase, float x, float y) {
    int n; const Row *rows = cur_rows(&n);
    if (phase == 0) {
        down_row = row_at(x, y); down_tab = tab_at(x, y); down_back = in_back(x, y);
        drag_row = down_row >= 0 && rows[down_row].t == R_SLIDER ? down_row : -1;
        if (drag_row >= 0) slider_set(&rows[drag_row], x);
        return;
    }
    if (phase == 1) { if (drag_row >= 0) slider_set(&rows[drag_row], x); return; }
    if (drag_row >= 0) { drag_row = -1; changed(); return; }
    if (down_back && in_back(x, y)) { options_close(); return; }
    if (down_tab >= 0 && tab_at(x, y) == down_tab) { tab = down_tab; return; }
    int i = row_at(x, y);
    if (i < 0 || i != down_row) return;
    const Row *r = &rows[i];
    switch (r->t) {
    case R_TOGGLE: *r->v = !*r->v; if (r->v == &S.immersive) rtx_plat_immersive(S.immersive); break;
    case R_CHOICE: {
        int back = x < ui_w() * 0.62f && x > ui_w() * 0.45f;       /* the "<" side steps back */
        int span = r->hi - r->lo + 1, v = *r->v - r->lo;
        *r->v = r->lo + (v + (back ? span - 1 : 1)) % span;
        break;
    }
    case R_ACTION: if (r->act) r->act(); return;
    case R_SLIDER: return;
    }
    changed();
    if (r->act) r->act();
}

/* ================================================================== pause screen */
static float pb_x(void) { return ui_w() - 50; }
static void draw_pause_button(void) {
    float x = pb_x() + 8, y = 6;
    ui_fill(x, y, 34, 30, (Col){ 0, 0, 0, 0.45f });
    ui_frame(x, y, 34, 30, 1, (Col){ 1, 1, 1, 0.35f });
    ui_fill(x + 11, y + 8, 4, 14, (Col){ 1, 1, 1, 0.85f });
    ui_fill(x + 19, y + 8, 4, 14, (Col){ 1, 1, 1, 0.85f });
}
static int pause_hit(float x, float y) { return x > pb_x() && y < 46; }
static void draw_pause(void) {
    float W = ui_w(), H = ui_h();
    ui_fill(0, 0, W, H, (Col){ 0, 0, 0, 0.62f });
    ui_text("PAUSE", W / 2, H * 0.22f, 26, 1, 1, ACCENT, 0);
    button(W / 2 - 80, H * 0.42f, 160, 36, "RESUME", 1);
    button(W / 2 - 80, H * 0.42f + 50, 160, 36, "RETOUCHED", 0);
    button(W / 2 - 80, H * 0.42f + 100, 160, 36, "QUIT", 0);
    if (quit_confirm) {                              /* "Are you sure?": NO is the highlighted, safe choice */
        ui_fill(0, 0, W, H, (Col){ 0, 0, 0, 0.55f });
        float bw = W < 300 ? W - 40 : 260, bh = 120, bx = (W - bw) / 2, by = (H - bh) / 2;
        ui_fill(bx, by, bw, bh, (Col){ 0.05f, 0.05f, 0.05f, 0.95f });
        ui_frame(bx, by, bw, bh, 1, (Col){ 1, 1, 1, 0.4f });
        ui_text("Quit this mission?", W / 2, by + 12, 15, 1, 1, WHITE, bw - 16);
        ui_text("Progress in it will be lost.", W / 2, by + 36, 10.5f, 0, 1, DIM, bw - 16);
        button(bx + 14, by + bh - 46, 100, 34, "YES", 0);
        button(bx + bw - 114, by + bh - 46, 100, 34, "NO", 1);
    }
}
static void do_quit_mission(void) {
    quit_confirm = 0;
    if (mcmd(CMD_QUIT_MISSION, 0) == 0) { paused = 0; screen = SCR_NONE; msg[0] = 0; }
    else snprintf(msg, sizeof msg, "Could not leave the mission right now.");
}
static int pause_down;
static void pause_touch(int phase, float x, float y) {
    float W = ui_w(), H = ui_h();
    int which;
    if (quit_confirm) {                              /* 1 = YES, 2 = NO */
        float bw = W < 300 ? W - 40 : 260, bh = 120, bx = (W - bw) / 2, by = (H - bh) / 2, yy = by + bh - 46;
        which = y > yy && y < yy + 34 ? (x > bx + 14 && x < bx + 114 ? 1 : x > bx + bw - 114 && x < bx + bw - 14 ? 2 : 0) : 0;
        if (phase == 0) { pause_down = which; return; }
        if (phase == 1) return;
        if (which && which == pause_down) { if (which == 1) do_quit_mission(); else quit_confirm = 0; }
        return;
    }
    which = fabsf(x - W / 2) < 84 ? (y > H * 0.42f - 4 && y < H * 0.42f + 40 ? 1 : y > H * 0.42f + 46 && y < H * 0.42f + 90 ? 2 : y > H * 0.42f + 96 && y < H * 0.42f + 140 ? 3 : 0) : 0;
    if (phase == 0) { pause_down = which; return; }
    if (phase == 1) return;
    if (which && which == pause_down) {
        if (which == 1) do_resume();
        else if (which == 3) quit_confirm = 1;
        else { screen = SCR_OPTIONS; opt_from_pause = 1; tab = 0; }
    }
}

/* ================================================================== art gallery */
static const char *const GALLERY[] = {
    "pct_story_act1", "pct_story_act2", "pct_story_act3", "pct_story_act4", "pct_story_act5",
    "pct_story_st01", "pct_story_st02", "pct_story_st03", "pct_story_st04", "pct_story_st05", "pct_story_st06", "pct_story_st07", "pct_story_st08",
    "pct_story_mission01", "pct_story_mission02", "pct_story_mission03", "pct_story_mission04", "pct_story_mission05",
    "pct_story_mission06", "pct_story_mission07", "pct_story_mission08", "pct_story_mission09", "pct_story_mission10",
    "pct_story_mission11", "pct_story_mission12", "pct_story_mission13", "pct_story_mission14", "pct_story_mission15",
    "pct_story_mission16", "pct_story_mission17", "pct_story_mission18", "pct_story_mission19", "pct_story_mission20",
    "pct_story_announce1", "pct_story_announce2", "pct_story_announce3", "pct_story_cvs", "pct_story_epilogue",
    "pct_history_1", "pct_history_2", "pct_history_3", "pct_history_4", "pct_history_5",
    "wp101", "wp102", "wp103", "wp104", "wp105", "wp106", "wp107", "wp108", "wp109", "wp110", "wp1005", "wp1006",
    "wp201", "wp202", "wp203", "wp204", "wp205", "wp301", "wp302", "wp303", "wp304", "wp305",
    "wp401", "wp402", "wp403", "wp404", "wp405",
};
#define NGALLERY (int)(sizeof GALLERY / sizeof GALLERY[0])
static int gal_i, gal_loaded = -1, gal_w, gal_h, gal_down;
static unsigned gal_tex;
static void act_gallery(void) { screen = SCR_GALLERY; }
static void draw_gallery(void) {
    float W = ui_w(), H = ui_h();
    ui_fill(0, 0, W, H, (Col){ 0, 0, 0, 1 });
    if (gal_loaded != gal_i) {
        ui_free_image(gal_tex); gal_tex = 0;
        char p[256]; snprintf(p, sizeof p, "/app/ipd_%s.png", GALLERY[gal_i]);   /* the iPad's larger version first */
        gal_tex = ui_load_image(p, &gal_w, &gal_h);
        if (!gal_tex) { snprintf(p, sizeof p, "/app/%s.png", GALLERY[gal_i]); gal_tex = ui_load_image(p, &gal_w, &gal_h); }
        gal_loaded = gal_i;
    }
    float top = 34, bottom = H - 44, aw = W - 8, ah = bottom - top;
    if (gal_tex && gal_w > 0 && gal_h > 0) {
        float s = aw / gal_w < ah / gal_h ? aw / gal_w : ah / gal_h, w = gal_w * s, h = gal_h * s;
        ui_image(gal_tex, (W - w) / 2, top + (ah - h) / 2, w, h, 1);
    } else ui_text("(not in this bundle)", W / 2, H / 2, 12, 0, 1, DIM, 0);
    char cap[96]; snprintf(cap, sizeof cap, "%d / %d", gal_i + 1, NGALLERY);
    ui_text("ART GALLERY", 12, 9, 12, 1, 0, ACCENT, 0);
    ui_text(cap, W - 12, 9, 12, 1, 2, WHITE, 0);
    button(8, H - 38, 70, 32, "<", 0); button(W - 78, H - 38, 70, 32, ">", 0);
    button(W / 2 - 50, H - 38, 100, 32, "BACK", 0);
}
static void gallery_touch(int phase, float x, float y) {
    float W = ui_w(), H = ui_h();
    int which = y > H - 44 ? (x < 84 ? 1 : x > W - 84 ? 2 : fabsf(x - W / 2) < 54 ? 3 : 0) : (x < W / 3 ? 1 : x > W * 2 / 3 ? 2 : 0);
    if (phase == 0) { gal_down = which; return; }
    if (phase != 3 || which != gal_down) return;
    if (which == 1) gal_i = (gal_i + NGALLERY - 1) % NGALLERY;
    else if (which == 2) gal_i = (gal_i + 1) % NGALLERY;
    else if (which == 3) screen = SCR_OPTIONS;
}

/* ================================================================== sound test (the game's own music player) */
static const char *const TRACKS[] = { "title", "shop", "result", "game over", "stage", "boss", "stage", "boss A", "boss B",
    "story 1", "story 2", "story 3", "story 4", "story 5", "story 6", "story 7", "story 8", "start", "history", "result",
    "stage", "boss", "stage", "stage", "stage", "boss", "stage", "stage", "stage", "stage", "boss", "boss", "boss", "boss", "result" };
#define NTRACKS 35
static int snd_page, snd_playing = -1, snd_down = -1;
static void act_sound(void) { screen = SCR_SOUND; }
static int snd_per_page(void) { return L() ? 6 : 9; }
static void draw_sound(void) {
    float W = ui_w(), H = ui_h();
    ui_fill(0, 0, W, H, PANEL);
    ui_text("SOUND TEST", W / 2, title_y(), L() ? 17 : 21, 1, 1, ACCENT, 0);
    if (!(st & ST_BGM_ON)) ui_text("Turn BGM on in the game's OPTIONS to hear music", W / 2, title_y() + 26, 9, 0, 1, DIM, W - 20);
    int per = snd_per_page(); float y = rows_y(), rh = L() ? 30 : 34;
    for (int i = 0; i < per; i++, y += rh) {
        int t = snd_page * per + i; if (t >= NTRACKS) break;
        char s[64]; snprintf(s, sizeof s, "%02d  %s", t, TRACKS[t]);
        ui_fill(12, y + rh - 1, W - 24, 1, LINE);
        ui_text(s, 18, y + rh / 2 - 8, 12.5f, t == snd_playing, 0, t == snd_playing ? ACCENT : WHITE, 0);
        if (t == snd_playing) ui_text("PLAYING", W - 18, y + rh / 2 - 7, 10, 1, 2, ACCENT, 0);
    }
    float by = back_y();
    button(8, by, 60, L() ? 32 : 38, "<", 0); button(W - 68, by, 60, L() ? 32 : 38, ">", 0);
    button(W / 2 - 60, by, 120, L() ? 32 : 38, "BACK", 0);
}
static void sound_close(void) {
    if (snd_playing >= 0) { mcmd(CMD_BGM_STOP, 0); if (st & ST_MAIN_MENU) mcmd(CMD_BGM_PLAY, 0); }
    snd_playing = -1; screen = SCR_OPTIONS;
}
static void sound_touch(int phase, float x, float y) {
    float W = ui_w(); int per = snd_per_page(), npages = (NTRACKS + per - 1) / per;
    float rh = L() ? 30 : 34; int which;
    if (y > back_y() - 4) which = x < 76 ? -2 : x > W - 76 ? -3 : fabsf(x - W / 2) < 64 ? -4 : -1;
    else which = y >= rows_y() ? snd_page * per + (int)((y - rows_y()) / rh) : -1;
    if (phase == 0) { snd_down = which; return; }
    if (phase != 3 || which != snd_down) return;
    if (which == -2) snd_page = (snd_page + npages - 1) % npages;
    else if (which == -3) snd_page = (snd_page + 1) % npages;
    else if (which == -4) sound_close();
    else if (which >= 0 && which < NTRACKS && which < (snd_page + 1) * per) {
        mcmd(CMD_BGM_STOP, 0); mcmd(CMD_BGM_PLAY, which); snd_playing = which;
    }
}

/* ================================================================== message box */
static void draw_msg(void) {
    float W = ui_w(), H = ui_h(), bw = W - 40, bh = 150;
    ui_fill(0, 0, W, H, (Col){ 0, 0, 0, 0.5f });
    ui_fill(20, (H - bh) / 2, bw, bh, (Col){ 0.07f, 0.09f, 0.07f, 0.98f });
    ui_frame(20, (H - bh) / 2, bw, bh, 1.5f, ACCENT);
    char tmp[256]; snprintf(tmp, sizeof tmp, "%s", msg);
    float y = (H - bh) / 2 + 16; char *line = tmp;
    for (int i = 0; i < 4 && line; i++) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        ui_text(line, W / 2, y, 11, 0, 1, WHITE, bw - 16); y += 18;
        line = nl ? nl + 1 : NULL;
    }
    button(W / 2 - 45, (H + bh) / 2 - 44, 90, 32, "OK", 1);
}

/* ================================================================== controller (a virtual finger) and gyro */
static float pad_x = 160, pad_y = 240, pad_ax, pad_ay; static int pad_down; static double pad_seen;
static int finger_down; static float finger_x, finger_y, gyro_dx, gyro_dy; static int reentry;
void rtx_pad_axis(float x, float y) { pad_ax = fabsf(x) > 0.15f ? x : 0; pad_ay = fabsf(y) > 0.15f ? y : 0; pad_seen = now_t; }
void rtx_pad_button(int down) {
    pad_seen = now_t;
    if (down == pad_down) return;
    pad_down = down; host_touch(down ? 0 : 3, 0, pad_x, pad_y);
}
static void pad_step(double dt) {
    if (!pad_ax && !pad_ay) return;
    static const float speed[3] = { 160, 280, 440 };       /* view units per second at full tilt */
    /* the stick moves on the screen as the player sees it: rotate into view space in landscape scenes */
    float dx = pad_ax, dy = pad_ay;
    if ((st & ST_MISSION) && screen == SCR_NONE) {
        if (st & ST_INCLINATION) { float t = dx; dx = -dy; dy = t; } else { float t = dx; dx = dy; dy = -t; }
    }
    pad_x += dx * speed[S.pad_speed] * (float)dt; pad_y += dy * speed[S.pad_speed] * (float)dt;
    pad_x = pad_x < 0 ? 0 : pad_x > 319 ? 319 : pad_x; pad_y = pad_y < 0 ? 0 : pad_y > 479 ? 479 : pad_y;
    if (pad_down) host_touch(1, 0, pad_x, pad_y);
}
int rtx_gyro_wanted(void) { return S.gyro > 0; }
void rtx_gyro(float wx, float wy, double dt) {
    if (!S.gyro || !finger_down || !(st & ST_MISSION) || paused || screen != SCR_NONE) return;
    static const float gain[4] = { 0, 90, 170, 280 };      /* view units per radian */
    float k = gain[S.gyro] * (float)dt * (S.gyro_invert ? -1.0f : 1.0f);
    /* turning the phone left/right (about its x axis when held sideways) moves the sight sideways on
     * the landscape screen, which is the view's y axis; tilting moves it along the view's x axis */
    float dvy = -wx * k, dvx = wy * k;
    if (!(st & ST_INCLINATION)) { dvy = -dvy; dvx = -dvx; }
    gyro_dx += dvx; gyro_dy += dvy;
    reentry = 1; host_touch(1, 0, finger_x + gyro_dx, finger_y + gyro_dy); reentry = 0;
}

/* ================================================================== input entry (every touch sees this first) */
static int ui_finger = -1, pb_down;
static int on_touch(int phase, int id, float vx, float vy) {
    if (reentry) return 0;
    if (id == 0) {
        if (phase == 0) { finger_down = 1; gyro_dx = gyro_dy = 0; }
        if (phase >= 3) finger_down = 0;
        if (phase != 4) { finger_x = vx; finger_y = vy; }
    }
    if (screen != SCR_NONE || msg[0]) {
        if (phase == 0) { if (ui_finger >= 0) return 1; ui_finger = id; }
        if (id != ui_finger) return 1;
        if (phase >= 3) ui_finger = -1;
        if (phase == 4) return 1;
        float x, y; ui_to_ui(vx, vy, &x, &y);
        if (msg[0]) { if (phase == 3) { msg[0] = 0; if (restart_pending) rtx_plat_restart(); } return 1; }
        switch (screen) {
        case SCR_OPTIONS: options_touch(phase, x, y); break;
        case SCR_PAUSE: pause_touch(phase, x, y); break;
        case SCR_GALLERY: gallery_touch(phase, x, y); break;
        case SCR_SOUND: sound_touch(phase, x, y); break;
        }
        return 1;
    }
    if (S.pause_button && (st & ST_MISSION) && !paused) {
        float x, y; ui_to_ui(vx, vy, &x, &y);
        if (phase == 0 && pause_hit(x, y)) { pb_down = 1; return 1; }
        if (pb_down) { if (phase >= 3) { pb_down = 0; if (phase == 3 && pause_hit(x, y)) do_pause(); } return 1; }
    }
    if (id == 0 && phase != 0 && (gyro_dx != 0 || gyro_dy != 0)) {
        reentry = 1; host_touch(phase, id, vx + gyro_dx, vy + gyro_dy); reentry = 0;
        return 1;
    }
    return 0;
}

/* ================================================================== drawing (inside the presenter) */
static double fps_t0; static int fps_n; static float fps;
static void overlay(int bx, int by, int bw, int bh) {
    fps_n++; if (now_t - fps_t0 >= 1.0) { fps = fps_n / (float)(now_t - fps_t0); fps_n = 0; fps_t0 = now_t; }
    int land = screen == SCR_PAUSE || (screen != SCR_NONE && opt_from_pause) || (screen == SCR_NONE && (st & ST_MISSION));
    int flip = !(st & ST_INCLINATION);
    ui_begin(bx, by, bw, bh, land, flip);
    switch (screen) {
    case SCR_NONE:
        if (S.pause_button && (st & ST_MISSION) && !paused) draw_pause_button();
        if (S.stats) { char s[32]; snprintf(s, sizeof s, "%.0f FPS", fps); ui_text(s, 6, ui_h() - 16, 9, 1, 0, ACCENT, 0); }
        break;
    case SCR_OPTIONS: draw_options(); break;
    case SCR_PAUSE: draw_pause(); break;
    case SCR_GALLERY: draw_gallery(); break;
    case SCR_SOUND: draw_sound(); break;
    }
    if (msg[0]) draw_msg();
    if (now_t - pad_seen < 4.0 && pad_seen > 0) {          /* the controller's finger */
        ui_begin(bx, by, bw, bh, 0, 0);
        Col c = pad_down ? ACCENT : (Col){ 1, 1, 1, 0.9f };
        ui_frame(pad_x - 7, pad_y - 7, 14, 14, 2, c); ui_fill(pad_x - 1, pad_y - 1, 2, 2, c);
    }
    ui_end();
}

/* ================================================================== platform entry points */
void w2c_env_host_rtx_event(struct w2c_env *e, u32 ev, u32 arg) {
    (void)e; (void)arg;
    if (ev == 1 && screen == SCR_NONE) { screen = SCR_OPTIONS; opt_from_pause = 0; tab = 0; }
}
static void pre_boot(void) {
    mset_locked(RTX_ENABLE, 1); mset_locked(RTX_SKIP_LOGOS, S.skip_logos);
    mset_locked(RTX_INF_LIFE, S.inf_life); mset_locked(RTX_ONE_HIT, S.one_hit);
}
void rtx_init(void) {
    settings_load();
    if (S.lang > 0) snprintf(g_cfg.lang, sizeof g_cfg.lang, "%s", LANGS[S.lang - 1]);
    plat_vibrate = platform_vibrate;
    g_ext.overlay = overlay; g_ext.touch = on_touch; g_ext.pre_boot = pre_boot;
    apply();
    hlog("METAL GEAR SOLID RETOUCHED: settings from %s/retouched.cfg, language %s", g_cfg.docs_dir, g_cfg.lang);
}
void rtx_frame(double now) {
    double dt = now_t > 0 ? now - now_t : 0; now_t = now;
    if (!booted) booted = 1;
    st = mcmd(CMD_STATE, 0); if (st < 0) st = 0;
    paused = (st & ST_PAUSED) != 0;
    if (st & ST_MAIN_MENU) {                         /* the save is loaded by now: reopen everything the toggles ask for */
        if (S.open_missions && !unlocked_missions) { unlocked_missions = 1; act_unlock_missions(); }   /* one try per start */
        if (S.open_walls && !unlocked_walls) { unlocked_walls = 1; act_unlock_extras(); }
    }
    if (paused && screen == SCR_NONE) screen = SCR_PAUSE;
    /* sharp rendering: the game framebuffer at the screen's resolution (whole multiples of 320x480) */
    int ww, wh; gfx_window_size(&ww, &wh);
    int box_h = wh < ww * 3 / 2 ? wh : ww * 3 / 2, k = (box_h + 479) / 480;
    if (!paused) g_ext.render_scale = S.sharp ? (k < 1 ? 1 : k > 4 ? 4 : k) : 1;   /* (a resize clears the frozen picture) */
    if (dt > 0 && dt < 0.5) pad_step(dt);
    io_poll();
    /* a paused game draws nothing: show its last picture under the overlay */
    if (paused && now - last_redraw >= 1.0 / 30) { last_redraw = now; gfx_redraw(); }
}
int rtx_back_wanted(void) { return msg[0] || screen != SCR_NONE || ((st & ST_MISSION) && !paused); }
int rtx_back(void) {
    if (msg[0]) { msg[0] = 0; if (restart_pending) rtx_plat_restart(); return 1; }
    switch (screen) {
    case SCR_GALLERY: screen = SCR_OPTIONS; return 1;
    case SCR_SOUND: sound_close(); return 1;
    case SCR_OPTIONS: options_close(); return 1;
    case SCR_PAUSE: if (quit_confirm) quit_confirm = 0; else do_resume(); return 1;
    }
    if ((st & ST_MISSION) && !paused) { do_pause(); return 1; }
    return 0;
}
int rtx_start_button(void) {
    if (screen == SCR_PAUSE) { if (!quit_confirm) do_resume(); return 1; }
    if (screen != SCR_NONE) return rtx_back();
    if (st & ST_MISSION) { do_pause(); return 1; }
    if (st & ST_MAIN_MENU) { screen = SCR_OPTIONS; opt_from_pause = 0; tab = 0; return 1; }
    return 0;
}
