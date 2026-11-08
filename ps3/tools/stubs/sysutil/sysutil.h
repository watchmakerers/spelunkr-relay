#pragma once
#include <ppu-types.h>
#define SYSUTIL_EVENT_SLOT0 0
#define SYSUTIL_EXIT_GAME 0x101
static inline int sysUtilRegisterCallback(int s, void (*cb)(u64,u64,void*), void *u){(void)s;(void)cb;(void)u;return 0;}
static inline int sysUtilCheckCallback(void){return 0;}
