#pragma once
#include "ppu-types.h"
#include "sys/memory.h"
typedef struct { u32 allowedPanels, firstViewPanel; struct { float x, y; } controlPoint; u32 prohibitFlags; } oskParam;
typedef struct { u16 *message; u16 *startText; s32 maxLength; } oskInputFieldInfo;
typedef struct { s32 res; s32 len; u16 *str; } oskCallbackReturnParam;
#define OSK_PANEL_TYPE_URL 0x00000040
#define OSK_PANEL_TYPE_ALPHABET 0x00000002
#define OSK_PANEL_TYPE_NUMERAL 0x00000004
#define OSK_PROHIBIT_RETURN 0x00000002
#define OSK_FULLKEY_PANEL 0x00000002
#define OSK_OK 0
#define OSK_NO_TEXT 2
int oskSetKeyLayoutOption(u32 o);
int oskLoadAsync(sys_mem_container_t c, const oskParam *p, const oskInputFieldInfo *f);
int oskUnloadAsync(oskCallbackReturnParam *r);
