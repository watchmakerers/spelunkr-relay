#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#if defined(_WIN32)
#  include <winsock2.h>
#  include <ws2tcpip.h>
   typedef int socklen_t;
#  define CLOSESOCK closesocket
#  define SHUT_RDWR SD_BOTH
#elif defined(__PPU__)
#  include <unistd.h>
#  include <net/net.h>
#  include <sys/socket.h>
#  include <netinet/in.h>
#  include <arpa/inet.h>
#  include <net/poll.h>
#  define CLOSESOCK close
#else
#  include <unistd.h>
#  include <fcntl.h>
#  include <sys/types.h>
#  include <sys/socket.h>
#  include <sys/select.h>
#  include <netinet/in.h>
#  define CLOSESOCK close
#endif

#include "net.h"

#define DISCOVERY_PORT 47555

// Waits up to `ms` for `s` to become readable: 1 readable, 0 timeout, <0 error.
static int wait_readable(int s, int ms)
{
#ifdef __PPU__
	struct pollfd p = { s, POLLIN, 0 };
	int r = poll(&p, 1, ms);
	if (r > 0 && (p.revents & (POLLERR | POLLNVAL))) return -1;
	return r;
#else
	fd_set rd;
	FD_ZERO(&rd);
	FD_SET(s, &rd);
	struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
	return select(s + 1, &rd, NULL, NULL, &tv);
#endif
}

// Dotted IPv4 <-> network-order address, without relying on inet_pton/inet_ntoa (missing
// or quirky on some of the consoles' C libraries).
static int parse_ipv4(const char *s, u32 *out)
{
	unsigned a, b, c, d;
	char tail;
	if (sscanf(s, "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4 || a > 255 || b > 255 || c > 255 || d > 255) return -1;
	*out = htonl((a << 24) | (b << 16) | (c << 8) | d);
	return 0;
}
static void format_ipv4(u32 net_order, char *out)
{
	u32 h = ntohl(net_order);
	sprintf(out, "%u.%u.%u.%u", (h >> 24) & 255, (h >> 16) & 255, (h >> 8) & 255, h & 255);
}

static int parse_hostport(const char *text, char *host, int *port)
{
	char line[128];
	snprintf(line, sizeof(line), "%s", text);
	for (char *p = line; *p; p++) if (isspace((unsigned char)*p)) { *p = 0; break; }
	char *colon = strchr(line, ':');
	*port = colon ? atoi(colon + 1) : 8642;
	if (colon) *colon = 0;
	u32 ignored;
	if (!line[0] || parse_ipv4(line, &ignored) != 0) return -1;
	strcpy(host, line);
	return 0;
}

static int read_config(char *host, int *port)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/relay.txt", plat_data_dir());
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	char line[128] = { 0 };
	int ok = fgets(line, sizeof(line), f) != NULL;
	fclose(f);
	return ok ? parse_hostport(line, host, port) : -1;
}

#ifndef __PPU__
// ---------------------------------------------------------------- LAN scan (not on PS3)
//
// Broadcast discovery fails on machines whose firewall drops the relay's reply (Linux with
// ufw/firewalld does: the reply comes from an address the request wasn't sent to). As a
// fallback, try TCP connections to port 8642 across the local network (the /22 around our
// address, which covers ordinary /24 home networks and mesh systems that use /22), then ask
// each answering host /ps3/hello. Outgoing connections pass any stateful firewall.

#define SCAN_PORT 8642
#define SCAN_BATCH 128

static void set_nonblocking(int s, int on)
{
#ifdef _WIN32
	u_long v = on;
	ioctlsocket(s, FIONBIO, &v);
#else
	int f = fcntl(s, F_GETFL, 0);
	fcntl(s, F_SETFL, on ? (f | O_NONBLOCK) : (f & ~O_NONBLOCK));
#endif
}

// Our LAN address (host order), via a UDP "connect" that sends nothing.
static u32 local_ipv4(void)
{
	int s = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s < 0) return 0;
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(53);
	a.sin_addr.s_addr = htonl(0x08080808);
	u32 ip = 0;
	socklen_t al = sizeof(a);
	if (connect(s, (struct sockaddr *)&a, sizeof(a)) == 0 && getsockname(s, (struct sockaddr *)&a, &al) == 0)
		ip = ntohl(a.sin_addr.s_addr);
	CLOSESOCK(s);
	return ip;
}

// Connects to addr:port within ms; returns a blocking socket or -1.
static int connect_within(u32 addr_host, int port, int ms)
{
	int s = (int)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s < 0) return -1;
	set_nonblocking(s, 1);
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = htonl(addr_host);
	connect(s, (struct sockaddr *)&a, sizeof(a));
	fd_set wr;
	FD_ZERO(&wr);
	FD_SET(s, &wr);
	struct timeval tv = { ms / 1000, (ms % 1000) * 1000 };
	int err = 1;
	socklen_t el = sizeof(err);
	if (select(s + 1, NULL, &wr, NULL, &tv) <= 0 || getsockopt(s, SOL_SOCKET, SO_ERROR, (char *)&err, &el) != 0 || err) {
		CLOSESOCK(s);
		return -1;
	}
	set_nonblocking(s, 0);
	return s;
}

// Is there a relay at addr:port? (GET /ps3/hello, a couple of seconds at most.)
static int is_relay(u32 addr_host, int port)
{
	int s = connect_within(addr_host, port, 1500);
	if (s < 0) return 0;
	char req[] = "GET /ps3/hello HTTP/1.0\r\n\r\n", buf[512];
	int len = 0;
	if (send(s, req, sizeof(req) - 1, 0) == (int)sizeof(req) - 1) {
		while (len < (int)sizeof(buf) - 1 && wait_readable(s, 2000) > 0) {
			int n = (int)recv(s, buf + len, sizeof(buf) - 1 - len, 0);
			if (n <= 0) break;
			len += n;
		}
	}
	CLOSESOCK(s);
	buf[len] = 0;
	return strstr(buf, "\r\n\r\nSPELUNKR ") != NULL;
}

static int scan_lan(char *host, int *port)
{
	u32 me = local_ipv4();
	if (!me) return -1;
	u32 base = me & 0xFFFFFC00u; // the /22 around us; our own /24 first
	u32 order[1024];
	int n = 0;
	for (u32 i = 0; i < 256; i++) order[n++] = (me & 0xFFFFFF00u) | i;
	for (u32 i = 0; i < 1024; i++) { u32 a = base | i; if ((a & 0xFFFFFF00u) != (me & 0xFFFFFF00u)) order[n++] = a; }

	for (int b = 0; b < n; b += SCAN_BATCH) {
		int socks[SCAN_BATCH], cnt = b + SCAN_BATCH <= n ? SCAN_BATCH : n - b, maxfd = -1;
		for (int i = 0; i < cnt; i++) {
			u32 a = order[b + i];
			socks[i] = -1;
			if ((a & 0xff) == 0 || (a & 0xff) == 255) continue;
			int s = (int)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
			if (s < 0) continue;
			set_nonblocking(s, 1);
			struct sockaddr_in sa;
			memset(&sa, 0, sizeof(sa));
			sa.sin_family = AF_INET;
			sa.sin_port = htons(SCAN_PORT);
			sa.sin_addr.s_addr = htonl(a);
			connect(s, (struct sockaddr *)&sa, sizeof(sa));
			socks[i] = s;
			if (s > maxfd) maxfd = s;
		}
		// Collect whatever connects within ~350 ms.
		s64 until = now_us() + 350000;
		int found = -1;
		while (found < 0 && now_us() < until) {
			fd_set wr;
			FD_ZERO(&wr);
			int any = 0;
			for (int i = 0; i < cnt; i++) if (socks[i] >= 0) { FD_SET(socks[i], &wr); any = 1; }
			if (!any) break;
			s64 left = until - now_us();
			struct timeval tv = { 0, (long)(left > 0 ? left : 0) };
			if (select(maxfd + 1, NULL, &wr, NULL, &tv) <= 0) break;
			for (int i = 0; i < cnt; i++) {
				if (socks[i] < 0 || !FD_ISSET(socks[i], &wr)) continue;
				int err = 1;
				socklen_t el = sizeof(err);
				getsockopt(socks[i], SOL_SOCKET, SO_ERROR, (char *)&err, &el);
				CLOSESOCK(socks[i]);
				socks[i] = -1;
				if (!err && is_relay(order[b + i], SCAN_PORT)) { found = i; break; }
			}
		}
		for (int i = 0; i < cnt; i++) if (socks[i] >= 0) CLOSESOCK(socks[i]);
		if (found >= 0) {
			format_ipv4(htonl(order[b + found]), host);
			*port = SCAN_PORT;
			return 0;
		}
	}
	return -1;
}

// Where the relay was last found, so the next start doesn't have to search.
static void remember(const char *host, int port)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/relay-found.txt", plat_data_dir());
	FILE *f = fopen(path, "w");
	if (f) { fprintf(f, "%s:%d\n", host, port); fclose(f); }
}

static int recall(char *host, int *port)
{
	char path[512], line[128] = { 0 };
	snprintf(path, sizeof(path), "%s/relay-found.txt", plat_data_dir());
	FILE *f = fopen(path, "r");
	if (!f) return -1;
	int ok = fgets(line, sizeof(line), f) != NULL;
	fclose(f);
	u32 a;
	if (!ok || parse_hostport(line, host, port) != 0 || parse_ipv4(host, &a) != 0) return -1;
	return is_relay(ntohl(a), *port) ? 0 : -1;
}
#endif

static int broadcast_discover(char *host, int *port);

int net_discover(char *host, int *port)
{
	if (plat_relay_override && parse_hostport(plat_relay_override, host, port) == 0) return 0;
	if (read_config(host, port) == 0) return 0;
#ifdef __PPU__
	return broadcast_discover(host, port);
#else
	if (recall(host, port) == 0) return 0;
	if (broadcast_discover(host, port) == 0 || scan_lan(host, port) == 0) { remember(host, *port); return 0; }
	return -1;
#endif
}

static int broadcast_discover(char *host, int *port)
{
	int s = (int)socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s < 0) return -1;
	int yes = 1;
	setsockopt(s, SOL_SOCKET, SO_BROADCAST, (const char *)&yes, sizeof(yes));

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
		int n = (int)recvfrom(s, buf, sizeof(buf) - 1, 0, (struct sockaddr *)&from, &fl);
		if (n > 9 && memcmp(buf, "SPELUNKR ", 9) == 0) {
			buf[n] = 0;
			*port = atoi(buf + 9);
			format_ipv4(from.sin_addr.s_addr, host);
			found = 0;
		}
	}
	CLOSESOCK(s);
	return found;
}

int http_open(http_t *h, const char *host, int port, const char *path)
{
	memset(h, 0, sizeof(*h));
	u32 addr;
	if (parse_ipv4(host, &addr) != 0) { h->sock = -1; return -1; }
	h->sock = (int)socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (h->sock < 0) return -1;
	struct sockaddr_in a;
	memset(&a, 0, sizeof(a));
	a.sin_family = AF_INET;
	a.sin_port = htons(port);
	a.sin_addr.s_addr = addr;
	if (connect(h->sock, (struct sockaddr *)&a, sizeof(a)) != 0) { http_close(h); return -1; }

	char req[512];
	int n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: %s:%d\r\nUser-Agent: spelunkr-client\r\n\r\n", path, host, port);
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
	return (int)recv(h->sock, buf, len, 0);
}

void http_close(http_t *h)
{
	if (h->sock >= 0) { shutdown(h->sock, SHUT_RDWR); CLOSESOCK(h->sock); }
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
