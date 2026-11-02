// Host build (UI tests): a scripted pad (tests set g_host_pad).
#pragma once
#include "ppu-types.h"
typedef struct { u32 max; u32 connected; u32 info; u16 vendor_id[7]; u16 product_id[7]; u8 status[7]; } padInfo;
typedef struct { s32 len; u16 BTN_CROSS, BTN_CIRCLE, BTN_TRIANGLE, BTN_SQUARE, BTN_UP, BTN_DOWN, BTN_LEFT, BTN_RIGHT,
	BTN_START, BTN_SELECT, BTN_L1, BTN_R1, BTN_L2, BTN_R2; } padData;
int ioPadInit(u32 max);
int ioPadGetInfo(padInfo *info);
int ioPadGetData(u32 port, padData *data);
