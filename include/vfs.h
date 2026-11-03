// Output files on any destination: the internal HDD and FAT32 USB drives through stdio, and USB
// drives the PS3 does not mount itself through FatFs (exFAT, paths "exfatN:/...", N = USB port).
#ifndef PSXRIP_VFS_H
#define PSXRIP_VFS_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

struct VFile;

VFile *vf_open(const char *path, const char *mode);   // "wb", "w+b", "rb" or "r+b"
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

// Entries of any directory (hard disk, FAT32 stick, exFAT drive), names starting with '.' left out;
// -1 when it cannot be opened (not there, or the drive is gone).
int vf_listdir(const char *dir, void (*cb)(const char *name, void *ctx), void *ctx);
int vf_size(const char *path, uint64_t *size);   // 0 = ok
// A folder's modification time; -1 where it does not tell whether its files changed: only the
// internal hard disk's file system updates it (FAT32 and exFAT folders keep their creation date).
int vf_mtime(const char *path, time_t *t);
// The volume label of the drive on a USB port (FAT32 or exFAT), as found by the last vf_scan;
// "" = it has none or it could not be read. -1 for a port without a drive.
int vf_label(int port, char *out, size_t n);

#endif
