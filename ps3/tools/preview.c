// Host-side preview of the PS3 screens: draws them with the app's real UI code from a guide
// file saved from the relay, and writes PPM images. Hardware calls are stubbed.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "../source/player.h"

static player_status_t fake_status;
static s64 fake_now_us;

// --- stubs for what main.c / guide.c use from the hardware modules
static u32 canvas_mem[1280 * 720];
u32 *gfx_overlay_canvas(void) { return canvas_mem; }
void gfx_overlay_commit(void) {}
void gfx_frame(float a, float b, float c, float d, int e) { (void)a; (void)b; (void)c; (void)d; (void)e; }
int gfx_init(void) { return 0; }
void gfx_shutdown(void) {}
int net_start(void) { return 0; }
int net_discover(char *h, int *p) { (void)h; (void)p; return -1; }
char *http_get_all(const char *h, int p, const char *path, int *l) { (void)h; (void)p; (void)path; (void)l; return NULL; }
s64 now_us(void) { return fake_now_us; }
int player_start(const char *h, int p) { (void)h; (void)p; return 0; }
void player_stop(void) {}
void player_tune(const char *id) { (void)id; }
void player_update(double dt) { (void)dt; }
void player_status(player_status_t *o) { *o = fake_status; }
int audio_start(void) { return 0; }
void audio_stop(void) {}

#define main app_main
#include "../source/main.c"
#undef main

static void dump(const char *path, int checker)
{
	// Composite the overlay over a stand-in "video" (a grey checkerboard) like the GPU would.
	FILE *f = fopen(path, "wb");
	fprintf(f, "P6\n%d %d\n255\n", UI_W, UI_H);
	for (int i = 0; i < UI_W * UI_H; i++) {
		int x = i % UI_W, y = i / UI_W;
		int in_pip = mode == MODE_GUIDE ? (x >= PIP_X && x < PIP_X + PIP_W && y >= PIP_Y && y < PIP_Y + PIP_H) : 1;
		u32 bg = checker && in_pip ? (((x / 40 + y / 40) & 1) ? 0x505060 : 0x707080) : 0;
		u32 p = canvas_mem[i], a = p >> 24;
		unsigned char rgb[3];
		for (int k = 0; k < 3; k++) {
			u32 sc = (p >> (16 - 8 * k)) & 255, dc = (bg >> (16 - 8 * k)) & 255;
			rgb[k] = (unsigned char)((sc * a + dc * (255 - a)) / 255);
		}
		fwrite(rgb, 1, 3, f);
	}
	fclose(f);
}

int main(int argc, char **argv)
{
	FILE *f = fopen(argv[1], "rb");
	fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
	char *text = malloc(n + 1); fread(text, 1, n, f); text[n] = 0; fclose(f);
	fake_now_us = 1000000;
	if (ui_init(gfx_overlay_canvas()) != 0 || guide_parse(&guide, text) != 0) { fprintf(stderr, "init/parse failed\n"); return 1; }
	printf("%d channels, tz %d\n", guide.nch, guide.tz_minutes);

	watching = 1;
	mode = MODE_GUIDE; focus_row(3); focus_ms = clock_ms();
	render_overlay(); dump(argv[2], 1);

	// Move focus right twice on that row to a later show.
	channel_t *c = &guide.ch[sel_row];
	const show_t *s = show_at(c, focus_ms);
	if (s && s + 2 < c->shows + c->nshows) focus_ms = (s + 2)->start;
	render_overlay(); dump(argv[3], 1);

	mode = MODE_WATCH; banner_until_us = fake_now_us + 5000000;
	fake_status.state = PLAYER_PLAYING; strcpy(fake_status.type, "video");
	render_overlay(); dump(argv[4], 1);

	fake_status.state = PLAYER_TUNING;
	render_overlay(); dump(argv[5], 0);
	return 0;
}
