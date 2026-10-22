/* METAL GEAR SOLID RETOUCHED: host side (shared by Android and the desktop test build).
 * The platform calls these; everything else reaches the engine through g_ext (engine/host/host.h). */
#ifndef RTX_HOST_H
#define RTX_HOST_H
#include "host.h"

/* ---- called by the platform ---- */
void rtx_init(void);                    /* before host_start: settings, language, engine extensions */
void rtx_frame(double now);             /* after every host_frame */
int rtx_back(void);                     /* Back key / Escape: 1 = handled */
int rtx_back_wanted(void);              /* would rtx_back() handle it? (else Android's default applies) */
int rtx_start_button(void);             /* controller START: pause or options */
void rtx_app_pause(void);               /* the app goes to the background (instead of host_lifecycle(0)) */
void rtx_app_resume(void);              /* ... and comes back (instead of host_lifecycle(1)) */
void rtx_pad_axis(float x, float y);    /* controller stick or d-pad, -1..1 */
void rtx_pad_button(int down);          /* controller A (or a trigger): the virtual finger */
void rtx_gyro(float wx, float wy, double dt);   /* rad/s about the device x and y axes */
int rtx_gyro_wanted(void);              /* the gyroscope is only needed while aiming with gyro on */

/* ---- provided by the platform ---- */
void rtx_plat_immersive(int on);        /* hide / show the system bars */
void rtx_plat_export_save(const uint8_t *data, int n);   /* ask where to write SAVE.data; answer via rtx_io_result */
void rtx_plat_import_save(void);        /* ask for a save file; answer via rtx_io_result */
void rtx_plat_restart(void);            /* restart the game (after an import) */
enum { RTX_IO_EXPORTED = 1, RTX_IO_IMPORTED, RTX_IO_FAILED, RTX_IO_CANCELLED };
void rtx_io_result(int kind, const uint8_t *data, int n);   /* any thread */

/* ---- internal (rtx_ui.c) ---- */
typedef struct { float r, g, b, a; } Col;
void ui_begin(int bx, int by, int bw, int bh, int land, int flip);
void ui_end(void);
void ui_fill(float x, float y, float w, float h, Col c);
void ui_frame(float x, float y, float w, float h, float t, Col c);
float ui_text(const char *s, float x, float y, float size, int bold, int align, Col c, float max_w);
void ui_image(unsigned tex, float x, float y, float w, float h, float alpha);
unsigned ui_load_image(const char *path, int *w, int *h);   /* GL texture from a bundle image, 0 if absent */
void ui_free_image(unsigned tex);
void ui_to_ui(float vx, float vy, float *x, float *y);     /* view (touch) -> UI coordinates */
float ui_w(void); float ui_h(void);
#endif
