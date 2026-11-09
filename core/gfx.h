// Video output, implemented per platform (PS3: RSX shaders; desktop/Vita: SDL2). Two
// things are drawn: the video (three YUV planes) and the UI overlay (an ARGB canvas the
// CPU draws into).
#ifndef SPELUNKR_GFX_H
#define SPELUNKR_GFX_H

#include "platform.h"

// The UI is laid out on a canvas scaled to the display: 1280x720 on widescreen, 720x540 on
// 4:3 sets (bigger text for SD/CRT). Fixed once gfx_init has run.
extern int ui_w, ui_h;
#define UI_W ui_w
#define UI_H ui_h
#define UI_MAX_W 1280
#define UI_MAX_H 720
int gfx_is_4x3(void);
// The frame shape to ask the relay for: "16x9" (848x480), "4x3" (640x480) or "16x9s"
// (640x360, for small screens / slower CPUs).
const char *gfx_stream_aspect(void);

int  gfx_init(void);
void gfx_shutdown(void);

// Video frames. Upload copies the planes into the texture set the GPU is not using.
// Planes are 8-bit; y is w x h, cb/cr are (w/2) x (h/2); strides are the source row lengths.
void gfx_video_upload(const u8 *y, u32 ystride, const u8 *cb, const u8 *cr, u32 cstride, u32 w, u32 h);
void gfx_video_clear(void);   // forget the last frame (draws nothing until the next upload)

// UI overlay: returns the CPU canvas (UI_W*UI_H ARGB, main memory) to draw into, then
// gfx_overlay_commit() copies it into the texture set the GPU is not using.
u32 *gfx_overlay_canvas(void);
void gfx_overlay_commit(void);

// One frame: clear, video in the given rect (UI coordinates; letterboxed to the frame's
// aspect), overlay on top, flip (waits for vsync).
void gfx_selftest(void);
void gfx_set_crt(int on);   // scanlines + vignette over everything (no-op where unsupported)
int  gfx_has_crt(void);
void gfx_frame(float vx, float vy, float vw, float vh, int show_video);

#endif
