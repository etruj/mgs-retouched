/* Shared by every platform (desktop, Android, web). */
#include "host.h"
#include <math.h>

/* Touch filtering, so a finger behaves like it did on the iPhone. The game treats any
 * touchesMoved during a press as a scroll and cancels the tap (MyEAGLView getTouchIn checks
 * touch_move). UIKit only sent touchesMoved when the location changed in whole points (an
 * iPhone 3G point is one pixel), so a resting finger produced none. Android reports sub-point
 * jitter at several times that resolution. So: positions are whole points; a move is only
 * delivered when that position changes; and until the finger has travelled TOUCH_SLOP points
 * from where it landed it is held still (Android applies the same idea, ViewConfiguration
 * touch slop). Past the slop, every point of movement is delivered (scrolling, aiming). */
/* TOUCH_SLOP is g_ext.touch_slop: 3 points, which Re-Touched lets the player change (it can also
 * scale a sliding finger's movement, g_ext.aim_gain, 1 by default). */
/* the engine's optional extensions (host.h); defined here because every platform links this file */
HostExt g_ext = { .render_scale = 1, .touch_slop = 3.0f, .aim_gain = 1.0f, .music_gain = 1.0f, .effects_gain = 1.0f };
typedef struct { int down, sliding; float sx, sy, lx, ly; } TouchState;
static TouchState tstate[8];
void host_touch(int phase, int id, float vx, float vy) {
    if (id < 0 || id >= 8) return;
    if (g_ext.touch && g_ext.touch(phase, id, vx, vy)) return;              /* an overlay took it */
    if (g_cfg.raw_touch) { host_touch_deliver(phase, id, vx, vy); return; }   /* diagnostics: unfiltered */
    TouchState *t = &tstate[id];
    if (g_ext.aim_gain != 1.0f && t->down && phase != 0) {
        vx = t->sx + (vx - t->sx) * g_ext.aim_gain; vy = t->sy + (vy - t->sy) * g_ext.aim_gain;
    }
    float x = floorf(vx + 0.5f), y = floorf(vy + 0.5f);
    const float TOUCH_SLOP = g_ext.touch_slop;
    switch (phase) {
    case 0:
        *t = (TouchState){ 1, 0, x, y, x, y };
        host_touch_deliver(0, id, x, y); break;
    case 1:
        if (!t->down) return;
        if (!t->sliding) {
            float dx = x - t->sx, dy = y - t->sy;
            if (dx * dx + dy * dy < TOUCH_SLOP * TOUCH_SLOP) return;
            t->sliding = 1;
        }
        if (x == t->lx && y == t->ly) return;
        t->lx = x; t->ly = y;
        host_touch_deliver(1, id, x, y); break;
    default:   /* ended / cancelled: a tap ends where it began */
        if (!t->down) return;
        if (!t->sliding) { x = t->sx; y = t->sy; }
        t->down = 0;
        host_touch_deliver(phase, id, x, y); break;
    }
}
