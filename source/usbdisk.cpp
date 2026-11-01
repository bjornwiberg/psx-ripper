// FatFs disk I/O for USB drives the PS3 does not mount itself (exFAT): raw sector access via the
// lv2 storage syscalls. Drive number = USB port (0-7).
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <malloc.h>
#include <unistd.h>
#include "ff.h"
#include "diskio.h"
#include "storage.h"

static int g_fd[8] = {-1, -1, -1, -1, -1, -1, -1, -1};
static uint32_t g_ss[8];
static uint64_t g_count[8];
static uint8_t *g_bounce;   // the syscalls want an aligned buffer
#define BOUNCE_SECTORS 256

int usb_info(int port, uint64_t *sectors, uint32_t *sector_size)
{
	static struct storage_device_info info __attribute__((aligned(16)));
	memset(&info, 0, sizeof(info));
	info.unknown03 = 0x12345678;
	if (sys_storage_get_device_info(usb_device_id(port), (uint8_t *)&info)) return -1;
	if (sectors) *sectors = info.total_sectors;
	if (sector_size) *sector_size = info.sector_size;
	return 0;
}

void usb_close(int port)
{
	if (port >= 0 && port < 8 && g_fd[port] >= 0) { sys_storage_close(g_fd[port]); g_fd[port] = -1; }
}

DSTATUS disk_initialize(BYTE pdrv)
{
	if (pdrv >= 8) return STA_NOINIT;
	if (g_fd[pdrv] >= 0) return 0;
	if (usb_info(pdrv, &g_count[pdrv], &g_ss[pdrv]) || g_ss[pdrv] < FF_MIN_SS || g_ss[pdrv] > FF_MAX_SS) return STA_NOINIT;
	if (!g_bounce) g_bounce = (uint8_t *)memalign(128, BOUNCE_SECTORS * FF_MAX_SS);
	if (!g_bounce || sys_storage_open(usb_device_id(pdrv), &g_fd[pdrv])) { g_fd[pdrv] = -1; return STA_NOINIT; }
	return 0;
}

DSTATUS disk_status(BYTE pdrv)
{
	return pdrv < 8 && g_fd[pdrv] >= 0 ? 0 : STA_NOINIT;
}

DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
	if (disk_status(pdrv)) return RES_NOTRDY;
	while (count) {
		UINT n = count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;
		uint32_t done = 0;
		int r = -1;
		for (int t = 0; t < 4 && r; t++) { r = sys_storage_read(g_fd[pdrv], (uint32_t)sector, n, g_bounce, &done); if (r) usleep(50000); }
		if (r || done != n) return RES_ERROR;
		memcpy(buff, g_bounce, (size_t)n * g_ss[pdrv]);
		buff += (size_t)n * g_ss[pdrv]; sector += n; count -= n;
	}
	return RES_OK;
}

DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
	if (disk_status(pdrv)) return RES_NOTRDY;
	while (count) {
		UINT n = count > BOUNCE_SECTORS ? BOUNCE_SECTORS : count;
		memcpy(g_bounce, buff, (size_t)n * g_ss[pdrv]);
		uint32_t done = 0;
		int r = -1;
		for (int t = 0; t < 4 && r; t++) { r = sys_storage_write(g_fd[pdrv], (uint32_t)sector, n, g_bounce, &done); if (r) usleep(50000); }
		if (r || done != n) return RES_ERROR;
		buff += (size_t)n * g_ss[pdrv]; sector += n; count -= n;
	}
	return RES_OK;
}

DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
	if (disk_status(pdrv)) return RES_NOTRDY;
	switch (cmd) {
	case CTRL_SYNC: return RES_OK;   // the syscalls write through
	case GET_SECTOR_COUNT: *(LBA_t *)buff = g_count[pdrv]; return RES_OK;
	case GET_SECTOR_SIZE: *(WORD *)buff = (WORD)g_ss[pdrv]; return RES_OK;
	case GET_BLOCK_SIZE: *(DWORD *)buff = 1; return RES_OK;
	}
	return RES_PARERR;
}

DWORD get_fattime(void)
{
	time_t t = time(NULL);
	struct tm *g = localtime(&t);
	if (!g || g->tm_year < 80) return ((DWORD)(2026 - 1980) << 25) | (1 << 21) | (1 << 16);
	return ((DWORD)(g->tm_year - 80) << 25) | ((DWORD)(g->tm_mon + 1) << 21) | ((DWORD)g->tm_mday << 16) |
	       ((DWORD)g->tm_hour << 11) | ((DWORD)g->tm_min << 5) | ((DWORD)g->tm_sec >> 1);
}
