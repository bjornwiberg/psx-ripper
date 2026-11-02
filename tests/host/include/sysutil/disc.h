#pragma once
#include "ppu-types.h"
int sysDiscRegisterDiscChangeCallback(void (*ej)(void), void (*in)(u32, char *));
int sysDiscUnregisterDiscChangeCallback();
