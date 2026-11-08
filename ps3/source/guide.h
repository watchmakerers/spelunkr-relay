// The channel guide, as sent by the relay's /ps3/guide.
#ifndef SPELUNKR_GUIDE_H
#define SPELUNKR_GUIDE_H

#include <ppu-types.h>

typedef struct {
	s64 start, end;          // ms since epoch (relay clock)
	char title[112];
	char year[8];
} show_t;

typedef struct {
	char id[48];
	int dial;
	char name[48];
	u32 color;               // 0xRRGGBB
	char desc[240];
	int has_logo;
	u8 logo[32 * 32];        // palette index per pixel, 0 = transparent
	u32 palette[16];         // index -> 0xRRGGBB
	show_t *shows;
	int nshows;
} channel_t;

typedef struct {
	channel_t *ch;
	int nch;
	int tz_minutes;          // relay's offset from UTC, for showing times
} guide_t;

// Parses the relay's text guide. Returns 0 on success; frees any previous contents.
int  guide_parse(guide_t *g, char *text);
void guide_free(guide_t *g);
int  guide_find(const guide_t *g, const char *id);   // index or -1
const show_t *guide_show_at(const channel_t *c, s64 ms);

// The relay's clock, kept by noting its NOW against our monotonic timer.
void clock_sync(s64 relay_ms);
s64  clock_ms(void);
int  clock_synced(void);
void format_clock(s64 ms, int tz_minutes, char *out, int outlen); // "3:30 PM"

#endif
