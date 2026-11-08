// Appends to USRDIR/log.txt (readable over FTP), so problems can be diagnosed without a screen.
#ifndef SPELUNKR_LOG_H
#define SPELUNKR_LOG_H
void log_open(void);
void logf_(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
#define LOG(...) logf_(__VA_ARGS__)
#endif
