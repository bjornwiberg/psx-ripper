#pragma once
#include "ppu-types.h"
typedef enum { MSG_DIALOG_BTN_NONE = 0, MSG_DIALOG_BTN_OK = 1, MSG_DIALOG_BTN_YES = 1, MSG_DIALOG_BTN_NO = 2, MSG_DIALOG_BTN_ESCAPE = 3 } msgButton;
typedef int msgType;
#define MSG_DIALOG_NORMAL 0x0
#define MSG_DIALOG_BTN_TYPE_OK 0x2
#define MSG_DIALOG_BTN_TYPE_YESNO 0x1
#define MSG_DIALOG_DISABLE_CANCEL_ON 0x80
