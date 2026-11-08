#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <net/net.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <net/poll.h>
#include <sys/systime.h>
#include <lv2/systime.h>

#include "net.h"
#include "app.h"

#define DISCOVERY_PORT 47555

s64 now_us(void) { return sysGetSystemTime(); }

int net_start(void) { return netInitialize(); }

// Waits up to `ms` for `s` to become readable: 1 readable, 0 timeout, <0 error.
static int wait_readable(int s, int ms)
{
	struct pollfd p = { s, POLLIN, 0 };
	int r = poll(&p, 1, ms);
	if (r > 0 && (p.revents & (POLLERR | POLLNVAL))) return -1;
	return r;
}

static int read_config(char *host, int *port)
{
	FILE *f = fopen(APP_DIR "/relay.txt", "r");
	if (!f) return -1;
	char line[128] = { 0 };
	int ok = fgets(line, sizeof(line), f) != NULL;
	fclose(f);
	if (!ok) return -1;
	char *colon = strchr(line, ':');
	*port = colon ? atoi(colon + 1) : 8642;
	if (colon) *colon = 0;
	for (char *p = line; *p; p++) if (isspace((unsigned char)*p)) { *p = 0; break; }
	if (!line[0]) return -1;
	strcpy(host, line);
	return 0;
}

int net_discover(char *host, int *port)
{
	if (read_config(host, port) == 0) return 0;

	int s = socket(PF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s < 0) return -1;
	int yes = 1;
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, &yes, sizeof(yes));

	struct sockaddr_in to;
	memset(&to, 0, sizeof(to));
	to.sin_family = AF_INET;
	to.sin_port = htons(DISCOVERY_PORT);
	to.sin_addr.s_addr = htonl(INADDR_BROADCAST);

	int found = -1;
	for (int attempt = 0; attempt < 4 && found < 0; attempt++) {
		sendto(s, "SPELUNKR?", 9, 0, (struct sockaddr *)&to, sizeof(to));
		char buf[64];
		struct sockaddr_in from;
		socklen_t fl = sizeof(from);
		if (wait_readable(s, 700) <= 0) continue;
		int n = recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
		if (n > 9 && memcmp(buf, "SPELUNKR ", 9) == 0) {
			buf[n] = 0;
			*port = atoi(buf + 9);
			strcpy(host, inet_ntoa(from.sin_addr));
			found = 0;
		}
	}
	close(s);
	return found;
}

int http_open(http_t *h, const char *host, int port, const char *path)
{
	memset(h, 0, sizeof(*h));
	h->sock = socket(PF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (h->sock < 0) return -1;
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	inet_pton(AF_INET, host, &a.sin_addr);
	if (connect(h->sock, (struct sockaddr *)&a, sizeof(a)) != 0) { http_close(h); return -1; }

	char req[512];
	int n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s:%d\r\nUser-Agent: spelunkr-ps3\r\n\r\n", path, host, port);
	if (send(h->sock, req, n, 0) != n) { http_close(h); return -1; }

	// Read the header block byte by byte (it's small; this keeps the body untouched).
	// The relay can take several seconds to seek into an archive.org video before replying.
	int len = 0;
	while (len < (int)sizeof(h->headers) - 1) {
		char c;
		if (wait_readable(h->sock, 30000) <= 0) { http_close(h); return -1; }
		if (recv(h->sock, &c, 1, 0) <= 0) { http_close(h); return -1; }
		h->headers[len++] = (char)tolower((unsigned char)c);
		if (len >= 4 && memcmp(h->headers + len - 4, "\r\n\r\n", 4) == 0) break;
	}
	h->headers[len] = 0;
	if (sscanf(h->headers, "http/%*d.%*d %d", &h->status) != 1) { http_close(h); return -1; }
	return 0;
}

int http_read(http_t *h, void *buf, int len)
{
	int w = wait_readable(h->sock, 500);
	if (w == 0) return -2;
	if (w < 0) return -1;
	return recv(h->sock, buf, len, 0);
}

void http_close(http_t *h)
{
	if (h->sock >= 0) { shutdown(h->sock, SHUT_RDWR); close(h->sock); }
	h->sock = -1;
}

const char *http_header(http_t *h, const char *name, char *out, int outlen)
{
	char key[64];
	snprintf(key, sizeof(key), "\r\n%s:", name);
	const char *p = strstr(h->headers, key);
	if (!p) return NULL;
	p += strlen(key);
	while (*p == ' ') p++;
	int i = 0;
	while (*p && *p != '\r' && i < outlen - 1) out[i++] = *p++;
	out[i] = 0;
	return out;
}

char *http_get_all(const char *host, int port, const char *path, int *outlen)
{
	http_t h;
	if (http_open(&h, host, port, path) != 0) return NULL;
	if (h.status != 200) { http_close(&h); return NULL; }
	int cap = 128 * 1024, len = 0, timeouts = 0;
	char *buf = malloc(cap + 1);
	while (buf) {
		if (len == cap) { cap *= 2; char *nb = realloc(buf, cap + 1); if (!nb) { free(buf); buf = NULL; break; } buf = nb; }
		int r = http_read(&h, buf + len, cap - len);
		if (r == -2 && ++timeouts < 20) continue;
		if (r <= 0) break;
		len += r;
	}
	http_close(&h);
	if (!buf) return NULL;
	buf[len] = 0;
	if (outlen) *outlen = len;
	return buf;
}
