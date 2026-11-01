// Minimal lv2 storage / ATAPI wrappers for the Blu-ray drive.
// Layout and syscall numbers taken from sacd-ripper (libs/libcommon/sys/storage.h)
// and webMAN-MOD (include/feat/scsi_info.h).
#ifndef PSXRIP_STORAGE_H
#define PSXRIP_STORAGE_H

#include <stdint.h>
#include <string.h>
#include <ppu-lv2.h>

#define BD_DEVICE 0x0101000000000006ULL

#define ATAPI_NON_DATA_PROTO    0
#define ATAPI_PIO_DATA_IN_PROTO 1
#define ATAPI_PIO_DATA_OUT_PROTO 2
#define ATAPI_DMA_PROTO         3

#define ATAPI_DIR_WRITE 0
#define ATAPI_DIR_READ  1

struct lv2_atapi_cmnd_block {
	uint8_t  pkt[32];
	uint32_t pktlen;
	uint32_t blocks;
	uint32_t block_size; // transfer size is block_size * blocks
	uint32_t proto;
	uint32_t in_out;
	uint32_t unknown;
} __attribute__((packed));

static inline int sys_storage_open(uint64_t id, int *fd)
{
	lv2syscall4(600, id, 0, (uint64_t)fd, 0);
	return_to_user_prog(int);
}

static inline int sys_storage_close(int fd)
{
	lv2syscall1(601, fd);
	return_to_user_prog(int);
}

static inline int sys_storage_read(int fd, uint32_t start, uint32_t count, uint8_t *buf, uint32_t *nread)
{
	lv2syscall7(602, fd, 0, start, count, (uint64_t)buf, (uint64_t)nread, 0);
	return_to_user_prog(int);
}

static inline int sys_storage_write(int fd, uint32_t start, uint32_t count, const uint8_t *buf, uint32_t *nwritten)
{
	lv2syscall7(603, fd, 0, start, count, (uint64_t)buf, (uint64_t)nwritten, 0);
	return_to_user_prog(int);
}

// Layout of the device info returned by syscall 609 (from libntfs_ext/sacd-ripper).
struct storage_device_info {
	uint8_t  name[7];
	uint8_t  unknown01;
	uint32_t unknown02;
	uint32_t zero01;
	uint32_t unknown03;
	uint32_t unknown04;
	uint8_t  zero02[16];
	uint64_t total_sectors;
	uint32_t sector_size;
	uint32_t unknown05;
	uint8_t  writable;
	uint8_t  unknown06[3];
	uint32_t unknown07;
} __attribute__((packed));

// USB mass storage devices: port 0-5 are ...0A-...0F, 6 and 7 follow after a gap.
static inline uint64_t usb_device_id(int port) { return 0x010300000000000AULL + port + (port >= 6 ? 0xF : 0); }

static inline int sys_storage_get_device_info(uint64_t device, uint8_t *info)
{
	lv2syscall2(609, device, (uint64_t)info);
	return_to_user_prog(int);
}

static inline int sys_storage_send_device_cmd(int fd, uint32_t cmd, void *cmd_block, uint32_t cmd_size, void *data, uint32_t data_len)
{
	lv2syscall6(604, fd, cmd, (uint64_t)cmd_block, cmd_size, (uint64_t)data, data_len);
	return_to_user_prog(int);
}

// syscall 864 needs lv2 access rights; IRISMAN patches them on supported firmwares.
static inline int sys_storage_reset_bd(void)
{
	lv2syscall2(864, 0x5004, 0x29);
	return_to_user_prog(int);
}

static inline int sys_storage_ctrl_bd(int func)
{
	uint64_t f[3] = {(uint64_t)func, 0, 0};
	lv2syscall2(864, 0x5007, (uint64_t)&f[0]);
	return_to_user_prog(int);
}

// syscall 616 is what sacd-ripper uses from a game-app context; webMAN (VSH plugin) uses 604.
static inline int sys_storage_send_atapi_command(uint32_t sc, int fd, struct lv2_atapi_cmnd_block *cmd, uint8_t *buffer)
{
	uint64_t tag;
	lv2syscall7(sc, fd, 1, (uint64_t)cmd, sizeof(struct lv2_atapi_cmnd_block),
	            (uint64_t)buffer, cmd->block_size, (uint64_t)&tag);
	return_to_user_prog(int);
}

static inline void atapi_init(struct lv2_atapi_cmnd_block *cmd, uint32_t size, uint32_t proto, uint32_t dir)
{
	memset(cmd, 0, sizeof(*cmd));
	cmd->pktlen = 12;
	cmd->blocks = 1;
	cmd->block_size = size;
	cmd->proto = proto;
	cmd->in_out = dir;
}

#endif
