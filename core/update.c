#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "platform.h"
#include "net.h"
#include "log.h"
#include "update.h"
#include "version.h"

// Compares dotted versions ("1.10" > "1.9"). Returns <0, 0, >0.
static int vercmp(const char *a, const char *b)
{
	while (*a || *b) {
		long x = strtol(a, (char **)&a, 10), y = strtol(b, (char **)&b, 10);
		if (x != y) return x < y ? -1 : 1;
		if (*a == '.') a++;
		if (*b == '.') b++;
		if (!*a && !*b) break;
		if ((*a && *a != '.' && (*a < '0' || *a > '9')) || (*b && *b != '.' && (*b < '0' || *b > '9'))) break;
	}
	return 0;
}

static long asset_size;

int update_check(const char *host, int port, char *latest, int latest_len)
{
	const char *asset = plat_update_asset();
	if (!asset) return 0;
	char *text = http_get_all(host, port, "/update/latest", NULL);
	if (!text) return -1;
	char version[32] = "";
	int has_file = 0;
	for (char *line = strtok(text, "\n"); line; line = strtok(NULL, "\n")) {
		char name[128];
		long size;
		if (sscanf(line, "VERSION %31s", version) == 1) continue;
		if (sscanf(line, "FILE %127s %ld", name, &size) == 2 && !strcmp(name, asset)) { has_file = 1; asset_size = size; }
	}
	free(text);
	if (!version[0] || !strcmp(version, "none")) return -1;
	snprintf(latest, latest_len, "%s", version);
	LOG("update: have %s, relay has %s (%s)", SPELUNKR_VERSION, version, has_file ? "file available" : "no file for this platform");
	return has_file && vercmp(version, SPELUNKR_VERSION) > 0;
}

int update_download(const char *host, int port, char *path, int path_len)
{
	const char *asset = plat_update_asset();
	if (!asset) return -1;
	char url[256];
	snprintf(url, sizeof(url), "/update/file/%s", asset);
	snprintf(path, path_len, "%s/update.tmp", plat_data_dir());
	http_t h;
	if (http_open(&h, host, port, url) != 0) return -1;
	if (h.status != 200) { http_close(&h); return -1; }
	FILE *f = fopen(path, "wb");
	if (!f) { http_close(&h); return -1; }
	static char buf[64 * 1024];
	long total = 0;
	int timeouts = 0, ok = 1;
	for (;;) {
		int r = http_read(&h, buf, sizeof(buf));
		if (r == -2) { if (++timeouts > 60) { ok = 0; break; } continue; }
		if (r < 0) { ok = 0; break; }
		if (r == 0) break;
		timeouts = 0;
		if (fwrite(buf, 1, r, f) != (size_t)r) { ok = 0; break; }
		total += r;
	}
	http_close(&h);
	if (fclose(f) != 0) ok = 0;
	if (ok && asset_size > 0 && total != asset_size) ok = 0; // cut short
	LOG("update: downloaded %ld of %ld bytes (%s)", total, asset_size, ok ? "ok" : "failed");
	if (!ok) remove(path);
	return ok ? 0 : -1;
}
