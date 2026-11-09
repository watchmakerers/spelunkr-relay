#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"

#include "pl_mpeg.h"
#include "player.h"
#include "net.h"
#include "guide.h"
#include "audio.h"
#include "gfx.h"
#include "log.h"

#define RING_SIZE   (12 * 1024 * 1024)  // ~30 s at the relay's bitrate
#define MAX_SEGS    8
#define START_BYTES (192 * 1024)        // buffered before a segment's decoder starts
#define RESUME_BYTES (1024 * 1024)      // after running dry, wait for this much
#define AUDIO_LEAD  0.25                // seconds of audio decoded ahead of the picture

typedef struct {
	u64 start_pos, end_pos;   // absolute ring positions; end_pos = ~0 until fully received
	s64 start_ms, end_ms;
	char title[128];
	char type[16];
} seg_t;

static char host[64];
static int port;
static plat_mutex *lock;
static plat_thread *thread;
static volatile int running;

// Shared with the network thread (under lock).
static u8 *ring;
static u64 wpos, rpos;
static seg_t segs[MAX_SEGS];
static int seg_head, seg_count;
static volatile u32 gen;          // bumps on every tune; stale network work is dropped
static char chan[48];
static s64 next_at;
static char net_error[96];
static int net_failures;

// Decoder state (main thread only).
static plm_t *plm;
static plm_buffer_t *pbuf;        // plm's input buffer (plm_t is opaque)
static int buffering;
static plm_frame_t *pending_frame;

static void L(void) { plat_lock(lock); }
static void U(void) { plat_unlock(lock); }

static void url_decode(char *s)
{
	char *o = s;
	for (; *s; s++) {
		if (*s == '%' && s[1] && s[2]) { char h[3] = { s[1], s[2], 0 }; *o++ = (char)strtol(h, NULL, 16); s += 2; }
		else *o++ = *s;
	}
	*o = 0;
}

// ---------------------------------------------------------------- network thread

static int ring_write(u32 my_gen, const u8 *data, int n)
{
	while (n > 0) {
		L();
		if (gen != my_gen) { U(); return -1; }
		u64 space = RING_SIZE - (wpos - rpos);
		if (space == 0) { U(); plat_sleep_us(5000); if (!running) return -1; continue; }
		int chunk = n < (int)space ? n : (int)space;
		for (int i = 0; i < chunk;) {
			u64 at = (wpos + i) % RING_SIZE;
			int run = chunk - i;
			if (at + run > RING_SIZE) run = RING_SIZE - at;
			memcpy(ring + at, data + i, run);
			i += run;
		}
		wpos += chunk;
		U();
		data += chunk;
		n -= chunk;
	}
	return 0;
}

static void net_thread(void *arg)
{
	(void)arg;
	static u8 buf[64 * 1024];
	while (running) {
		L();
		u32 my_gen = gen;
		char id[48];
		strcpy(id, chan);
		s64 at = next_at;
		int full = seg_count >= MAX_SEGS;
		U();
		if (!id[0] || full) { plat_sleep_us(20000); continue; }

		char path[160];
		snprintf(path, sizeof(path), "/ps3/seg/%s?at=%lld&aspect=%s", id, (long long)at, gfx_stream_aspect());
		http_t h;
		int opened = http_open(&h, host, port, path);
		LOG("segment %s -> %s (status %d)", path, opened == 0 ? "open" : "failed", h.status);
		if (opened != 0 || h.status != 200) {
			if (h.sock >= 0) http_close(&h);
			L();
			if (gen == my_gen) { net_failures++; snprintf(net_error, sizeof(net_error), "Can't reach the relay at %s:%d", host, port); }
			U();
			for (int i = 0; i < 20 && running && gen == my_gen; i++) plat_sleep_us(100000);
			continue;
		}

		seg_t s;
		memset(&s, 0, sizeof(s));
		char v[256];
		s.start_ms = http_header(&h, "x-seg-start", v, sizeof(v)) ? strtoll(v, NULL, 10) : at;
		s.end_ms = http_header(&h, "x-seg-end", v, sizeof(v)) ? strtoll(v, NULL, 10) : at + 60000;
		if (http_header(&h, "x-seg-title", v, sizeof(v))) { url_decode(v); snprintf(s.title, sizeof(s.title), "%.127s", v); }
		if (http_header(&h, "x-seg-type", v, sizeof(v))) snprintf(s.type, sizeof(s.type), "%.15s", v);

		L();
		if (gen != my_gen) { U(); http_close(&h); continue; }
		net_failures = 0;
		net_error[0] = 0;
		s.start_pos = wpos;
		s.end_pos = ~0ull;
		int idx = (seg_head + seg_count) % MAX_SEGS;
		segs[idx] = s;
		seg_count++;
		U();

		for (;;) {
			int r = http_read(&h, buf, sizeof(buf));
			if (!running || gen != my_gen) break;
			if (r == -2) continue;
			if (r <= 0) break;
			if (ring_write(my_gen, buf, r) != 0) break;
		}
		http_close(&h);

		L();
		if (gen == my_gen) {
			segs[idx].end_pos = wpos;
			// Whatever happened, carry on with the next entry on the schedule.
			next_at = s.end_ms > at ? s.end_ms : at + 1000;
		}
		U();
	}
}

// ---------------------------------------------------------------- decoder (main thread)

static void on_video(plm_t *p, plm_frame_t *f, void *user) { (void)p; (void)user; pending_frame = f; }

static void on_audio(plm_t *p, plm_samples_t *s, void *user) { (void)p; (void)user; audio_push(s->interleaved, s->count); }

static void on_load(plm_buffer_t *b, void *user)
{
	(void)user;
	static u8 tmp[64 * 1024];
	L();
	seg_t *s = seg_count ? &segs[seg_head] : NULL;
	u64 limit = s ? (s->end_pos != ~0ull ? s->end_pos : wpos) : rpos;
	u64 n = limit - rpos;
	if (n > sizeof(tmp)) n = sizeof(tmp);
	for (u64 i = 0; i < n;) {
		u64 at = (rpos + i) % RING_SIZE;
		u64 run = n - i;
		if (at + run > RING_SIZE) run = RING_SIZE - at;
		memcpy(tmp + i, ring + at, run);
		i += run;
	}
	rpos += n;
	int ended = s && s->end_pos != ~0ull && rpos == s->end_pos;
	U();
	if (n) plm_buffer_write(b, tmp, n);
	if (ended) plm_buffer_signal_end(b);
}

static void close_decoder(void)
{
	if (plm) plm_destroy(plm);
	plm = NULL;
	pbuf = NULL;
	pending_frame = NULL;
}

void player_update(double dt)
{
	for (int guard = 0; guard < 2; guard++) {
		L();
		int have = seg_count > 0;
		seg_t s = have ? segs[seg_head] : (seg_t){ 0 };
		u64 avail = have ? wpos - rpos : 0;
		int complete = have && s.end_pos != ~0ull;
		U();
		if (!have) return;

		if (!plm) {
			if (!complete && avail < START_BYTES) { buffering = 1; return; }
			plm_buffer_t *b = plm_buffer_create_with_capacity(256 * 1024);
			plm_buffer_set_load_callback(b, on_load, NULL);
			plm = plm_create_with_buffer(b, 1);
			if (!plm) { plm_buffer_destroy(b); return; }
			pbuf = b;
			plm_set_video_decode_callback(plm, on_video, NULL);
			plm_set_audio_decode_callback(plm, on_audio, NULL);
			plm_set_audio_enabled(plm, 1);
			plm_set_audio_lead_time(plm, AUDIO_LEAD);
			plm_set_loop(plm, 0);
			buffering = 0;
		}

		// Ran dry mid-segment: hold (rather than letting the decoder clock run ahead) until
		// a comfortable amount has arrived.
		if (!complete && avail + plm_buffer_get_remaining(pbuf) < 32 * 1024) buffering = 1;
		if (buffering && (complete || avail >= RESUME_BYTES)) buffering = 0;
		if (buffering) return;

		// Keep the audio queue near the lead time by nudging the decode clock.
		double q = audio_buffered();
		double tick = dt;
		if (q > AUDIO_LEAD + 0.15) tick *= 0.9;
		else if (q < AUDIO_LEAD - 0.1) tick *= 1.1;
		if (tick > 0.1) tick = 0.1;

		plm_decode(plm, tick);
		if (pending_frame) {
			plm_frame_t *f = pending_frame;
			gfx_video_upload(f->y.data, f->y.width, f->cb.data, f->cr.data, f->cb.width, f->width, f->height);
			pending_frame = NULL;
		}

		if (!plm_has_ended(plm)) return;
		// Segment over: on to the next one (in the same frame, so there is no gap).
		close_decoder();
		L();
		seg_head = (seg_head + 1) % MAX_SEGS;
		seg_count--;
		U();
		dt = 0;
	}
}

void player_tune(const char *id)
{
	if (!lock) return;
	close_decoder();
	L();
	gen++;
	strncpy(chan, id, sizeof(chan) - 1);
	next_at = clock_synced() ? clock_ms() : 0;
	wpos = rpos = 0;
	seg_head = seg_count = 0;
	net_error[0] = 0;
	net_failures = 0;
	U();
	audio_clear();
	gfx_video_clear();
	buffering = 1;
}

void player_status(player_status_t *o)
{
	memset(o, 0, sizeof(*o));
	if (!lock) { o->state = PLAYER_IDLE; return; } // not started yet (still finding the relay)
	L();
	strcpy(o->channel, chan);
	if (seg_count) {
		seg_t *s = &segs[seg_head];
		strcpy(o->title, s->title);
		strcpy(o->type, s->type);
		o->seg_start = s->start_ms;
		o->seg_end = s->end_ms;
	}
	strcpy(o->error, net_error);
	int failing = net_failures >= 2;
	int have = seg_count > 0;
	U();
	if (!chan[0]) o->state = PLAYER_IDLE;
	else if (failing && !plm) o->state = PLAYER_ERROR;
	else if (!have || !plm) o->state = PLAYER_TUNING;
	else if (buffering) o->state = PLAYER_BUFFERING;
	else o->state = PLAYER_PLAYING;
}

int player_start(const char *h, int p)
{
	strncpy(host, h, sizeof(host) - 1);
	port = p;
	ring = malloc(RING_SIZE);
	if (!ring) return -1;
	if (!(lock = plat_mutex_new())) return -1;
	running = 1;
	thread = plat_thread_start(net_thread, NULL, "spelunkr_net");
	return thread ? 0 : -1;
}

void player_stop(void)
{
	if (!running) return;
	running = 0;
	plat_thread_join(thread);
	close_decoder();
}
