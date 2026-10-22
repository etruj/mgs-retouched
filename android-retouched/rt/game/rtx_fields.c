/* METAL GEAR SOLID RE-TOUCHED: typed access to the game's own state for android-retouched/rt/rtx.c.
 * Compiled with the game's generated types (like the game itself), so field offsets always match
 * the decompilation. Only the Re-Touched module contains this file. */
#include "mgst_types.fixed.h"

typedef CrashLandingAppDelegate D;
int *rtxf_scene(void *d) { return &((D *)d)->_Scene; }
int *rtxf_case(void *d) { return &((D *)d)->_case; }
int *rtxf_timecount(void *d) { return &((D *)d)->_timecount; }
unsigned char *rtxf_sleep_on(void *d) { return &((D *)d)->SleepOn; }
unsigned char *rtxf_start(void *d) { return &((D *)d)->Start; }
unsigned char *rtxf_inclination(void *d) { return &((D *)d)->DeviceInclination; }
int rtxf_menu_blocked(void *d) { D *s = d; return s->MenuMoveflg || s->immueMenuTouch || s->flg_help; }
float rtxf_menu_scroll_y(void *d) { return ((D *)d)->menu_scr_pos.y; }
void *rtxf_menu_bar(void *d) { return ((D *)d)->tex_menu_bar[0]; }
void **rtxf_menu_label(void *d, int i) { return &((D *)d)->tex_menu_str[i]; }
void rtxf_bar_anim(void *d, int row) { D *s = d; s->bar_anim_num = row; s->bar_anim_cnt = 6; }
int rtxf_anim_busy(void *d) { D *s = d; return s->back_anim_cnt != 0 || s->bar_anim_cnt != 0; }
void *rtxf_font_name(void *d) { return ((D *)d)->FontName; }
/* leave the mission the way the game's own MISSION FAILED "quit" does: its result handler (scene 0x962,
 * case 2) fades out, and with result_select == 0 and sh_case != 1 releases the mission and returns to the
 * main menu without saving a result. */
void rtxf_quit_mission(void *d) { D *s = d; s->sh_case = 0; s->result_select = 0; s->_case = 2; }
void *rtxf_view(void *d) { return ((D *)d)->glView; }
