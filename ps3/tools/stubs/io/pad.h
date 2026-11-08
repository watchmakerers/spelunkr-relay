#pragma once
#include <ppu-types.h>
#define MAX_PADS 7
typedef struct { int len; unsigned BTN_UP:1, BTN_DOWN:1, BTN_LEFT:1, BTN_RIGHT:1, BTN_CROSS:1, BTN_CIRCLE:1, BTN_TRIANGLE:1, BTN_SQUARE:1, BTN_START:1, BTN_L1:1, BTN_R1:1; } padData;
typedef struct { u8 status[MAX_PADS]; } padInfo;
static inline s32 ioPadInit(u32 m) { (void)m; return 0; }
static inline s32 ioPadEnd(void) { return 0; }
static inline s32 ioPadGetInfo(padInfo *i) { for (int k = 0; k < MAX_PADS; k++) i->status[k] = 0; return 0; }
static inline s32 ioPadGetData(u32 p, padData *d) { (void)p; d->len = 0; return 0; }
