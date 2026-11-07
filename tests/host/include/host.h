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
// The console's drives: paths /dev_... and exfatN:/... are looked up under host_root ("" = they do
// not exist, as on an empty console); host_exfat[port] = an exFAT drive is on that port;
// host_label[port] = its volume label (FAT32 or exFAT).
extern char host_root[256];
extern int host_exfat[8];
extern int (*host_rename_hook)(const char *from, const char *to);   // nonzero: vf_rename fails
extern char host_label[8][24];
extern const char *host_listdir_extra;   // a name vf_listdir adds to every folder (NULL = none)
// Redump's local copy: host_rd_local = there is one; it knows one disc of one track, with MD5
// host_rd_md5, named host_rd_name (0 = no copy, as before: every check is "unchecked").
extern int host_rd_local;
extern unsigned char host_rd_md5[16];
extern char host_rd_name[160];
