// Drawing on the CPU overlay canvas (UI_W x UI_H, ARGB, straight alpha).
#ifndef SPELUNKR_UI_H
#define SPELUNKR_UI_H

#include "platform.h"
#include "guide.h"

#define RGB(c)       (0xff000000u | (c))
#define ARGB(a, c)   (((u32)(a) << 24) | ((c) & 0xffffff))

int  ui_init(u32 *canvas);
void ui_clear(u32 argb);
void ui_rect(int x, int y, int w, int h, u32 argb);   // blended
void ui_clear_rect(int x, int y, int w, int h);         // fully transparent
void ui_frame(int x, int y, int w, int h, int t, u32 argb); // outline
// Draws UTF-8 text with its top-left at (x, y); clips to maxw (0 = no limit) with "...".
// Returns the drawn width.
int  ui_text(int x, int y, int size, u32 argb, const char *s, int maxw);
int  ui_text_width(int size, const char *s);
// Wraps text into at most `lines` lines of width w; returns lines used.
int  ui_text_wrap(int x, int y, int size, int line_h, u32 argb, const char *s, int w, int lines);
void ui_logo(int x, int y, int px, const channel_t *c);

#endif
