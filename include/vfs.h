// Output files on any destination: the internal HDD and FAT32 USB drives through stdio, and USB
// drives the PS3 does not mount itself through FatFs (exFAT, paths "exfatN:/...", N = USB port).
#ifndef PSXRIP_VFS_H
#define PSXRIP_VFS_H

#include <stdint.h>
#include <stddef.h>

struct VFile;

VFile *vf_open(const char *path, const char *mode);   // "wb", "rb" or "r+b"
size_t vf_write(VFile *f, const void *p, size_t n);
size_t vf_read(VFile *f, void *p, size_t n);
int vf_seek(VFile *f, uint64_t pos);
int vf_printf(VFile *f, const char *fmt, ...);
int vf_close(VFile *f);
int vf_unlink(const char *path);
int vf_rename(const char *from, const char *to);
int vf_mkdir(const char *path);
int vf_exists(const char *path);

enum { VF_EXFAT = 1 };
struct VfVolume {
	char root[16];       // "exfat3:"
	int port, type;
	uint64_t free_mb;
};
// Finds USB drives with a file system the PS3 does not mount (exFAT) and mounts them.
int vf_scan(VfVolume *out, int max);
int vf_free_mb(const char *path, uint64_t *mb);
// Entries of an exFAT directory; -1 for paths the PS3 serves itself (list those with opendir).
int vf_list(const char *dir, void (*cb)(const char *name, void *ctx), void *ctx);

#endif
