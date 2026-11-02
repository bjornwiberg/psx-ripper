// Host build (UI tests): what a test sets up instead of hardware.
#pragma once
struct HostPad { int cross, circle, triangle, square, up, down, left, right, start, select, l1, r1, l2, r2; };
struct HostUsb { int attached, port; char name[40]; };
extern HostPad host_pad;
extern HostUsb host_usb[];
extern int host_lang;
