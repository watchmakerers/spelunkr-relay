// Small helpers that work across PSL1GHT releases.
#ifndef SPELUNKR_COMPAT_H
#define SPELUNKR_COMPAT_H

#include <string.h>
#include <sys/mutex.h>

// A plain (non-recursive, priority-ordered) mutex; older SDKs lack sysMutexAttrInitialize.
static inline int mutex_create(sys_mutex_t *m)
{
	sys_mutex_attr_t attr;
	memset(&attr, 0, sizeof(attr));
	attr.attr_protocol = SYS_MUTEX_PROTOCOL_PRIO;
	attr.attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE;
	attr.attr_pshared = SYS_MUTEX_ATTR_PSHARED;
	attr.attr_adaptive = SYS_MUTEX_ATTR_NOT_ADAPTIVE;
	return sysMutexCreate(m, &attr);
}

#endif
