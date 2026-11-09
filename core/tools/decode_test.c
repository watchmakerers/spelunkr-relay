// Host-side check: can pl_mpeg decode the relay's segments, fed in small chunks the way the
// PS3 player feeds it? Usage: decode_test file.mpg
#include <stdio.h>
#include <stdlib.h>
#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg.h"

static FILE *f; static long fed; static int frames, w, h; static long samples; static double last_t;
static void load(plm_buffer_t *b, void *u) {
	(void)u; unsigned char tmp[16384]; size_t n = fread(tmp, 1, sizeof tmp, f);
	if (n) { plm_buffer_write(b, tmp, n); fed += n; } else plm_buffer_signal_end(b);
}
static void vid(plm_t *p, plm_frame_t *fr, void *u) { (void)p; (void)u; frames++; w = fr->width; h = fr->height; last_t = fr->time; }
static void aud(plm_t *p, plm_samples_t *s, void *u) { (void)p; (void)u; samples += s->count; }
int main(int c, char **v) {
	f = fopen(v[1], "rb"); if (!f) return 1;
	plm_buffer_t *b = plm_buffer_create_with_capacity(256 * 1024);
	plm_buffer_set_load_callback(b, load, NULL);
	plm_t *p = plm_create_with_buffer(b, 1);
	plm_set_video_decode_callback(p, vid, NULL); plm_set_audio_decode_callback(p, aud, NULL);
	plm_set_audio_enabled(p, 1); plm_set_audio_lead_time(p, 0.25);
	printf("streams: video=%d audio=%d rate=%d fps=%.3f\n", plm_get_num_video_streams(p), plm_get_num_audio_streams(p), plm_get_samplerate(p), plm_get_framerate(p));
	int steps = 0;
	while (!plm_has_ended(p) && steps < 200000) { plm_decode(p, 1.0 / 60); steps++; }
	printf("frames=%d size=%dx%d video_end=%.2fs audio=%.2fs ended=%d steps=%d fed=%ld\n", frames, w, h, last_t, samples / 48000.0, plm_has_ended(p), steps, fed);
	return 0;
}
