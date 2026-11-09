// SDL2 platform layer, shared by the desktop app (Windows, macOS, Linux) and the PS Vita app.
//
// Video: the decoded frame goes into a YUV texture (or, if the renderer can't do YUV, is
// converted on the CPU), the UI canvas into an ARGB texture blended on top. The renderer's
// logical size is the UI canvas, so the window can be any size.
// Audio: a ring buffer drained by SDL's audio callback.
// Input: keyboard and game controllers (desktop), the built-in buttons (Vita).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL.h>

#ifdef _WIN32
#  include <winsock2.h>
#elif defined(__vita__)
#  include <psp2/ctrl.h>
#  include <psp2/io/stat.h>
#  include <psp2/net/net.h>
#  include <psp2/net/netctl.h>
#  include <psp2/power.h>
#  include <psp2/sysmodule.h>
#else
#  include <signal.h>
#endif

#include "platform.h"
#include "gfx.h"
#include "audio.h"

#ifdef __vita__
#  define DATA_DIR "ux0:data/spelunkr"
// The default newlib heap is too small for the 12 MB stream buffer plus SDL's textures.
int _newlib_heap_size_user = 160 * 1024 * 1024;
#endif

const char *plat_relay_override = NULL;

// ---------------------------------------------------------------- time and threads

s64 now_us(void)
{
	static Uint64 freq, base;
	if (!freq) { freq = SDL_GetPerformanceFrequency(); base = SDL_GetPerformanceCounter(); }
	Uint64 t = SDL_GetPerformanceCounter() - base;
	return (s64)(t / freq) * 1000000 + (s64)((t % freq) * 1000000 / freq);
}

void plat_sleep_us(int us) { SDL_Delay(us < 1000 ? 1 : (Uint32)(us / 1000)); }

struct plat_mutex { SDL_mutex *m; };
struct plat_thread { SDL_Thread *t; };

plat_mutex *plat_mutex_new(void)
{
	plat_mutex *m = malloc(sizeof(*m));
	if (m && !(m->m = SDL_CreateMutex())) { free(m); m = NULL; }
	return m;
}
void plat_lock(plat_mutex *m) { SDL_LockMutex(m->m); }
void plat_unlock(plat_mutex *m) { SDL_UnlockMutex(m->m); }

typedef struct { void (*fn)(void *); void *arg; } start_t;
static int trampoline(void *p)
{
	start_t s = *(start_t *)p;
	free(p);
	s.fn(s.arg);
	return 0;
}
plat_thread *plat_thread_start(void (*fn)(void *), void *arg, const char *name)
{
	plat_thread *t = malloc(sizeof(*t));
	start_t *s = malloc(sizeof(*s));
	if (!t || !s) { free(t); free(s); return NULL; }
	s->fn = fn;
	s->arg = arg;
	if (!(t->t = SDL_CreateThread(trampoline, name, s))) { free(t); free(s); return NULL; }
	return t;
}
void plat_thread_join(plat_thread *t)
{
	if (!t) return;
	SDL_WaitThread(t->t, NULL);
	free(t);
}

// ---------------------------------------------------------------- files and network

static char data_dir[512];

const char *plat_data_dir(void)
{
	if (!data_dir[0]) {
#ifdef __vita__
		sceIoMkdir(DATA_DIR, 0777);
		snprintf(data_dir, sizeof(data_dir), "%s", DATA_DIR);
#else
		char *p = SDL_GetPrefPath("spelunkr", "spelunkr");
		snprintf(data_dir, sizeof(data_dir), "%s", p ? p : ".");
		size_t n = strlen(data_dir);
		if (n > 1 && (data_dir[n - 1] == '/' || data_dir[n - 1] == '\\')) data_dir[n - 1] = 0;
		SDL_free(p);
#endif
	}
	return data_dir;
}

int plat_net_init(void)
{
#ifdef _WIN32
	WSADATA wsa;
	return WSAStartup(MAKEWORD(2, 2), &wsa);
#elif defined(__vita__)
	sceSysmoduleLoadModule(SCE_SYSMODULE_NET);
	static char pool[1024 * 1024];
	SceNetInitParam p = { pool, sizeof(pool), 0 };
	int r = sceNetInit(&p);
	if (r < 0 && r != (int)0x80410201 /* already initialised */) return r;
	return sceNetCtlInit() < 0 ? -1 : 0;
#else
	signal(SIGPIPE, SIG_IGN); // a relay that hangs up mid-send must not kill the app
	return 0;
#endif
}

// ---------------------------------------------------------------- video

int ui_w = 1280, ui_h = 720;
static int want_4x3;
#ifndef __vita__
static int want_fullscreen;
#endif
static SDL_Window *win;
static SDL_Renderer *ren;
static SDL_Texture *overlay, *video;
static int video_w, video_h, video_yuv = 1, have_video;
static u32 canvas[UI_MAX_W * UI_MAX_H];
static u32 *rgb;                   // CPU conversion target when the renderer lacks YUV
static int rgb_cap;

int gfx_is_4x3(void) { return want_4x3; }

const char *gfx_stream_aspect(void)
{
#ifdef __vita__
	return "16x9s";  // 640x360: a 5" screen doesn't need more, and it decodes easily
#else
	return want_4x3 ? "4x3" : "16x9";
#endif
}

int gfx_init(void)
{
	if (want_4x3) { ui_w = 720; ui_h = 540; }
#ifdef __vita__
	win = SDL_CreateWindow("SPELUNKR", 0, 0, 960, 544, SDL_WINDOW_SHOWN);
#else
	Uint32 flags = SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI | (want_fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
	int ww = want_4x3 ? 960 : 1280, wh = 720;
	win = SDL_CreateWindow("SPELUNKR", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, ww, wh, flags);
#endif
	if (!win) { fprintf(stderr, "window: %s\n", SDL_GetError()); return -1; }
	ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
	if (!ren) ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
	if (!ren) { fprintf(stderr, "renderer: %s\n", SDL_GetError()); return -1; }
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
	SDL_RenderSetLogicalSize(ren, ui_w, ui_h);
	overlay = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, ui_w, ui_h);
	if (!overlay) return -1;
	SDL_SetTextureBlendMode(overlay, SDL_BLENDMODE_BLEND);
	return 0;
}

void gfx_shutdown(void)
{
	if (video) SDL_DestroyTexture(video);
	if (overlay) SDL_DestroyTexture(overlay);
	if (ren) SDL_DestroyRenderer(ren);
	if (win) SDL_DestroyWindow(win);
	free(rgb);
}

static void yuv_to_argb(const u8 *y, u32 ys, const u8 *cb, const u8 *cr, u32 cs, u32 w, u32 h)
{
	if ((int)(w * h) > rgb_cap) { free(rgb); rgb_cap = w * h; rgb = malloc(rgb_cap * 4); }
	if (!rgb) { rgb_cap = 0; return; }
	for (u32 j = 0; j < h; j++) {
		const u8 *yr = y + j * ys, *ur = cb + (j / 2) * cs, *vr = cr + (j / 2) * cs;
		u32 *o = rgb + j * w;
		for (u32 i = 0; i < w; i++) {
			int Y = (yr[i] - 16) * 298, U = ur[i / 2] - 128, V = vr[i / 2] - 128;
			int r = (Y + 409 * V + 128) >> 8, g = (Y - 100 * U - 208 * V + 128) >> 8, b = (Y + 516 * U + 128) >> 8;
			r = r < 0 ? 0 : r > 255 ? 255 : r;
			g = g < 0 ? 0 : g > 255 ? 255 : g;
			b = b < 0 ? 0 : b > 255 ? 255 : b;
			o[i] = 0xff000000u | (u32)r << 16 | (u32)g << 8 | (u32)b;
		}
	}
}

void gfx_video_upload(const u8 *y, u32 ystride, const u8 *cb, const u8 *cr, u32 cstride, u32 w, u32 h)
{
	if (!video || (int)w != video_w || (int)h != video_h) {
		if (video) SDL_DestroyTexture(video);
		video = video_yuv ? SDL_CreateTexture(ren, SDL_PIXELFORMAT_IYUV, SDL_TEXTUREACCESS_STREAMING, w, h) : NULL;
		if (!video) {
			video_yuv = 0;
			video = SDL_CreateTexture(ren, SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING, w, h);
		}
		if (!video) return;
		video_w = w;
		video_h = h;
	}
	if (video_yuv) SDL_UpdateYUVTexture(video, NULL, y, ystride, cb, cstride, cr, cstride);
	else {
		yuv_to_argb(y, ystride, cb, cr, cstride, w, h);
		if (rgb) SDL_UpdateTexture(video, NULL, rgb, w * 4);
	}
	have_video = 1;
}

void gfx_video_clear(void) { have_video = 0; }
u32 *gfx_overlay_canvas(void) { return canvas; }
void gfx_overlay_commit(void) { SDL_UpdateTexture(overlay, NULL, canvas, ui_w * 4); }
void gfx_selftest(void) {}

static const char *shot_path;
static double shot_after;
static int shot_taken;
#ifndef __vita__
static int shot_guide;
#endif

void gfx_frame(float vx, float vy, float vw, float vh, int show_video)
{
	int dark = show_video && have_video;
	SDL_SetRenderDrawColor(ren, dark ? 0 : 0x1C, dark ? 0 : 0x0A, dark ? 0 : 0x57, 255);
	SDL_RenderClear(ren);
	if (show_video && have_video && video) {
		// Fit the frame (square pixels from the relay) into the rect.
		float fa = (float)video_w / video_h, ra = vw / vh, w = vw, h = vh;
		if (fa > ra) h = vw / fa; else w = vh * fa;
		SDL_FRect r = { vx + (vw - w) / 2, vy + (vh - h) / 2, w, h };
		SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
		SDL_FRect bg = { vx, vy, vw, vh };
		SDL_RenderFillRectF(ren, &bg);
		SDL_RenderCopyF(ren, video, NULL, &r);
	}
	SDL_RenderCopy(ren, overlay, NULL, NULL);
	if (shot_path && !shot_taken && now_us() / 1e6 >= shot_after) {
		// Development aid (--shot): save this frame and quit.
		int ow = ui_w, oh = ui_h; // read at the window's real size (never smaller than what's read)
		SDL_GetRendererOutputSize(ren, &ow, &oh);
		SDL_Surface *s = SDL_CreateRGBSurfaceWithFormat(0, ow, oh, 32, SDL_PIXELFORMAT_ARGB8888);
		if (s && SDL_RenderReadPixels(ren, NULL, SDL_PIXELFORMAT_ARGB8888, s->pixels, s->pitch) == 0) SDL_SaveBMP(s, shot_path);
		SDL_FreeSurface(s);
		shot_taken = 1;
	}
	SDL_RenderPresent(ren);
}

// ---------------------------------------------------------------- audio

#define A_RATE 48000
#define A_RING (A_RATE * 2 * 2)   // 2 seconds of stereo frames, in floats
static SDL_AudioDeviceID adev;
static float aring[A_RING];
static int ahead, acount;          // in floats
static float avol = 1.0f;

static void audio_cb(void *u, Uint8 *stream, int len)
{
	(void)u;
	float *out = (float *)stream;
	int want = len / (int)sizeof(float), n = want < acount ? want : acount;
	for (int i = 0; i < n; i++) out[i] = aring[(ahead + i) % A_RING] * avol;
	for (int i = n; i < want; i++) out[i] = 0;
	ahead = (ahead + n) % A_RING;
	acount -= n;
}

int audio_start(void)
{
	SDL_AudioSpec want = { 0 }, have;
	want.freq = A_RATE;
	want.format = AUDIO_F32SYS;
	want.channels = 2;
	want.samples = 1024;
	want.callback = audio_cb;
	adev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
	if (!adev) return -1;
	SDL_PauseAudioDevice(adev, 0);
	return 0;
}

void audio_stop(void) { if (adev) SDL_CloseAudioDevice(adev); adev = 0; }

void audio_push(const float *s, int frames)
{
	if (!adev) return;
	SDL_LockAudioDevice(adev);
	int n = frames * 2;
	if (n > A_RING - acount) n = A_RING - acount;
	int tail = (ahead + acount) % A_RING;
	for (int i = 0; i < n; i++) aring[(tail + i) % A_RING] = s[i];
	acount += n;
	SDL_UnlockAudioDevice(adev);
}

void audio_clear(void)
{
	if (!adev) return;
	SDL_LockAudioDevice(adev);
	acount = 0;
	SDL_UnlockAudioDevice(adev);
}

double audio_buffered(void)
{
	if (!adev) return 0;
	SDL_LockAudioDevice(adev);
	int n = acount;
	SDL_UnlockAudioDevice(adev);
	return n / 2.0 / A_RATE;
}

void audio_set_volume(float v) { avol = v; }

// ---------------------------------------------------------------- input and events

static int quit;

int plat_poll(void)
{
	SDL_Event e;
	while (SDL_PollEvent(&e)) {
		switch (e.type) {
		case SDL_QUIT: quit = 1; break;
		case SDL_CONTROLLERDEVICEADDED: SDL_GameControllerOpen(e.cdevice.which); break;
		case SDL_KEYDOWN:
			if (e.key.keysym.sym == SDLK_F11 || (e.key.keysym.sym == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
				Uint32 fs = SDL_GetWindowFlags(win) & SDL_WINDOW_FULLSCREEN_DESKTOP;
				SDL_SetWindowFullscreen(win, fs ? 0 : SDL_WINDOW_FULLSCREEN_DESKTOP);
			}
			if (e.key.keysym.sym == SDLK_q && (e.key.keysym.mod & (KMOD_CTRL | KMOD_GUI))) quit = 1;
			break;
		}
	}
	if (shot_path && shot_taken) quit = 1;
	return !quit;
}

u32 plat_buttons(void)
{
	u32 b = 0;
#ifdef __vita__
	SceCtrlData pad;
	if (sceCtrlPeekBufferPositive(0, &pad, 1) > 0) {
		u32 x = pad.buttons;
		b |= (x & SCE_CTRL_UP ? BTN(B_UP) : 0) | (x & SCE_CTRL_DOWN ? BTN(B_DOWN) : 0) |
		     (x & SCE_CTRL_LEFT ? BTN(B_LEFT) : 0) | (x & SCE_CTRL_RIGHT ? BTN(B_RIGHT) : 0) |
		     (x & SCE_CTRL_CROSS ? BTN(B_OK) : 0) | (x & SCE_CTRL_CIRCLE ? BTN(B_BACK) : 0) |
		     (x & SCE_CTRL_TRIANGLE ? BTN(B_GUIDE) : 0) | (x & SCE_CTRL_SQUARE ? BTN(B_INFO) : 0) |
		     (x & SCE_CTRL_START ? BTN(B_START) : 0) | (x & SCE_CTRL_LTRIGGER ? BTN(B_PAGE_UP) : 0) |
		     (x & SCE_CTRL_RTRIGGER ? BTN(B_PAGE_DOWN) : 0);
		// The left stick works as a d-pad too.
		if (pad.ly < 40) b |= BTN(B_UP);
		if (pad.ly > 215) b |= BTN(B_DOWN);
		if (pad.lx < 40) b |= BTN(B_LEFT);
		if (pad.lx > 215) b |= BTN(B_RIGHT);
	}
#else
	// --shot-guide (development aid): press the guide button once, shortly before the shot.
	static int guide_pressed;
	if (shot_guide && !guide_pressed && now_us() / 1e6 >= shot_after - 3) { guide_pressed = 1; return BTN(B_GUIDE); }
	const Uint8 *k = SDL_GetKeyboardState(NULL);
	if (k[SDL_SCANCODE_UP] || k[SDL_SCANCODE_W]) b |= BTN(B_UP);
	if (k[SDL_SCANCODE_DOWN] || k[SDL_SCANCODE_S]) b |= BTN(B_DOWN);
	if (k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_A]) b |= BTN(B_LEFT);
	if (k[SDL_SCANCODE_RIGHT] || k[SDL_SCANCODE_D]) b |= BTN(B_RIGHT);
	if ((k[SDL_SCANCODE_RETURN] || k[SDL_SCANCODE_KP_ENTER]) && !(SDL_GetModState() & KMOD_ALT)) b |= BTN(B_OK);
	if (k[SDL_SCANCODE_ESCAPE] || k[SDL_SCANCODE_BACKSPACE]) b |= BTN(B_BACK);
	if (k[SDL_SCANCODE_G] || k[SDL_SCANCODE_TAB]) b |= BTN(B_GUIDE);
	if (k[SDL_SCANCODE_I] || k[SDL_SCANCODE_SPACE]) b |= BTN(B_INFO);
	if (k[SDL_SCANCODE_PAGEUP]) b |= BTN(B_PAGE_UP);
	if (k[SDL_SCANCODE_PAGEDOWN]) b |= BTN(B_PAGE_DOWN);
	for (int i = 0; i < SDL_NumJoysticks(); i++) {
		SDL_GameController *c = SDL_GameControllerFromInstanceID(SDL_JoystickGetDeviceInstanceID(i));
		if (!c) continue;
#define P(btn) SDL_GameControllerGetButton(c, SDL_CONTROLLER_BUTTON_##btn)
		if (P(DPAD_UP)) b |= BTN(B_UP);
		if (P(DPAD_DOWN)) b |= BTN(B_DOWN);
		if (P(DPAD_LEFT)) b |= BTN(B_LEFT);
		if (P(DPAD_RIGHT)) b |= BTN(B_RIGHT);
		if (P(A)) b |= BTN(B_OK);
		if (P(B)) b |= BTN(B_BACK);
		if (P(Y)) b |= BTN(B_GUIDE);
		if (P(X)) b |= BTN(B_INFO);
		if (P(START)) b |= BTN(B_START);
		if (P(LEFTSHOULDER)) b |= BTN(B_PAGE_UP);
		if (P(RIGHTSHOULDER)) b |= BTN(B_PAGE_DOWN);
#undef P
		Sint16 ly = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTY), lx = SDL_GameControllerGetAxis(c, SDL_CONTROLLER_AXIS_LEFTX);
		if (ly < -20000) b |= BTN(B_UP);
		if (ly > 20000) b |= BTN(B_DOWN);
		if (lx < -20000) b |= BTN(B_LEFT);
		if (lx > 20000) b |= BTN(B_RIGHT);
	}
#endif
	return b;
}

#ifdef __vita__
const char *plat_hint_guide(void) { return "X WATCH     LEFT/RIGHT SHOWS     L/R PAGE     TRIANGLE CLOSE"; }
const char *plat_relay_help(void)
{
	return "Start the relay on your PC (in the relay folder run: npm start) and make sure the Vita is on the same "
	       "Wi-Fi. If it still isn't found, put its address in ux0:data/spelunkr/relay.txt, e.g. 192.168.1.20:8642. "
	       "Press X to try again.";
}
#else
const char *plat_hint_guide(void) { return "ENTER WATCH     LEFT/RIGHT SHOWS     PGUP/PGDN PAGE     ESC CLOSE"; }
const char *plat_relay_help(void)
{
	static char msg[1024];
	snprintf(msg, sizeof(msg),
	         "Start the relay on your PC (in the relay folder run: npm start) and make sure this computer can reach "
	         "it. If it still isn't found, start the app with --relay 192.168.1.20:8642 (your relay's address), or "
	         "put that address in %s/relay.txt. Press Enter to try again.", plat_data_dir());
	return msg;
}
#endif

// ---------------------------------------------------------------- main

int main(int argc, char *argv[])
{
#ifdef __vita__
	(void)argc; (void)argv;
	scePowerSetArmClockFrequency(444);
	scePowerSetBusClockFrequency(222);
	scePowerSetGpuClockFrequency(222);
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
#else
	for (int i = 1; i < argc; i++) {
		if (!strcmp(argv[i], "--relay") && i + 1 < argc) plat_relay_override = argv[++i];
		else if (!strcmp(argv[i], "--fullscreen")) want_fullscreen = 1;
		else if (!strcmp(argv[i], "--4x3")) want_4x3 = 1;
		else if (!strcmp(argv[i], "--shot") && i + 2 < argc) { shot_path = argv[++i]; shot_after = atof(argv[++i]); }
		else if (!strcmp(argv[i], "--shot-guide")) shot_guide = 1;
		else if (!strcmp(argv[i], "--help") || !strcmp(argv[i], "-h")) {
			printf("SPELUNKR - cable TV, but weirder (connects to a Spelunkr relay on your network)\n\n"
			       "  --relay HOST:PORT   relay address (otherwise relay.txt, then a LAN search)\n"
			       "  --fullscreen        start fullscreen (F11 or Alt+Enter toggles)\n"
			       "  --4x3               4:3 layout and video, like the PS3 app on an SD TV\n\n"
			       "Keys: Up/Down channels, Enter watch, G/Tab guide, I/Space info, PgUp/PgDn page, Esc back,\n"
			       "      Ctrl+Q quit. Game controllers work too (A watch, B back, Y guide, X info, LB/RB page).\n"
			       "Data folder: %s\n", plat_data_dir());
			return 0;
		}
	}
#endif
	if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) != 0) {
		fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
		return 1;
	}
	int r = app_main();
	SDL_Quit();
	return r;
}
