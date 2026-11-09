// Settings that survive restarts (settings.txt in the data folder).
#ifndef SPELUNKR_SETTINGS_H
#define SPELUNKR_SETTINGS_H
typedef struct {
	int volume;   // 0-100
	int crt;      // CRT filter on/off
} settings_t;
extern settings_t settings;
void settings_load(void);
void settings_save(void);
#endif
