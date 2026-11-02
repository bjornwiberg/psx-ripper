// Host build (UI tests): the screen is just the canvas.
#pragma once
#include "ppu-types.h"
extern u32 display_width, display_height, color_pitch, curr_fb;
extern u32 *color_buffer[2];
extern int flip_timeouts;
void flip_sync();
void flip();
void waitflip();
#include "rsx/rsx.h"
#define HOST_SIZE (32 * 1024 * 1024)
extern gcmContextData *context;
void init_screen(void *host_addr, u32 size);
#define gcmSetWaitFlip(c) (void)0
#define rsxFinish(c, r) (void)0
