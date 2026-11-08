// 48 kHz stereo float output through the PS3 audio port, fed from a ring buffer.
#ifndef SPELUNKR_AUDIO_H
#define SPELUNKR_AUDIO_H

int    audio_start(void);
void   audio_stop(void);
void   audio_push(const float *interleaved, int frames); // drops what doesn't fit
void   audio_clear(void);
double audio_buffered(void);   // seconds queued but not yet played
void   audio_set_volume(float v);

#endif
