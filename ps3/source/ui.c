#include <stdlib.h>
#include <string.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"

#include "gfx.h"
#include "ui.h"
#include "spacemono_bold_ttf.h"

static u32 *cv;
static stbtt_fontinfo font;
static int ascent, descent;

typedef struct { int size, cp, w, h, xoff, yoff, adv; u8 *bmp; } glyph_t;
#define GCACHE 4096
static glyph_t cache[GCACHE];

int ui_init(u32 *canvas)
{
	cv = canvas;
	if (!stbtt_InitFont(&font, spacemono_bold_ttf, stbtt_GetFontOffsetForIndex(spacemono_bold_ttf, 0))) return -1;
	int gap;
	stbtt_GetFontVMetrics(&font, &ascent, &descent, &gap);
	return 0;
}

static inline u32 blend(u32 d, u32 s)
{
	u32 sa = s >> 24;
	if (sa == 255) return s;
	if (sa == 0) return d;
	u32 da = d >> 24;
	u32 oa = sa + da * (255 - sa) / 255;
	if (!oa) return 0;
	u32 r = (((s >> 16) & 255) * sa + ((d >> 16) & 255) * da * (255 - sa) / 255) / oa;
	u32 g = (((s >> 8) & 255) * sa + ((d >> 8) & 255) * da * (255 - sa) / 255) / oa;
	u32 b = ((s & 255) * sa + (d & 255) * da * (255 - sa) / 255) / oa;
	return oa << 24 | r << 16 | g << 8 | b;
}

void ui_clear(u32 argb)
{
	for (int i = 0; i < UI_W * UI_H; i++) cv[i] = argb;
}

void ui_rect(int x, int y, int w, int h, u32 argb)
{
	if (x < 0) { w += x; x = 0; }
	if (y < 0) { h += y; y = 0; }
	if (x + w > UI_W) w = UI_W - x;
	if (y + h > UI_H) h = UI_H - y;
	if (w <= 0 || h <= 0) return;
	for (int j = y; j < y + h; j++) {
		u32 *row = cv + j * UI_W;
		if ((argb >> 24) == 255) for (int i = x; i < x + w; i++) row[i] = argb;
		else for (int i = x; i < x + w; i++) row[i] = blend(row[i], argb);
	}
}

void ui_clear_rect(int x, int y, int w, int h)
{
	for (int j = y < 0 ? 0 : y; j < y + h && j < UI_H; j++)
		for (int i = x < 0 ? 0 : x; i < x + w && i < UI_W; i++) cv[j * UI_W + i] = 0;
}

void ui_frame(int x, int y, int w, int h, int t, u32 argb)
{
	ui_rect(x, y, w, t, argb);
	ui_rect(x, y + h - t, w, t, argb);
	ui_rect(x, y + t, t, h - 2 * t, argb);
	ui_rect(x + w - t, y + t, t, h - 2 * t, argb);
}

static int utf8_next(const char **s)
{
	const u8 *p = (const u8 *)*s;
	int cp;
	if (p[0] < 0x80) { cp = p[0]; *s += 1; }
	else if ((p[0] & 0xe0) == 0xc0 && p[1]) { cp = (p[0] & 0x1f) << 6 | (p[1] & 0x3f); *s += 2; }
	else if ((p[0] & 0xf0) == 0xe0 && p[1] && p[2]) { cp = (p[0] & 0x0f) << 12 | (p[1] & 0x3f) << 6 | (p[2] & 0x3f); *s += 3; }
	else if ((p[0] & 0xf8) == 0xf0 && p[1] && p[2] && p[3]) { cp = (p[0] & 0x07) << 18 | (p[1] & 0x3f) << 12 | (p[2] & 0x3f) << 6 | (p[3] & 0x3f); *s += 4; }
	else { cp = '?'; *s += 1; }
	// Typographic quotes/dashes the font may lack fall back to ASCII.
	if (cp == 0x2018 || cp == 0x2019) cp = '\'';
	if (cp == 0x201c || cp == 0x201d) cp = '"';
	if (cp == 0x2013 || cp == 0x2014) cp = '-';
	return cp;
}

static glyph_t *glyph(int size, int cp)
{
	u32 hsh = ((u32)cp * 2654435761u ^ (u32)size * 40503u) % GCACHE;
	for (int probe = 0; probe < 16; probe++) {
		glyph_t *g = &cache[(hsh + probe) % GCACHE];
		if (g->size == size && g->cp == cp) return g;
		if (g->size == 0) {
			float sc = stbtt_ScaleForPixelHeight(&font, size);
			int idx = stbtt_FindGlyphIndex(&font, cp);
			if (!idx && cp != ' ') idx = stbtt_FindGlyphIndex(&font, '?');
			int adv, lsb;
			stbtt_GetGlyphHMetrics(&font, idx, &adv, &lsb);
			g->size = size; g->cp = cp;
			g->adv = (int)(adv * sc + 0.5f);
			g->bmp = stbtt_GetGlyphBitmap(&font, sc, sc, idx, &g->w, &g->h, &g->xoff, &g->yoff);
			return g;
		}
	}
	return NULL; // cache crowded: skip the glyph rather than evict
}

static int baseline(int size) { return (int)(ascent * stbtt_ScaleForPixelHeight(&font, size) + 0.5f); }

static void draw_glyph(glyph_t *g, int x, int y, u32 argb)
{
	u32 a = argb >> 24, rgb = argb & 0xffffff;
	for (int j = 0; j < g->h; j++) {
		int py = y + g->yoff + j;
		if (py < 0 || py >= UI_H) continue;
		for (int i = 0; i < g->w; i++) {
			int px = x + g->xoff + i;
			if (px < 0 || px >= UI_W) continue;
			u32 cov = g->bmp[j * g->w + i];
			if (!cov) continue;
			u32 *d = &cv[py * UI_W + px];
			*d = blend(*d, (cov * a / 255) << 24 | rgb);
		}
	}
}

int ui_text_width(int size, const char *s)
{
	int w = 0;
	while (*s) { glyph_t *g = glyph(size, utf8_next(&s)); if (g) w += g->adv; }
	return w;
}

int ui_text(int x, int y, int size, u32 argb, const char *s, int maxw)
{
	int by = y + baseline(size);
	int full = ui_text_width(size, s);
	int limit = maxw > 0 && full > maxw ? maxw - ui_text_width(size, "...") : 1 << 30;
	int cx = x;
	while (*s) {
		glyph_t *g = glyph(size, utf8_next(&s));
		if (!g) continue;
		if (cx - x + g->adv > limit) { cx += ui_text(cx, y, size, argb, "...", 0); return cx - x; }
		draw_glyph(g, cx, by, argb);
		cx += g->adv;
	}
	return cx - x;
}

int ui_text_wrap(int x, int y, int size, int line_h, u32 argb, const char *s, int w, int lines)
{
	char buf[512];
	int used = 0;
	while (*s && used < lines) {
		while (*s == ' ') s++;
		// Take words while they fit.
		int n = 0, last_fit = 0;
		const char *p = s;
		while (*p) {
			const char *word_end = p;
			while (*word_end && *word_end != ' ') word_end++;
			int len = (int)(word_end - s);
			if (len >= (int)sizeof(buf)) break;
			memcpy(buf, s, len); buf[len] = 0;
			if (ui_text_width(size, buf) > w && last_fit) break;
			last_fit = len;
			p = word_end;
			if (*p == ' ') p++;
			n = len;
		}
		if (!n) break;
		memcpy(buf, s, n); buf[n] = 0;
		int final_line = used == lines - 1 && s[n];
		ui_text(x, y + used * line_h, size, argb, final_line ? s : buf, final_line ? w : 0);
		s += n;
		used++;
	}
	return used;
}

void ui_logo(int x, int y, int px, const channel_t *c)
{
	if (!c->has_logo) {
		ui_rect(x, y, 32 * px, 32 * px, RGB(c->color));
		return;
	}
	for (int j = 0; j < 32; j++)
		for (int i = 0; i < 32; i++) {
			u8 k = c->logo[j * 32 + i];
			if (k) ui_rect(x + i * px, y + j * px, px, px, RGB(c->palette[k]));
		}
}
