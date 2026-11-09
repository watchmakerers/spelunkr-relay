// Plays a channel: fetches consecutive segments from the relay on a network thread and
// decodes them (MPEG-1 video + MP2 audio, pl_mpeg) on the main thread.
#ifndef SPELUNKR_PLAYER_H
#define SPELUNKR_PLAYER_H

#include "platform.h"

typedef enum { PLAYER_IDLE, PLAYER_TUNING, PLAYER_BUFFERING, PLAYER_PLAYING, PLAYER_ERROR } player_state_t;

typedef struct {
	player_state_t state;
	char channel[48];
	char title[128];      // current segment's title ("" for breaks)
	char type[16];        // video | ad | bump-break | up-next
	s64 seg_start, seg_end;
	char error[96];
} player_status_t;

int  player_start(const char *host, int port);
void player_stop(void);
void player_tune(const char *channel_id);
void player_update(double dt);          // call once per frame
void player_status(player_status_t *out);

#endif
