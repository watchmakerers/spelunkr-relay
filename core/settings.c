#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "settings.h"

settings_t settings = { 80, 0 };

static FILE *open_settings(const char *mode)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/settings.txt", plat_data_dir());
	return fopen(path, mode);
}

void settings_load(void)
{
	FILE *f = open_settings("r");
	if (!f) return;
	char line[128];
	while (fgets(line, sizeof(line), f)) {
		char *eq = strchr(line, '=');
		if (!eq) continue;
		*eq = 0;
		int v = atoi(eq + 1);
		if (!strcmp(line, "volume")) settings.volume = v < 0 ? 0 : v > 100 ? 100 : v;
		else if (!strcmp(line, "crt")) settings.crt = v != 0;
	}
	fclose(f);
}

void settings_save(void)
{
	FILE *f = open_settings("w");
	if (!f) return;
	fprintf(f, "volume=%d\ncrt=%d\n", settings.volume, settings.crt);
	fclose(f);
}
