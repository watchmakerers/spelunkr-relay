// Plain HTTP/1.0 over the LAN to the relay, and relay discovery by UDP broadcast.
#ifndef SPELUNKR_NET_H
#define SPELUNKR_NET_H

#include <ppu-types.h>

typedef struct {
	int sock;
	int status;
	char headers[2048];   // raw header block, lower-cased names
} http_t;

int  net_start(void);

// Finds the relay: USRDIR/relay.txt ("host:port") if present, else a broadcast on the LAN.
// Fills host (dotted IPv4) and port. Returns 0 on success.
int  net_discover(char *host, int *port);

int  http_open(http_t *h, const char *host, int port, const char *path); // 0 on success (status in h->status)
int  http_read(http_t *h, void *buf, int len);  // >0 bytes, 0 = end, <0 = error, -2 = timeout (retry)
void http_close(http_t *h);
const char *http_header(http_t *h, const char *name, char *out, int outlen); // NULL if absent

// Reads a whole (small) response body; caller frees. NULL on failure.
char *http_get_all(const char *host, int port, const char *path, int *len);

// Monotonic microseconds.
s64 now_us(void);

#endif
