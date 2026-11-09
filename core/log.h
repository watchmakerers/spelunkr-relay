// Writes log.txt in the app's data folder (on the PS3 it's readable over FTP), so problems
// can be diagnosed without a screen.
#ifndef SPELUNKR_LOG_H
#define SPELUNKR_LOG_H
#if defined(__GNUC__) || defined(__clang__)
#define SPELUNKR_PRINTF __attribute__((format(printf, 1, 2)))
#else
#define SPELUNKR_PRINTF
#endif
void log_open(void);
void logf_(const char *fmt, ...) SPELUNKR_PRINTF;
#define LOG(...) logf_(__VA_ARGS__)
#endif
