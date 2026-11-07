// Host build (UI tests): what a test sets up instead of hardware.
#pragma once
struct HostPad { int cross, circle, triangle, square, up, down, left, right, start, select, l1, r1, l2, r2; };
struct HostUsb { int attached, port; char name[40]; };
extern HostPad host_pad;
extern HostUsb host_usb[];
extern int host_lang;
// called once per frame (the screens' flip): a test can look at a screen and press its buttons
extern void (*host_frame)();
// MCP tests: webMAN requests started, the web server's paths (remote_set; -1 never called), a
// mutex held by the calling thread, a call from each drive scan / free-space query
extern int host_webman_calls, host_remote_flags;
int host_mutex_held(unsigned m);
extern void (*host_io)();
