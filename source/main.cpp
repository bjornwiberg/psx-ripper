// PSX Ripper - dumps a PS1 disc from the PS3 Blu-ray drive to
// /dev_hdd0/PSXISO/<NAME> [<SERIAL>].bin/.cue and writes a LibCrypt .sbi
// to /dev_hdd0/tmp/sbi/<SERIAL>.sbi (read by Cobra 8.x at mount time).
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <malloc.h>
#include <time.h>

#include <io/pad.h>
#include <sys/thread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sysutil/msg.h>
#include <sysutil/sysutil.h>
#include <sysutil/disc.h>
#include <net/net.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <dirent.h>

#include "rsxutil.h"
#include "storage.h"
#include "lang.h"
#include "ui.h"
#include "md5.h"

#define LOG_PATH   "/dev_hdd0/tmp/psxrip.log"
// Output locations; the mount test redirects both so a mounted image is never overwritten
static const char *OUT_DIR = "/dev_hdd0/PSXISO";
static const char *SBI_DIR = "/dev_hdd0/tmp/sbi";
static int g_mounttest = 0;   // flag psxrip.mounttest: rip the image Cobra has mounted (development aid)

// ---------------------------------------------------------------- settings (saved in CFG_PATH)
#define CFG_PATH "/dev_hdd0/tmp/psxrip.cfg"
static char g_outdir[64] = "/dev_hdd0/PSXISO";
static int g_retries = 4;          // attempts per READ CD before falling back / giving up
static int g_speed = 0;            // 0 = max, 1 = 8x, 2 = 4x, 3 = 2x
static int g_eject = 1;            // eject the disc after a successful rip
static int g_autostart = 0;        // rip an inserted disc without the confirmation
static const int speed_x[4] = {0, 8, 4, 2};

static void cfg_load()
{
	FILE *f = fopen(CFG_PATH, "r");
	if (!f) return;
	char line[128];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "outdir=", 7) && line[7] == '/') snprintf(g_outdir, sizeof(g_outdir), "%s", line + 7);
		else if (!strncmp(line, "retries=", 8)) { int r = atoi(line + 8); if (r >= 1 && r <= 64) g_retries = r; }
		else if (!strncmp(line, "speed=", 6)) { int v = atoi(line + 6); if (v >= 0 && v <= 3) g_speed = v; }
		else if (!strncmp(line, "eject=", 6)) g_eject = atoi(line + 6) ? 1 : 0;
		else if (!strncmp(line, "autostart=", 10)) g_autostart = atoi(line + 10) ? 1 : 0;
	}
	fclose(f);
}

static void cfg_save()
{
	FILE *f = fopen(CFG_PATH, "w");
	if (!f) return;
	fprintf(f, "outdir=%s\nretries=%d\nspeed=%d\neject=%d\nautostart=%d\n", g_outdir, g_retries, g_speed, g_eject, g_autostart);
	fclose(f);
}

#define RAW_SIZE   2352
#define SUBQ_SIZE  16
#define CHUNK      1024            // sectors per READ CD (same as IRISMAN)
#define MAX_TRACKS 99
#define MAX_LC     512             // max mismatching Q sectors we keep

// ---------------------------------------------------------------- logging
static void logf(const char *fmt, ...)
{
	FILE *f = fopen(LOG_PATH, "a");
	if (!f) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

// ---------------------------------------------------------------- dialogs
static volatile int dialog_action = 0;

extern "C" {
static void dialog_handler(msgButton button, void *usrData)
{
	switch (button) {
	case MSG_DIALOG_BTN_OK: dialog_action = 1; break;   // also YES
	case MSG_DIALOG_BTN_NO:
	case MSG_DIALOG_BTN_ESCAPE: dialog_action = 2; break;
	case MSG_DIALOG_BTN_NONE: dialog_action = -1; break;
	default: break;
	}
}

static volatile int exit_requested = 0;
static void sysutil_callback(u64 status, u64 param, void *usrdata)
{
	if (status == SYSUTIL_EXIT_GAME) exit_requested = 1;
}

static volatile int disc_inserted = 0, disc_present = 1;   // assume a disc until the drive says otherwise
static void disc_eject_cb(void) { disc_inserted = 0; disc_present = 0; }
static void disc_insert_cb(u32 type, char *title) { disc_inserted = 1; disc_present = 1; }

static void lv1_storage_unpatch();
static void program_exit_callback()
{
	lv1_storage_unpatch();   // safety net; normally already done
	gcmSetWaitFlip(context);
	rsxFinish(context, 1);
}
}

// Development aid: if /dev_hdd0/tmp/psxrip.shot exists, the next frame is saved as a BMP.
static void maybe_screenshot()
{
	static int frame = 0;
	if (++frame % 30) return;
	struct stat st;
	if (stat("/dev_hdd0/tmp/psxrip.shot", &st)) return;
	unlink("/dev_hdd0/tmp/psxrip.shot");
	FILE *f = fopen("/dev_hdd0/tmp/psxrip_screen.bmp", "wb");
	if (!f) return;
	int w = ui_w(), h = ui_h(), rowb = w * 3, pad = (4 - rowb % 4) % 4;
	uint32_t size = 54 + (rowb + pad) * h;
	uint8_t hd[54] = {'B', 'M'};
	memcpy(hd + 2, &size, 4);          // little-endian fields, written byte by byte below
	auto le32 = [&](int o, uint32_t v) { hd[o] = v; hd[o + 1] = v >> 8; hd[o + 2] = v >> 16; hd[o + 3] = v >> 24; };
	le32(2, size); le32(10, 54); le32(14, 40); le32(18, w); le32(22, h);
	hd[26] = 1; hd[28] = 24;
	fwrite(hd, 1, 54, f);
	uint8_t *row = (uint8_t *)malloc(rowb + pad);
	memset(row, 0, rowb + pad);
	for (int y = h - 1; y >= 0; y--) {
		const uint32_t *src = ui_canvas() + y * w;
		for (int x = 0; x < w; x++) { uint32_t c = src[x]; row[x * 3] = c; row[x * 3 + 1] = c >> 8; row[x * 3 + 2] = c >> 16; }
		fwrite(row, 1, rowb + pad, f);
	}
	free(row);
	fclose(f);
}

static void draw_backdrop()
{
	ui_background();
	ui_text(UI_TITLE, ui_w() / 2, ui_h() / 10, 0xFFFFFF, "PSX Ripper", UI_CENTER);
}

// Frames still to (re)draw: content is drawn into both buffers once, then only flipped.
static int g_redraw = 2;          // canvas needs to be redrawn
static int g_drawn = 0;           // canvas changed: copy it to both RSX buffers on the next flips
static int g_backdrop_shown = 0;

static void present()
{
	if (g_drawn > 0) {
		ui_blit();
		g_drawn--;
	}
	maybe_screenshot();
	flip_sync();   // the buffer we draw into next is never the one on screen
	static int last_to = 0;
	if (flip_timeouts != last_to) { logf("flip timeout (total %d)", flip_timeouts); last_to = flip_timeouts; }
}

static void do_flip()
{
	sysUtilCheckCallback();
	if (!g_backdrop_shown) { g_redraw = 1; g_backdrop_shown = 1; }
	if (g_redraw > 0) { draw_backdrop(); g_redraw = 0; g_drawn = 2; }
	present();
}

// Shows a dialog and blocks until answered. Returns 1 = OK/YES, 2 = NO/cancel.
static int ask(int type, const char *msg)
{
	dialog_action = 0;
	msgDialogOpen2((msgType)type, msg, dialog_handler, NULL, NULL);
	while (!dialog_action && !exit_requested) do_flip();
	msgDialogAbort();
	return exit_requested ? 2 : dialog_action;
}

static int ask_yesno(const char *msg)
{
	return ask(MSG_DIALOG_NORMAL | MSG_DIALOG_BTN_TYPE_YESNO | MSG_DIALOG_DISABLE_CANCEL_ON, msg) == 1;
}

static int g_headless_flag;
static void tell(const char *msg)
{
	if (g_headless_flag) { logf("[dialog] %s", msg); return; }
	ask(MSG_DIALOG_NORMAL | MSG_DIALOG_BTN_TYPE_OK | MSG_DIALOG_DISABLE_CANCEL_ON, msg);   // X only, no O/back
}

// ---------------------------------------------------------------- drive
static int g_fd = -1;
static uint32_t g_sc = 616;        // atapi syscall that worked
static uint8_t *g_cmdbuf;          // aligned command transfer buffer
static uint8_t *g_sec;   // 128-byte aligned, allocated in main()

static int atapi(const uint8_t *cdb, uint8_t *buf, uint32_t len, uint32_t proto)
{
	struct lv2_atapi_cmnd_block cmd;
	atapi_init(&cmd, len, proto, len ? ATAPI_DIR_READ : ATAPI_DIR_WRITE);
	if (!len) cmd.proto = ATAPI_NON_DATA_PROTO;
	memcpy(cmd.pkt, cdb, 12);
	return sys_storage_send_atapi_command(g_sc, g_fd, &cmd, buf);
}

struct Track {
	int num;
	int ctrl;        // 0x4 bit set = data
	uint32_t lba;
};
static Track g_tracks[MAX_TRACKS + 1];
static int g_ntracks = 0;
static uint32_t g_leadout = 0;

static uint32_t be32(const uint8_t *p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

static int read_toc()
{
	uint8_t cdb[12] = {0x43, 0x00, 0x00, 0, 0, 0, 0x01, 0x03, 0x24, 0, 0, 0};  // LBA, format 0, from track 1, alloc 804
	memset(g_cmdbuf, 0, 0x324);
	int ret = -1;
	// probe which syscall/proto combination the drive accepts
	const uint32_t scs[2] = {616, 604};
	const uint32_t protos[2] = {ATAPI_PIO_DATA_IN_PROTO, ATAPI_DMA_PROTO};
	for (int s = 0; s < 2 && ret; s++)
		for (int p = 0; p < 2 && ret; p++) {
			g_sc = scs[s];
			ret = atapi(cdb, g_cmdbuf, 0x324, protos[p]);
			logf("READ TOC sc=%u proto=%u -> 0x%x", g_sc, protos[p], ret);
		}
	if (ret) return ret;

	uint32_t len = (g_cmdbuf[0] << 8) | g_cmdbuf[1];
	int first = g_cmdbuf[2], last = g_cmdbuf[3];
	logf("TOC len=%u first=%d last=%d", len, first, last);
	g_ntracks = 0;
	for (uint32_t off = 4; off + 8 <= len + 2 && g_ntracks <= MAX_TRACKS; off += 8) {
		const uint8_t *d = g_cmdbuf + off;
		int ctrl = d[1] & 0x0F, num = d[2];
		uint32_t lba = be32(d + 4);
		logf("  track %d ctrl=%x lba=%u", num, ctrl, lba);
		if (num == 0xAA) { g_leadout = lba; break; }
		g_tracks[g_ntracks].num = num;
		g_tracks[g_ntracks].ctrl = ctrl;
		g_tracks[g_ntracks].lba = lba;
		g_ntracks++;
	}
	return (g_ntracks > 0 && g_leadout > 0) ? 0 : -2;
}

// Reads `count` raw sectors, each 2352 bytes followed by 16 bytes formatted Q,
// the same way IRISMAN does (syscall 604, one ATAPI block per sector, DMA).
#define SECT_Q (RAW_SIZE + SUBQ_SIZE)
static int g_read_mode = 0;   // 0 = syscall 604/6 args, 1 = syscall 616/7 args

// READ CD with a chosen subchannel selection: 0 = none, 1 = raw P-W (96 bytes), 2 = formatted Q (16 bytes)
static int read_cd_sub(uint32_t lba, uint32_t count, uint8_t *buf, uint8_t sub, uint32_t blk)
{
	struct lv2_atapi_cmnd_block cmd;
	memset(&cmd, 0, sizeof(cmd));
	cmd.pkt[0] = 0xBE;
	cmd.pkt[2] = lba >> 24; cmd.pkt[3] = lba >> 16; cmd.pkt[4] = lba >> 8; cmd.pkt[5] = lba;
	cmd.pkt[6] = count >> 16; cmd.pkt[7] = count >> 8; cmd.pkt[8] = count;
	cmd.pkt[9] = 0xF8;   // sync + header + user data + EDC/ECC
	cmd.pkt[10] = sub;
	cmd.pktlen = 12;
	cmd.blocks = count;
	cmd.block_size = blk;
	cmd.proto = ATAPI_DMA_PROTO;
	cmd.in_out = ATAPI_DIR_READ;
	int ret;
	do {
		ret = g_read_mode == 0
			? sys_storage_send_device_cmd(g_fd, 1, &cmd, sizeof(cmd), buf, blk * count)
			: sys_storage_send_atapi_command(616, g_fd, &cmd, buf);
	} while ((uint32_t)ret == 0x8001000A);   // EBUSY: drive spinning up
	return ret;
}

static int read_cd(uint32_t lba, uint32_t count, uint8_t *buf)
{
	return read_cd_sub(lba, count, buf, 0x02, SECT_Q);
}

// Q channel out of raw P-W subchannel data: bit 6 of each of the 96 bytes is one Q bit.
static void raw_pw_to_q(const uint8_t *pw, uint8_t *q12)
{
	memset(q12, 0, 12);
	for (int i = 0; i < 96; i++)
		if (pw[i] & 0x40) q12[i >> 3] |= 0x80 >> (i & 7);
}

// Development aid (flag file psxrip.diag): dump Q around known LibCrypt positions, formatted vs raw.
static void subq_diag()
{
	static const uint32_t lbas[] = {13953, 13954, 13955, 13956, 13959, 13960, 13961, 14105, 14110, 20000};
	uint8_t *b = (uint8_t *)memalign(128, 8192);
	for (unsigned k = 0; k < sizeof(lbas) / sizeof(lbas[0]); k++) {
		for (int rep = 0; rep < 3; rep++) {
			uint32_t l = lbas[k];
			int r1 = read_cd_sub(l, 1, b, 0x02, RAW_SIZE + 16);
			char f[64]; for (int i = 0; i < 12; i++) sprintf(f + i * 3, "%02x ", b[RAW_SIZE + i]);
			int r2 = read_cd_sub(l, 1, b, 0x01, RAW_SIZE + 96);
			uint8_t q[12]; raw_pw_to_q(b + RAW_SIZE, q);
			char rq[64]; for (int i = 0; i < 12; i++) sprintf(rq + i * 3, "%02x ", q[i]);
			logf("diag lba=%u rep=%d fmt(0x%x)=%s raw(0x%x)=%s", l, rep, r1, f, r2, rq);
		}
	}
	free(b);
}


// Re-initialises and authenticates the BD drive the way IRISMAN does on 4.21+.
// syscall 864 may be restricted by lv2 access rights; the return codes are logged.
static void bd_auth()
{
	int ret, n = 0;
	do { ret = sys_storage_reset_bd(); } while ((uint32_t)ret == 0x8001000A && ++n < 500000);
	logf("bd reset -> 0x%x", ret);
	if (ret) return;
	n = 0;
	do { ret = sys_storage_ctrl_bd(0x43); } while ((uint32_t)ret == 0x8001000A && ++n < 500000);
	logf("bd ctrl 0x43 -> 0x%x", ret);
	ret = sys_storage_ctrl_bd(0x3F);
	logf("bd ctrl 0x3f -> 0x%x", ret);
}

static int probe_read()
{
	int ret = -1;
	for (int m = 0; m < 2 && ret; m++) {
		g_read_mode = m;
		memset(g_sec, 0, 4096);
		ret = read_cd(16, 1, g_sec);
		logf("READ CD probe mode=%d -> 0x%x [%02x %02x .. %02x %02x %02x %02x]", m, ret,
		     g_sec[0], g_sec[1], g_sec[24], g_sec[25], g_sec[26], g_sec[27]);
	}
	uint32_t nread = 0;
	int r = sys_storage_read(g_fd, 16, 1, g_sec + 2048, &nread);
	logf("sys_storage_read(16) -> 0x%x nread=%u", r, nread);
	return ret;
}

// ---------------------------------------------------------------- iso9660 helpers

static int read_user_sector(uint32_t lba, uint8_t *out2048)
{
	int ret = read_cd(lba, 1, g_sec);
	if (ret) return ret;
	// Mode 2 Form 1: 12 sync + 4 header + 8 subheader; Mode 1: 16 bytes
	int off = (g_sec[15] == 2) ? 24 : 16;
	memcpy(out2048, g_sec + off, 2048);
	return 0;
}

static int find_file(const uint8_t *dir, uint32_t dirlen, const char *name, uint32_t *lba, uint32_t *size)
{
	size_t nl = strlen(name);
	for (uint32_t i = 0; i < dirlen && i < 2048;) {
		uint8_t rl = dir[i];
		if (!rl) break;
		uint8_t fl = dir[i + 32];
		const char *fn = (const char *)dir + i + 33;
		if (fl >= nl && !strncasecmp(fn, name, nl)) {
			*lba = (dir[i + 6] << 24) | (dir[i + 7] << 16) | (dir[i + 8] << 8) | dir[i + 9];
			*size = (dir[i + 14] << 24) | (dir[i + 15] << 16) | (dir[i + 16] << 8) | dir[i + 17];
			return 0;
		}
		i += rl;
	}
	return -1;
}

static char g_serial[16];   // "SLUS_005.53"
static char g_volid[33];

static void detect_disc_id()
{
	uint8_t pvd[2048], dir[2048], cnf[2048];
	g_serial[0] = 0;
	g_volid[0] = 0;
	if (read_user_sector(16, pvd) || memcmp(pvd + 1, "CD001", 5)) { logf("no ISO9660 PVD"); return; }

	memcpy(g_volid, pvd + 40, 32);
	g_volid[32] = 0;
	for (int i = 31; i >= 0 && (g_volid[i] == ' ' || g_volid[i] == 0); i--) g_volid[i] = 0;

	uint32_t root = pvd[158] | (pvd[159] << 8) | (pvd[160] << 16) | (pvd[161] << 24);  // root extent, little-endian copy
	logf("volume=%s root=%u", g_volid, root);
	if (read_user_sector(root, dir)) { logf("root dir read failed"); return; }
	uint32_t lba, size;
	if (find_file(dir, 2048, "SYSTEM.CNF", &lba, &size) || read_user_sector(lba, cnf)) {
		logf("SYSTEM.CNF not found (vol=%s)", g_volid);
		return;
	}
	cnf[2047] = 0;
	// BOOT = cdrom:\SLUS_005.53;1
	for (char *p = (char *)cnf; *p; p++) {
		if ((p[0] >= 'A' && p[0] <= 'Z') && p[4] == '_' && p[8] == '.' && p[1] >= 'A' && p[3] >= 'A') {
			memcpy(g_serial, p, 11);
			g_serial[11] = 0;
			break;
		}
	}
	logf("volume=%s serial=%s", g_volid, g_serial);
}

// ---------------------------------------------------------------- serial -> title
#include "titles.inc"

// "SLUS_005.53" -> "Alundra" (NULL when unknown)
static const char *lookup_title(const char *serial)
{
	if (strlen(serial) != 11) return NULL;
	int px = -1;
	for (unsigned i = 0; i < sizeof(title_prefixes) / sizeof(title_prefixes[0]); i++)
		if (!strncmp(serial, title_prefixes[i], 4)) { px = i; break; }
	if (px < 0) return NULL;
	uint32_t num = (serial[5] - '0') * 10000 + (serial[6] - '0') * 1000 + (serial[7] - '0') * 100 + (serial[9] - '0') * 10 + (serial[10] - '0');
	uint32_t key = px * 100000 + num;
	static const uint32_t n = sizeof(title_keys) / sizeof(title_keys[0]);
	static const char **ptrs = NULL;
	if (!ptrs) {   // titles are stored back to back in key order: index them once
		ptrs = (const char **)malloc(n * sizeof(*ptrs));
		const char *s = (const char *)title_blob;
		for (uint32_t i = 0; i < n; i++) { ptrs[i] = s; s += strlen(s) + 1; }
	}
	int lo = 0, hi = (int)n - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		if (title_keys[mid] == key) return ptrs[mid];
		if (title_keys[mid] < key) lo = mid + 1; else hi = mid - 1;
	}
	return NULL;
}

#include "libcrypt.inc"

// True when the serial is on the list of LibCrypt-protected PAL discs.
static int is_libcrypt_disc(const char *serial)
{
	if (strlen(serial) != 11) return 0;
	char key[10];
	memcpy(key, serial, 4);
	memcpy(key + 4, serial + 5, 3);
	memcpy(key + 7, serial + 9, 2);
	key[9] = 0;
	int lo = 0, hi = (int)(sizeof(libcrypt_serials) / sizeof(libcrypt_serials[0])) - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		int c = strcmp(key, libcrypt_serials[mid]);
		if (!c) return 1;
		if (c < 0) hi = mid - 1; else lo = mid + 1;
	}
	return 0;
}

#ifdef PRIVATE_SBI
// ---------------------------------------------------------------- private build: built-in SBI collection
#include "private_sbi.inc"

static const PrivSbi *find_private_sbi(const char *serial)
{
	if (strlen(serial) != 11) return NULL;
	char key[10];
	memcpy(key, serial, 4); memcpy(key + 4, serial + 5, 3); memcpy(key + 7, serial + 9, 2); key[9] = 0;
	int lo = 0, hi = (int)(sizeof(private_sbi) / sizeof(private_sbi[0])) - 1;
	while (lo <= hi) {
		int mid = (lo + hi) / 2;
		int c = strcmp(key, private_sbi[mid].serial);
		if (!c) return &private_sbi[mid];
		if (c < 0) hi = mid - 1; else lo = mid + 1;
	}
	return NULL;
}

// Writes the built-in SBI for `serial` to /dev_hdd0/tmp/sbi/ unless one exists. Returns 1 if written.
static int install_private_sbi(const char *serial)
{
	char path[128];
	snprintf(path, sizeof(path), "/dev_hdd0/tmp/sbi/%s.sbi", serial);
	struct stat st;
	if (stat(path, &st) == 0) return 0;
	const PrivSbi *e = find_private_sbi(serial);
	if (!e) return 0;
	mkdir("/dev_hdd0/tmp", 0777);
	mkdir("/dev_hdd0/tmp/sbi", 0777);
	FILE *f = fopen(path, "wb");
	if (!f) return 0;
	fwrite(e->data, 1, e->len, f);
	fclose(f);
	logf("private sbi installed: %s", path);
	return 1;
}

// Adds missing SBIs for games already in PSXISO (named "... [SLES_xxx.xx].ext").
static void install_missing_sbis()
{
	DIR *d = opendir("/dev_hdd0/PSXISO");
	if (!d) return;
	int n = 0;
	struct dirent *de;
	while ((de = readdir(d))) {
		const char *b = strchr(de->d_name, '[');
		while (b) {
			if (strlen(b) >= 13 && b[12] == ']' && b[5] == '_' && b[9] == '.') {
				char serial[12];
				memcpy(serial, b + 1, 11); serial[11] = 0;
				n += install_private_sbi(serial);
			}
			b = strchr(b + 1, '[');
		}
	}
	closedir(d);
	logf("private sbi scan: %d installed", n);
}
#endif

// ---------------------------------------------------------------- subchannel Q / LibCrypt
static uint8_t bcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static void lba_to_msf(uint32_t lba, uint8_t *m)
{
	m[0] = bcd(lba / (75 * 60));
	m[1] = bcd((lba / 75) % 60);
	m[2] = bcd(lba % 75);
}

struct LcEntry { uint32_t lba; uint8_t q[10]; };
static LcEntry g_lc[MAX_LC];
static int g_nlc = 0;
static uint32_t g_qzero = 0, g_qchecked = 0;

static int bcd2(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }

// Many drives return the Q subchannel of a neighbouring sector (typically +1). The offset is
// learned from the first sectors (LibCrypt sectors live far beyond them) and then applied.
#define QCAL_SECTORS 512
#define LC_LBA_MIN 13000
#define LC_LBA_MAX 45000
static int g_qoff = 0, g_qcal_done = 0;
static int g_qhist[5];   // delta -2..+2

static void build_q(uint32_t lba, uint8_t *exp)
{
	exp[0] = 0x41; exp[1] = 0x01; exp[2] = 0x01;
	lba_to_msf(lba, exp + 3);          // relative to track 1 (starts at LBA 0)
	exp[6] = 0;
	lba_to_msf(lba + 150, exp + 7);    // absolute
}

static void check_subq(uint32_t lba, const uint8_t *q)
{
	g_qchecked++;
	int allzero = 1;
	for (int i = 0; i < 10; i++) if (q[i]) { allzero = 0; break; }
	if (allzero) { g_qzero++; return; }

	if (!g_qcal_done) {
		int abs_lba = (bcd2(q[7]) * 60 + bcd2(q[8])) * 75 + bcd2(q[9]) - 150;
		int d = abs_lba - (int)lba;
		if (d >= -2 && d <= 2) g_qhist[d + 2]++;
		if (lba + 1 >= QCAL_SECTORS) {
			int best = 2;
			for (int i = 0; i < 5; i++) if (g_qhist[i] > g_qhist[best]) best = i;
			g_qoff = best - 2;
			g_qcal_done = 1;
			logf("subq offset %+d (hist %d %d %d %d %d)", g_qoff, g_qhist[0], g_qhist[1], g_qhist[2], g_qhist[3], g_qhist[4]);
		}
		return;
	}

	uint32_t owner = lba + g_qoff;     // the sector this Q actually belongs to
	// LibCrypt only alters the MSF of track 1 / index 1 sectors between LBA ~13955 and ~44167
	if (owner < LC_LBA_MIN || owner > LC_LBA_MAX || q[1] != 0x01 || q[2] != 0x01) return;
	uint8_t exp[10];
	build_q(owner, exp);
	if (!memcmp(q, exp, 10)) return;
	if (g_nlc < MAX_LC) {
		g_lc[g_nlc].lba = owner;
		memcpy(g_lc[g_nlc].q, q, 10);
	}
	g_nlc++;
}

// Random subchannel read errors change between reads; LibCrypt modifications don't.
// Re-read every candidate and keep only those that return the same mismatching Q each time.
#define LC_REREADS 4
static void verify_lc_candidates(uint8_t *tmp)
{
	int kept = 0, n = g_nlc < MAX_LC ? g_nlc : MAX_LC;
	for (int i = 0; i < n; i++) {
		uint32_t owner = g_lc[i].lba;
		uint32_t at = owner - g_qoff;      // the sector whose read returns owner's Q
		int stable = 1;
		for (int r = 0; r < LC_REREADS && stable; r++) {
			if (read_cd(at, 1, tmp) || memcmp(tmp + RAW_SIZE, g_lc[i].q, 10)) stable = 0;
		}
		if (stable) g_lc[kept++] = g_lc[i];
	}
	logf("libcrypt verify: %d candidates, %d stable", n, kept);
	// LibCrypt modifies sectors in pairs 5 sectors apart (e.g. 14105/14110); lone hits are disc noise
	int paired = 0;
	for (int i = 0; i < kept; i++) {
		int partner = 0;
		for (int j = 0; j < kept && !partner; j++)
			if (j != i && (g_lc[j].lba == g_lc[i].lba + 5 || g_lc[j].lba + 5 == g_lc[i].lba)) partner = 1;
		if (partner) g_lc[paired++] = g_lc[i];
		else logf("  lone stable mismatch lba=%u dropped", g_lc[i].lba);
	}
	logf("libcrypt pairs: %d sectors", paired);
	g_nlc = paired;
}

static int write_sbi(const char *path)
{
	FILE *f = fopen(path, "wb");
	if (!f) return -1;
	fwrite("SBI\0", 1, 4, f);
	for (int i = 0; i < g_nlc; i++) {
		uint8_t rec[14];
		lba_to_msf(g_lc[i].lba + 150, rec);
		rec[3] = 0x01;                   // full 10-byte Q follows
		memcpy(rec + 4, g_lc[i].q, 10);
		fwrite(rec, 1, 14, f);
	}
	fclose(f);
	return 0;
}

// ---------------------------------------------------------------- rip worker
static volatile uint32_t g_done = 0, g_total = 0;
static volatile int g_abort = 0, g_finished = 0, g_result = 0;
static volatile uint32_t g_readerrs = 0;
static int g_ripped = 0;    // discs ripped this session
static int g_is_pal = 0;    // LibCrypt is only checked on PAL discs
static int g_headless = 0;  // remote test mode: no dialogs, no eject, exit after one rip
#define AUTO_FLAG "/dev_hdd0/tmp/psxrip.auto"
static char g_base[256];      // "/dev_hdd0/PSXISO/NAME [SERIAL]"

static int track_is_audio(uint32_t lba)
{
	for (int t = g_ntracks - 1; t >= 0; t--)
		if (lba >= g_tracks[t].lba) return !(g_tracks[t].ctrl & 0x4);
	return 0;
}

// Double buffering: the reader thread fills one buffer from the drive while the writer
// thread flushes the other to the hard disk, so drive and disk work in parallel.
// MD5 per track, computed by the writer thread (Redump hashes each track separately)
static Md5 g_md5[MAX_TRACKS + 1];
static char g_track_md5[MAX_TRACKS + 1][33];

static uint8_t *g_bufs[2];
static volatile uint32_t g_buf_len[2];   // sectors in buffer, 0 = empty
static volatile uint32_t g_buf_end[2];   // lba after the last sector in buffer
static volatile int g_reader_done = 0, g_write_failed = 0;
static FILE *g_out;

static void writer_thread(void *arg)
{
	for (int k = 0;; k ^= 1) {
		while (!g_buf_len[k] && !(g_reader_done && !g_buf_len[k ^ 1] && !g_buf_len[k])) usleep(500);
		if (!g_buf_len[k]) break;   // reader done and nothing left
		__sync_synchronize();
		uint32_t n = g_buf_len[k];
		if (fwrite(g_bufs[k], RAW_SIZE, n, g_out) != n) g_write_failed = 1;
		// feed each sector range to the MD5 of the track it belongs to
		uint32_t first = g_buf_end[k] - n;
		for (uint32_t i = 0; i < n;) {
			uint32_t lba = first + i;
			int t = g_ntracks - 1;
			while (t > 0 && lba < g_tracks[t].lba) t--;
			uint32_t end = (t + 1 < g_ntracks) ? g_tracks[t + 1].lba : g_leadout;
			uint32_t cnt = end - lba;
			if (cnt > n - i) cnt = n - i;
			md5_update(&g_md5[t], g_bufs[k] + i * RAW_SIZE, cnt * RAW_SIZE);
			i += cnt;
		}
		g_done = g_buf_end[k];
		g_buf_len[k] = 0;
	}
	sysThreadExit(0);
}

static void rip_thread(void *arg)
{
	char path[300];
	snprintf(path, sizeof(path), "%s.bin.part", g_base);   // renamed to .bin when complete
	g_out = fopen(path, "wb");
	if (!g_out) { logf("cannot create %s", path); g_result = -1; g_finished = 1; sysThreadExit(0); }
	setvbuf(g_out, NULL, _IONBF, 0);   // we always write whole 2.4 MB blocks

	g_bufs[0] = (uint8_t *)memalign(128, CHUNK * SECT_Q);
	g_bufs[1] = (uint8_t *)memalign(128, CHUNK * SECT_Q);
	g_buf_len[0] = g_buf_len[1] = 0;
	for (int t = 0; t < g_ntracks; t++) md5_init(&g_md5[t]);
	g_reader_done = 0; g_write_failed = 0;
	uint32_t data_end = (g_ntracks > 1) ? g_tracks[1].lba : g_leadout;  // LibCrypt lives in track 1

	sys_ppu_thread_t wtid;
	sysThreadCreate(&wtid, writer_thread, NULL, 1000, 0x10000, THREAD_JOINABLE, (char *)"psxwrite");

	struct stat st;
	int single = stat("/dev_hdd0/tmp/psxrip_single", &st) == 0;   // test switch: disable overlap
	logf("buffering: %s", single ? "single" : "double");
	time_t t0 = time(NULL);
	int k = 0;
	for (uint32_t lba = 0; lba < g_leadout && !g_abort && !g_write_failed; k ^= 1) {
		while (g_buf_len[k] && !g_write_failed) usleep(500);   // wait until the writer has emptied it
		uint8_t *buf = g_bufs[k];
		uint32_t n = CHUNK;
		if (lba + n > g_leadout) n = g_leadout - lba;

		int ret = -1;
		for (int tries = 0; tries < g_retries && ret; tries++) ret = read_cd(lba, n, buf);
		if (ret) {
			// fall back to single sectors so one bad sector doesn't zero the whole chunk
			for (uint32_t i = 0; i < n; i++) {
				uint8_t *sp = buf + i * SECT_Q;
				int r = -1;
				for (int tries = 0; tries < g_retries && r; tries++) r = read_cd(lba + i, 1, sp);
				if (r) { memset(sp, 0, SECT_Q); g_readerrs++; logf("unreadable sector %u (0x%x)", lba + i, r); }
			}
		}
		// check Q first, then pack the 2352-byte sectors in place for a single write
		for (uint32_t i = 0; i < n; i++)
			if (g_is_pal && lba + i < data_end && !track_is_audio(lba + i)) check_subq(lba + i, buf + i * SECT_Q + RAW_SIZE);
		for (uint32_t i = 1; i < n; i++) memmove(buf + i * RAW_SIZE, buf + i * SECT_Q, RAW_SIZE);
		lba += n;
		g_buf_end[k] = lba;
		__sync_synchronize();
		g_buf_len[k] = n;   // hand over to the writer
		if (single) while (g_buf_len[k] && !g_write_failed) usleep(500);
	}
	g_reader_done = 1;
	u64 rv;
	sysThreadJoin(wtid, &rv);
	fclose(g_out);
	for (int t = 0; t < g_ntracks; t++) {
		uint8_t d[16];
		md5_final(&g_md5[t], d);
		md5_hex(d, g_track_md5[t]);
		logf("track %d md5 %s", g_tracks[t].num, g_track_md5[t]);
	}
	free(g_bufs[0]);
	free(g_bufs[1]);
	time_t secs = time(NULL) - t0;
	logf("rip finished in %ld s (%u KB/s), write_failed=%d", (long)secs,
	     secs ? (unsigned)((uint64_t)g_done * RAW_SIZE / 1024 / secs) : 0, g_write_failed);
	g_result = g_abort ? 1 : (g_write_failed ? -1 : 0);
	g_finished = 1;
	sysThreadExit(0);
}

static void write_cue(const char *binname)
{
	char path[300];
	snprintf(path, sizeof(path), "%s.cue", g_base);
	FILE *f = fopen(path, "wb");
	if (!f) return;
	fprintf(f, "FILE \"%s\" BINARY\r\n", binname);
	for (int t = 0; t < g_ntracks; t++) {
		int audio = !(g_tracks[t].ctrl & 0x4);
		fprintf(f, "  TRACK %02d %s\r\n", g_tracks[t].num, audio ? "AUDIO" : "MODE2/2352");
		uint32_t l = g_tracks[t].lba;
		if (t > 0 && audio && l >= 150 && (g_tracks[t - 1].ctrl & 0x4)) {
			uint32_t p = l - 150;   // 2 s pregap after the data track
			fprintf(f, "    INDEX 00 %02u:%02u:%02u\r\n", p / 4500, (p / 75) % 60, p % 75);
		}
		fprintf(f, "    INDEX 01 %02u:%02u:%02u\r\n", l / 4500, (l / 75) % 60, l % 75);
	}
	fclose(f);
}

static void sanitize(char *s)
{
	// "Gekido: Urban Fighters" -> "Gekido - Urban Fighters"
	char tmp[256];
	size_t o = 0;
	for (const char *p = s; *p && o < sizeof(tmp) - 4; p++) {
		if (*p == ':') {
			while (o && tmp[o - 1] == ' ') o--;
			tmp[o++] = ' '; tmp[o++] = '-';
			if (p[1] != ' ') tmp[o++] = ' ';
		} else {
			tmp[o++] = *p;
		}
	}
	tmp[o] = 0;
	strcpy(s, tmp);
	for (; *s; s++)
		if (*s == '/' || *s == '\\' || *s == '"' || *s == '*' || *s == '?' || *s == '<' || *s == '>' || *s == '|')
			*s = '_';
}

// Sends a web command to webMAN on this console (no internet involved). Returns 0 if webMAN answered.
static int webman_get(const char *path)
{
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) return -1;
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(80);
	sa.sin_addr.s_addr = htonl(0x7F000001);
	int ret = -1;
	if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
		char req[256];
		int n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n", path);
		send(sock, req, n, 0);
		char tmp[256];
		while (recv(sock, tmp, sizeof(tmp), 0) > 0) {}
		ret = 0;
	}
	close(sock);
	logf("webMAN %s -> %s", path, ret ? "not reachable" : "ok");
	return ret;
}

// Unmounts any mounted disc image so the real disc is read.
static void webman_unmount()
{
	if (webman_get("/mount.ps3/unmount") == 0)
		usleep(1500 * 1000);   // let Cobra hand the drive back to the real disc
}

// ---------------------------------------------------------------- lv1 storage patch
static u64 lv1_peek(u64 addr)
{
	lv2syscall3(8, 0x7777, 0x1008, addr);   // PS3MAPI lv1 peek
	return_to_user_prog(u64);
}

static void lv1_poke(u64 addr, u64 value)
{
	lv2syscall4(8, 0x7777, 0x1009, addr, value);   // PS3MAPI lv1 poke
}

// lv1 storage-manager patch from IRISMAN (payload_480.c). The lv1 code at these offsets is
// unchanged on 4.8x-4.93 (verified by read-back below); it only lives in RAM and is restored
// after ripping, a reboot also undoes it.
#define LV1_BAK "/dev_hdd0/tmp/psxrip_lv1.bak"
static const u64 lv1_off[4]   = {0x16fa60, 0x16fa84, 0x16fafc, 0x16fb04};
static const u64 lv1_orig[3]  = {0x7f83e378f8010098ULL, 0x7f85e3784bfff0e5ULL, 0x7f84e37838a10070ULL};
static const u64 lv1_patch[4] = {0x7f83e37860000000ULL, 0x7f85e37838600001ULL, 0x7f84e3783be00001ULL, 0x9be1007038600000ULL};
static u64 lv1_saved[4];
static int lv1_patched = 0;

static int lv1_is_orig(const u64 *v)
{
	return v[0] == lv1_orig[0] && v[1] == lv1_orig[1] && v[2] == lv1_orig[2] &&
	       (v[3] >> 32) == 0x9be10070ULL && (v[3] & 0xFC000003ULL) == 0x48000001ULL;  // stb + bl
}

static int lv1_storage_patch()
{
	u64 v[4];
	for (int i = 0; i < 4; i++) v[i] = lv1_peek(lv1_off[i]);
	logf("lv1 now: %016llx %016llx %016llx %016llx", (unsigned long long)v[0], (unsigned long long)v[1],
	     (unsigned long long)v[2], (unsigned long long)v[3]);

	if (!memcmp(v, lv1_patch, sizeof(v))) {
		// left patched by an earlier run that didn't exit cleanly: recover originals from backup
		FILE *f = fopen(LV1_BAK, "rb");
		if (!f || fread(lv1_saved, sizeof(lv1_saved), 1, f) != 1 || !lv1_is_orig(lv1_saved)) {
			if (f) fclose(f);
			logf("lv1 already patched, no valid backup; leaving as is until reboot");
			return 0;
		}
		fclose(f);
		lv1_patched = 1;
		logf("lv1 already patched, originals restored from backup");
		return 0;
	}
	if (!lv1_is_orig(v)) {
		logf("lv1 code does not match the expected layout, NOT patching");
		return -1;
	}
	memcpy(lv1_saved, v, sizeof(v));
	FILE *f = fopen(LV1_BAK, "wb");
	if (f) { fwrite(lv1_saved, sizeof(lv1_saved), 1, f); fclose(f); }

	for (int i = 0; i < 4; i++) lv1_poke(lv1_off[i], lv1_patch[i]);
	for (int i = 0; i < 4; i++) {
		u64 r = lv1_peek(lv1_off[i]);
		if (r != lv1_patch[i]) {
			logf("lv1 poke %llx failed (read %016llx), restoring", (unsigned long long)lv1_off[i], (unsigned long long)r);
			for (int k = 0; k < 4; k++) lv1_poke(lv1_off[k], lv1_saved[k]);
			return -2;
		}
	}
	lv1_patched = 1;
	logf("lv1 storage patch applied");
	return 0;
}

static void lv1_storage_unpatch()
{
	if (!lv1_patched) return;
	for (int i = 0; i < 4; i++) lv1_poke(lv1_off[i], lv1_saved[i]);
	int ok = 1;
	for (int i = 0; i < 4; i++) if (lv1_peek(lv1_off[i]) != lv1_saved[i]) ok = 0;
	lv1_patched = 0;
	if (ok) unlink(LV1_BAK);
	logf("lv1 storage patch removed (%s)", ok ? "verified" : "VERIFY FAILED");
}

// ---------------------------------------------------------------- settings UI
// Human-readable name of a destination directory.
static void target_name(const char *dir, char *out, size_t n)
{
	int port;
	if (sscanf(dir, "/dev_usb%3d", &port) == 1) snprintf(out, n, T(STR_TARGET_USB), port, dir);
	else snprintf(out, n, T(STR_TARGET_HDD), dir);
}

// Next available destination after the current one: hdd0, then each connected USB device.
static void next_target()
{
	char cands[9][32];
	int nc = 0;
	snprintf(cands[nc++], 32, "/dev_hdd0/PSXISO");
	for (int i = 0; i < 8; i++) {
		char dev[16];
		snprintf(dev, sizeof(dev), "/dev_usb%03d", i);
		struct stat st;
		if (stat(dev, &st) == 0) snprintf(cands[nc++], 32, "%s/PSXISO", dev);
	}
	int cur = 0;
	for (int i = 0; i < nc; i++) if (!strcmp(cands[i], g_outdir)) cur = i;
	snprintf(g_outdir, sizeof(g_outdir), "%s", cands[(cur + 1) % nc]);
}

// ---------------------------------------------------------------- disc identification for the home screen
static char g_name[128];   // title (or volume id) of the disc in the drive
static int prepare_disc(int *err, int *code);
enum { DISC_NONE, DISC_READING, DISC_READY, DISC_ERROR };
static volatile int g_disc_state = DISC_NONE;
static volatile int g_ident_busy = 0;
static char g_disc_title[128], g_disc_serial[16];
static uint32_t g_disc_mb;

static void identify_thread(void *arg)
{
	if ((long)arg) usleep(2000 * 1000);   // freshly inserted: let the drive spin up
	int err = 0, code = 0;
	int r = prepare_disc(&err, &code);
	if (r == 0) sys_storage_close(g_fd);
	// Undo the lv1 patch straight away: if the app is then killed (webMAN exit, XMB quit) the
	// hypervisor would stay patched and the next system storage operation (pkg install) hangs.
	lv1_storage_unpatch();
	if (r == 0) {
		snprintf(g_disc_title, sizeof(g_disc_title), "%s", g_name);
		snprintf(g_disc_serial, sizeof(g_disc_serial), "%s", g_serial[0] ? g_serial : T(STR_UNKNOWN));
		g_disc_mb = (uint32_t)((uint64_t)g_leadout * RAW_SIZE / (1024 * 1024));
		__sync_synchronize();
		g_disc_state = DISC_READY;
	} else {
		g_disc_state = (r == -2) ? DISC_NONE : DISC_ERROR;
	}
	logf("identify -> %d (state %d)", r, g_disc_state);
	g_ident_busy = 0;
	sysThreadExit(0);
}

static void identify_start(int fresh)
{
	if (g_ident_busy) return;
	g_ident_busy = 1;
	g_disc_state = DISC_READING;
	sys_ppu_thread_t t;
	sysThreadCreate(&t, identify_thread, (void *)(long)fresh, 1000, 0x10000, 0, (char *)"psxident");
}

// ---------------------------------------------------------------- own screens (home, settings)
enum { B_CROSS = 1, B_CIRCLE = 2, B_TRIANGLE = 4, B_UP = 8, B_DOWN = 16, B_LEFT = 32, B_RIGHT = 64, B_START = 128, B_SQUARE = 256 };

// Buttons newly pressed since the last call, over all connected pads. Each pad keeps its own
// last state: ioPadGetData only reports changes, so a quiet pad must not resurrect another pad's buttons.
static int pad_pressed()
{
	static int pad_state[7];
	static int last = 0, held = 0;
	padInfo info;
	int now = 0;
	if (ioPadGetInfo(&info) == 0) {
		for (int i = 0; i < 7; i++) {
			if (!info.status[i]) { pad_state[i] = 0; continue; }
			padData pd;
			if (ioPadGetData(i, &pd) == 0 && pd.len > 0) {
				int b = 0;
				if (pd.BTN_CROSS) b |= B_CROSS;
				if (pd.BTN_CIRCLE) b |= B_CIRCLE;
				if (pd.BTN_TRIANGLE) b |= B_TRIANGLE;
				if (pd.BTN_UP) b |= B_UP;
				if (pd.BTN_DOWN) b |= B_DOWN;
				if (pd.BTN_LEFT) b |= B_LEFT;
				if (pd.BTN_RIGHT) b |= B_RIGHT;
				if (pd.BTN_START) b |= B_START;
				if (pd.BTN_SQUARE) b |= B_SQUARE;
				pad_state[i] = b;
			}
			now |= pad_state[i];
		}
	}
	int pressed = now & ~last;
	// auto-repeat a single held arrow: first after ~0.4 s, then every ~0.1 s (called once per frame)
	const int arrows = B_UP | B_DOWN | B_LEFT | B_RIGHT;
	int a = now & arrows;
	if (a && (a & (a - 1)) == 0 && a == (last & arrows)) {
		if (++held >= 24 && (held - 24) % 6 == 0) pressed |= a;
	} else {
		held = 0;
	}
	last = now;
	return pressed;
}

static void hint_bar(const char *const *buttons, const char *const *labels, int n)
{
	int total = 0, gap = ui_w() / 24;
	for (int i = 0; i < n; i++) total += ui_hint_width(buttons[i], labels[i]) + (i ? gap : 0);
	int x = (ui_w() - total) / 2, y = ui_h() - ui_h() / 8;
	for (int i = 0; i < n; i++) x += ui_hint(x, y, buttons[i], labels[i]) + gap;
}

static void flip_frame()
{
	sysUtilCheckCallback();
	present();
}

static void settings_screen()
{
	// edit locally; each change is applied and saved at once, O goes back
	char outdir[64];
	snprintf(outdir, sizeof(outdir), "%s", g_outdir);
	int retries = g_retries, speed = g_speed, eject = g_eject, autostart = g_autostart, row = 0;
	const int NROWS = 5;
	static const int steps[] = {1, 2, 4, 8, 16, 32};
	pad_pressed();
	g_redraw = 2;
	while (!exit_requested) {
		if (g_redraw > 0) {
		g_redraw = 0;
		g_drawn = 2;
		draw_backdrop();
		ui_text(UI_BODY, ui_w() / 2, ui_h() / 10 + ui_line(UI_TITLE) + ui_line(UI_BODY) / 2, 0xA8B0C8, T(STR_SETTINGS_TITLE), UI_CENTER);
		char vals[5][96];
		char save_dir[64]; snprintf(save_dir, sizeof(save_dir), "%s", g_outdir);
		snprintf(g_outdir, sizeof(g_outdir), "%s", outdir);
		target_name(outdir, vals[0], sizeof(vals[0]));
		snprintf(g_outdir, sizeof(g_outdir), "%s", save_dir);
		snprintf(vals[1], sizeof(vals[1]), "%d", retries);
		if (speed) snprintf(vals[2], sizeof(vals[2]), "%d\xC3\x97", speed_x[speed]); else snprintf(vals[2], sizeof(vals[2]), "%s", T(STR_SPEED_MAX));
		snprintf(vals[3], sizeof(vals[3]), "%s", eject ? T(STR_ON) : T(STR_OFF));
		snprintf(vals[4], sizeof(vals[4]), "%s", autostart ? T(STR_ON) : T(STR_OFF));
		const char *labels[5] = {T(STR_L_TARGET), T(STR_L_RETRIES), T(STR_L_SPEED), T(STR_L_EJECT), T(STR_L_AUTOSTART)};
		int top = ui_h() / 3, lh = ui_line(UI_BODY) * 2, left = ui_w() / 6, right = ui_w() * 5 / 6;
		for (int r = 0; r < NROWS; r++) {
			int y = top + r * lh;
			if (r == row) ui_rect(left - ui_w() / 60, y - lh / 4, right - left + ui_w() / 30, lh * 3 / 4 + lh / 4, 0x703D6FD9);
			ui_text(UI_BODY, left, y, 0xFFFFFF, labels[r]);
			char v[128];
			snprintf(v, sizeof(v), r == row ? "‹ %s ›" : "%s", vals[r]);
			ui_text(UI_BODY, right, y, r == row ? 0xFFFFFF : 0xC8D0E0, v, UI_RIGHT);
		}
		const char *b[] = {"updown", "leftright", "circle"};
		const char *l[] = {T(STR_HINT_SELECT), T(STR_HINT_CHANGE), T(STR_HINT_BACK)};
		hint_bar(b, l, 3);
		}
		flip_frame();
		int p = pad_pressed();
		if (p) g_redraw = 2;
		if (p & B_UP) row = (row + NROWS - 1) % NROWS;
		if (p & B_DOWN) row = (row + 1) % NROWS;
		int d = (p & B_RIGHT) ? 1 : (p & B_LEFT) ? -1 : 0;
		if (d) {
			if (row == 0) {
				char save_dir2[64]; snprintf(save_dir2, sizeof(save_dir2), "%s", g_outdir);
				snprintf(g_outdir, sizeof(g_outdir), "%s", outdir);
				next_target();                                    // only forward through the list
				snprintf(outdir, sizeof(outdir), "%s", g_outdir);
				snprintf(g_outdir, sizeof(g_outdir), "%s", save_dir2);
			} else if (row == 1) {
				int k = 0; while (k < 6 && steps[k] != retries) k++;
				retries = steps[(k + (d > 0 ? 1 : 5)) % 6];
			} else if (row == 2) {
				speed = (speed + (d > 0 ? 1 : 3)) % 4;
			} else if (row == 3) {
				eject = !eject;
			} else {
				autostart = !autostart;
			}
			// every change is applied and saved immediately
			snprintf(g_outdir, sizeof(g_outdir), "%s", outdir);
			g_retries = retries; g_speed = speed; g_eject = eject; g_autostart = autostart;
			OUT_DIR = g_outdir;
			cfg_save();
			logf("settings saved: outdir=%s retries=%d speed=%d eject=%d autostart=%d", g_outdir, g_retries, g_speed, g_eject, g_autostart);
		}
		if (p & B_CIRCLE) return;
	}
}


// Ejects the disc (START STOP UNIT with LoEj).
static void eject_disc()
{
	while (g_ident_busy) usleep(10000);
	if (sys_storage_open(BD_DEVICE, &g_fd)) return;
	uint8_t cdb[12] = {0x1B, 0, 0, 0, 0x02, 0, 0, 0, 0, 0, 0, 0};
	logf("eject (home) -> 0x%x", atapi(cdb, NULL, 0, ATAPI_NON_DATA_PROTO));
	sys_storage_close(g_fd);
	g_disc_state = DISC_NONE;
}

// Home screen. Returns 1 = rip the disc in the drive, 2 = rip a freshly inserted disc, 0 = exit.
static int home_screen()
{
	disc_inserted = 0;
	g_redraw = 2;
	pad_pressed();   // swallow buttons still held from a previous screen
	if (g_disc_state != DISC_READY && !g_ident_busy) identify_start(0);
	int last_state = -1, inserted = 0;
	while (!exit_requested) {
		if (disc_inserted) { disc_inserted = 0; inserted = 1; while (g_ident_busy) usleep(10000); identify_start(1); }
		if (!disc_present) { if (g_disc_state != DISC_NONE && !g_ident_busy) g_disc_state = DISC_NONE; inserted = 0; }
		int st = g_disc_state;
		if (st != last_state) { last_state = st; g_redraw = 2; }
		if (st == DISC_READY && inserted && g_autostart) return 2;
		char tname[96], saving[160];
		target_name(g_outdir, tname, sizeof(tname));
		snprintf(saving, sizeof(saving), T(STR_HOME_SAVING), tname);
		g_backdrop_shown = 0;   // a dialog opened later must redraw its backdrop
		if (g_redraw > 0) {
			g_redraw = 0;
			g_drawn = 2;
			draw_backdrop();
			int y = ui_h() * 2 / 5;
			if (st == DISC_READY) {
				char info[96];
				snprintf(info, sizeof(info), "%s  ·  %u MB", g_disc_serial, g_disc_mb);
				ui_text(UI_TITLE, ui_w() / 2, y - ui_line(UI_BODY), 0xFFFFFF, g_disc_title, UI_CENTER);
				ui_text(UI_BODY, ui_w() / 2, y + ui_line(UI_TITLE) - ui_line(UI_BODY) / 2, 0xA8B0C8, info, UI_CENTER);
			} else {
				const char *msg = st == DISC_READING ? T(STR_HOME_READING) : st == DISC_ERROR ? T(STR_HOME_NOTPSX) : T(STR_HOME_INSERT);
				ui_text(UI_BODY, ui_w() / 2, y, 0xFFFFFF, msg, UI_CENTER);
			}
			ui_text(UI_SMALL, ui_w() / 2, y + ui_line(UI_TITLE) + ui_line(UI_BODY) * 2, 0x8890A8, saving, UI_CENTER);
			if (st == DISC_READY) {
				const char *b[] = {"cross", "triangle", "square", "circle"};
				const char *l[] = {T(STR_HINT_RIP), T(STR_HINT_EJECT), T(STR_HINT_SETTINGS), T(STR_HINT_EXIT)};
				hint_bar(b, l, 4);
			} else if (st == DISC_ERROR) {
				const char *b[] = {"triangle", "square", "circle"};
				const char *l[] = {T(STR_HINT_EJECT), T(STR_HINT_SETTINGS), T(STR_HINT_EXIT)};
				hint_bar(b, l, 3);
			} else {
				const char *b[] = {"square", "circle"};
				const char *l[] = {T(STR_HINT_SETTINGS), T(STR_HINT_EXIT)};
				hint_bar(b, l, 2);
			}
		}
		flip_frame();
		int p = pad_pressed();
		if ((p & B_CROSS) && st == DISC_READY) return inserted ? 2 : 1;
		if ((p & B_TRIANGLE) && (st == DISC_READY || st == DISC_ERROR)) { eject_disc(); g_redraw = 2; }
		if (p & (B_SQUARE | B_START)) { settings_screen(); g_redraw = 2; }
		if (p & B_CIRCLE) return 0;
	}
	return 0;
}

// ---------------------------------------------------------------- one disc
static char g_done_msg[512];   // summary of the last successful rip

// Returns 1 when the disc was ripped successfully. `autorip` skips the confirmation.
// Opens the drive, unlocks raw reads, reads the TOC and identifies the disc (g_serial, g_name, g_base).
// Leaves g_fd open on success (returns 0). On failure g_fd is closed and *err/*code describe why;
// -2 means no readable disc.
static int prepare_disc(int *err, int *code)
{
	char *name = g_name;
	g_ntracks = 0; g_leadout = 0; g_nlc = 0; g_qzero = 0; g_qchecked = 0; g_readerrs = 0;
	g_qoff = 0; g_qcal_done = 0; memset(g_qhist, 0, sizeof(g_qhist));
	if (!g_mounttest) webman_unmount();

	int ret = sys_storage_open(BD_DEVICE, &g_fd);
	logf("storage_open -> 0x%x fd=%d", ret, g_fd);
	if (ret) { *err = STR_OPEN_FAILED; *code = ret; return -1; }

	ret = read_toc();
	if (ret) { sys_storage_close(g_fd); *err = STR_TOC_FAILED; *code = ret; return -2; }   // -2: no (readable) disc

	ret = probe_read();
	if (ret && lv1_storage_patch() == 0) {
		bd_auth();
		ret = probe_read();
	}
	if (ret) { sys_storage_close(g_fd); *err = STR_READCD_FAILED; *code = ret; return -1; }

	// the TOC read before authentication can be incomplete (seen on multi-track discs): read it again
	logf("re-reading TOC after auth");
	if (read_toc()) { sys_storage_close(g_fd); *err = STR_TOC_FAILED; *code = -3; return -1; }

	struct stat dst;
	if (stat("/dev_hdd0/tmp/psxrip.diag", &dst) == 0) {
		subq_diag();
		sys_storage_close(g_fd);
		*err = STR_UNKNOWN; *code = 0;
		return -1;
	}

	detect_disc_id();
	const char *title = g_serial[0] ? lookup_title(g_serial) : NULL;
	snprintf(name, sizeof(g_name), "%s", title ? title : (g_volid[0] ? g_volid : "PSX DISC"));
	logf("title=%s", title ? title : "(not in database)");
	sanitize(name);
	if (g_serial[0])
		snprintf(g_base, sizeof(g_base), "%s/%s [%s]", OUT_DIR, name, g_serial);
	else
		snprintf(g_base, sizeof(g_base), "%s/%s %lu", OUT_DIR, name, (unsigned long)time(NULL));

	g_is_pal = g_serial[0] && g_serial[2] == 'E';   // SLES/SCES/SLED/SCED
	return 0;
}

static int rip_disc_inner(int fresh, int autorip)
{
	char msg[512];
	int err = 0, code = 0;
	if (prepare_disc(&err, &code)) {
		if (err == STR_OPEN_FAILED) snprintf(msg, sizeof(msg), T(STR_OPEN_FAILED), code);
		else if (err == STR_UNKNOWN) return 0;   // diag run
		else snprintf(msg, sizeof(msg), T((StrId)err), code, LOG_PATH);
		tell(msg);
		return 0;
	}
	const char *name = g_name;
	uint32_t mb = (uint32_t)((uint64_t)g_leadout * RAW_SIZE / (1024 * 1024));
	snprintf(msg, sizeof(msg), T(STR_FOUND),
	         name, g_serial[0] ? g_serial : T(STR_UNKNOWN), g_ntracks, mb, OUT_DIR);
	if (!autorip && !ask_yesno(msg)) { sys_storage_close(g_fd); return 0; }

	mkdir(OUT_DIR, 0777);
	g_done = 0; g_total = g_leadout; g_abort = 0; g_finished = 0;

	{
		// SET CD SPEED: maximum read speed
		uint16_t kbs = g_speed ? (uint16_t)(176 * speed_x[g_speed]) : 0xFFFF;
		uint8_t cdb[12] = {0xBB, 0, (uint8_t)(kbs >> 8), (uint8_t)kbs, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0};
		logf("set cd speed %u KB/s -> 0x%x", kbs, atapi(cdb, NULL, 0, ATAPI_NON_DATA_PROTO));
	}
	sys_ppu_thread_t tid;
	sysThreadCreate(&tid, rip_thread, NULL, 1000, 0x10000, THREAD_JOINABLE, (char *)"psxrip");

	dialog_action = 0;
	char ripmsg[256];
	snprintf(ripmsg, sizeof(ripmsg), T(STR_RIPPING), name, g_serial[0] ? g_serial : T(STR_UNKNOWN));
	msgDialogOpen2((msgType)(MSG_DIALOG_SINGLE_PROGRESSBAR | MSG_DIALOG_MUTE_ON), ripmsg, dialog_handler, NULL, NULL);
	uint32_t shown = 0;
	while (!g_finished) {
		if (dialog_action == 2 || exit_requested) g_abort = 1;
		uint32_t pct = (uint32_t)((uint64_t)g_done * 100 / (g_total ? g_total : 1));
		if (pct > shown) {
			msgDialogProgressBarInc(MSG_PROGRESSBAR_INDEX0, pct - shown);
			shown = pct;
			char pm[96];
			snprintf(pm, sizeof(pm), T(STR_PROGRESS), g_done, g_total);
			msgDialogProgressBarSetMsg(MSG_PROGRESSBAR_INDEX0, pm);
		}
		do_flip();
	}
	u64 rv;
	sysThreadJoin(tid, &rv);
	msgDialogAbort();
	int is_pal = g_is_pal;
	if (!g_result && is_pal && g_qcal_done && g_nlc > 0 && g_nlc <= MAX_LC) {
		uint8_t *tmp = (uint8_t *)memalign(128, 4096);
		verify_lc_candidates(tmp);
		free(tmp);
	}
	if (!g_result && !g_headless && g_eject) {
		// eject so the next disc can go straight in (START STOP UNIT, LoEj)
		uint8_t cdb[12] = {0x1B, 0, 0, 0, 0x02, 0, 0, 0, 0, 0, 0, 0};
		logf("eject -> 0x%x", atapi(cdb, NULL, 0, ATAPI_NON_DATA_PROTO));
	}
	sys_storage_close(g_fd);

	if (g_result) {
		snprintf(msg, sizeof(msg), "%s.bin.part", g_base);
		unlink(msg);
		tell(g_result == 1 ? T(STR_ABORTED) : T(STR_CREATE_FAILED));
		return 0;
	}

	{
		char part[300], bin[300];
		snprintf(part, sizeof(part), "%s.bin.part", g_base);
		snprintf(bin, sizeof(bin), "%s.bin", g_base);
		unlink(bin);
		if (rename(part, bin)) logf("rename %s failed", part);
	}
	const char *slash = strrchr(g_base, '/');
	char binname[256];
	snprintf(binname, sizeof(binname), "%s.bin", slash + 1);
	write_cue(binname);
	{
		char mpath[300];
		snprintf(mpath, sizeof(mpath), "%s.md5", g_base);
		FILE *mf = fopen(mpath, "w");
		if (mf) {
			for (int t = 0; t < g_ntracks; t++) fprintf(mf, "%s  track %02d\n", g_track_md5[t], g_tracks[t].num);
			fclose(mf);
		}
	}

	// Only discs on the known LibCrypt list get an SBI and a LibCrypt line in the dialog.
	// Other PAL discs are still checked, but a hit is only logged as a hint.
	char lcinfo[200] = "";
	int known_lc = g_serial[0] && is_libcrypt_disc(g_serial);
	if (is_pal) logf("subq: checked=%u zero=%u mismatches=%d known_libcrypt=%d", g_qchecked, g_qzero, g_nlc, known_lc);
	if (!known_lc) {
		if (is_pal && g_nlc > 0 && g_nlc <= 64)
			logf("hint: %d paired Q mismatches on a PAL disc not in the LibCrypt list (no SBI written)", g_nlc);
	} else if (g_qchecked == 0 || g_qzero > g_qchecked / 2 || g_nlc > 64 || !g_qcal_done) {
		snprintf(lcinfo, sizeof(lcinfo), "%s", T(STR_LC_UNCHECKED));
		logf("subq unreliable (%d mismatches), no SBI written", g_nlc);
	} else if (g_nlc == 0) {
		snprintf(lcinfo, sizeof(lcinfo), "%s", T(STR_LC_MISSING));
		logf("known LibCrypt disc but no protected sectors found");
#ifdef PRIVATE_SBI
		if (install_private_sbi(g_serial)) snprintf(lcinfo, sizeof(lcinfo), "%s", T(STR_LC_BUILTIN));
#endif
	} else {
		char p[300];
		mkdir("/dev_hdd0/tmp", 0777);
		mkdir(SBI_DIR, 0777);
		snprintf(p, sizeof(p), "%s/%s.sbi", SBI_DIR, g_serial);
		write_sbi(p);
		if (!g_mounttest) {
			snprintf(p, sizeof(p), "%s.sbi", g_base);
			write_sbi(p);
		}
		snprintf(p, sizeof(p), "%s/%s.sbi", SBI_DIR, g_serial);
		snprintf(lcinfo, sizeof(lcinfo), T(STR_LC_FOUND), g_nlc, p);
	}
	for (int i = 0; i < g_nlc && i < MAX_LC && i < 40; i++)
		logf("  Q mismatch lba=%u q=%02x %02x %02x %02x%02x%02x %02x %02x%02x%02x", g_lc[i].lba,
		     g_lc[i].q[0], g_lc[i].q[1], g_lc[i].q[2], g_lc[i].q[3], g_lc[i].q[4], g_lc[i].q[5],
		     g_lc[i].q[6], g_lc[i].q[7], g_lc[i].q[8], g_lc[i].q[9]);

	g_ripped++;
	char lcline[208] = "";
	if (lcinfo[0]) snprintf(lcline, sizeof(lcline), "\n%s", lcinfo);
	snprintf(msg, sizeof(msg), T(STR_DONE),
	         g_base, lcline, g_readerrs);
	logf("done: %s errs=%u", g_base, g_readerrs);
	snprintf(g_done_msg, sizeof(g_done_msg), "%s", msg);
	return 1;
}

static int rip_disc(int fresh, int autorip)
{
	while (g_ident_busy) usleep(10000);   // the identify thread patches/unpatches lv1 itself
	int ok = rip_disc_inner(fresh, autorip);
	lv1_storage_unpatch();
	return ok;
}

int main(int argc, char *argv[])
{
	void *host_addr = memalign(1024 * 1024, HOST_SIZE);
	init_screen(host_addr, HOST_SIZE);
	ui_init();
	ioPadInit(7);
	atexit(program_exit_callback);
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_callback, NULL);
	g_cmdbuf = (uint8_t *)memalign(128, 4096);
	g_sec = (uint8_t *)memalign(128, 4096);
	lang_init();
	netInitialize();
	cfg_load();
	OUT_DIR = g_outdir;
	{ struct stat gs; ui_guides = stat("/dev_hdd0/tmp/psxrip.guides", &gs) == 0; }
#ifdef PRIVATE_SBI
	install_missing_sbis();
#endif

	logf("---- PSX Ripper start ----");
	logf("video %ux%u pitch=%u", display_width, display_height, color_pitch);
	struct stat ast;
	if (stat(AUTO_FLAG, &ast) == 0) {
		// remote test mode (used for development via webMAN /play.ps3): rip the disc in the drive once
		g_headless = g_headless_flag = 1;
		logf("headless mode");
		if (stat("/dev_hdd0/tmp/psxrip.mounttest", &ast) == 0) {
			g_mounttest = 1;
			OUT_DIR = SBI_DIR = "/dev_hdd0/tmp/psxrip_test";
			mkdir(OUT_DIR, 0777);
			logf("mount test: keeping the mounted image, output to %s", OUT_DIR);
		}
		int ok = rip_disc(0, 1);
		logf("headless result=%d", ok);
		if (ok) logf("[dialog] %s", g_done_msg);
		lv1_storage_unpatch();
		logf("---- PSX Ripper exit ----");
		return 0;
	}
	sysDiscRegisterDiscChangeCallback(disc_eject_cb, disc_insert_cb);
	int autorip = 0;   // set after a successful rip: the next inserted disc is ripped without asking
	while (!exit_requested) {
		int fresh;
		if (autorip) {
			fresh = 1;
		} else {
			int a = home_screen();
			if (exit_requested || a == 0) break;
			fresh = (a == 2);
		}
		logf("start rip (%s%s)", fresh ? "disc inserted" : "disc already in drive", autorip ? ", auto" : "");
		if (fresh) {
			// give the drive a moment to spin up before we talk to it
			for (int i = 0; i < 120 && !exit_requested; i++) do_flip();
		}
		int ok = rip_disc(fresh, 1);   // the home screen already shows what will be ripped
		g_disc_state = DISC_NONE;     // ejected (or ripped): identify again on the next home screen
		autorip = 0;
		if (!ok) continue;

		// auto mode: show the result and wait for the next disc. X hides the summary, O leaves auto mode.
		int show_summary = 1;
		while (!exit_requested) {
			char text[700];
			if (show_summary) snprintf(text, sizeof(text), "%s\n\n%s", g_done_msg, T(g_eject ? STR_NEXT_DISC : STR_NEXT_DISC_NOEJECT));
			else snprintf(text, sizeof(text), "%s", T(g_eject ? STR_NEXT_DISC : STR_NEXT_DISC_NOEJECT));
			disc_inserted = 0;
			dialog_action = 0;
			msgDialogOpen2((msgType)(MSG_DIALOG_NORMAL | MSG_DIALOG_BTN_TYPE_OK), text, dialog_handler, NULL, NULL);
			while (!dialog_action && !disc_inserted && !exit_requested) do_flip();
			msgDialogAbort();
			if (disc_inserted) { autorip = 1; break; }
			if (dialog_action == 1) { show_summary = 0; continue; }   // X
			break;                                                     // O: back to the start screen
		}
	}
	sysDiscUnregisterDiscChangeCallback();
	while (g_ident_busy) usleep(10000);
	if (g_ripped && !exit_requested && ask_yesno(T(STR_ASK_REFRESH))) {
		if (webman_get("/refresh.ps3")) tell(T(STR_REFRESH_MANUAL));
	}
	lv1_storage_unpatch();
	logf("---- PSX Ripper exit ----");
	return 0;
}
