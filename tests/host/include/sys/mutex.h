#pragma once
#include "ppu-types.h"
typedef u32 sys_mutex_t;
typedef struct { u32 attr_protocol, attr_recursive, attr_pshared, attr_adaptive; u64 key; s32 flags; u32 _pad; char name[8]; } sys_mutex_attr_t;
#define SYS_MUTEX_PROTOCOL_PRIO 2
#define SYS_MUTEX_ATTR_RECURSIVE 0x10
#define SYS_MUTEX_ATTR_NOT_RECURSIVE 0x20
#define SYS_MUTEX_ATTR_NOT_PSHARED 0x200
#define SYS_MUTEX_ATTR_NOT_ADAPTIVE 0x2000
#define sysMutexAttrInitialize(x) do { (x).attr_recursive = SYS_MUTEX_ATTR_NOT_RECURSIVE; } while (0)
int sysMutexCreate(sys_mutex_t *m, const sys_mutex_attr_t *a);
int sysMutexLock(sys_mutex_t m, u64 timeout);
int sysMutexUnlock(sys_mutex_t m);
