// SPELUNKR for PS3: a cable-style channel guide and player for spelunkr.tv.
//
// The relay on your PC runs Spelunkr's real schedule and streams each channel as MPEG-1; this
// app finds it on the LAN, shows the guide, and plays channels.
//
// Controls
//   Watching:  Up/Down channel surf · X/Square show info · Triangle/Circle/Start open guide
//   Guide:     Up/Down channel · Left/Right show · L1/R1 page · X watch · Triangle/Circle close

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <io/pad.h>
#include <sys/process.h>
#include <sysutil/sysutil.h>

#include "app.h"
#include "gfx.h"
#include "ui.h"
#include "net.h"
#include "guide.h"
#include "player.h"
#include "audio.h"

SYS_PROCESS_PARAM(1001, 0x100000);

// Network palette (the site's "8s" look).
#define C_PURPLE 0x1C0A57
#define C_INK    0x0b0f26
#define C_PAPER  0xe8eaf0
#define C_GOLD   0xffd740
#define C_NAVY   0x2a3580
#define C_SLATE  0x8899cc
#define C_ORANGE 0xd2500c

#define MARGIN_X 48
#define MARGIN_Y 32

#define GRID_X     304
#define GRID_W     (UI_W - MARGIN_X - GRID_X)
#define GRID_Y     308
#define GRID_HEAD  32
#define ROW_H      56
#define ROWS       6
#define WINDOW_MIN 90
#define GUIDE_HOURS 4
#define GUIDE_REFRESH_US (5 * 60 * 1000000LL)

#define PIP_X 912
#define PIP_Y 112
#define PIP_W 320
#define PIP_H 180

typedef enum { MODE_CONNECT, MODE_NORELAY, MODE_WATCH, MODE_GUIDE } mode_t_;

static volatile int running = 1;
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

static void sysutil_cb(u64 status, u64 param, void *user)
{
	(void)param; (void)user;
	if (status == SYSUTIL_EXIT_GAME) running = 0;
}

// ------------------------------------------------------------------ helpers

static void save_last(const char *id)
{
	FILE *f = fopen(APP_DIR "/last.txt", "w");
	if (f) { fputs(id, f); fclose(f); }
}

static int load_last(void)
{
	char id[64] = { 0 };
	FILE *f = fopen(APP_DIR "/last.txt", "r");
	if (f) { if (!fgets(id, sizeof(id), f)) id[0] = 0; fclose(f); }
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
	ui_text(MARGIN_X, MARGIN_Y, 40, RGB(C_GOLD), "SPELUNKR", 0);
	ui_text(MARGIN_X + 2, MARGIN_Y + 48, 16, RGB(C_SLATE), "CABLE TV, BUT WEIRDER", 0);
	char clk[16];
	format_clock(clock_ms(), guide.tz_minutes, clk, sizeof(clk));
	int w = ui_text_width(30, clk);
	ui_text(UI_W - MARGIN_X - w, MARGIN_Y + 6, 30, RGB(C_PAPER), clk, 0);
}

static void draw_center_message(const char *title, const char *body)
{
	ui_clear(RGB(C_PURPLE));
	draw_header();
	int w = ui_text_width(34, title);
	ui_text((UI_W - w) / 2, 270, 34, RGB(C_GOLD), title, 0);
	ui_text_wrap(UI_W / 2 - 420, 330, 22, 32, RGB(C_PAPER), body, 840, 6);
}

static void draw_tuning_card(const channel_t *c, const char *what)
{
	// Shown over black while a channel starts (the relay is seeking into the tape).
	ui_clear(0);
	ui_logo(UI_W / 2 - 64, 200, 4, c);
	char line[96];
	snprintf(line, sizeof(line), "%s %s", what, c->name);
	int w = ui_text_width(28, line);
	ui_text((UI_W - w) / 2, 360, 28, RGB(C_PAPER), line, 0);
}

static void draw_banner(const channel_t *c, const player_status_t *ps)
{
	int x = MARGIN_X, y = 552, w = UI_W - 2 * MARGIN_X, h = 136;
	ui_rect(x, y, w, h, ARGB(235, C_INK));
	ui_rect(x, y, 8, h, RGB(c->color));
	ui_logo(x + 28, y + 20, 3, c);

	char dial[16];
	dial_label(c, dial, sizeof(dial));
	int tx = x + 148;
	int dw = ui_text(tx, y + 14, 34, RGB(C_GOLD), dial, 0);
	ui_text(tx + dw + (dw ? 16 : 0), y + 20, 26, RGB(C_PAPER), c->name, 0);

	s64 now = clock_ms();
	const show_t *s = guide_show_at(c, now);
	char clk[16];
	format_clock(now, guide.tz_minutes, clk, sizeof(clk));
	int cw = ui_text_width(24, clk);
	ui_text(x + w - 24 - cw, y + 18, 24, RGB(C_PAPER), clk, 0);

	if (s) {
		const char *tag = NULL;
		if (!strcmp(ps->type, "bump-break")) tag = "STATION BREAK";
		else if (!strcmp(ps->type, "ad")) tag = "COMMERCIAL";
		else if (!strcmp(ps->type, "up-next")) tag = "UP NEXT";
		int tagw = 0;
		if (tag) tagw = ui_text(tx, y + 62, 18, RGB(C_ORANGE), tag, 0) + 16;
		ui_text(tx + tagw, y + 60, 22, RGB(C_PAPER), s->title, x + w - 24 - tx - tagw);
		// Progress through the show.
		int bx = tx, by = y + 98, bw = 360;
		float p = (float)(now - s->start) / (float)(s->end - s->start);
		if (p < 0) p = 0;
		if (p > 1) p = 1;
		ui_rect(bx, by, bw, 8, RGB(C_NAVY));
		ui_rect(bx, by, (int)(bw * p), 8, RGB(C_GOLD));
		char tr[48];
		time_range(s, tr, sizeof(tr));
		ui_text(bx + bw + 16, by - 8, 18, RGB(C_SLATE), tr, 0);
		const show_t *next = s + 1 < c->shows + c->nshows ? s + 1 : NULL;
		if (next) {
			char nx[160], t[16];
			format_clock(next->start, guide.tz_minutes, t, sizeof(t));
			snprintf(nx, sizeof(nx), "NEXT %s  %s", t, next->title);
			int nxx = bx + bw + 16 + ui_text_width(18, tr) + 32;
			ui_text(nxx, by - 8, 18, RGB(C_SLATE), nx, x + w - 24 - nxx);
		}
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
	ui_rect(PIP_X - 4, PIP_Y - 4, PIP_W + 8, PIP_H + 8, RGB(C_GOLD));
	ui_clear_rect(PIP_X, PIP_Y, PIP_W, PIP_H);  // hole: the live channel shows through here
	draw_header();
	if (!guide.nch) return;

	s64 now = clock_ms();
	channel_t *sc = &guide.ch[sel_row];
	const show_t *focus = show_at(sc, focus_ms);

	// Info panel for the focused show.
	ui_logo(MARGIN_X, 116, 2, sc);
	char dial[16], head[96];
	dial_label(sc, dial, sizeof(dial));
	snprintf(head, sizeof(head), "%s%s%s", dial, dial[0] ? "  " : "", sc->name);
	ui_text(MARGIN_X + 80, 120, 28, RGB(C_GOLD), head, PIP_X - 32 - MARGIN_X - 80);
	if (focus) {
		int lines = ui_text_wrap(MARGIN_X + 80, 160, 22, 28, RGB(C_PAPER), focus->title, PIP_X - 32 - MARGIN_X - 80, 2);
		char tr[48];
		time_range(focus, tr, sizeof(tr));
		int on_now = focus->start <= now && now < focus->end;
		int ty = 160 + lines * 28 + 6;
		int tw = ui_text(MARGIN_X + 80, ty, 18, RGB(on_now ? C_ORANGE : C_SLATE), on_now ? "ON NOW" : "LATER", 0);
		ui_text(MARGIN_X + 80 + tw + 16, ty, 18, RGB(C_SLATE), tr, 0);
	}
	ui_text_wrap(MARGIN_X, 252, 16, 22, RGB(C_SLATE), sc->desc, PIP_X - 32 - MARGIN_X, 2);

	// Time header.
	s64 ws = window_start(), we = ws + WINDOW_MIN * 60 * 1000LL;
	float ppm = (float)GRID_W / (WINDOW_MIN * 60 * 1000.0f);
	ui_rect(MARGIN_X, GRID_Y, UI_W - 2 * MARGIN_X, GRID_HEAD, RGB(C_INK));
	for (int k = 0; k < WINDOW_MIN / 30; k++) {
		char t[16];
		format_clock(ws + k * 30 * 60 * 1000LL, guide.tz_minutes, t, sizeof(t));
		ui_text(GRID_X + 8 + (int)(k * 30 * 60 * 1000LL * ppm), GRID_Y + 6, 18, RGB(C_PAPER), t, 0);
	}
	ui_text(MARGIN_X + 8, GRID_Y + 6, 18, RGB(C_GOLD), "CHANNEL", 0);

	for (int r = 0; r < ROWS && top_row + r < guide.nch; r++) {
		int ci = top_row + r;
		channel_t *c = &guide.ch[ci];
		int y = GRID_Y + GRID_HEAD + r * ROW_H;
		int selected = ci == sel_row;

		ui_rect(MARGIN_X, y + 2, GRID_X - MARGIN_X - 4, ROW_H - 4, RGB(selected ? C_NAVY : C_INK));
		if (ci == watching) ui_rect(MARGIN_X, y + 2, 6, ROW_H - 4, RGB(C_ORANGE));
		char d[16];
		dial_label(c, d, sizeof(d));
		ui_text(MARGIN_X + 14, y + 14, 22, RGB(C_GOLD), d, 0);
		ui_logo(MARGIN_X + 56, y + 12, 1, c);
		ui_text(MARGIN_X + 96, y + 18, 16, RGB(C_PAPER), c->name, GRID_X - MARGIN_X - 104);

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
			ui_text(x0 + 10, y + 17, 18, RGB(focused ? C_INK : C_PAPER), label, x1 - x0 - 20);
		}
	}
	// Now line.
	if (now >= ws && now < we) ui_rect(GRID_X + (int)((now - ws) * ppm) - 1, GRID_Y, 3, GRID_HEAD + ROWS * ROW_H, RGB(C_ORANGE));

	ui_text(MARGIN_X, UI_H - 30, 16, RGB(C_SLATE),
	        "X WATCH     LEFT/RIGHT SHOWS     L1/R1 PAGE     TRIANGLE CLOSE", 0);
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
		draw_center_message("CAN'T FIND THE RELAY",
			"Start the relay on your PC (in spelunkr-ps3/relay run: npm start) and make sure the PS3 is on the same "
			"network. If it still isn't found, put its address in " APP_DIR "/relay.txt, e.g. 192.168.1.20:8642. "
			"Press X to try again.");
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
		if (ps.state == PLAYER_BUFFERING) ui_text(UI_W - MARGIN_X - 170, MARGIN_Y, 22, RGB(C_GOLD), "BUFFERING...", 0);
		break;
	}
	case MODE_GUIDE:
		draw_guide();
		break;
	}
	gfx_overlay_commit();
}

// ------------------------------------------------------------------ input

typedef struct { padData now, prev; s64 held_since[16]; s64 last_repeat[16]; } pad_t;
static pad_t pad;

enum { B_UP, B_DOWN, B_LEFT, B_RIGHT, B_CROSS, B_CIRCLE, B_TRIANGLE, B_SQUARE, B_START, B_L1, B_R1, B_COUNT };

static int button(const padData *d, int b)
{
	switch (b) {
	case B_UP: return d->BTN_UP;
	case B_DOWN: return d->BTN_DOWN;
	case B_LEFT: return d->BTN_LEFT;
	case B_RIGHT: return d->BTN_RIGHT;
	case B_CROSS: return d->BTN_CROSS;
	case B_CIRCLE: return d->BTN_CIRCLE;
	case B_TRIANGLE: return d->BTN_TRIANGLE;
	case B_SQUARE: return d->BTN_SQUARE;
	case B_START: return d->BTN_START;
	case B_L1: return d->BTN_L1;
	case B_R1: return d->BTN_R1;
	}
	return 0;
}

// Pressed this frame, or held long enough to auto-repeat (d-pad and shoulders).
static int pressed(int b)
{
	int down = button(&pad.now, b), was = button(&pad.prev, b);
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
	padInfo info;
	ioPadGetInfo(&info);
	for (int i = 0; i < MAX_PADS; i++) {
		if (!info.status[i]) continue;
		padData d;
		if (ioPadGetData(i, &d) == 0 && d.len > 0) { pad.now = d; return; }
		if (d.len == 0) return; // no change since last read: keep state
	}
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
		if (pressed(B_CROSS)) { mode = MODE_CONNECT; dirty = 1; }
		return;
	}
	if (mode == MODE_WATCH) {
		if (pressed(B_UP)) tune(watching <= 0 ? guide.nch - 1 : watching - 1);
		if (pressed(B_DOWN)) tune(watching >= guide.nch - 1 ? 0 : watching + 1);
		if (pressed(B_CROSS) || pressed(B_SQUARE)) { banner_until_us = now_us() + 5000000; dirty = 1; }
		if (pressed(B_TRIANGLE) || pressed(B_CIRCLE) || pressed(B_START)) {
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
		if (pressed(B_L1)) focus_row(sel_row - ROWS < 0 ? 0 : sel_row - ROWS);
		if (pressed(B_R1)) focus_row(sel_row + ROWS >= guide.nch ? guide.nch - 1 : sel_row + ROWS);
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
		if (pressed(B_CROSS)) {
			if (sel_row != watching) tune(sel_row);
			mode = MODE_WATCH;
			dirty = 1;
		}
		if ((pressed(B_TRIANGLE) || pressed(B_CIRCLE)) && watching >= 0) { mode = MODE_WATCH; dirty = 1; }
	}
}

// ------------------------------------------------------------------ main

static void connect_relay(void)
{
	if (net_discover(relay_host, &relay_port) != 0 || fetch_guide() != 0) { mode = MODE_NORELAY; dirty = 1; return; }
	static int player_up = 0;
	if (!player_up) { player_start(relay_host, relay_port); player_up = 1; }
	tune(load_last());
	mode = MODE_WATCH;
}

int main(int argc, char *argv[])
{
	(void)argc; (void)argv;
	if (gfx_init() != 0) return 1;
	if (ui_init(gfx_overlay_canvas()) != 0) return 1;
	ioPadInit(7);
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_cb, NULL);
	net_start();
	audio_start();

	s64 last = now_us(), last_tick = 0;
	player_state_t last_state = PLAYER_IDLE;
	while (running) {
		sysUtilCheckCallback();
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
		if (ps.state != last_state) { last_state = ps.state; dirty = 1; }
		if (t - last_tick > 1000000) { last_tick = t; dirty = 1; } // clock, progress, banner timeout
		if (dirty) { render_overlay(); dirty = 0; }

		if (mode == MODE_GUIDE) gfx_frame(PIP_X, PIP_Y, PIP_W, PIP_H, 1);
		else gfx_frame(0, 0, UI_W, UI_H, mode == MODE_WATCH);
	}

	player_stop();
	audio_stop();
	gfx_shutdown();
	ioPadEnd();
	return 0;
}
