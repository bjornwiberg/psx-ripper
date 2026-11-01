// Output files on any destination, see vfs.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
#include "ff.h"
#include "vfs.h"

int usb_info(int port, uint64_t *sectors, uint32_t *sector_size);
void usb_close(int port);

struct VFile {
	FILE *std;   // stdio file, or NULL for FatFs
	FIL fil;
};

static FATFS g_fs[8];
static int g_mounted[8];

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
		return f;
	}
	BYTE m = !strcmp(mode, "rb") ? FA_READ : !strcmp(mode, "r+b") ? (FA_READ | FA_WRITE) : (FA_WRITE | FA_CREATE_ALWAYS);
	if (f_open(&f->fil, fp, m) != FR_OK) { free(f); return NULL; }
	return f;
}

size_t vf_write(VFile *f, const void *p, size_t n)
{
	if (f->std) return fwrite(p, 1, n, f->std);
	UINT w = 0;
	return f_write(&f->fil, p, (UINT)n, &w) == FR_OK ? w : 0;
}

size_t vf_read(VFile *f, void *p, size_t n)
{
	if (f->std) return fread(p, 1, n, f->std);
	UINT r = 0;
	return f_read(&f->fil, p, (UINT)n, &r) == FR_OK ? r : 0;
}

int vf_seek(VFile *f, uint64_t pos)
{
	if (f->std) return fseeko(f->std, (off_t)pos, SEEK_SET);
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
	int r = f->std ? fclose(f->std) : (f_close(&f->fil) == FR_OK ? 0 : -1);
	free(f);
	return r;
}

int vf_unlink(const char *path)
{
	char fp[320];
	if (fat_path(path, fp, sizeof(fp)) < 0) return unlink(path);
	return f_unlink(fp) == FR_OK ? 0 : -1;
}

int vf_rename(const char *from, const char *to)
{
	char a[320], b[320];
	if (fat_path(from, a, sizeof(a)) < 0) return rename(from, to);
	if (fat_path(to, b, sizeof(b)) < 0) return -1;
	// f_rename takes the new name without the drive number
	const char *nb = strchr(b, ':');
	return f_rename(a, nb ? nb + 1 : b) == FR_OK ? 0 : -1;
}

int vf_mkdir(const char *path)
{
	char fp[320];
	if (fat_path(path, fp, sizeof(fp)) < 0) return mkdir(path, 0777);
	FRESULT r = f_mkdir(fp);
	return r == FR_OK || r == FR_EXIST ? 0 : -1;
}

int vf_exists(const char *path)
{
	char fp[320];
	if (fat_path(path, fp, sizeof(fp)) < 0) { struct stat st; return stat(path, &st) == 0; }
	FILINFO fi;
	return f_stat(fp, &fi) == FR_OK;
}

int vf_free_mb(const char *path, uint64_t *mb)
{
	char fp[320];
	int d = fat_path(path, fp, sizeof(fp));
	if (d < 0) return -1;
	DWORD clusters = 0;
	FATFS *fs;
	char drv[4] = {(char)('0' + d), ':', 0};
	if (f_getfree(drv, &clusters, &fs) != FR_OK) return -1;
	*mb = (uint64_t)clusters * fs->csize * (fs->ssize ? fs->ssize : 512) / (1024 * 1024);
	return 0;
}

int vf_scan(VfVolume *out, int max)
{
	int n = 0;
	for (int port = 0; port < 8; port++) {
		uint64_t sectors;
		uint32_t ss;
		if (usb_info(port, &sectors, &ss)) {   // nothing on this port (any more)
			if (g_mounted[port]) { char drv[4] = {(char)('0' + port), ':', 0}; f_unmount(drv); usb_close(port); g_mounted[port] = 0; }
			continue;
		}
		char drv[4] = {(char)('0' + port), ':', 0};
		if (!g_mounted[port]) {
			// mount reads the boot sector only; a FAT32 drive is mounted by the PS3 itself and is
			// released again at once, so it is never written through FatFs
			if (f_mount(&g_fs[port], drv, 1) != FR_OK) { usb_close(port); continue; }
			if (g_fs[port].fs_type != FS_EXFAT) { f_unmount(drv); usb_close(port); continue; }
			g_mounted[port] = 1;
		}
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
