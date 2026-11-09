// PS3 platform layer: process setup, pad input, XMB exit, threads, the clock and files.
// Video and audio are in gfx.c and audio.c.
#include <stdlib.h>
#include <unistd.h>
#include <io/pad.h>
#include <net/net.h>
#include <sys/process.h>
#include <sys/thread.h>
#include <sys/mutex.h>
#include <sys/systime.h>
#include <lv2/systime.h>
#include <sysutil/sysutil.h>

#include "compat.h"
#include "platform.h"

#define APP_DIR "/dev_hdd0/game/SPLK00001/USRDIR"

SYS_PROCESS_PARAM(1001, 0x100000);

const char *plat_relay_override = NULL;
static volatile int running = 1;

s64 now_us(void) { return sysGetSystemTime(); }
void plat_sleep_us(int us) { usleep(us); }

struct plat_mutex { sys_mutex_t m; };
struct plat_thread { sys_ppu_thread_t t; };

plat_mutex *plat_mutex_new(void)
{
	plat_mutex *m = malloc(sizeof(*m));
	if (m && mutex_create(&m->m) != 0) { free(m); m = NULL; }
	return m;
}
void plat_lock(plat_mutex *m) { sysMutexLock(m->m, 0); }
void plat_unlock(plat_mutex *m) { sysMutexUnlock(m->m); }

typedef struct { void (*fn)(void *); void *arg; } start_t;
static void trampoline(void *p)
{
	start_t s = *(start_t *)p;
	free(p);
	s.fn(s.arg);
	sysThreadExit(0);
}
plat_thread *plat_thread_start(void (*fn)(void *), void *arg, const char *name)
{
	plat_thread *t = malloc(sizeof(*t));
	start_t *s = malloc(sizeof(*s));
	if (!t || !s) { free(t); free(s); return NULL; }
	s->fn = fn;
	s->arg = arg;
	if (sysThreadCreate(&t->t, trampoline, s, 1200, 0x8000, THREAD_JOINABLE, (char *)name) != 0) { free(t); free(s); return NULL; }
	return t;
}
void plat_thread_join(plat_thread *t)
{
	if (!t) return;
	u64 rv;
	sysThreadJoin(t->t, &rv);
	free(t);
}

const char *plat_data_dir(void) { return APP_DIR; }
int plat_net_init(void) { return netInitialize(); }

static void sysutil_cb(u64 status, u64 param, void *user)
{
	(void)param; (void)user;
	if (status == SYSUTIL_EXIT_GAME) running = 0;
}

int plat_poll(void)
{
	sysUtilCheckCallback();
	return running;
}

u32 plat_buttons(void)
{
	static u32 held;
	padInfo info;
	ioPadGetInfo(&info);
	for (int i = 0; i < MAX_PADS; i++) {
		if (!info.status[i]) continue;
		padData d;
		if (ioPadGetData(i, &d) != 0 || d.len == 0) return held; // no change since last read
		held = (d.BTN_UP ? BTN(B_UP) : 0) | (d.BTN_DOWN ? BTN(B_DOWN) : 0) |
		       (d.BTN_LEFT ? BTN(B_LEFT) : 0) | (d.BTN_RIGHT ? BTN(B_RIGHT) : 0) |
		       (d.BTN_CROSS ? BTN(B_OK) : 0) | (d.BTN_CIRCLE ? BTN(B_BACK) : 0) |
		       (d.BTN_TRIANGLE ? BTN(B_GUIDE) : 0) | (d.BTN_SQUARE ? BTN(B_INFO) : 0) |
		       (d.BTN_START ? BTN(B_START) : 0) | (d.BTN_L1 ? BTN(B_PAGE_UP) : 0) | (d.BTN_R1 ? BTN(B_PAGE_DOWN) : 0);
		return held;
	}
	return held;
}

const char *plat_hint_guide(void) { return "X WATCH     LEFT/RIGHT SHOWS     L1/R1 PAGE     TRIANGLE CLOSE"; }
const char *plat_relay_help(void)
{
	return "Start the relay on your PC (in the relay folder run: npm start) and make sure the PS3 is on the same "
	       "network. If it still isn't found, put its address in " APP_DIR "/relay.txt, e.g. 192.168.1.20:8642. "
	       "Press X to try again.";
}

int main(int argc, char *argv[])
{
	(void)argc; (void)argv;
	ioPadInit(7);
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_cb, NULL);
	int r = app_main();
	ioPadEnd();
	return r;
}
