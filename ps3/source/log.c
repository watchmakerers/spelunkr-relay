#include <stdio.h>
#include <stdarg.h>
#include "app.h"
#include "log.h"
#include "net.h"

static FILE *f;

void log_open(void)
{
	f = fopen(APP_DIR "/log.txt", "w");
}

void logf_(const char *fmt, ...)
{
	if (!f) return;
	fprintf(f, "[%8.3f] ", now_us() / 1e6);
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fflush(f);
}
