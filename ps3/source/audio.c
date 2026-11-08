#include <string.h>
#include <audio/audio.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/event_queue.h>

#include "audio.h"

#define RATE        48000
#define RING_FRAMES (RATE * 2)   // 2 seconds

static float ring[RING_FRAMES * 2];
static u32 rd, wr, fill;         // in frames
static sys_mutex_t lock;
static volatile int running;
static float volume = 1.0f;
static u32 port;
static audioPortConfig cfg;
static sys_event_queue_t queue;
static u64 key;
static sys_ppu_thread_t thread;

static void lockm(void) { sysMutexLock(lock, 0); }
static void unlockm(void) { sysMutexUnlock(lock); }

static void fill_block(float *dst)
{
	lockm();
	u32 n = fill < AUDIO_BLOCK_SAMPLES ? fill : AUDIO_BLOCK_SAMPLES;
	for (u32 i = 0; i < n; i++) {
		dst[i * 2] = ring[rd * 2] * volume;
		dst[i * 2 + 1] = ring[rd * 2 + 1] * volume;
		rd = (rd + 1) % RING_FRAMES;
	}
	fill -= n;
	unlockm();
	if (n < AUDIO_BLOCK_SAMPLES) memset(dst + n * 2, 0, (AUDIO_BLOCK_SAMPLES - n) * 2 * sizeof(float));
}

static void audio_thread(void *arg)
{
	(void)arg;
	sys_event_t ev;
	float *base = (float *)(u64)cfg.audioDataStart;
	while (running) {
		sysEventQueueReceive(queue, &ev, 20 * 1000);
		u64 cur = *(u64 *)(u64)cfg.readIndex;
		u32 next = (cur + 1) % cfg.numBlocks;
		fill_block(base + cfg.channelCount * AUDIO_BLOCK_SAMPLES * next);
	}
	sysThreadExit(0);
}

int audio_start(void)
{
	sys_mutex_attr_t attr;
	sysMutexAttrInitialize(attr);
	if (sysMutexCreate(&lock, &attr) != 0) return -1;
	if (audioInit() != 0) return -1;
	audioPortParam p;
	memset(&p, 0, sizeof(p));
	p.numChannels = AUDIO_PORT_2CH;
	p.numBlocks = AUDIO_BLOCK_8;
	p.attrib = AUDIO_PORT_INITLEVEL;
	p.level = 1.0f;
	if (audioPortOpen(&p, &port) != 0) return -1;
	audioGetPortConfig(port, &cfg);
	audioCreateNotifyEventQueue(&queue, &key);
	audioSetNotifyEventQueue(key);
	sysEventQueueDrain(queue);
	audioPortStart(port);
	running = 1;
	return sysThreadCreate(&thread, audio_thread, NULL, 900, 0x4000, THREAD_JOINABLE, "spelunkr_audio");
}

void audio_stop(void)
{
	if (!running) return;
	running = 0;
	u64 rv;
	sysThreadJoin(thread, &rv);
	audioPortStop(port);
	audioRemoveNotifyEventQueue(key);
	audioPortClose(port);
	sysEventQueueDestroy(queue, 0);
	audioQuit();
}

void audio_push(const float *src, int frames)
{
	lockm();
	for (int i = 0; i < frames && fill < RING_FRAMES; i++) {
		ring[wr * 2] = src[i * 2];
		ring[wr * 2 + 1] = src[i * 2 + 1];
		wr = (wr + 1) % RING_FRAMES;
		fill++;
	}
	unlockm();
}

void audio_clear(void)
{
	lockm();
	rd = wr = fill = 0;
	unlockm();
}

double audio_buffered(void)
{
	return (double)fill / RATE + (double)(cfg.numBlocks * AUDIO_BLOCK_SAMPLES) / RATE / 2;
}

void audio_set_volume(float v) { volume = v; }
