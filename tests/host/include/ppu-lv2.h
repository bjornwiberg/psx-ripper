// Host build (UI tests): system calls do nothing and fail.
#pragma once
#include "ppu-types.h"
#define lv2syscall1(n, a) (void)0
#define lv2syscall2(n, a, b) (void)0
#define lv2syscall3(n, a, b, c) (void)0
#define lv2syscall4(n, a, b, c, d) (void)0
#define lv2syscall5(n, a, b, c, d, e) (void)0
#define lv2syscall6(n, a, b, c, d, e, f) (void)0
#define lv2syscall7(n, a, b, c, d, e, f, g) (void)0
#define lv2syscall8(n, a, b, c, d, e, f, g, h) (void)0
#define return_to_user_prog(t) return (t)-1
#define LV2_SYSCALL static inline int
#define LV2_INLINE static inline
static const u64 p1 = 0;   // the register a call returns in: nothing on the host
