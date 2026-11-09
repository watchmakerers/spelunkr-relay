#include <stdio.h>
#include <stdarg.h>
#include "platform.h"
#include "log.h"

static FILE *f;

void log_open(void)
{
	char path[512];
	snprintf(path, sizeof(path), "%s/log.txt", plat_data_dir());
	f = fopen(path, "w");
}

void logf_(const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	if (f) {
		fprintf(f, "[%8.3f] ", now_us() / 1e6);
		vfprintf(f, fmt, ap);
		fputc('\n', f);
		fflush(f);
	}
	va_end(ap);
}
