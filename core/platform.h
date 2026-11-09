// What a platform (PS3, SDL2 desktop, PS Vita) provides to the shared Spelunkr client.
//
// core/ holds everything that is the same everywhere: the guide, the screens, input
// handling, streaming from the relay and MPEG-1 decoding. A platform adds a main(), video
// output (gfx.h), audio output (audio.h) and the small set of services below.
#ifndef SPELUNKR_PLATFORM_H
#define SPELUNKR_PLATFORM_H

#ifdef __PPU__
#include <ppu-types.h>
#else
#include <stdint.h>
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int32_t s32;
typedef int64_t s64;
#endif

// --- time
s64  now_us(void);                 // monotonic microseconds
void plat_sleep_us(int us);

// --- threads
typedef struct plat_mutex plat_mutex;
typedef struct plat_thread plat_thread;
plat_mutex  *plat_mutex_new(void);
void         plat_lock(plat_mutex *m);
void         plat_unlock(plat_mutex *m);
plat_thread *plat_thread_start(void (*fn)(void *), void *arg, const char *name);
void         plat_thread_join(plat_thread *t);

// --- files and setup
const char *plat_data_dir(void);   // writable: relay.txt, last.txt, log.txt live here
int  plat_net_init(void);          // 0 on success

// --- input: buttons held right now, as a bitmask of these
enum {
	B_UP, B_DOWN, B_LEFT, B_RIGHT,
	B_OK,        // PS: Cross       keyboard: Enter     pad: A    - watch / select
	B_BACK,      // PS: Circle      keyboard: Esc       pad: B    - close
	B_GUIDE,     // PS: Triangle    keyboard: G / Tab   pad: Y    - open the guide
	B_INFO,      // PS: Square      keyboard: I / Space pad: X    - show the info banner
	B_START,     // PS: Start       keyboard: -         pad: Start
	B_PAGE_UP,   // PS: L1/L        keyboard: PgUp      pad: LB
	B_PAGE_DOWN, // PS: R1/R        keyboard: PgDn      pad: RB
	B_COUNT
};
#define BTN(b) (1u << (b))
u32  plat_buttons(void);

// Pumps system events. Returns 0 when the app should quit.
int  plat_poll(void);

// Text for the screens: the guide's control hint, and how to point the app at the relay.
const char *plat_hint_guide(void);
const char *plat_relay_help(void);

// Relay address given on the command line ("host:port"), or NULL.
extern const char *plat_relay_override;

// The shared app: runs until plat_poll() says stop.
int app_main(void);

#endif
