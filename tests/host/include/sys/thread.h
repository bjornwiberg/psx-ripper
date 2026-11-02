#pragma once
#include "ppu-types.h"
typedef u64 sys_ppu_thread_t;
#define THREAD_JOINABLE 1
int sysThreadCreate(sys_ppu_thread_t *id, void (*entry)(void *), void *arg, int prio, u64 stack, u64 flags, char *name);
int sysThreadJoin(sys_ppu_thread_t id, u64 *ret);
void sysThreadExit(u64 v);
