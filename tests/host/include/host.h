// Host build (UI tests): what a test sets up instead of hardware.
#pragma once
struct HostPad { int cross, circle, triangle, square, up, down, left, right, start, select, l1, r1, l2, r2; };
struct HostUsb { int attached, port; char name[40]; };
extern HostPad host_pad;
extern HostUsb host_usb[];
extern int host_lang;
// The console's drives: paths /dev_... and exfatN:/... are looked up under host_root ("" = they do
// not exist, as on an empty console); host_exfat[port] = an exFAT drive is on that port;
// host_label[port] = its volume label (FAT32 or exFAT).
extern char host_root[256];
extern int host_exfat[8];
extern char host_label[8][24];
