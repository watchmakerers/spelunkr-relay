// SPELUNKR: a cable-style channel guide and player for spelunkr.tv, shared by the PS3,
// PS Vita and desktop apps (each platform supplies main(), video, audio and input).
//
// The relay on your PC runs Spelunkr's real schedule and streams each channel as MPEG-1; this
// app finds it on the LAN, shows the guide, and plays channels.
//
// Controls (PlayStation names; keyboard and other pads map onto these, see platform.h)
//   Watching:  Up/Down channel surf · X/Square show info · Triangle/Circle/Start open guide
//   Guide:     Up/Down channel · Left/Right show · L1/R1 page · X watch · Triangle/Circle close

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform.h"
#include "gfx.h"
#include "ui.h"
#include "net.h"
#include "guide.h"
#include "player.h"
#include "audio.h"
#include "log.h"

// Network palette (the site's "8s" look).
#define C_PURPLE 0x1C0A57
#define C_INK    0x0b0f26
#define C_PAPER  0xe8eaf0
#define C_GOLD   0xffd740
#define C_NAVY   0x2a3580
#define C_SLATE  0x8899cc
#define C_ORANGE 0xd2500c

#define GUIDE_HOURS 4
#define GUIDE_REFRESH_US (5 * 60 * 1000000LL)

// Screen layout, picked once the display is known: widescreen HD, or 4:3 (SD/CRT) with
// bigger text, wider overscan margins, a shorter guide window and a 4:3 preview.
typedef struct {
	int mx, my;
	int f_brand, f_tag, f_clock, info_y;
	int pip_x, pip_y, pip_w, pip_h;
	int logo_px, f_name, f_title, title_lh, f_meta, f_desc, desc_lh, desc_y;
	int grid_x, grid_y, grid_head, row_h, rows, window_min;
	int f_head, f_cell, f_dial, f_chname, row_logo_x, f_hint;
	int ban_h, ban_logo_px, f_ban_dial, f_ban_name, f_ban_title, f_ban_meta, ban_bar_w, ban_meta_below;
	int f_msg_title, f_msg_body;
} layout_t;

static const layout_t LAYOUT_HD = {
	48, 32,
	40, 16, 30, 112,
	912, 112, 320, 180,
	2, 28, 22, 28, 18, 16, 22, 252,
	304, 308, 32, 56, 6, 90,
	18, 18, 22, 16, 56, 16,
	136, 3, 34, 26, 22, 18, 360, 0,
	34, 22,
};

static const layout_t LAYOUT_4X3 = {
	40, 30,
	30, 0, 24, 80,
	500, 80, 180, 135,
	2, 22, 18, 22, 15, 14, 18, 176,
	124, 226, 26, 50, 5, 60,
	15, 15, 18, 0, 46, 0,
	108, 2, 26, 20, 17, 14, 220, 1,
	24, 16,
};

static layout_t L;

#define MARGIN_X   (L.mx)
#define MARGIN_Y   (L.my)
#define GRID_X     (L.grid_x)
#define GRID_W     (UI_W - MARGIN_X - GRID_X)
#define GRID_Y     (L.grid_y)
#define GRID_HEAD  (L.grid_head)
#define ROW_H      (L.row_h)
#define ROWS       (L.rows)
#define WINDOW_MIN (L.window_min)
#define PIP_X      (L.pip_x)
#define PIP_Y      (L.pip_y)
#define PIP_W      (L.pip_w)
#define PIP_H      (L.pip_h)

typedef enum { MODE_CONNECT, MODE_NORELAY, MODE_WATCH, MODE_GUIDE } mode_t_;

static mode_t_ mode = MODE_CONNECT;
static char relay_host[64];
static int relay_port;
static guide_t guide;
static s64 guide_fetched_us;

static int watching = -1;          // channel index on air
static s64 banner_until_us;
static int sel_row;                // guide: selected channel
static int top_row;                // guide: first visible row
static s64 focus_ms;               // guide: time in focus (selects the show in the row)
static int dirty = 1;

// ------------------------------------------------------------------ helpers

static FILE *data_file(const char *name, const char *mode)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/%s", plat_data_dir(), name);
	return fopen(path, mode);
}

static void save_last(const char *id)
{
	FILE *f = data_file("last.txt", "w");
	if (f) { fputs(id, f); fclose(f); }
}

static int load_last(void)
{
	char id[64] = { 0 };
	FILE *f = data_file("last.txt", "r");
	if (f) { if (!fgets(id, sizeof(id), f)) id[0] = 0; fclose(f); }
	id[strcspn(id, " \t\r\n")] = 0;
	int i = guide_find(&guide, id);
	return i >= 0 ? i : 0;
}

static int fetch_guide(void)
{
	char path[64];
	snprintf(path, sizeof(path), "/ps3/guide?hours=%d", GUIDE_HOURS);
	char *text = http_get_all(relay_host, relay_port, path, NULL);
	if (!text) return -1;
	char cur[48] = { 0 };
	if (watching >= 0) strcpy(cur, guide.ch[watching].id);
	int r = guide_parse(&guide, text);
	free(text);
	if (r == 0) {
		guide_fetched_us = now_us();
		if (cur[0]) watching = guide_find(&guide, cur);
		if (sel_row >= guide.nch) sel_row = guide.nch - 1;
	}
	return r;
}

static void tune(int idx)
{
	if (idx < 0 || idx >= guide.nch) return;
	watching = idx;
	LOG("tune %s", guide.ch[idx].id);
	player_tune(guide.ch[idx].id);
	save_last(guide.ch[idx].id);
	banner_until_us = now_us() + 5000000;
	dirty = 1;
}

static const show_t *show_at(const channel_t *c, s64 ms)
{
	const show_t *s = guide_show_at(c, ms);
	if (s) return s;
	// Before the first listed show or past the last: nearest one.
	if (!c->nshows) return NULL;
	return ms < c->shows[0].start ? &c->shows[0] : &c->shows[c->nshows - 1];
}

static void time_range(const show_t *s, char *out, int len)
{
	char a[16], b[16];
	format_clock(s->start, guide.tz_minutes, a, sizeof(a));
	format_clock(s->end, guide.tz_minutes, b, sizeof(b));
	snprintf(out, len, "%s - %s%s%s", a, b, s->year[0] ? "  ·  " : "", s->year);
}

static void dial_label(const channel_t *c, char *out, int len)
{
	if (c->dial) snprintf(out, len, "%d", c->dial);
	else out[0] = 0;
}

// ------------------------------------------------------------------ screens

static void draw_header(void)
{
	ui_text(MARGIN_X, MARGIN_Y, L.f_brand, RGB(C_GOLD), "SPELUNKR", 0);
	if (L.f_tag) ui_text(MARGIN_X + 2, MARGIN_Y + L.f_brand + 8, L.f_tag, RGB(C_SLATE), "CABLE TV, BUT WEIRDER", 0);
	char clk[16];
	format_clock(clock_ms(), guide.tz_minutes, clk, sizeof(clk));
	int w = ui_text_width(L.f_clock, clk);
	ui_text(UI_W - MARGIN_X - w, MARGIN_Y + (L.f_brand - L.f_clock) / 2, L.f_clock, RGB(C_PAPER), clk, 0);
}

static void draw_center_message(const char *title, const char *body)
{
	ui_clear(RGB(C_PURPLE));
	draw_header();
	int w = ui_text_width(L.f_msg_title, title);
	ui_text((UI_W - w) / 2, UI_H * 3 / 8, L.f_msg_title, RGB(C_GOLD), title, 0);
	ui_text_wrap(MARGIN_X + 20, UI_H * 3 / 8 + L.f_msg_title * 2, L.f_msg_body, L.f_msg_body * 3 / 2, RGB(C_PAPER), body,
	             UI_W - 2 * MARGIN_X - 40, 7);
}

static void draw_tuning_card(const channel_t *c, const char *what)
{
	// Shown over black while a channel starts (the relay is seeking into the tape).
	int px = UI_H >= 720 ? 4 : 3;
	ui_clear(0);
	ui_logo(UI_W / 2 - 16 * px, UI_H * 2 / 7, px, c);
	char line[96];
	snprintf(line, sizeof(line), "%s %s", what, c->name);
	int w = ui_text_width(L.f_msg_body + 6, line);
	ui_text((UI_W - w) / 2, UI_H / 2, L.f_msg_body + 6, RGB(C_PAPER), line, 0);
}

static void draw_banner(const channel_t *c, const player_status_t *ps)
{
	int x = MARGIN_X, h = L.ban_h, y = UI_H - MARGIN_Y - h, w = UI_W - 2 * MARGIN_X;
	int pad = h / 6;
	ui_rect(x, y, w, h, ARGB(235, C_INK));
	ui_rect(x, y, 6, h, RGB(c->color));
	int logo = 32 * L.ban_logo_px;
	ui_logo(x + pad + 6, y + (h - logo) / 2, L.ban_logo_px, c);

	char dial[16];
	dial_label(c, dial, sizeof(dial));
	int tx = x + pad + 6 + logo + pad, right = x + w - pad;
	int dw = ui_text(tx, y + pad - 4, L.f_ban_dial, RGB(C_GOLD), dial, 0);
	s64 now = clock_ms();
	char clk[16];
	format_clock(now, guide.tz_minutes, clk, sizeof(clk));
	int cw = ui_text_width(L.f_ban_name, clk);
	ui_text(right - cw, y + pad, L.f_ban_name, RGB(C_PAPER), clk, 0);
	int nx = tx + dw + (dw ? 12 : 0);
	ui_text(nx, y + pad + (L.f_ban_dial - L.f_ban_name) / 2 - 2, L.f_ban_name, RGB(C_PAPER), c->name, right - cw - 12 - nx);

	const show_t *s = guide_show_at(c, now);
	if (!s) return;
	const char *tag = NULL;
	if (!strcmp(ps->type, "bump-break")) tag = "STATION BREAK";
	else if (!strcmp(ps->type, "ad")) tag = "COMMERCIAL";
	else if (!strcmp(ps->type, "up-next")) tag = "UP NEXT";
	int ty = y + pad + L.f_ban_dial + 6;
	int tagw = tag ? ui_text(tx, ty + 2, L.f_ban_meta, RGB(C_ORANGE), tag, 0) + 12 : 0;
	ui_text(tx + tagw, ty, L.f_ban_title, RGB(C_PAPER), s->title, right - tx - tagw);

	// Progress through the show, with its times beside it (or below on 4:3).
	int by = ty + L.f_ban_title + 12;
	float p = (float)(now - s->start) / (float)(s->end - s->start);
	if (p < 0) p = 0;
	if (p > 1) p = 1;
	ui_rect(tx, by, L.ban_bar_w, 6, RGB(C_NAVY));
	ui_rect(tx, by, (int)(L.ban_bar_w * p), 6, RGB(C_GOLD));
	char tr[48];
	time_range(s, tr, sizeof(tr));
	int mx = L.ban_meta_below ? tx + L.ban_bar_w + 12 : tx + L.ban_bar_w + 16;
	int my = by - L.f_ban_meta / 2 + 2;
	int trw = ui_text(mx, my, L.f_ban_meta, RGB(C_SLATE), tr, right - mx);
	const show_t *next = s + 1 < c->shows + c->nshows ? s + 1 : NULL;
	if (next && !L.ban_meta_below) {
		char nxt[160], t[16];
		format_clock(next->start, guide.tz_minutes, t, sizeof(t));
		snprintf(nxt, sizeof(nxt), "NEXT %s  %s", t, next->title);
		int nxx = mx + trw + 28;
		ui_text(nxx, my, L.f_ban_meta, RGB(C_SLATE), nxt, right - nxx);
	}
}

static s64 window_start(void)
{
	s64 slot = 30 * 60 * 1000LL;
	s64 now = clock_ms();
	s64 base = now - now % slot;
	// Scroll so the focused time stays in view.
	while (focus_ms >= base + (WINDOW_MIN - 30) * 60 * 1000LL) base += slot;
	return base;
}

static void draw_guide(void)
{
	ui_clear(ARGB(240, C_PURPLE));
	ui_rect(PIP_X - 3, PIP_Y - 3, PIP_W + 6, PIP_H + 6, RGB(C_GOLD));
	ui_clear_rect(PIP_X, PIP_Y, PIP_W, PIP_H);  // hole: the live channel shows through here
	draw_header();
	if (!guide.nch) return;

	s64 now = clock_ms();
	channel_t *sc = &guide.ch[sel_row];
	const show_t *focus = show_at(sc, focus_ms);

	// Info panel for the focused show.
	int logo = 32 * L.logo_px, tx = MARGIN_X + logo + 14, tw = PIP_X - 16 - tx;
	ui_logo(MARGIN_X, L.info_y + 4, L.logo_px, sc);
	char dial[16], head[96];
	dial_label(sc, dial, sizeof(dial));
	snprintf(head, sizeof(head), "%s%s%s", dial, dial[0] ? "  " : "", sc->name);
	ui_text(tx, L.info_y, L.f_name, RGB(C_GOLD), head, tw);
	if (focus) {
		int ty = L.info_y + L.f_name + 12;
		int lines = ui_text_wrap(tx, ty, L.f_title, L.title_lh, RGB(C_PAPER), focus->title, tw, 2);
		char tr[48];
		time_range(focus, tr, sizeof(tr));
		int on_now = focus->start <= now && now < focus->end;
		int my = ty + lines * L.title_lh + 4;
		int w = ui_text(tx, my, L.f_meta, RGB(on_now ? C_ORANGE : C_SLATE), on_now ? "ON NOW" : "LATER", 0);
		ui_text(tx + w + 12, my, L.f_meta, RGB(C_SLATE), tr, tw - w - 12);
	}
	ui_text_wrap(MARGIN_X, L.desc_y, L.f_desc, L.desc_lh, RGB(C_SLATE), sc->desc, PIP_X - 16 - MARGIN_X, 2);

	// Time header.
	s64 ws = window_start(), we = ws + WINDOW_MIN * 60 * 1000LL;
	float ppm = (float)GRID_W / (WINDOW_MIN * 60 * 1000.0f);
	ui_rect(MARGIN_X, GRID_Y, UI_W - 2 * MARGIN_X, GRID_HEAD, RGB(C_INK));
	for (int k = 0; k < WINDOW_MIN / 30; k++) {
		char t[16];
		format_clock(ws + k * 30 * 60 * 1000LL, guide.tz_minutes, t, sizeof(t));
		ui_text(GRID_X + 8 + (int)(k * 30 * 60 * 1000LL * ppm), GRID_Y + (GRID_HEAD - L.f_head) / 2 - 2, L.f_head, RGB(C_PAPER), t, 0);
	}
	if (L.f_chname) ui_text(MARGIN_X + 8, GRID_Y + (GRID_HEAD - L.f_head) / 2 - 2, L.f_head, RGB(C_GOLD), "CHANNEL", 0);

	for (int r = 0; r < ROWS && top_row + r < guide.nch; r++) {
		int ci = top_row + r;
		channel_t *c = &guide.ch[ci];
		int y = GRID_Y + GRID_HEAD + r * ROW_H;
		int selected = ci == sel_row;

		ui_rect(MARGIN_X, y + 2, GRID_X - MARGIN_X - 4, ROW_H - 4, RGB(selected ? C_NAVY : C_INK));
		if (ci == watching) ui_rect(MARGIN_X, y + 2, 5, ROW_H - 4, RGB(C_ORANGE));
		char d[16];
		dial_label(c, d, sizeof(d));
		ui_text(MARGIN_X + 10, y + (ROW_H - L.f_dial) / 2 - 2, L.f_dial, RGB(C_GOLD), d, 0);
		ui_logo(MARGIN_X + L.row_logo_x, y + (ROW_H - 32) / 2, 1, c);
		if (L.f_chname)
			ui_text(MARGIN_X + L.row_logo_x + 40, y + (ROW_H - L.f_chname) / 2 - 2, L.f_chname, RGB(C_PAPER), c->name,
			        GRID_X - MARGIN_X - L.row_logo_x - 48);

		for (int k = 0; k < c->nshows; k++) {
			show_t *s = &c->shows[k];
			if (s->end <= ws || s->start >= we) continue;
			s64 a = s->start < ws ? ws : s->start, b = s->end > we ? we : s->end;
			int x0 = GRID_X + (int)((a - ws) * ppm), x1 = GRID_X + (int)((b - ws) * ppm);
			int focused = selected && s == focus;
			int airing = s->start <= now && now < s->end;
			u32 bg = focused ? RGB(C_GOLD) : airing ? RGB(C_NAVY) : ARGB(200, C_INK);
			ui_rect(x0 + 2, y + 2, x1 - x0 - 4, ROW_H - 4, bg);
			char label[128];
			snprintf(label, sizeof(label), "%s%s", s->start < ws ? "< " : "", s->title);
			ui_text(x0 + 8, y + (ROW_H - L.f_cell) / 2 - 2, L.f_cell, RGB(focused ? C_INK : C_PAPER), label, x1 - x0 - 16);
		}
	}
	// Now line.
	if (now >= ws && now < we) ui_rect(GRID_X + (int)((now - ws) * ppm) - 1, GRID_Y, 3, GRID_HEAD + ROWS * ROW_H, RGB(C_ORANGE));

	if (L.f_hint)
		ui_text(MARGIN_X, UI_H - MARGIN_Y + 2, L.f_hint, RGB(C_SLATE), plat_hint_guide(), 0);
}

static void render_overlay(void)
{
	player_status_t ps;
	player_status(&ps);
	switch (mode) {
	case MODE_CONNECT:
		draw_center_message("LOOKING FOR THE RELAY...", "Searching your network for the Spelunkr relay running on your PC.");
		break;
	case MODE_NORELAY:
		draw_center_message("CAN'T FIND THE RELAY", plat_relay_help());
		break;
	case MODE_WATCH: {
		ui_clear(0);
		if (watching < 0) break;
		channel_t *c = &guide.ch[watching];
		if (ps.state == PLAYER_ERROR) {
			draw_center_message("SIGNAL LOST", ps.error[0] ? ps.error : "The relay stopped responding. Retrying...");
		} else if (ps.state == PLAYER_TUNING) {
			draw_tuning_card(c, "TUNING TO");
		}
		if (now_us() < banner_until_us || ps.state == PLAYER_BUFFERING) draw_banner(c, &ps);
		if (ps.state == PLAYER_BUFFERING) ui_text(UI_W - MARGIN_X - ui_text_width(L.f_ban_name, "BUFFERING..."), MARGIN_Y, L.f_ban_name, RGB(C_GOLD), "BUFFERING...", 0);
		break;
	}
	case MODE_GUIDE:
		draw_guide();
		break;
	}
	gfx_overlay_commit();
}

// ------------------------------------------------------------------ input

typedef struct { u32 now, prev; s64 held_since[B_COUNT]; s64 last_repeat[B_COUNT]; } pad_t;
static pad_t pad;

// Pressed this frame, or held long enough to auto-repeat (d-pad).
static int pressed(int b)
{
	int down = (pad.now & BTN(b)) != 0, was = (pad.prev & BTN(b)) != 0;
	s64 t = now_us();
	if (down && !was) { pad.held_since[b] = t; pad.last_repeat[b] = t; return 1; }
	if (down && b <= B_RIGHT && t - pad.held_since[b] > 400000 && t - pad.last_repeat[b] > 110000) {
		pad.last_repeat[b] = t;
		return 1;
	}
	return 0;
}

static void read_pad(void)
{
	pad.prev = pad.now;
	pad.now = plat_buttons();
}

static void focus_row(int row)
{
	if (row < 0) row = guide.nch - 1;
	if (row >= guide.nch) row = 0;
	sel_row = row;
	if (sel_row < top_row) top_row = sel_row;
	if (sel_row >= top_row + ROWS) top_row = sel_row - ROWS + 1;
	dirty = 1;
}

static void handle_input(void)
{
	if (mode == MODE_NORELAY) {
		if (pressed(B_OK)) { mode = MODE_CONNECT; dirty = 1; }
		return;
	}
	if (mode == MODE_WATCH) {
		if (pressed(B_UP)) tune(watching <= 0 ? guide.nch - 1 : watching - 1);
		if (pressed(B_DOWN)) tune(watching >= guide.nch - 1 ? 0 : watching + 1);
		if (pressed(B_OK) || pressed(B_INFO)) { banner_until_us = now_us() + 5000000; dirty = 1; }
		if (pressed(B_GUIDE) || pressed(B_BACK) || pressed(B_START)) {
			mode = MODE_GUIDE;
			focus_row(watching >= 0 ? watching : 0);
			focus_ms = clock_ms();
		}
		return;
	}
	if (mode == MODE_GUIDE) {
		channel_t *c = &guide.ch[sel_row];
		if (pressed(B_UP)) focus_row(sel_row - 1);
		if (pressed(B_DOWN)) focus_row(sel_row + 1);
		if (pressed(B_PAGE_UP)) focus_row(sel_row - ROWS < 0 ? 0 : sel_row - ROWS);
		if (pressed(B_PAGE_DOWN)) focus_row(sel_row + ROWS >= guide.nch ? guide.nch - 1 : sel_row + ROWS);
		if (pressed(B_RIGHT)) {
			const show_t *s = show_at(c, focus_ms);
			if (s && s + 1 < c->shows + c->nshows) { focus_ms = (s + 1)->start; dirty = 1; }
		}
		if (pressed(B_LEFT)) {
			const show_t *s = show_at(c, focus_ms);
			s64 now = clock_ms();
			if (s && s > c->shows && (s - 1)->end > now) { focus_ms = (s - 1)->start > now ? (s - 1)->start : now; dirty = 1; }
			else if (focus_ms != now) { focus_ms = now; dirty = 1; }
		}
		if (pressed(B_OK)) {
			if (sel_row != watching) tune(sel_row);
			mode = MODE_WATCH;
			dirty = 1;
		}
		if ((pressed(B_GUIDE) || pressed(B_BACK)) && watching >= 0) { mode = MODE_WATCH; dirty = 1; }
	}
}

// ------------------------------------------------------------------ main

static void connect_relay(void)
{
	if (net_discover(relay_host, &relay_port) != 0) { LOG("relay not found"); mode = MODE_NORELAY; dirty = 1; return; }
	LOG("relay at %s:%d", relay_host, relay_port);
	if (fetch_guide() != 0) { LOG("guide fetch failed"); mode = MODE_NORELAY; dirty = 1; return; }
	LOG("guide: %d channels", guide.nch);
	static int player_up = 0;
	if (!player_up) { player_start(relay_host, relay_port); player_up = 1; }
	tune(load_last());
	mode = MODE_WATCH;
}

int app_main(void)
{
	log_open();
	LOG("SPELUNKR starting");
	if (gfx_init() != 0) { LOG("gfx_init failed"); return 1; }
	L = gfx_is_4x3() ? LAYOUT_4X3 : LAYOUT_HD;
	if (ui_init(gfx_overlay_canvas()) != 0) { LOG("ui_init failed"); return 1; }
	LOG("graphics ready");
	gfx_selftest();
	LOG("net init %d", plat_net_init());
	LOG("audio_start %d", audio_start());

	s64 last = now_us(), last_tick = 0;
	player_state_t last_state = PLAYER_IDLE;
	while (plat_poll()) {
		read_pad();

		if (mode == MODE_CONNECT) {
			// Paint the "looking" screen before the (blocking) discovery.
			render_overlay();
			gfx_frame(0, 0, UI_W, UI_H, 0);
			connect_relay();
			last = now_us();
			continue;
		}

		handle_input();
		s64 t = now_us();
		double dt = (t - last) / 1e6;
		last = t;
		if (mode == MODE_WATCH || mode == MODE_GUIDE) player_update(dt);
		if (guide.nch && t - guide_fetched_us > GUIDE_REFRESH_US) { fetch_guide(); dirty = 1; }

		player_status_t ps;
		player_status(&ps);
		if (ps.state != last_state) { LOG("player state %d (%s) %s", ps.state, ps.type, ps.error); last_state = ps.state; dirty = 1; }
		if (t - last_tick > 1000000) { last_tick = t; dirty = 1; } // clock, progress, banner timeout
		if (dirty) { render_overlay(); dirty = 0; }

		if (mode == MODE_GUIDE) gfx_frame(PIP_X, PIP_Y, PIP_W, PIP_H, 1);
		else gfx_frame(0, 0, UI_W, UI_H, mode == MODE_WATCH);
	}

	LOG("exiting");
	player_stop();
	audio_stop();
	gfx_shutdown();
	return 0;
}
