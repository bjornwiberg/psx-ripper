#pragma once
#include "ppu-types.h"
typedef u32 sys_mem_container_t;
int sysMemContainerCreate(sys_mem_container_t *c, u64 size);
int sysMemContainerDestroy(sys_mem_container_t c);
