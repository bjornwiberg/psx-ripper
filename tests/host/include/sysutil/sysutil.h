#pragma once
#include "ppu-types.h"
#define SYSUTIL_EXIT_GAME 0x0101
#define SYSUTIL_DRAW_BEGIN 0x0121
#define SYSUTIL_DRAW_END 0x0122
#define SYSUTIL_MENU_OPEN 0x0131
#define SYSUTIL_MENU_CLOSE 0x0132
#define SYSUTIL_OSK_LOADED 0x0502
#define SYSUTIL_OSK_DONE 0x0503
#define SYSUTIL_OSK_UNLOADED 0x0504
#define SYSUTIL_OSK_INPUT_CANCELED 0x0505
#define SYSUTIL_SYSTEMPARAM_ID_LANG 0x0111
#define SYSUTIL_SYSTEMPARAM_ID_DATE_FORMAT 0x0114
#define SYSUTIL_SYSTEMPARAM_ID_TIME_FORMAT 0x0115
#define SYSUTIL_SYSTEMPARAM_ID_TIMEZONE 0x0116
#define SYSUTIL_SYSTEMPARAM_ID_SUMMERTIME 0x0117
#define SYSUTIL_EVENT_SLOT0 0
#define PS3_LANG_ENGLISH_US 1
#define PS3_LANG_ENGLISH_UK 18
#define PS3_LANG_SWEDISH 14
typedef void (*sysutilCallback)(u64 status, u64 param, void *usrdata);
int sysUtilRegisterCallback(int slot, sysutilCallback cb, void *usrdata);
int sysUtilCheckCallback();
int sysUtilGetSystemParamInt(int id, s32 *value);
