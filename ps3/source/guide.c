#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guide.h"
#include "net.h"

static s64 sync_relay_ms, sync_us;
static int synced;

void clock_sync(s64 relay_ms) { sync_relay_ms = relay_ms; sync_us = now_us(); synced = 1; }
s64 clock_ms(void) { return sync_relay_ms + (now_us() - sync_us) / 1000; }
int clock_synced(void) { return synced; }

void format_clock(s64 ms, int tz, char *out, int outlen)
{
	s64 local = ms / 1000 + (s64)tz * 60;
	int mins = (int)(((local / 60) % (24 * 60) + 24 * 60) % (24 * 60));
	int h = mins / 60, m = mins % 60;
	snprintf(out, outlen, "%d:%02d %s", h % 12 ? h % 12 : 12, m, h < 12 ? "AM" : "PM");
}

void guide_free(guide_t *g)
{
	for (int i = 0; i < g->nch; i++) free(g->ch[i].shows);
	free(g->ch);
	g->ch = NULL;
	g->nch = 0;
}

static int split(char *line, char **f, int max)
{
	int n = 0;
	f[n++] = line;
	for (char *p = line; *p && n < max; p++) if (*p == '\t') { *p = 0; f[n++] = p + 1; }
	return n;
}

static void copy(char *dst, const char *src, int cap) { strncpy(dst, src, cap - 1); dst[cap - 1] = 0; }

int guide_parse(guide_t *g, char *text)
{
	guide_t ng = { 0 };
	int cap = 64;
	ng.ch = calloc(cap, sizeof(channel_t));
	if (!ng.ch || strncmp(text, "SPELUNKR\t1", 10) != 0) { free(ng.ch); return -1; }
	int show_cap = 0, ok = 0;
	channel_t *c = NULL;
	char *save = NULL;
	for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
		char *f[8];
		int n = split(line, f, 8);
		if (!strcmp(f[0], "NOW") && n >= 2) clock_sync(strtoll(f[1], NULL, 10));
		else if (!strcmp(f[0], "TZ") && n >= 2) ng.tz_minutes = atoi(f[1]);
		else if (!strcmp(f[0], "CH") && n >= 6) {
			if (ng.nch == cap) { cap *= 2; ng.ch = realloc(ng.ch, cap * sizeof(channel_t)); memset(ng.ch + ng.nch, 0, (cap - ng.nch) * sizeof(channel_t)); }
			c = &ng.ch[ng.nch++];
			copy(c->id, f[1], sizeof(c->id));
			c->dial = atoi(f[2]);
			copy(c->name, f[3], sizeof(c->name));
			c->color = (u32)strtoul(f[4][0] == '#' ? f[4] + 1 : f[4], NULL, 16);
			copy(c->desc, f[5], sizeof(c->desc));
			show_cap = 0;
		} else if (!strcmp(f[0], "LOGO") && n >= 3 && c && strlen(f[1]) == 32 * 32) {
			// Palette entries "k=0b0f26,c=ff6f00,..."; map each letter to a small index.
			char letters[16] = { 0 };
			int np = 0;
			for (char *e = f[2]; *e && np < 15;) {
				letters[np + 1] = e[0];
				c->palette[np + 1] = (u32)strtoul(e + 2, NULL, 16);
				np++;
				char *comma = strchr(e, ',');
				if (!comma) break;
				e = comma + 1;
			}
			for (int i = 0; i < 32 * 32; i++) {
				c->logo[i] = 0;
				for (int k = 1; k <= np; k++) if (f[1][i] == letters[k]) { c->logo[i] = k; break; }
			}
			c->has_logo = 1;
		} else if (!strcmp(f[0], "S") && n >= 4 && c) {
			if (c->nshows == show_cap) {
				show_cap = show_cap ? show_cap * 2 : 8;
				c->shows = realloc(c->shows, show_cap * sizeof(show_t));
			}
			show_t *s = &c->shows[c->nshows++];
			s->start = strtoll(f[1], NULL, 10);
			s->end = strtoll(f[2], NULL, 10);
			copy(s->title, f[3], sizeof(s->title));
			copy(s->year, n >= 5 ? f[4] : "", sizeof(s->year));
		} else if (!strcmp(f[0], "END")) ok = 1;
	}
	if (!ok || !ng.nch) { guide_free(&ng); return -1; }
	guide_free(g);
	*g = ng;
	return 0;
}

int guide_find(const guide_t *g, const char *id)
{
	for (int i = 0; i < g->nch; i++) if (!strcmp(g->ch[i].id, id)) return i;
	return -1;
}

const show_t *guide_show_at(const channel_t *c, s64 ms)
{
	for (int i = 0; i < c->nshows; i++) if (c->shows[i].start <= ms && ms < c->shows[i].end) return &c->shows[i];
	return NULL;
}
