// Output files on any destination, see vfs.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/mutex.h>
// FatFs and the C library both name a directory DIR: FatFs's is FF_DIR here (its functions are C,
// so the name does not reach the linker)
#define DIR FF_DIR
#include "ff.h"
#undef DIR
#include <dirent.h>
#include "vfs.h"

int usb_info(int port, uint64_t *sectors, uint32_t *sector_size);
void usb_close(int port);

struct VFile {
	FILE *std;   // stdio file, or NULL for FatFs
	FIL fil;
};

static FATFS g_fs[8];
static int g_mounted[8];
// The volume label of the drive on each port, read once when the drive is first seen (keyed by its
// size, so another drive on the same port is read again). FatFs reads it from the root directory's
// label entry, for FAT32 and exFAT alike, while the volume is mounted read-only by vf_scan anyway.
static char g_label[8][48];
static uint64_t g_label_of[8];   // sectors of the drive the label belongs to, 0 = none read

// FatFs is built without re-entrancy: drives that rip at the same time take turns on it (the
// stdio paths need no lock). Recursive, created by the first caller (vf_scan at start-up).
static sys_mutex_t g_lock;
static int g_lock_made = 0;
struct Lk {
	Lk()
	{
		if (!g_lock_made) {
			sys_mutex_attr_t a;
			sysMutexAttrInitialize(a);
			a.attr_recursive = SYS_MUTEX_ATTR_RECURSIVE;
			sysMutexCreate(&g_lock, &a);
			g_lock_made = 1;
		}
		sysMutexLock(g_lock, 0);
	}
	~Lk() { sysMutexUnlock(g_lock); }
};

// "exfat3:/PSXISO/x.bin" -> drive 3, FatFs path "3:/PSXISO/x.bin". Returns -1 for stdio paths.
static int fat_path(const char *path, char *out, size_t n)
{
	if (strncmp(path, "exfat", 5) || path[5] < '0' || path[5] > '7' || path[6] != ':') return -1;
	int d = path[5] - '0';
	snprintf(out, n, "%d:%s", d, path[7] ? path + 7 : "/");
	return d;
}

VFile *vf_open(const char *path, const char *mode)
{
	VFile *f = (VFile *)calloc(1, sizeof(VFile));
	if (!f) return NULL;
	char fp[320];
	if (fat_path(path, fp, sizeof(fp)) < 0) {
		f->std = fopen(path, mode);
		if (!f->std) { free(f); return NULL; }
		// the image is written in 2.4 MB blocks: unbuffered, or stdio splits them into tiny writes
		// (very slow on USB drives)
		if (mode[0] != 'r') setvbuf(f->std, NULL, _IONBF, 0);
		return f;
	}
	Lk lk;
	BYTE m = !strcmp(mode, "rb") ? FA_READ : !strcmp(mode, "r+b") ? (FA_READ | FA_WRITE)
	       : !strcmp(mode, "w+b") ? (FA_READ | FA_WRITE | FA_CREATE_ALWAYS) : (FA_WRITE | FA_CREATE_ALWAYS);
	if (f_open(&f->fil, fp, m) != FR_OK) { free(f); return NULL; }
	return f;
}

size_t vf_write(VFile *f, const void *p, size_t n)
{
	if (f->std) return fwrite(p, 1, n, f->std);
	Lk lk;
	UINT w = 0;
	return f_write(&f->fil, p, (UINT)n, &w) == FR_OK ? w : 0;
}

size_t vf_read(VFile *f, void *p, size_t n)
{
	if (f->std) return fread(p, 1, n, f->std);
	Lk lk;
	UINT r = 0;
	return f_read(&f->fil, p, (UINT)n, &r) == FR_OK ? r : 0;
}

int vf_seek(VFile *f, uint64_t pos)
{
	if (f->std) return fseeko(f->std, (off_t)pos, SEEK_SET);
	Lk lk;
	return f_lseek(&f->fil, (FSIZE_t)pos) == FR_OK ? 0 : -1;
}

int vf_printf(VFile *f, const char *fmt, ...)
{
	char b[512];
	va_list ap;
	va_start(ap, fmt);
	int n = vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	if (n < 0) return n;
	if (n >= (int)sizeof(b)) n = sizeof(b) - 1;
	return (int)vf_write(f, b, n);
}

int vf_close(VFile *f)
{
	int r;
	if (f->std) r = fclose(f->std);
	else { Lk lk; r = f_close(&f->fil) == FR_OK ? 0 : -1; }
	free(f);
	return r;
}

int vf_unlink(const char *path)
{
	char fp[320];
	if (fat_path(path, fp, sizeof(fp)) < 0) return unlink(path);
	Lk lk;
	return f_unlink(fp) == FR_OK ? 0 : -1;
}

int vf_rename(const char *from, const char *to)
{
	char a[320], b[320];
	if (fat_path(from, a, sizeof(a)) < 0) return rename(from, to);
	if (fat_path(to, b, sizeof(b)) < 0) return -1;
	Lk lk;
	// f_rename takes the new name without the drive number
	const char *nb = strchr(b, ':');
	return f_rename(a, nb ? nb + 1 : b) == FR_OK ? 0 : -1;
}

int vf_mkdir(const char *path)
{
	char fp[320];
	if (fat_path(path, fp, sizeof(fp)) < 0) return mkdir(path, 0777);
	Lk lk;
	FRESULT r = f_mkdir(fp);
	return r == FR_OK || r == FR_EXIST ? 0 : -1;
}

int vf_exists(const char *path)
{
	char fp[320];
	if (fat_path(path, fp, sizeof(fp)) < 0) { struct stat st; return stat(path, &st) == 0; }
	Lk lk;
	FILINFO fi;
	return f_stat(fp, &fi) == FR_OK;
}

int vf_free_mb(const char *path, uint64_t *mb)
{
	char fp[320];
	int d = fat_path(path, fp, sizeof(fp));
	if (d < 0) return -1;
	Lk lk;
	DWORD clusters = 0;
	FATFS *fs;
	char drv[4] = {(char)('0' + d), ':', 0};
	if (f_getfree(drv, &clusters, &fs) != FR_OK) return -1;
	*mb = (uint64_t)clusters * fs->csize * (fs->ssize ? fs->ssize : 512) / (1024 * 1024);
	return 0;
}

// The label of the mounted volume on a port into g_label (whole UTF-8 characters, printable only).
static void read_label(int port, const char *drv)
{
	char l[48] = "";
	if (f_getlabel(drv, l, NULL) != FR_OK) l[0] = 0;
	l[sizeof(l) - 1] = 0;
	for (char *p = l; *p; p++) if ((unsigned char)*p < 0x20 || *p == 0x7F) *p = ' ';
	snprintf(g_label[port], sizeof(g_label[port]), "%s", l);
}

int vf_label(int port, char *out, size_t n)
{
	if (port < 0 || port > 7) return -1;
	Lk lk;
	if (!g_label_of[port]) return -1;
	snprintf(out, n, "%s", g_label[port]);
	return 0;
}

int vf_scan(VfVolume *out, int max)
{
	Lk lk;
	int n = 0;
	for (int port = 0; port < 8; port++) {
		uint64_t sectors;
		uint32_t ss;
		if (usb_info(port, &sectors, &ss)) {   // nothing on this port (any more)
			if (g_mounted[port]) { char drv[4] = {(char)('0' + port), ':', 0}; f_unmount(drv); usb_close(port); g_mounted[port] = 0; }
			g_label_of[port] = 0; g_label[port][0] = 0;
			continue;
		}
		char drv[4] = {(char)('0' + port), ':', 0};
		int need_label = g_label_of[port] != sectors;
		if (need_label) { g_label_of[port] = sectors; g_label[port][0] = 0; }
		if (!g_mounted[port]) {
			// mount reads the boot sector only; a FAT32 drive is mounted by the PS3 itself and is
			// released again at once, so it is never written through FatFs. Its label is read (once
			// per drive) before that: reading the root directory's first sectors, nothing written.
			if (f_mount(&g_fs[port], drv, 1) != FR_OK) { usb_close(port); continue; }
			if (need_label) read_label(port, drv);
			if (g_fs[port].fs_type != FS_EXFAT) { f_unmount(drv); usb_close(port); continue; }
			g_mounted[port] = 1;
		} else if (need_label) read_label(port, drv);
		if (n < max) {
			snprintf(out[n].root, sizeof(out[n].root), "exfat%d:", port);
			out[n].port = port;
			out[n].type = VF_EXFAT;
			out[n].free_mb = 0;
			vf_free_mb(out[n].root, &out[n].free_mb);
		}
		n++;
	}
	return n;
}

// Calls cb(name, ctx) for each entry of an exFAT directory. Returns the number of entries, or -1
// for a path the PS3 itself serves (the caller lists those with opendir).
int vf_list(const char *dir, void (*cb)(const char *name, void *ctx), void *ctx)
{
	char fp[320];
	if (fat_path(dir, fp, sizeof(fp)) < 0) return -1;
	Lk lk;
	FF_DIR d;
	if (f_opendir(&d, fp) != FR_OK) return 0;
	int n = 0;
	FILINFO fi;
	while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
		if (fi.fname[0] == '.') continue;
		cb(fi.fname, ctx);
		n++;
	}
	f_closedir(&d);
	return n;
}

int vf_listdir(const char *dir, void (*cb)(const char *name, void *ctx), void *ctx)
{
	char fp[320];
	if (fat_path(dir, fp, sizeof(fp)) < 0) {
		DIR *d = opendir(dir);
		if (!d) return -1;
		int n = 0;
		struct dirent *de;
		while ((de = readdir(d))) if (de->d_name[0] != '.') { cb(de->d_name, ctx); n++; }
		closedir(d);
		return n;
	}
	Lk lk;
	FF_DIR d;
	if (f_opendir(&d, fp) != FR_OK) return -1;
	int n = 0;
	FILINFO fi;
	while (f_readdir(&d, &fi) == FR_OK && fi.fname[0]) {
		if (fi.fname[0] == '.') continue;
		cb(fi.fname, ctx);
		n++;
	}
	f_closedir(&d);
	return n;
}

int vf_size(const char *path, uint64_t *size)
{
	char fp[320];
	if (fat_path(path, fp, sizeof(fp)) < 0) {
		struct stat st;
		if (stat(path, &st)) return -1;
		*size = (uint64_t)st.st_size;
		return 0;
	}
	Lk lk;
	FILINFO fi;
	if (f_stat(fp, &fi) != FR_OK) return -1;
	*size = (uint64_t)fi.fsize;
	return 0;
}

int vf_mtime(const char *path, time_t *t)
{
	if (strncmp(path, "/dev_hdd0", 9)) return -1;
	struct stat st;
	if (stat(path, &st)) return -1;
	*t = st.st_mtime;
	return 0;
}
