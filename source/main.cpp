// PSX Ripper - dumps a PS1 disc from the PS3 Blu-ray drive to
// /dev_hdd0/PSXISO/<NAME> [<SERIAL>].bin/.cue and writes a LibCrypt .sbi
// to /dev_hdd0/tmp/sbi/<SERIAL>.sbi (read by Cobra 8.x at mount time).
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <malloc.h>
#include <time.h>
#include <sys/time.h>

#include <io/pad.h>
#include <sys/thread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sysutil/msg.h>
#include <sysutil/sysutil.h>
#include <sysutil/osk.h>
#include <sys/memory.h>
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
#include "redump.h"
#include "remote.h"
#include "webhook.h"
#include "vfs.h"
#include "usbodd.h"

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
static int g_redump = RD_OFF;      // verification against Redump: off, online or local copy
static int g_remote = 0;           // remote view (web server on port 8090)
enum { CHECK_OFF, CHECK_ON, CHECK_BITPERFECT };
static int g_check = CHECK_BITPERFECT;   // sector check: off, EDC/ECC + re-read, + secure read and confirmation
static int g_webhook_on = 0;       // send webhooks
static char g_webhook[512] = "";   // webhook address (http://...); may contain a secret id, never logged
static int g_webhook_mask = WH_DEFAULT_MASK;   // which WEBHOOK_EVENTS are sent
#define WH_HEADERS 3
static char g_webhook_hdr[WH_HEADERS][256];    // extra headers "Name: value" (often a secret: never logged or shown)
static int g_webhook_hdr_on = 0;               // send them (off keeps them stored)

// "Name: value" cleaned of CR/LF, so a header can never add lines to the request
static void clean_header(char *h)
{
	char *o = h;
	for (char *p = h; *p; p++) if (*p != '\r' && *p != '\n') *o++ = *p;
	*o = 0;
	if (!strchr(h, ':')) h[0] = 0;   // not "Name: value"
}

static void webhook_headers(char *out, size_t n)
{
	out[0] = 0;
	if (!g_webhook_hdr_on) return;
	for (int i = 0; i < WH_HEADERS; i++)
		if (g_webhook_hdr[i][0]) snprintf(out + strlen(out), n - strlen(out), "%s\r\n", g_webhook_hdr[i]);
}
static const int wh_steps[] = {1, 2, 5, 10, 15, 25, 50};
static int g_webhook_step = 5;                 // rip_progress every this many percent
static const int speed_x[4] = {0, 8, 4, 2};

// Returns 0 when there is no config file yet.
static int cfg_load()
{
	FILE *f = fopen(CFG_PATH, "r");
	if (!f) return 0;
	char line[640];
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		if (!strncmp(line, "outdir=", 7) && (line[7] == '/' || !strncmp(line + 7, "exfat", 5)))   // also exFAT drives
			snprintf(g_outdir, sizeof(g_outdir), "%s", line + 7);
		else if (!strncmp(line, "retries=", 8)) { int r = atoi(line + 8); if (r >= 1 && r <= 64) g_retries = r; }
		else if (!strncmp(line, "speed=", 6)) { int v = atoi(line + 6); if (v >= 0 && v <= 3) g_speed = v; }
		else if (!strncmp(line, "eject=", 6)) g_eject = atoi(line + 6) ? 1 : 0;
		else if (!strncmp(line, "autostart=", 10)) g_autostart = atoi(line + 10) ? 1 : 0;
		else if (!strncmp(line, "redump=", 7)) { int v = atoi(line + 7); if (v >= RD_OFF && v <= RD_LOCAL) g_redump = v; }
		else if (!strncmp(line, "remote=", 7)) g_remote = atoi(line + 7) ? 1 : 0;
		else if (!strncmp(line, "check=", 6)) { int v = atoi(line + 6); if (v >= CHECK_OFF && v <= CHECK_BITPERFECT) g_check = v; }
		else if (!strncmp(line, "webhook_on=", 11)) g_webhook_on = atoi(line + 11) ? 1 : 0;
		else if (!strncmp(line, "webhook=", 8)) snprintf(g_webhook, sizeof(g_webhook), "%s", line + 8);
		else if (!strncmp(line, "webhook_events=", 15)) g_webhook_mask = atoi(line + 15) & ((1 << WH_COUNT) - 1);
		else if (!strncmp(line, "webhook_headers_on=", 19)) g_webhook_hdr_on = atoi(line + 19) ? 1 : 0;
		else if (!strncmp(line, "webhook_header", 14) && line[14] >= '1' && line[14] <= '0' + WH_HEADERS && line[15] == '=') {
			snprintf(g_webhook_hdr[line[14] - '1'], sizeof(g_webhook_hdr[0]), "%s", line + 16);
			clean_header(g_webhook_hdr[line[14] - '1']);
		}
		else if (!strncmp(line, "webhook_step=", 13)) { int v = atoi(line + 13); for (int i = 0; i < 7; i++) if (wh_steps[i] == v) g_webhook_step = v; }
	}
	fclose(f);
	return 1;
}

static void cfg_save()
{
	FILE *f = fopen(CFG_PATH, "w");
	if (!f) return;
	fprintf(f, "outdir=%s\nretries=%d\nspeed=%d\neject=%d\nautostart=%d\nredump=%d\nremote=%d\nwebhook_on=%d\nwebhook=%s\ncheck=%d\nwebhook_events=%d\nwebhook_step=%d\n",
	        g_outdir, g_retries, g_speed, g_eject, g_autostart, g_redump, g_remote, g_webhook_on, g_webhook, g_check, g_webhook_mask, g_webhook_step);
	fprintf(f, "webhook_headers_on=%d\n", g_webhook_hdr_on);
	for (int i = 0; i < WH_HEADERS; i++) fprintf(f, "webhook_header%d=%s\n", i + 1, g_webhook_hdr[i]);
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
static volatile int g_osk_event = 0;   // last on-screen keyboard event (SYSUTIL_OSK_*)
static void sysutil_callback(u64 status, u64 param, void *usrdata)
{
	if (status == SYSUTIL_EXIT_GAME) exit_requested = 1;
	if (status >= SYSUTIL_OSK_LOADED && status <= SYSUTIL_OSK_INPUT_CANCELED) g_osk_event = (int)status;
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
static int own_dialog(const char *msg, int yesno);   // own UI, defined with the other screens
static int ask(int type, const char *msg)
{
	return own_dialog(msg, (type & MSG_DIALOG_BTN_TYPE_YESNO) != 0);
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

// ---------------------------------------------------------------- USB optical drives (diagnosis)
// Flag file /dev_hdd0/tmp/psxrip.usbprobe: list the storage devices the PS3 knows, and for any
// device that looks optical (2048-byte sectors) other than the internal drive, send INQUIRY and
// the Apple SuperDrive unlock command (EA 00 00 00 00 00 01). Results go to the log only.
static void usb_optical_probe()
{
	static const uint64_t bases[] = {0x0101000000000000ULL, 0x0103000000000000ULL};
	int keep_fd = g_fd;
	for (int b = 0; b < 2; b++)
		for (int n = 0; n < 0x40; n++) {
			uint64_t id = bases[b] + n;
			struct storage_device_info info __attribute__((aligned(16)));
			memset(&info, 0, sizeof(info));
			info.unknown03 = 0x12345678;
			if (sys_storage_get_device_info(id, (uint8_t *)&info)) continue;
			char name[8];
			memcpy(name, info.name, 7); name[7] = 0;
			for (int i = 0; i < 7; i++) if ((unsigned char)name[i] < 32 || (unsigned char)name[i] > 126) name[i] = '.';
			logf("usbprobe: id %016llx name '%s' sectors %llu size %u writable %u", (unsigned long long)id, name,
			     (unsigned long long)info.total_sectors, info.sector_size, info.writable);
			// an optical drive reports 2048-byte sectors, or 0 sectors of size 0 when it has no disc
			if (id == BD_DEVICE || (info.sector_size != 2048 && !(info.sector_size == 0 && info.total_sectors == 0))) continue;
			int fd = -1;
			int r = sys_storage_open(id, &fd);
			logf("usbprobe: open %016llx -> 0x%x", (unsigned long long)id, r);
			if (r) continue;
			g_fd = fd;
			// which syscall/transfer reaches a USB drive? (the BD drive uses 616)
			uint32_t keep_sc = g_sc;
			static const uint32_t scs[2] = {616, 604};
			static const uint32_t protos[2] = {ATAPI_PIO_DATA_IN_PROTO, ATAPI_DMA_PROTO};
			static uint8_t inq[96] __attribute__((aligned(128)));
			int found = 0;
			for (int si = 0; si < 2 && !found; si++)
				for (int pi = 0; pi < 2 && !found; pi++) {
					g_sc = scs[si];
					memset(inq, 0, sizeof(inq));
					uint8_t c1[12] = {0x12, 0, 0, 0, 96, 0, 0, 0, 0, 0, 0, 0};   // INQUIRY
					r = atapi(c1, inq, 96, protos[pi]);
					char vend[9], prod[17];
					memcpy(vend, inq + 8, 8); vend[8] = 0; memcpy(prod, inq + 16, 16); prod[16] = 0;
					logf("usbprobe: INQUIRY sc %u proto %u -> 0x%x type %u vendor '%s' product '%s'", scs[si], protos[pi], r, inq[0] & 0x1F, vend, prod);
					if (r == 0) found = 1;
				}
			if (!found) g_sc = keep_sc;
			uint8_t c2[12] = {0xEA, 0, 0, 0, 0, 0, 0x01, 0, 0, 0, 0, 0};    // SuperDrive unlock
			r = atapi(c2, NULL, 0, ATAPI_NON_DATA_PROTO);
			logf("usbprobe: unlock EA..01 -> 0x%x", r);
			uint8_t c3[12] = {0x1B, 0, 0, 0, 0x03, 0, 0, 0, 0, 0, 0, 0};    // START STOP UNIT: load tray
			r = atapi(c3, NULL, 0, ATAPI_NON_DATA_PROTO);
			logf("usbprobe: load -> 0x%x (sc %u)", r, g_sc);
			g_sc = keep_sc;
			sys_storage_close(fd);
		}
	g_fd = keep_fd;
}

// ---------------------------------------------------------------- debug commands over HTTP
// /dbg?op=... on the remote view, only when /dev_hdd0/tmp/psxrip.debug exists and the app does not
// use the drive. For finding out how to talk to devices (USB optical drives) without rebuilding.
static const char *qget(const char *q, const char *key, char *out, size_t n)
{
	size_t kl = strlen(key);
	for (const char *p = q; p && *p; ) {
		if (!strncmp(p, key, kl) && p[kl] == '=') {
			size_t v = strcspn(p + kl + 1, "&");
			if (v >= n) v = n - 1;
			memcpy(out, p + kl + 1, v); out[v] = 0;
			return out;
		}
		p = strchr(p, '&');
		if (p) p++;
	}
	return NULL;
}

static int unhex(const char *h, uint8_t *out, int max)
{
	int n = 0;
	while (h[0] && h[1] && n < max) { char b[3] = {h[0], h[1], 0}; out[n++] = (uint8_t)strtol(b, NULL, 16); h += 2; }
	return n;
}

static void hexdump(char *out, size_t n, const uint8_t *d, int len)
{
	size_t o = strlen(out);
	for (int i = 0; i < len && o + 4 < n; i++) {
		o += snprintf(out + o, n - o, "%02x", d[i]);
		if (i % 16 == 15 && o + 2 < n) out[o++] = '\n', out[o] = 0;
	}
	if (o + 2 < n) { out[o++] = '\n'; out[o] = 0; }
}

static volatile int g_debug_on = 0;
static int g_state_busy();
static int debug_cmd(const char *q, char *out, int n)
{
	char v[80], v2[600], opbuf[16];
	if (!g_debug_on) { snprintf(out, n, "debug mode off (create /dev_hdd0/tmp/psxrip.debug)\n"); return 0; }
	if (g_state_busy()) { snprintf(out, n, "busy: the app is using the drive\n"); return 0; }
	const char *op = qget(q, "op", opbuf, sizeof(opbuf));   // own buffer: v is reused below
	if (!op) { snprintf(out, n, "ops: list | usb | usbscsi&cdb=hex&len=&dir=in|out | scsi&id=&sc=616|604&proto=0|1|3&dir=0|1&cdb=hex&len= | devcmd&id=&cmd=&in=hex&len= | read&id=&lba=\n"); return 0; }
	if (!strcmp(op, "usb")) { usbodd_status(out, n); return 0; }
	if (!strcmp(op, "usbreset")) { snprintf(out, n, "bulk-only reset -> %d\n", usbodd_reset()); return 0; }
	if (!strcmp(op, "usbscsi")) {   // SCSI to the USB optical drive over our own Bulk-Only Transport
		uint8_t cdb[16] = {0};
		int cl = unhex(qget(q, "cdb", v2, sizeof(v2)) ? v2 : "", cdb, 16);
		int len = qget(q, "len", v, sizeof(v)) ? atoi(v) : 0;
		int din = !qget(q, "dir", v, sizeof(v)) || strcmp(v, "out");
		static uint8_t data[65536];
		memset(data, 0, sizeof(data));
		int st = -1, got = 0;
		char lg[600] = "";
		int r = usbodd_scsi(cdb, cl ? cl : 12, data, len < 0 || len > 65536 ? 0 : len, din, &st, &got, lg, sizeof(lg));
		snprintf(out, n, "%susbscsi -> %d, CSW status %d, data %d\n", lg, r, st, got);
		if (got > 0) hexdump(out, n, data, got < 512 ? got : 512);
		logf("debug: %s -> %d status %d", q, r, st);
		return 0;
	}
	if (!strcmp(op, "list")) {
		static const uint64_t bases[] = {0x0101000000000000ULL, 0x0103000000000000ULL};
		for (int b = 0; b < 2; b++)
			for (int i = 0; i < 0x40; i++) {
				struct storage_device_info info __attribute__((aligned(16)));
				memset(&info, 0, sizeof(info));
				info.unknown03 = 0x12345678;
				if (sys_storage_get_device_info(bases[b] + i, (uint8_t *)&info)) continue;
				size_t o = strlen(out);
				snprintf(out + o, n - o, "%016llx sectors=%llu size=%u writable=%u raw=", (unsigned long long)(bases[b] + i),
				         (unsigned long long)info.total_sectors, info.sector_size, info.writable);
				hexdump(out, n, (const uint8_t *)&info, sizeof(info));
			}
		return 0;
	}
	uint64_t id = qget(q, "id", v, sizeof(v)) ? strtoull(v, NULL, 16) : 0;
	if (!id) { snprintf(out, n, "id= missing\n"); return 0; }
	int fd = -1, r = sys_storage_open(id, &fd);
	snprintf(out, n, "open %016llx -> 0x%x\n", (unsigned long long)id, r);
	if (r) return 0;
	static uint8_t buf[8192] __attribute__((aligned(128)));
	memset(buf, 0, sizeof(buf));
	int len = qget(q, "len", v, sizeof(v)) ? atoi(v) : 0;
	if (len < 0 || len > (int)sizeof(buf)) len = 0;
	if (!strcmp(op, "scsi")) {
		uint8_t cdb[16] = {0};
		unhex(qget(q, "cdb", v2, sizeof(v2)) ? v2 : "", cdb, 16);
		uint32_t sc = qget(q, "sc", v, sizeof(v)) ? strtoul(v, NULL, 0) : 616;
		uint32_t proto = qget(q, "proto", v, sizeof(v)) ? strtoul(v, NULL, 0) : (len ? ATAPI_PIO_DATA_IN_PROTO : ATAPI_NON_DATA_PROTO);
		uint32_t dir = qget(q, "dir", v, sizeof(v)) ? strtoul(v, NULL, 0) : (len ? ATAPI_DIR_READ : ATAPI_DIR_WRITE);
		struct lv2_atapi_cmnd_block cmd;
		atapi_init(&cmd, len, proto, dir);
		if (!len) cmd.proto = ATAPI_NON_DATA_PROTO;
		memcpy(cmd.pkt, cdb, 12);
		r = sys_storage_send_atapi_command(sc, fd, &cmd, buf);
		size_t o = strlen(out);
		snprintf(out + o, n - o, "scsi sc=%u proto=%u dir=%u len=%d -> 0x%x\n", sc, proto, dir, len, r);
		if (len) hexdump(out, n, buf, len);
	} else if (!strcmp(op, "devcmd")) {
		uint32_t c = qget(q, "cmd", v, sizeof(v)) ? strtoul(v, NULL, 0) : 0;
		static uint8_t in[512] __attribute__((aligned(128)));
		int inl = unhex(qget(q, "in", v2, sizeof(v2)) ? v2 : "", in, sizeof(in));
		r = sys_storage_send_device_cmd(fd, c, in, inl, buf, len);
		size_t o = strlen(out);
		snprintf(out + o, n - o, "devcmd 0x%x in=%d len=%d -> 0x%x\n", c, inl, len, r);
		if (len) hexdump(out, n, buf, len);
	} else if (!strcmp(op, "read")) {
		uint32_t lba = qget(q, "lba", v, sizeof(v)) ? strtoul(v, NULL, 0) : 0, got = 0;
		r = sys_storage_read(fd, lba, 1, buf, &got);
		size_t o = strlen(out);
		snprintf(out + o, n - o, "read lba %u -> 0x%x got %u\n", lba, r, got);
		if (!r) hexdump(out, n, buf, 64);
	}
	sys_storage_close(fd);
	logf("debug: %s", q);
	return 0;
}

// Data-out command (MODE SELECT).
static int atapi_out(const uint8_t *cdb, uint8_t *buf, uint32_t len)
{
	struct lv2_atapi_cmnd_block cmd;
	atapi_init(&cmd, len, ATAPI_PIO_DATA_OUT_PROTO, ATAPI_DIR_WRITE);
	memcpy(cmd.pkt, cdb, 12);
	return sys_storage_send_atapi_command(g_sc, g_fd, &cmd, buf);
}

static uint64_t now_ms()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

// Read/write error recovery mode page (0x01). Its read retry count decides how long the drive
// itself keeps retrying a bad sector before it reports an error. The first pass sets it low so a
// scratch costs little time; the original value is restored for the second pass and afterwards.
static uint8_t g_er_page[12];
static int g_er_valid = 0;

static int er_read(uint8_t page[12])
{
	static uint8_t b[64] __attribute__((aligned(128)));
	uint8_t cdb[12] = {0x5A, 0x08, 0x01, 0, 0, 0, 0, 0, sizeof(b), 0, 0, 0};   // MODE SENSE(10), DBD, page 1
	memset(b, 0, sizeof(b));
	int r = atapi(cdb, b, sizeof(b), ATAPI_PIO_DATA_IN_PROTO);
	if (r) return r;
	int bdl = b[6] << 8 | b[7];
	const uint8_t *pg = b + 8 + bdl;
	if ((pg[0] & 0x3F) != 0x01 || pg[1] < 10) return -1;
	memcpy(page, pg, 12);
	return 0;
}

static int er_write(const uint8_t page[12])
{
	static uint8_t b[20] __attribute__((aligned(128)));
	memset(b, 0, sizeof(b));
	memcpy(b + 8, page, 12);
	b[8] &= 0x3F;   // PS bit must be 0 in MODE SELECT
	uint8_t cdb[12] = {0x55, 0x10, 0, 0, 0, 0, 0, 0, sizeof(b), 0, 0, 0};   // MODE SELECT(10), PF, not saved
	return atapi_out(cdb, b, sizeof(b));
}

static void er_set_retries(int n)   // n < 0: restore the original page
{
	if (!g_er_valid) {
		int r = er_read(g_er_page);
		if (r) { logf("error recovery page not available (0x%x)", r); return; }
		g_er_valid = 1;
		logf("error recovery page: flags=%02x read retries=%u", g_er_page[2], g_er_page[3]);
	}
	uint8_t pg[12];
	memcpy(pg, g_er_page, 12);
	if (n >= 0) pg[3] = (uint8_t)n;
	int r = er_write(pg);
	uint8_t chk[12];
	int rr = er_read(chk);
	logf("set read retries %d -> 0x%x (now %d)", n >= 0 ? n : g_er_page[3], r, rr ? -1 : chk[3]);
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
	VFile *f = vf_open(path, "wb");
	if (!f) return -1;
	vf_write(f, "SBI\0", 4);
	for (int i = 0; i < g_nlc; i++) {
		uint8_t rec[14];
		lba_to_msf(g_lc[i].lba + 150, rec);
		rec[3] = 0x01;                   // full 10-byte Q follows
		memcpy(rec + 4, g_lc[i].q, 10);
		vf_write(f, rec, 14);
	}
	vf_close(f);
	return 0;
}

// ---------------------------------------------------------------- rip worker
static volatile uint32_t g_done = 0, g_total = 0;
static volatile uint32_t g_readpos = 0;   // reader position inside a slow (damaged) chunk
// sector being re-read right now (shown on the progress screen); g_rr_max = 0 when not re-reading
static volatile uint32_t g_rr_lba = 0;
static volatile int g_rr_try = 0, g_rr_max = 0;
// Sectors that failed in the first pass (unreadable, or bit 31 set: wrong EDC). They get a second
// pass at low speed once the whole disc has been read, the way data recovery tools work.
#define MAX_HARD 8192
static uint32_t g_hard[MAX_HARD];
static uint32_t g_nhard = 0;
static volatile uint32_t g_pass2_total = 0, g_pass2_done = 0;

static void note_hard(uint32_t lba, int edc_bad)
{
	if (g_nhard < MAX_HARD) g_hard[g_nhard++] = lba | (edc_bad ? 0x80000000u : 0);
}

// Sectors that were wrong at first but fixed in the first pass (EDC fixed by a re-read, or two
// reads that disagreed): they are confirmed in the second pass. Their data is kept to see whether
// the confirmation changes them.
#define MAX_SUSPECT 512
static uint32_t g_suspect[MAX_SUSPECT];
static uint8_t *g_suspect_data;   // MAX_SUSPECT * RAW_SIZE
static uint32_t g_nsuspect = 0;
static uint8_t g_suspect_ok[MAX_SUSPECT];   // confirmed in the second pass
#define HARD_RESOLVED 0x40000000u          // g_hard flag: read correctly in the second pass
static volatile uint32_t g_confirmed = 0, g_changed = 0;

static void note_suspect(uint32_t lba, const uint8_t *sector)
{
	if (!g_suspect_data) g_suspect_data = (uint8_t *)malloc(MAX_SUSPECT * RAW_SIZE);
	if (!g_suspect_data || g_nsuspect >= MAX_SUSPECT) return;
	for (uint32_t i = 0; i < g_nsuspect; i++) if (g_suspect[i] == lba) { memcpy(g_suspect_data + i * RAW_SIZE, sector, RAW_SIZE); return; }
	g_suspect[g_nsuspect] = lba;
	g_suspect_ok[g_nsuspect] = 0;
	memcpy(g_suspect_data + g_nsuspect * RAW_SIZE, sector, RAW_SIZE);
	g_nsuspect++;
}
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
static uint8_t g_track_md5bin[MAX_TRACKS + 1][16];
// First sector of each track as Redump splits a disc: tracks after the first start at their
// pregap (INDEX 00), not at INDEX 01 from the TOC. Starts with the usual 2 s pregap after a data
// track and is corrected from the Q subchannel (absolute time + track number) while reading.
static volatile uint32_t g_tstart[MAX_TRACKS + 1];

static void tstart_init()
{
	g_tstart[0] = 0;
	for (int t = 1; t < g_ntracks; t++) {
		uint32_t l = g_tracks[t].lba;
		int audio = !(g_tracks[t].ctrl & 0x4);
		g_tstart[t] = (audio && l >= 150 && (g_tracks[t - 1].ctrl & 0x4)) ? l - 150 : l;
	}
}

static void tstart_from_q(const uint8_t *q)
{
	if ((q[0] & 0x0F) != 1) return;   // ADR 1: position data
	int num = bcd2(q[1]);
	int abs_lba = (bcd2(q[7]) * 60 + bcd2(q[8])) * 75 + bcd2(q[9]) - 150;
	for (int t = 1; t < g_ntracks; t++) {
		int l = (int)g_tracks[t].lba;
		if (abs_lba >= l || abs_lba + 750 < l) continue;      // only look within 10 s before INDEX 01
		if (num == g_tracks[t].num && abs_lba < (int)g_tstart[t]) g_tstart[t] = abs_lba;            // pregap starts earlier
		else if (num == g_tracks[t - 1].num && abs_lba >= (int)g_tstart[t]) g_tstart[t] = abs_lba + 1;   // or later
	}
}

static uint8_t *g_bufs[2];
static volatile uint32_t g_buf_len[2];   // sectors in buffer, 0 = empty
static volatile uint32_t g_buf_end[2];   // lba after the last sector in buffer
static volatile int g_reader_done = 0, g_write_failed = 0;
static VFile *g_out;

// Sector check (data track 1): sync pattern, header address and EDC. The drive can hand out a
// sector with uncorrected errors and still report success, so the reader re-reads such sectors.
static uint32_t g_edc_tab[256];
static volatile uint32_t g_badsect = 0, g_recovered = 0;

static void edc_init()
{
	for (uint32_t i = 0; i < 256; i++) {
		uint32_t e = i;
		for (int k = 0; k < 8; k++) e = (e >> 1) ^ ((e & 1) ? 0xD8018001 : 0);
		g_edc_tab[i] = e;
	}
}

static uint32_t edc(const uint8_t *p, int n)
{
	uint32_t e = 0;
	while (n--) e = g_edc_tab[(e ^ *p++) & 255] ^ (e >> 8);
	return e;
}

// ECC (P and Q parity, ECMA-130) of mode 1 and mode 2 form 1 sectors. The drive can return a
// sector whose data and EDC are right but whose ECC bytes are not; Redump compares those too.
static uint8_t g_ecc_f[256], g_ecc_b[256];

static void ecc_init()
{
	for (int i = 0; i < 256; i++) {
		int j = (i << 1) ^ ((i & 0x80) ? 0x11D : 0);
		g_ecc_f[i] = (uint8_t)j;
		g_ecc_b[i ^ j] = (uint8_t)i;
	}
}

static int ecc_part_ok(const uint8_t *src, int major_count, int minor_count, int major_mult, int minor_inc, const uint8_t *dest)
{
	int size = major_count * minor_count;
	for (int major = 0; major < major_count; major++) {
		int index = (major >> 1) * major_mult + (major & 1);
		uint8_t a = 0, b = 0;
		for (int minor = 0; minor < minor_count; minor++) {
			uint8_t t = src[index];
			index += minor_inc;
			if (index >= size) index -= size;
			a ^= t; b ^= t;
			a = g_ecc_f[a];
		}
		a = g_ecc_b[g_ecc_f[a] ^ b];
		if (dest[major] != a || dest[major + major_count] != (uint8_t)(a ^ b)) return 0;
	}
	return 1;
}

static int ecc_ok(const uint8_t *s, int mode2)
{
	static uint8_t t[RAW_SIZE];
	const uint8_t *p = s;
	if (mode2) { memcpy(t, s, RAW_SIZE); memset(t + 12, 0, 4); p = t; }   // mode 2: header counts as zero
	return ecc_part_ok(p + 0xC, 86, 24, 2, 86, p + 0x81C) && ecc_part_ok(p + 0xC, 52, 43, 86, 88, p + 0x8C8);
}

static const char *check_sector(uint32_t lba, const uint8_t *s)
{
	static const uint8_t sync[12] = {0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0};
	if (memcmp(s, sync, 12)) return "sync";
	uint32_t a = lba + 150;
	if (s[12] != bcd(a / 4500) || s[13] != bcd((a / 75) % 60) || s[14] != bcd(a % 75)) return "address";
	const uint8_t *st;
	uint32_t calc;
	if (s[15] == 1) { calc = edc(s, 2064); st = s + 2064; }
	else if (s[15] == 2 && (s[18] & 0x20)) { calc = edc(s + 16, 2332); st = s + 2348; }   // form 2
	else if (s[15] == 2) { calc = edc(s + 16, 2056); st = s + 2072; }                     // form 1
	else return 0;
	uint32_t stored = st[0] | st[1] << 8 | st[2] << 16 | (uint32_t)st[3] << 24;
	if (s[15] == 2 && (s[18] & 0x20) && !stored) return 0;   // form 2 EDC is optional
	if (stored != calc) return "EDC";
	if (s[15] == 1 || !(s[18] & 0x20)) return ecc_ok(s, s[15] == 2) ? 0 : "ECC";   // form 2 has no ECC
	return 0;
}

// Form 2 sectors (video, XA audio) may have no EDC (stored as 0): a read error there cannot be
// detected from the sector itself. Such sectors are read twice and compared ("secure read").
static int unverifiable(const uint8_t *s)
{
	return s[15] == 2 && (s[18] & 0x20) && !(s[2348] | s[2349] | s[2350] | s[2351]);
}
static volatile uint32_t g_unstable = 0, g_secure = 0;   // sectors that read differently / sectors compared
static uint8_t *g_vbuf;                                   // second read of a run, CHUNK sectors
// The first pass only records a fingerprint of each sector without EDC; a verify pass after it
// re-reads them in long runs at full speed (no seeking back and forth while ripping).
#define MAX_UV (256 * 1024)
static uint32_t *g_uv_lba;
static uint64_t *g_uv_hash;
static uint32_t g_nuv = 0;
static volatile int g_phase = 0;   // 0 reading, 1 verify pass, 2 second pass

// Recent events for the progress screen ("Sector 125454 fixed by re-reading"), newest last.
#define NEV 5
struct RipEvent { char text[120]; uint32_t col; };
static RipEvent g_ev[NEV];
static volatile int g_nev = 0, g_ev_seq = 0;
static void rip_event(uint32_t col, StrId fmt, uint32_t lba, const char *why = NULL)
{
	if (g_nev == NEV) { memmove(g_ev, g_ev + 1, sizeof(RipEvent) * (NEV - 1)); g_nev--; }
	snprintf(g_ev[g_nev].text, sizeof(g_ev[g_nev].text), T(fmt), lba, why ? why : "");
	g_ev[g_nev].col = col;
	__sync_synchronize();
	g_nev++;
	g_ev_seq++;
}

// "EDC" -> plain words for the screen
static const char *why_words(const char *why)
{
	if (!strcmp(why, "EDC")) return T(STR_WHY_CHECKSUM);
	if (!strcmp(why, "ECC")) return T(STR_WHY_ECC);
	return T(STR_WHY_HEADER);
}

static uint64_t fnv64(const uint8_t *p, int n)
{
	uint64_t h = 1469598103934665603ULL;
	while (n--) { h ^= *p++; h *= 1099511628211ULL; }
	return h;
}

static void writer_thread(void *arg)
{
	for (int k = 0;; k ^= 1) {
		while (!g_buf_len[k] && !(g_reader_done && !g_buf_len[k ^ 1] && !g_buf_len[k])) usleep(500);
		if (!g_buf_len[k]) break;   // reader done and nothing left
		__sync_synchronize();
		uint32_t n = g_buf_len[k];
		if (vf_write(g_out, g_bufs[k], (size_t)RAW_SIZE * n) != (size_t)RAW_SIZE * n) g_write_failed = 1;
		// feed each sector range to the MD5 of the track it belongs to
		uint32_t first = g_buf_end[k] - n;
		for (uint32_t i = 0; i < n;) {
			uint32_t lba = first + i;
			int t = g_ntracks - 1;
			while (t > 0 && lba < g_tstart[t]) t--;
			uint32_t end = (t + 1 < g_ntracks) ? g_tstart[t + 1] : g_leadout;
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

static void set_cd_speed(int x)   // x = 0: maximum, otherwise the CD speed factor
{
	uint16_t kbs = x ? (uint16_t)(176 * x) : 0xFFFF;
	uint8_t cdb[12] = {0xBB, 0, (uint8_t)(kbs >> 8), (uint8_t)kbs, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0};
	logf("set cd speed %u KB/s -> 0x%x", kbs, atapi(cdb, NULL, 0, ATAPI_NON_DATA_PROTO));
}

// Second pass, at low speed (2x) with the drive's own retries restored:
//  - sectors that failed (unreadable, wrong EDC, reads that never agreed) are read again;
//  - sectors that were fixed in the first pass are confirmed.
// A sector counts as read only when three reads in a row are identical (and, where the sector has
// one, the EDC is right); it is then written into the image. O stops the pass and keeps the image.
static int confirm_read(uint32_t lba, int need_edc, int max_reads, uint8_t *out)
{
	static uint8_t prev[SECT_Q] __attribute__((aligned(128)));
	int streak = 0;
	for (int t = 0; t < max_reads && !g_abort; t++) {
		g_rr_try = t + 1;
		if (read_cd(lba, 1, g_sec) || (need_edc && check_sector(lba, g_sec))) { streak = 0; continue; }
		if (streak && !memcmp(prev, g_sec, RAW_SIZE)) streak++;
		else { streak = 1; memcpy(prev, g_sec, RAW_SIZE); }
		if (streak == 3) { memcpy(out, prev, RAW_SIZE); return 1; }
	}
	return 0;
}

// Verify pass (bit-perfect mode): re-reads the sectors without EDC in long runs at full speed and
// compares them with their first-pass fingerprint. A sector that differs is read until three reads
// agree and written into the image. Returns the number of sectors written.
static int verify_pass()
{
	if (!g_nuv || g_abort || g_write_failed) return 0;
	g_phase = 1;
	g_pass2_total = g_nuv; g_pass2_done = 0;
	uint64_t t0 = now_ms();
	int patched = 0, reads = g_retries * 3 + 3;
	static uint8_t sec[RAW_SIZE] __attribute__((aligned(128)));
	for (uint32_t k = 0; k < g_nuv && !g_abort; ) {
		uint32_t a = g_uv_lba[k], m = 1;   // sectors within one window of CHUNK, read as one run
		while (k + m < g_nuv && g_uv_lba[k + m] - a < CHUNK) m++;
		uint32_t span = g_uv_lba[k + m - 1] - a + 1;
		g_readpos = a;
		if (read_cd(a, span, g_vbuf) == 0) {
			for (uint32_t j = 0; j < m; j++) {
				uint32_t lba = g_uv_lba[k + j];
				g_secure++;
				if (fnv64(g_vbuf + (lba - a) * SECT_Q, RAW_SIZE) == g_uv_hash[k + j]) continue;
				g_unstable++;
				g_rr_lba = lba; g_rr_max = reads;
				int ok = confirm_read(lba, 0, reads, sec);
				g_rr_max = 0;
				if (ok && fnv64(sec, RAW_SIZE) != g_uv_hash[k + j]) {
					if (vf_seek(g_out, (uint64_t)lba * RAW_SIZE) == 0 && vf_write(g_out, sec, RAW_SIZE) == RAW_SIZE) { patched++; g_changed++; }
					rip_event(0x7CE0A0, STR_EV_DIFFER, lba);
				} else if (ok) {
					rip_event(0xA8B0C8, STR_EV_FIRST_OK, lba);   // the first read was right
				}
				if (!ok) note_hard(lba, 1);
				if (g_unstable <= 32) logf("verify: sector %u read differently, %s", lba, ok ? (fnv64(sec, RAW_SIZE) != g_uv_hash[k + j] ? "confirmed new data" : "first read confirmed") : "NOT confirmed");
			}
		}
		k += m;
		g_pass2_done = k;
	}
	logf("verify pass: %u sectors without EDC re-read, %u differed, %d written, %u ms", g_secure, g_unstable, patched, (unsigned)(now_ms() - t0));
	return patched;
}

static int second_pass()
{
	if (g_check != CHECK_BITPERFECT) g_nsuspect = 0;   // confirmation only in bit-perfect mode
	uint32_t total = g_nhard + g_nsuspect;
	if (!total || g_abort || g_write_failed) return 0;
	logf("second pass: %u failed and %u fixed sector(s) at 2x", g_nhard, g_nsuspect);
	g_phase = 2;
	g_pass2_total = total; g_pass2_done = 0;
	set_cd_speed(2);
	uint32_t t1_end = (g_ntracks > 1) ? g_tstart[1] : g_leadout;
	int data1 = g_tracks[0].ctrl & 0x4, patched = 0, reads = g_retries * 3 + 3;
	static uint8_t sec[RAW_SIZE] __attribute__((aligned(128)));
	time_t t0 = time(NULL);
	for (uint32_t k = 0; k < total && !g_abort; k++) {
		int suspect = k >= g_nhard;
		uint32_t lba = suspect ? g_suspect[k - g_nhard] : (g_hard[k] & 0x3FFFFFFF);
		int edc_bad = !suspect && (g_hard[k] >> 31);
		if (!suspect && (g_hard[k] & HARD_RESOLVED)) { g_pass2_done = k + 1; continue; }
		int in_data = data1 && lba < t1_end;
		g_rr_lba = lba; g_rr_max = reads;
		int ok = confirm_read(lba, in_data, reads, sec);   // check_sector() skips sectors without EDC
		g_rr_max = 0;
		if (suspect) {
			if (ok) {
				g_confirmed++;
				g_suspect_ok[k - g_nhard] = 1;
				if (memcmp(sec, g_suspect_data + (k - g_nhard) * RAW_SIZE, RAW_SIZE)) {
					g_changed++;
					if (vf_seek(g_out, (uint64_t)lba * RAW_SIZE) == 0 && vf_write(g_out, sec, RAW_SIZE) == RAW_SIZE) patched++;
					logf("second pass: sector %u confirmed with different data, image updated", lba);
					rip_event(0x7CE0A0, STR_EV_DIFFER, lba);
				} else {
					rip_event(0x7CE0A0, STR_EV_CONFIRMED, lba);   // the fix from the first pass holds
				}
			} else {
				logf("second pass: sector %u could not be confirmed", lba);
			}
		} else if (ok && vf_seek(g_out, (uint64_t)lba * RAW_SIZE) == 0 && vf_write(g_out, sec, RAW_SIZE) == RAW_SIZE) {
			patched++;
			g_hard[k] |= HARD_RESOLVED;
			if (edc_bad) g_badsect--; else g_readerrs--;
			g_recovered++;
			logf("second pass: sector %u recovered", lba);
			rip_event(0x7CE0A0, STR_EV_RECOVERED, lba);
		}
		g_pass2_done = k + 1;
	}
	set_cd_speed(g_speed ? speed_x[g_speed] : 0);
	logf("second pass: %d sector(s) written, %u of %u fixed sectors confirmed (%u changed), %ld s%s",
	     patched, g_confirmed, g_nsuspect, g_changed, (long)(time(NULL) - t0), g_abort ? " (stopped with O)" : "");
	g_abort = 0;   // O during the second pass keeps the image from the first pass
	return patched;
}

// Recomputes the per-track MD5s from the finished image (after the second pass patched it).
static void md5_from_file(const char *path)
{
	VFile *f = vf_open(path, "rb");
	if (!f) return;
	uint8_t *b = (uint8_t *)malloc(256 * RAW_SIZE);
	uint32_t lba = 0;
	int t = 0;
	md5_init(&g_md5[0]);
	while (b && lba < g_leadout) {
		uint32_t n = g_leadout - lba < 256 ? g_leadout - lba : 256;
		if (vf_read(f, b, (size_t)RAW_SIZE * n) != (size_t)RAW_SIZE * n) break;
		for (uint32_t i = 0; i < n;) {
			while (t + 1 < g_ntracks && lba + i >= g_tstart[t + 1]) {
				md5_final(&g_md5[t], g_track_md5bin[t]);
				md5_init(&g_md5[++t]);
			}
			uint32_t end = (t + 1 < g_ntracks) ? g_tstart[t + 1] : g_leadout, cnt = end - (lba + i);
			if (cnt > n - i) cnt = n - i;
			md5_update(&g_md5[t], b + i * RAW_SIZE, cnt * RAW_SIZE);
			i += cnt;
		}
		lba += n;
	}
	while (t < g_ntracks) { md5_final(&g_md5[t], g_track_md5bin[t]); if (++t < g_ntracks) md5_init(&g_md5[t]); }
	free(b);
	vf_close(f);
	logf("md5 recomputed from %s", path);
}

static void rip_thread(void *arg)
{
	char path[300];
	snprintf(path, sizeof(path), "%s.bin.part", g_base);   // renamed to .bin when complete
	g_out = vf_open(path, "wb");
	if (!g_out) { logf("cannot create %s", path); g_result = -1; g_finished = 1; sysThreadExit(0); }

	g_bufs[0] = (uint8_t *)memalign(128, CHUNK * SECT_Q);
	g_bufs[1] = (uint8_t *)memalign(128, CHUNK * SECT_Q);
	g_buf_len[0] = g_buf_len[1] = 0;
	for (int t = 0; t < g_ntracks; t++) md5_init(&g_md5[t]);
	tstart_init();
	edc_init();
	ecc_init();
	g_badsect = 0; g_recovered = 0; g_nhard = 0; g_pass2_total = 0; g_pass2_done = 0;
	g_unstable = 0; g_secure = 0; g_nsuspect = 0; g_confirmed = 0; g_changed = 0;
	if (!g_vbuf) g_vbuf = (uint8_t *)memalign(128, CHUNK * SECT_Q);
	if (!g_uv_lba) { g_uv_lba = (uint32_t *)malloc(MAX_UV * 4); g_uv_hash = (uint64_t *)malloc(MAX_UV * 8); }
	g_nuv = 0; g_phase = 0; g_nev = 0;
	g_reader_done = 0; g_write_failed = 0;
	uint32_t data_end = (g_ntracks > 1) ? g_tracks[1].lba : g_leadout;  // LibCrypt lives in track 1

	sys_ppu_thread_t wtid;
	sysThreadCreate(&wtid, writer_thread, NULL, 1000, 0x10000, THREAD_JOINABLE, (char *)"psxwrite");

	struct stat st;
	int single = stat("/dev_hdd0/tmp/psxrip_single", &st) == 0;   // test switch: disable overlap
	logf("buffering: %s", single ? "single" : "double");
	time_t t0 = time(NULL);
	er_set_retries(1);   // first pass: let the drive give up quickly on bad sectors
	int k = 0;
	for (uint32_t lba = 0; lba < g_leadout && !g_abort && !g_write_failed; k ^= 1) {
		while (g_buf_len[k] && !g_write_failed) usleep(500);   // wait until the writer has emptied it
		uint8_t *buf = g_bufs[k];
		uint32_t n = CHUNK;
		if (lba + n > g_leadout) n = g_leadout - lba;

		int ret = -1;
		static uint8_t dead[CHUNK];   // sectors the drive could not read at all (zero-filled)
		memset(dead, 0, n);
		for (int tries = 0; tries < g_retries && ret; tries++) ret = read_cd(lba, n, buf);
		if (ret) {
			// Fall back to reads of 16 sectors; only a 16-sector piece that fails is read sector by
			// sector. Inside a damaged area (an unreadable sector within the last 256) each sector
			// gets one try, otherwise a scratch that hits once per revolution costs hours.
			// Cancelling is checked per sector.
			static uint32_t last_bad = 0;
			const uint32_t PIECE = 16;
			for (uint32_t i0 = 0; i0 < n; i0 += PIECE) {
				uint32_t m = n - i0 < PIECE ? n - i0 : PIECE;
				g_readpos = lba + i0;   // keep the progress bar moving in slow areas
				if (g_abort) { for (uint32_t i = i0; i < i0 + m; i++) { memset(buf + i * SECT_Q, 0, SECT_Q); dead[i] = 1; } continue; }
				// inside a damaged area go straight to single sectors: a failing 16-sector read costs as
				// much as a failing single one
				int in_damage = g_readerrs && lba + i0 - last_bad < 256;
				if (!in_damage && read_cd(lba + i0, m, buf + i0 * SECT_Q) == 0) continue;
				for (uint32_t i = i0; i < i0 + m; i++) {
					uint8_t *sp = buf + i * SECT_Q;
					if (g_abort) { memset(sp, 0, SECT_Q); dead[i] = 1; continue; }
					int damaged = g_readerrs && lba + i - last_bad < 256;
					int r = -1, tries = damaged ? 1 : g_retries;
					g_rr_lba = lba + i; g_rr_max = tries;
					uint64_t t0 = now_ms();
					for (int t = 0; t < tries && r && !g_abort; t++) { g_rr_try = t + 1; r = read_cd(lba + i, 1, sp); }
					g_rr_max = 0;
					if (r) {
						memset(sp, 0, SECT_Q); dead[i] = 1; g_readerrs++; last_bad = lba + i;
						note_hard(lba + i, 0);
						rip_event(0xFF8A80, STR_EV_UNREADABLE, lba + i);
						if (g_readerrs <= 64 || g_readerrs % 100 == 0)
							logf("unreadable sector %u (0x%x, %d tries, %u ms, %u so far)", lba + i, r, tries, (unsigned)(now_ms() - t0), g_readerrs);
					}
				}
			}
		}
		// data track 1: sectors without EDC are read a second time and compared; a sector that
		// differs is read until two reads in a row agree
		if (g_check == CHECK_BITPERFECT && (g_tracks[0].ctrl & 0x4) && g_uv_lba) {
			uint32_t t1_end = (g_ntracks > 1) ? g_tstart[1] : g_leadout;
			for (uint32_t i = 0; i < n && lba + i < t1_end; i++)
				if (!dead[i] && unverifiable(buf + i * SECT_Q) && g_nuv < MAX_UV) {
					g_uv_lba[g_nuv] = lba + i;
					g_uv_hash[g_nuv++] = fnv64(buf + i * SECT_Q, RAW_SIZE);
				}
		}
		// data track 1: re-read sectors whose sync, address, EDC or ECC is wrong
		if (g_check != CHECK_OFF && (g_tracks[0].ctrl & 0x4)) {
			uint32_t t1_end = (g_ntracks > 1) ? g_tstart[1] : g_leadout;
			for (uint32_t i = 0; i < n && lba + i < t1_end; i++) {
				uint8_t *sp = buf + i * SECT_Q;
				const char *why = dead[i] ? 0 : check_sector(lba + i, sp);
				if (!why) continue;
				int fixed = 0, tries = 0;
				g_rr_lba = lba + i; g_rr_max = g_retries;
				while (!fixed && tries < g_retries && !g_abort) {
					tries++;
					g_rr_try = tries;
					if (read_cd(lba + i, 1, g_sec) == 0 && !check_sector(lba + i, g_sec)) { memcpy(sp, g_sec, SECT_Q); fixed = 1; }
				}
				g_rr_max = 0;
				if (fixed) { g_recovered++; note_suspect(lba + i, sp); rip_event(0x7CE0A0, STR_EV_FIXED, lba + i, why_words(why)); logf("sector %u: %s, fixed by re-read %d", lba + i, why, tries); }
				else {
					note_hard(lba + i, 1);
					rip_event(0xFF8A80, STR_EV_STILLBAD, lba + i, why_words(why));
					if (++g_badsect <= 32) logf("bad sector %u: %s, still wrong after %d re-reads", lba + i, why, tries);
				}
			}
		}
		// check Q first, then pack the 2352-byte sectors in place for a single write
		for (uint32_t i = 0; i < n; i++) {
			if (g_is_pal && lba + i < data_end && !track_is_audio(lba + i)) check_subq(lba + i, buf + i * SECT_Q + RAW_SIZE);
			if (g_ntracks > 1) tstart_from_q(buf + i * SECT_Q + RAW_SIZE);
		}
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
	int patched = verify_pass();
	if (g_er_valid) er_set_retries(-1);   // the drive's own retries back for the second pass
	g_phase = 2;
	patched += second_pass();
	g_phase = 3;   // finishing: MD5, LibCrypt, Redump
	vf_close(g_out);
	if (patched) md5_from_file(path);   // the streamed MD5s no longer match the patched file
	for (int t = 0; t < g_ntracks; t++) {
		if (!patched) md5_final(&g_md5[t], g_track_md5bin[t]);
		md5_hex(g_track_md5bin[t], g_track_md5[t]);
		logf("track %d start %u (index 01 %u) md5 %s", g_tracks[t].num, g_tstart[t], g_tracks[t].lba, g_track_md5[t]);
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
	VFile *f = vf_open(path, "wb");
	if (!f) return;
	vf_printf(f, "FILE \"%s\" BINARY\r\n", binname);
	for (int t = 0; t < g_ntracks; t++) {
		int audio = !(g_tracks[t].ctrl & 0x4);
		vf_printf(f, "  TRACK %02d %s\r\n", g_tracks[t].num, audio ? "AUDIO" : "MODE2/2352");
		uint32_t l = g_tracks[t].lba;
		if (t > 0 && g_tstart[t] < l) {
			uint32_t p = g_tstart[t];   // pregap, from the Q subchannel
			vf_printf(f, "    INDEX 00 %02u:%02u:%02u\r\n", p / 4500, (p / 75) % 60, p % 75);
		}
		vf_printf(f, "    INDEX 01 %02u:%02u:%02u\r\n", l / 4500, (l / 75) % 60, l % 75);
	}
	vf_close(f);
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
	// short socket timeouts and an overall limit: a busy or hung webMAN must never freeze the app
	struct timeval tv = {1, 0};
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	int ret = -1;
	if (connect(sock, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
		char req[600];
		int n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: 127.0.0.1\r\n\r\n", path);
		send(sock, req, n, 0);
		char tmp[256];
		time_t t0 = time(NULL);
		for (;;) {
			int r = recv(sock, tmp, sizeof(tmp), 0);
			if (r == 0) break;
			if (r < 0 && time(NULL) - t0 >= 3) { logf("webMAN %s: no answer within 3 s", path); break; }
			if (r < 0) usleep(20000);
		}
		ret = 0;
	}
	close(sock);
	logf("webMAN %s -> %s", path, ret ? "not reachable" : "ok");
	return ret;
}

// webMAN request from a background thread (notifications): the caller never waits for it.
static void webman_thread(void *arg)
{
	webman_get((const char *)arg);
	free(arg);
	sysThreadExit(0);
}

static void webman_get_async(const char *path)
{
	char *p = strdup(path);
	sys_ppu_thread_t t;
	if (p && sysThreadCreate(&t, webman_thread, p, 1500, 0x4000, 0, (char *)"psxwebman")) free(p);
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

// At start: if an earlier run was killed while patched, put the original lv1 code back now
// (from the backup), before anything touches the drive. Returns 0 = clean, 1 = restored,
// -1 = patched but no valid backup (a reboot fixes it), -2 = unexpected code (left alone).
static int lv1_startup_check()
{
	u64 v[4];
	for (int i = 0; i < 4; i++) v[i] = lv1_peek(lv1_off[i]);
	if (lv1_is_orig(v)) { unlink(LV1_BAK); return 0; }
	if (memcmp(v, lv1_patch, sizeof(v))) { logf("startup: lv1 code unexpected, left alone"); return -2; }
	FILE *f = fopen(LV1_BAK, "rb");
	int ok = f && fread(lv1_saved, sizeof(lv1_saved), 1, f) == 1 && lv1_is_orig(lv1_saved);
	if (f) fclose(f);
	if (!ok) { logf("startup: lv1 still patched and no valid backup"); return -1; }
	lv1_patched = 1;
	lv1_storage_unpatch();
	logf("startup: lv1 was left patched by an earlier run, originals restored");
	return 1;
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
// Free space of a mounted device ("/dev_hdd0", "/dev_usb001"): lv2 syscall 840 (sys_fs_disk_free).
static int disk_free(const char *dev, uint64_t *mb)
{
	uint64_t total = 0, avail = 0;
	lv2syscall3(840, (uint64_t)dev, (uint64_t)&total, (uint64_t)&avail);
	int r = (int)p1;
	if (r == 0) *mb = avail / (1024 * 1024);
	return r;
}

// Free space at a destination in MB (0 when unknown).
static uint64_t dest_free_mb(const char *dir)
{
	uint64_t mb = 0;
	if (!strncmp(dir, "exfat", 5)) { vf_free_mb(dir, &mb); return mb; }
	char dev[16];
	snprintf(dev, sizeof(dev), "%.12s", dir);
	char *sl = strchr(dev + 1, '/');
	if (sl) *sl = 0;
	if (disk_free(dev, &mb)) mb = 0;
	return mb;
}

// Does the identified disc fit at the destination? (image + some room for cue/md5/log/sbi)
static int g_space_short = 0;   // MB missing, 0 = fits or unknown
static void space_check(uint32_t disc_mb)
{
	uint64_t fr = dest_free_mb(OUT_DIR);
	g_space_short = (fr && fr < (uint64_t)disc_mb + 2) ? (int)(disc_mb + 2 - fr) : 0;
}

static void target_name(const char *dir, char *out, size_t n)
{
	// "Hard disk (/PSXISO, 120 GB free)", "USB 0 · exFAT (/PSXISO, 3 GB free)", "USB 1 · FAT32 (...)":
	// the same for every destination; /dev_usbNNN is just the PS3's internal name
	int port;
	uint64_t mb = 0;
	if (sscanf(dir, "exfat%1d:", &port) == 1) {
		vf_free_mb(dir, &mb);
		snprintf(out, n, T(STR_TARGET_EXFAT), port, (unsigned)(mb / 1024));
		return;
	}
	char dev[16];
	snprintf(dev, sizeof(dev), "%.12s", dir);   // "/dev_hdd0" or "/dev_usb001"
	char *sl = strchr(dev + 1, '/');
	if (sl) *sl = 0;
	if (disk_free(dev, &mb)) mb = 0;
	if (sscanf(dir, "/dev_usb%3d", &port) == 1) snprintf(out, n, T(STR_TARGET_USB), port, (unsigned)(mb / 1024));
	else snprintf(out, n, T(STR_TARGET_HDD), (unsigned)(mb / 1024));
}

// Destination next to the current one in the list hard disk, USB 0, USB 1, ... (by port; a FAT32
// drive is mounted by the PS3 as /dev_usbNNN, an exFAT drive is reached through FatFs).
// dir = +1 forward, -1 back.
static void next_target(int dir)
{
	char cands[9][32];
	int nc = 0;
	snprintf(cands[nc++], 32, "/dev_hdd0/PSXISO");
	VfVolume vols[8];
	int nv = vf_scan(vols, 8);
	for (int port = 0; port < 8; port++) {
		char dev[16];
		snprintf(dev, sizeof(dev), "/dev_usb%03d", port);
		struct stat st;
		if (stat(dev, &st) == 0) { snprintf(cands[nc++], 32, "%s/PSXISO", dev); continue; }
		for (int i = 0; i < nv && i < 8; i++)
			if (vols[i].port == port) snprintf(cands[nc++], 32, "%s/PSXISO", vols[i].root);
	}
	int cur = 0;
	for (int i = 0; i < nc; i++) if (!strcmp(cands[i], g_outdir)) cur = i;
	snprintf(g_outdir, sizeof(g_outdir), "%s", cands[(cur + (dir < 0 ? nc - 1 : 1)) % nc]);
}

// ---------------------------------------------------------------- disc identification for the home screen
static char g_name[128];   // title (or volume id) of the disc in the drive
static int prepare_disc(int *err, int *code);
enum { DISC_NONE, DISC_READING, DISC_READY, DISC_ERROR };
static volatile int g_disc_state = DISC_NONE;
static volatile int g_ident_busy = 0;
static volatile int g_repairing = 0;   // a repair reads the drive
static char g_disc_title[128], g_disc_serial[16];
static uint32_t g_disc_mb;
static volatile int g_repair_n = 0;   // sectors a previous rip of this disc still has wrong (from its log)
static volatile int g_prev_rip = 0;   // the destination already has <name>.bin for this disc

// Counts the sectors in <g_base>.log that are still wrong (unreadable, bad, unconfirmed).
static int riplog_open_count()
{
	char path[320];
	snprintf(path, sizeof(path), "%s.log", g_base);
	snprintf(path + strlen(path) - 4, 8, ".bin");
	if (!vf_exists(path)) return 0;
	snprintf(path, sizeof(path), "%s.log", g_base);
	VFile *f = vf_open(path, "rb");
	if (!f) return 0;
	static char buf[65536];
	int n = (int)vf_read(f, buf, sizeof(buf) - 1), cnt = 0;
	vf_close(f);
	buf[n > 0 ? n : 0] = 0;
	for (char *l = strtok(buf, "\n"); l; l = strtok(NULL, "\n"))
		if (!strncmp(l, "unreadable=", 11) || !strncmp(l, "bad=", 4) || (!strncmp(l, "uncertain=", 10) && strstr(l, "unconfirmed")))
			cnt++;
	return cnt;
}

// Redump lookup for the home screen; runs after identification (network only, no drive access).
static char g_disc_rd[RD_NAME + 48];   // line shown on the home screen, empty = none
static volatile int g_rd_seq = 0;      // bumped when g_disc_rd changes
static volatile int g_rd_busy = 0;     // a lookup is running (redump.cpp uses static buffers)
static RdDisc g_rd_discs[4];           // online results, reused to verify the rip
static int g_rd_ndiscs = -1;           // -1 = no online lookup for the current disc
static char g_rd_serial[16];           // serial the online results belong to

static void rd_set(const char *text)
{
	snprintf(g_disc_rd, sizeof(g_disc_rd), "%s", text);
	__sync_synchronize();
	g_rd_seq++;
}

static void rd_guess_names(int hits, char (*names)[RD_NAME])
{
	char line[RD_NAME + 48];
	if (hits <= 0) snprintf(line, sizeof(line), "%s", T(STR_RD_GUESS_NONE));
	else if (hits == 1) snprintf(line, sizeof(line), T(STR_RD_GUESS), names[0]);
	else snprintf(line, sizeof(line), T(STR_RD_GUESS_MORE), names[0], hits - 1);
	rd_set(line);
}

// What Redump says about the identified disc: by size (local copy) or by serial (online).
static void redump_lookup(const char *serial, uint64_t total, int ntracks)
{
	while (g_rd_busy) usleep(10000);
	g_rd_busy = 1;
	g_rd_ndiscs = -1;
	if (g_redump == RD_LOCAL) {
		char names[1][RD_NAME];
		int hits = rd_local_by_size(total, ntracks, names, 1);
		if (hits < 0) rd_set(T(STR_RD_UNCHECKED));
		else rd_guess_names(hits, names);
		logf("redump local: %d disc(s) with %d track(s) and %llu bytes", hits, ntracks, (unsigned long long)total);
	} else if (g_redump == RD_ONLINE && serial[0]) {
		rd_set(T(STR_RD_SEARCHING));
		int nd = rd_online(serial, g_rd_discs, 4);
		if (nd < 0) { rd_set(T(STR_RD_UNCHECKED)); }
		else if (nd == 0) { rd_set(T(STR_RD_NOTFOUND)); g_rd_ndiscs = 0; }
		else {
			char names[4][RD_NAME];
			for (int i = 0; i < nd; i++) snprintf(names[i], RD_NAME, "%s", g_rd_discs[i].name);
			rd_guess_names(nd, names);
			g_rd_ndiscs = nd;
		}
		snprintf(g_rd_serial, sizeof(g_rd_serial), "%s", serial);
	}
	g_rd_busy = 0;
}

// Re-run the lookup for the disc already shown (the Redump setting was changed).
static char g_id_serial[16];
static uint64_t g_id_total;
static int g_id_ntracks;
static void lookup_thread(void *arg)
{
	rd_set("");
	if (g_redump != RD_OFF) redump_lookup(g_id_serial, g_id_total, g_id_ntracks);
	sysThreadExit(0);
}

static void lookup_start()
{
	sys_ppu_thread_t t;
	sysThreadCreate(&t, lookup_thread, NULL, 1000, 0x10000, 0, (char *)"psxlookup");
}

// Compares the track MD5s of the finished rip with Redump. Fills `out` with the result line.
static int g_rd_result = -3;            // last verification: 1 verified, 0 no match, -1 unchecked, -2 not found, -3 off
static char g_rd_result_name[RD_NAME];
static void redump_verify(char *out, size_t n)
{
	char name[RD_NAME] = "";
	int r;   // 1 = verified, 0 = no match, -1 = could not check, -2 = serial unknown to Redump
	while (g_rd_busy) usleep(10000);
	if (g_redump == RD_LOCAL) {
		r = rd_local_verify(g_track_md5bin, g_ntracks, name, sizeof(name));
	} else if (!g_serial[0]) {
		r = -1;
	} else {
		g_rd_busy = 1;
		if (g_rd_ndiscs < 0 || strcmp(g_rd_serial, g_serial)) {
			g_rd_ndiscs = rd_online(g_serial, g_rd_discs, 4);
			snprintf(g_rd_serial, sizeof(g_rd_serial), "%s", g_serial);
		}
		g_rd_busy = 0;
		if (g_rd_ndiscs < 0) r = -1;
		else if (g_rd_ndiscs == 0) r = -2;
		else {
			r = 0;
			for (int i = 0; i < g_rd_ndiscs && r == 0; i++) {
				const RdDisc *d = &g_rd_discs[i];
				int ok = d->n == g_ntracks;
				for (int t = 0; t < g_ntracks && ok; t++) ok = !memcmp(d->md5[t], g_track_md5bin[t], 16);
				if (ok) { r = 1; snprintf(name, sizeof(name), "%s", d->name); }
			}
		}
	}
	if (r == 1) snprintf(out, n, T(STR_RD_VERIFIED), name);
	else if (r == 0) snprintf(out, n, "%s", T(STR_RD_NOMATCH));
	else if (r == -2) snprintf(out, n, "%s", T(STR_RD_NOTFOUND));
	else snprintf(out, n, "%s", T(STR_RD_UNCHECKED));
	logf("redump verify (%s) -> %d %s", g_redump == RD_LOCAL ? "local" : "online", r, name);
	g_rd_result = r;
	snprintf(g_rd_result_name, sizeof(g_rd_result_name), "%s", name);
}

// Is there already a rip of the identified disc in the *current* destination? g_base keeps the
// file name; its directory follows OUT_DIR, which the settings may have changed since.
static void prev_rip_check()
{
	const char *slash = strrchr(g_base, '/');
	if (slash) {
		char name[200];
		snprintf(name, sizeof(name), "%s", slash + 1);
		snprintf(g_base, sizeof(g_base), "%s/%s", OUT_DIR, name);
	}
	if (!strncmp(OUT_DIR, "exfat", 5)) { VfVolume v[8]; vf_scan(v, 8); }   // mounted before looking
	char bp[320];
	snprintf(bp, sizeof(bp), "%s.bin", g_base);
	g_prev_rip = vf_exists(bp);
	g_repair_n = g_prev_rip ? riplog_open_count() : 0;
	space_check(g_disc_mb ? g_disc_mb : (uint32_t)((uint64_t)g_leadout * RAW_SIZE / (1024 * 1024)));
}

static void fire_event(const char *event);
static char g_state_title[128], g_state_serial[16];
static void identify_thread(void *arg)
{
	if ((long)arg) usleep(2000 * 1000);   // freshly inserted: let the drive spin up
	int err = 0, code = 0;
	int r = prepare_disc(&err, &code);
	if (r == 0) sys_storage_close(g_fd);
	// Undo the lv1 patch straight away: if the app is then killed (webMAN exit, XMB quit) the
	// hypervisor would stay patched and the next system storage operation (pkg install) hangs.
	lv1_storage_unpatch();
	rd_set("");
	if (r == 0) {
		snprintf(g_disc_title, sizeof(g_disc_title), "%s", g_name);
		snprintf(g_disc_serial, sizeof(g_disc_serial), "%s", g_serial[0] ? g_serial : T(STR_UNKNOWN));
		g_disc_mb = (uint32_t)((uint64_t)g_leadout * RAW_SIZE / (1024 * 1024));
		prev_rip_check();
		snprintf(g_state_title, sizeof(g_state_title), "%s", g_name);
		snprintf(g_state_serial, sizeof(g_state_serial), "%s", g_serial);
		// only for a disc inserted while the app runs; a disc already in the drive at start is part
		// of app_started, and the home screen identifying the same disc again sends nothing
		if ((long)arg) fire_event("disc_identified");
		if (g_repair_n) logf("previous rip has %d sector(s) to repair", g_repair_n);
		__sync_synchronize();
		g_disc_state = DISC_READY;
	} else {
		g_disc_state = (r == -2) ? DISC_NONE : DISC_ERROR;
		if (r != -2 && (long)arg) {   // a disc was inserted but is not a readable PS1 disc
			g_state_title[0] = 0; g_state_serial[0] = 0;
			fire_event("disc_not_identified");
		}
	}
	logf("identify -> %d (state %d)", r, g_disc_state);
	static int started_sent = 0;
	if (!started_sent) {   // first identification after the app started: report the start once
		started_sent = 1;
		if (r != 0) { g_state_title[0] = 0; g_state_serial[0] = 0; }
		fire_event("app_started");
	}
	snprintf(g_id_serial, sizeof(g_id_serial), "%s", g_serial);
	g_id_total = (uint64_t)g_leadout * RAW_SIZE;
	g_id_ntracks = g_ntracks;
	g_ident_busy = 0;   // the drive is free; the lookup below only uses the network
	if (r == 0 && g_redump != RD_OFF) redump_lookup(g_id_serial, g_id_total, g_id_ntracks);
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
enum { B_CROSS = 1, B_CIRCLE = 2, B_TRIANGLE = 4, B_UP = 8, B_DOWN = 16, B_LEFT = 32, B_RIGHT = 64, B_START = 128, B_SQUARE = 256, B_SELECT = 512 };

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
				if (pd.BTN_SELECT) b |= B_SELECT;
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

// Top of the button hint bar: low on the screen (inside the TV safe area), so lists above it get room.
static int hint_y() { return ui_h() - ui_h() * 4 / 100 - ui_line(UI_BODY); }

// Small grey note just above the hint bar, at the same place on every screen.
static void footer_note(const char *text)
{
	ui_text(UI_SMALL, ui_w() / 2, hint_y() - ui_line(UI_SMALL) * 2, 0x6870A0, text, UI_CENTER);
}

static void hint_bar(const char *const *buttons, const char *const *labels, int n)
{
	int total = 0, gap = ui_w() / 24;
	for (int i = 0; i < n; i++) total += ui_hint_width(buttons[i], labels[i]) + (i ? gap : 0);
	int x = (ui_w() - total) / 2, y = hint_y();
	for (int i = 0; i < n; i++) x += ui_hint(x, y, buttons[i], labels[i]) + gap;
}

static void flip_frame()
{
	sysUtilCheckCallback();
	present();
}

// Date (and time) in the console's date and time format.
static void fmt_datetime(int y, int mo, int d, int h, int mi, int with_time, char *out, size_t n)
{
	s32 df = 0, tf = 1;   // 0 = YYYYMMDD, 1 = DDMMYYYY, 2 = MMDDYYYY; 0 = 12 h, 1 = 24 h
	sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_DATE_FORMAT, &df);
	sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_TIME_FORMAT, &tf);
	char date[16], tm[16] = "";
	if (df == 1) snprintf(date, sizeof(date), "%02d/%02d/%04d", d, mo, y);
	else if (df == 2) snprintf(date, sizeof(date), "%02d/%02d/%04d", mo, d, y);
	else snprintf(date, sizeof(date), "%04d-%02d-%02d", y, mo, d);
	if (with_time) {
		if (tf == 0) snprintf(tm, sizeof(tm), " %d:%02d %s", (h + 11) % 12 + 1, mi, h < 12 ? "AM" : "PM");
		else snprintf(tm, sizeof(tm), " %02d:%02d", h, mi);
	}
	snprintf(out, n, "%s%s", date, tm);
}

// Redump's dat version ("2026-06-15 11-55-46") in the console's format.
static void fmt_rd_version(const char *ver, char *out, size_t n)
{
	int y, mo, d, h = 0, mi = 0;
	int k = sscanf(ver, "%d-%d-%d %d-%d", &y, &mo, &d, &h, &mi);
	if (k < 3) { snprintf(out, n, "%s", ver); return; }
	fmt_datetime(y, mo, d, h, mi, k == 5, out, n);
}

// A UTC time stamp as local time (console time zone and summer time).
static void fmt_local(time_t t, char *out, size_t n)
{
	s32 tz = 0, dst = 0;   // minutes from UTC; summer time on/off
	sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_TIMEZONE, &tz);
	sysUtilGetSystemParamInt(SYSUTIL_SYSTEMPARAM_ID_SUMMERTIME, &dst);
	t += (time_t)(tz + (dst ? 60 : 0)) * 60;
	struct tm *g = gmtime(&t);
	if (!g) { out[0] = 0; return; }
	fmt_datetime(g->tm_year + 1900, g->tm_mon + 1, g->tm_mday, g->tm_hour, g->tm_min, 1, out, n);
}

// Own progress screen (instead of the system dialog, whose button reads "Back"): title, bar,
// detail text and "O Cancel". Draws only when called; the caller decides when something changed.
static void progress_draw(const char *title, const char *detail, uint32_t pct, const char *note = NULL, int cancelling = 0,
                          const char *timeline = NULL)
{
	if (pct > 100) pct = 100;
	draw_backdrop();
	// layout, top to bottom: title, (gap), sector count, bar, percentage, status line
	int lb = ui_line(UI_BODY), ls = ui_line(UI_SMALL);
	int ty = ui_h() * 2 / 5 - lb, y = ty + lb * 3;
	int bw = ui_w() * 3 / 5, bx = (ui_w() - bw) / 2, bh = lb / 3;
	ui_text(UI_BODY, ui_w() / 2, ty, 0xFFFFFF, title, UI_CENTER);
	if (detail && detail[0]) ui_text(UI_SMALL, bx + bw, y - ls - ls / 3, 0xA8B0C8, detail, UI_RIGHT);
	ui_rect(bx, y, bw, bh, 0x50FFFFFF);
	ui_rect(bx, y, (int)((uint64_t)bw * pct / 100), bh, 0xFFC8E05A);
	char p[16];
	snprintf(p, sizeof(p), "%u %%", pct);
	ui_text(UI_SMALL, ui_w() / 2, y + bh + ls / 2, 0xE8E8F0, p, UI_CENTER);
	if (timeline && timeline[0]) ui_text(UI_SMALL, ui_w() / 2, y + bh + ls * 2, 0xA8B0C8, timeline, UI_CENTER);
	if (cancelling) note = T(STR_CANCELLING);   // O was pressed: say what we are waiting for
	int ny = y + bh + ls * (timeline && timeline[0] ? 7 : 4) / 2;
	if (note && note[0]) ui_text(UI_SMALL, ui_w() / 2, ny, 0xF0C060, note, UI_CENTER);
	// event feed (rip and repair only): newest at the bottom
	if (timeline && timeline[0]) {
		int n = g_nev, ey = ny + ls * 2;
		for (int i = 0; i < n; i++) ui_text(UI_SMALL, ui_w() / 2, ey + i * ls * 11 / 10, g_ev[i].col, g_ev[i].text, UI_CENTER);
	}
	if (!cancelling) {
		const char *b[] = {"circle"};
		const char *l[] = {T(STR_HINT_CANCEL)};
		hint_bar(b, l, 1);
	}
	g_drawn = 2;
	g_backdrop_shown = 0;   // a system dialog shown afterwards needs its plain backdrop again
}

static void fmt_dur(char *out, size_t n, long secs)
{
	if (secs >= 3600) snprintf(out, n, "%ld:%02ld:%02ld", secs / 3600, (secs / 60) % 60, secs % 60);
	else snprintf(out, n, "%ld:%02ld", secs / 60, secs % 60);
}

// "Time 2:14 · about 1:35 left · 2.6 MB/s": speed over the last ~20 s (the drive is faster on the
// outer part of the disc and slow in damaged areas), remaining time from that speed.
struct Eta { uint64_t t0, tw; uint32_t pw; double rate; };
static void eta_start(Eta *e) { e->t0 = e->tw = now_ms(); e->pw = 0; e->rate = 0; }
static void eta_line(Eta *e, uint32_t pos, uint32_t total, char *out, size_t n)
{
	uint64_t t = now_ms();
	if (t - e->tw >= 2000) {   // sample every 2 s, smooth over ~20 s
		double r = (double)(pos - e->pw) * 1000.0 / (double)(t - e->tw);
		e->rate = e->rate > 0 ? e->rate * 0.9 + r * 0.1 : r;
		e->tw = t; e->pw = pos;
	}
	char el[16], left[16];
	fmt_dur(el, sizeof(el), (long)((t - e->t0) / 1000));
	if (e->rate > 1 && t - e->t0 > 6000) {
		fmt_dur(left, sizeof(left), (long)((total - pos) / e->rate));
		unsigned kbs = (unsigned)(e->rate * RAW_SIZE / 1024);
		if (g_phase) snprintf(out, n, T(STR_TIME_LEFT), el, left);   // later passes: no meaningful speed
		else snprintf(out, n, T(STR_TIME_ETA), el, left, kbs / 1024, (kbs % 1024) * 10 / 1024);
	} else {
		snprintf(out, n, T(STR_TIME), el);
	}
}

// Downloads the Redump dat to the console and builds the local index (progress dialog, O cancels).
static volatile uint32_t g_dl_done, g_dl_total;
static volatile int g_dl_phase, g_dl_abort, g_dl_result, g_dl_finished;
static void download_thread(void *arg)
{
	while (g_rd_busy) usleep(10000);
	g_rd_busy = 1;
	g_dl_result = rd_download(&g_dl_done, &g_dl_total, &g_dl_phase, &g_dl_abort);
	g_rd_busy = 0;
	g_dl_finished = 1;
	sysThreadExit(0);
}

static void redump_download_screen()
{
	g_dl_done = g_dl_total = 0; g_dl_phase = 0; g_dl_abort = 0; g_dl_finished = 0;
	sys_ppu_thread_t tid;
	sysThreadCreate(&tid, download_thread, NULL, 1000, 0x10000, THREAD_JOINABLE, (char *)"psxredump");
	pad_pressed();
	uint32_t shown = 101, shown_kb = ~0u;
	int last_phase = -1, shown_abort = 0;
	while (!g_dl_finished) {
		if ((pad_pressed() & B_CIRCLE) || exit_requested) g_dl_abort = 1;
		// each phase runs the bar from 0 to 100 %: first the download (bytes), then the index
		uint32_t tot = g_dl_total ? g_dl_total : 1;
		uint32_t pct = g_dl_total ? (uint32_t)((uint64_t)g_dl_done * 100 / tot) : 0;
		uint32_t kb = g_dl_done / 1024;
		if (pct != shown || g_dl_phase != last_phase || (!g_dl_phase && kb / 64 != shown_kb / 64) || g_dl_abort != shown_abort) {
			char pm[96];
			if (g_dl_phase) snprintf(pm, sizeof(pm), "%s", T(STR_RD_INDEXING));
			else snprintf(pm, sizeof(pm), T(STR_RD_DL_PROGRESS), kb, g_dl_total / 1024);
			progress_draw(T(STR_RD_DOWNLOADING), pm, pct, NULL, g_dl_abort);
			shown = pct; shown_kb = kb; last_phase = g_dl_phase; shown_abort = g_dl_abort;
		}
		flip_frame();
	}
	u64 rv;
	sysThreadJoin(tid, &rv);
	logf("redump download result %d", g_dl_result);
	if (g_dl_result == -7 || exit_requested) return;   // cancelled
	char msg[256], ver[64] = "";
	if (g_dl_result > 0) {
		char raw[64] = "";
		rd_local_info(raw, sizeof(raw), NULL);
		fmt_rd_version(raw, ver, sizeof(ver));
		snprintf(msg, sizeof(msg), T(STR_RD_DL_DONE), ver, g_dl_result);
	}
	else snprintf(msg, sizeof(msg), T(STR_RD_DL_FAILED), g_dl_result);
	tell(msg);
}

// Shows a system notification (top right) through webMAN: "Watch the screen at http://ip:8090/".
static void notify_url(StrId fmt, const char *page)
{
	char ip[20], text[200], url[600];
	remote_ip(ip, sizeof(ip));
	snprintf(text, sizeof(text), T(fmt), ip, REMOTE_PORT);
	(void)page;
	size_t o = snprintf(url, sizeof(url), "/popup.ps3/");
	for (const unsigned char *p = (const unsigned char *)text; *p && o + 4 < sizeof(url); p++) {
		if (isalnum(*p) || strchr("-_.~/:", *p)) url[o++] = *p;
		else o += snprintf(url + o, sizeof(url) - o, "%%%02X", *p);
	}
	url[o] = 0;
	webman_get_async(url);
}

// PS3 on-screen keyboard: edits `text` (UTF-8, BMP characters). Returns 1 when OK was pressed.
static int osk_input(const char *title, char *text, size_t n)
{
	static u16 msg[128], start[512], result[512];
	size_t o = 0;
	for (const char *p = title; *p && o < 127; ) o < 127 ? (void)(msg[o++] = (u16)ui_next_cp(&p)) : (void)0;
	msg[o] = 0;
	o = 0;
	for (const char *p = text; *p && o < 511; ) start[o++] = (u16)ui_next_cp(&p);
	start[o] = 0;
	sys_mem_container_t cont;
	if (sysMemContainerCreate(&cont, 4 * 1024 * 1024)) return 0;
	oskParam prm;
	memset(&prm, 0, sizeof(prm));
	prm.allowedPanels = OSK_PANEL_TYPE_URL | OSK_PANEL_TYPE_ALPHABET | OSK_PANEL_TYPE_NUMERAL;
	prm.firstViewPanel = OSK_PANEL_TYPE_URL;
	prm.prohibitFlags = OSK_PROHIBIT_RETURN;
	oskInputFieldInfo fld;
	fld.message = msg; fld.startText = start; fld.maxLength = 500;
	oskSetKeyLayoutOption(OSK_FULLKEY_PANEL);
	g_osk_event = 0;
	if (oskLoadAsync(cont, &prm, &fld)) { sysMemContainerDestroy(cont); return 0; }
	oskCallbackReturnParam ret;
	memset(&ret, 0, sizeof(ret));
	ret.str = result; ret.len = 500;
	int unloading = 0;
	while (!exit_requested) {
		do_flip();
		if (!unloading && (g_osk_event == SYSUTIL_OSK_DONE || g_osk_event == SYSUTIL_OSK_INPUT_CANCELED)) {
			oskUnloadAsync(&ret);
			unloading = 1;
		}
		if (g_osk_event == SYSUTIL_OSK_UNLOADED) break;
	}
	sysMemContainerDestroy(cont);
	g_redraw = 2;
	g_backdrop_shown = 0;
	if (ret.res != OSK_OK && ret.res != OSK_NO_TEXT) return 0;
	o = 0;
	for (int i = 0; result[i] && o + 4 < n; i++) {   // UTF-16 -> UTF-8
		u16 c = result[i];
		if (c < 0x80) text[o++] = (char)c;
		else if (c < 0x800) { text[o++] = (char)(0xC0 | c >> 6); text[o++] = (char)(0x80 | (c & 63)); }
		else { text[o++] = (char)(0xE0 | c >> 12); text[o++] = (char)(0x80 | ((c >> 6) & 63)); text[o++] = (char)(0x80 | (c & 63)); }
	}
	text[o] = 0;
	return 1;
}

static void set_webhook(const char *url);
static int test_webhook();

// The extra headers, each edited with the on-screen keyboard; only the name is shown.
static void webhook_headers_screen()
{
	int row = 0;
	pad_pressed();
	g_redraw = 2;
	while (!exit_requested) {
		if (g_redraw > 0) {
			g_redraw = 0;
			draw_backdrop();
			ui_text(UI_BODY, ui_w() / 2, ui_h() / 10 + ui_line(UI_TITLE) + ui_line(UI_BODY) / 2, 0xA8B0C8, T(STR_WH_HEADERS), UI_CENTER);
			int top = ui_h() / 10 + ui_line(UI_TITLE) + ui_line(UI_BODY) * 2, lh = ui_line(UI_BODY) * 17 / 10;
			int left = ui_w() / 6, right = ui_w() * 5 / 6;
			for (int r = 0; r < WH_HEADERS; r++) {
				int y = top + r * lh;
				char label[32], val[160];
				const char *h = g_webhook_hdr[r];
				snprintf(label, sizeof(label), T(STR_WH_HEADER_N), r + 1);
				if (h[0]) { const char *c = strchr(h, ':'); snprintf(val, sizeof(val), "%.*s: ••••", (int)(c ? c - h : 0), h); }
				else snprintf(val, sizeof(val), "%s", T(STR_WH_NONE));
				if (r == row) ui_rect(left - ui_w() / 60, y - lh / 4, right - left + ui_w() / 30, lh, 0x703D6FD9);
				ui_text(UI_BODY, left, y, 0xFFFFFF, label);
				ui_text(UI_BODY, right, y, r == row ? 0xFFFFFF : 0xC8D0E0, val, UI_RIGHT);
			}
			const char *b[] = {"updown", "cross", "circle"};
			const char *l[] = {T(STR_HINT_SELECT), T(STR_HINT_EDIT), T(STR_HINT_BACK)};
			hint_bar(b, l, 3);
			g_drawn = 2;
			g_backdrop_shown = 0;
		}
		flip_frame();
		int p = pad_pressed();
		if (p) g_redraw = 2;
		if (p & B_UP) row = (row + WH_HEADERS - 1) % WH_HEADERS;
		if (p & B_DOWN) row = (row + 1) % WH_HEADERS;
		if (p & B_CIRCLE) return;
		if (p & B_CROSS) {
			char h[256];
			snprintf(h, sizeof(h), "%s", g_webhook_hdr[row]);
			if (osk_input(T(STR_WH_HEADER_Q), h, sizeof(h))) {
				clean_header(h);
				snprintf(g_webhook_hdr[row], sizeof(g_webhook_hdr[0]), "%s", h);
				cfg_save();
				logf("webhook header %d %s", row + 1, h[0] ? "set" : "cleared");   // never the value
			}
			pad_pressed();
		}
	}
}

// Webhook settings: address (on-screen keyboard), each event on/off, progress step (off or every
// 1 ... 50 %), and a test. The values live in CFG_PATH and can also be edited there over FTP.
static void webhook_settings_screen()
{
	// rows: 0 address, events except progress, progress, test
	int ev[WH_COUNT], nev = 0;
	for (int i = 0; i < WH_COUNT; i++) if (i != WH_RIP_PROGRESS) ev[nev++] = i;
	const int R_ADDR = 0, R_HDR = 1, R_EV = 2, R_PROG = R_EV + nev, R_TEST = R_PROG + 1, NROWS = R_TEST + 1, VIS = 9;
	int row = 0, first = 0, test_status = 0;
	pad_pressed();
	g_redraw = 2;
	while (!exit_requested) {
		if (g_redraw > 0) {
			g_redraw = 0;
			draw_backdrop();
			ui_text(UI_BODY, ui_w() / 2, ui_h() / 10 + ui_line(UI_TITLE) + ui_line(UI_BODY) / 2, 0xA8B0C8, T(STR_WH_SETTINGS), UI_CENTER);
			if (row < first) first = row;
			if (row >= first + VIS) first = row - VIS + 1;
			int top = ui_h() / 10 + ui_line(UI_TITLE) + ui_line(UI_BODY) * 2, lh = ui_line(UI_BODY) * 17 / 10;
			int left = ui_w() / 6, right = ui_w() * 5 / 6;
			for (int r = first; r < NROWS && r < first + VIS; r++) {
				int y = top + (r - first) * lh;
				char label[160], val[160];
				if (r == R_ADDR) {
					snprintf(label, sizeof(label), "%s", T(STR_WH_ADDRESS));
					if (g_webhook[0]) webhook_host(g_webhook, val, sizeof(val)); else snprintf(val, sizeof(val), "%s", T(STR_WH_NONE));
				} else if (r == R_HDR) {
					int nh = 0;
					for (int i = 0; i < WH_HEADERS; i++) nh += g_webhook_hdr[i][0] != 0;
					snprintf(label, sizeof(label), "%s", T(STR_WH_HEADERS));
					if (g_webhook_hdr_on) snprintf(val, sizeof(val), T(STR_WH_HEADERS_ON), nh); else snprintf(val, sizeof(val), "%s", T(STR_OFF));
				} else if (r < R_PROG) {
					int i = ev[r - R_EV];
					snprintf(label, sizeof(label), "%s", T(wh_label(i)));
					snprintf(val, sizeof(val), "%s", (g_webhook_mask >> i) & 1 ? T(STR_ON) : T(STR_OFF));
				} else if (r == R_PROG) {
					snprintf(label, sizeof(label), "%s", T(STR_WH_PROGRESS));
					if ((g_webhook_mask >> WH_RIP_PROGRESS) & 1) snprintf(val, sizeof(val), T(STR_WH_PROGRESS_N), g_webhook_step);
					else snprintf(val, sizeof(val), "%s", T(STR_OFF));
				} else {
					snprintf(label, sizeof(label), "%s", T(STR_WH_TEST));
					if (test_status) snprintf(val, sizeof(val), "HTTP %d", test_status); else val[0] = 0;
				}
				if (r == row) ui_rect(left - ui_w() / 60, y - lh / 4, right - left + ui_w() / 30, lh, 0x703D6FD9);
				ui_text(UI_BODY, left, y, 0xFFFFFF, label);
				char v[200];
				int toggle = r >= R_HDR && r != R_TEST;
				snprintf(v, sizeof(v), r == row && toggle ? "‹ %s ›" : "%s", val);
				ui_text(UI_BODY, right, y, r == row ? 0xFFFFFF : 0xC8D0E0, v, UI_RIGHT);
			}
			if (row == R_HDR) {   // on/off with left/right, X edits the headers
				const char *b[] = {"updown", "leftright", "cross", "circle"};
				const char *l[] = {T(STR_HINT_SELECT), T(STR_HINT_CHANGE), T(STR_HINT_EDIT), T(STR_HINT_BACK)};
				hint_bar(b, l, 4);
			} else if (row == R_ADDR || row == R_TEST) {
				const char *b[] = {"updown", "cross", "circle"};
				const char *l[] = {T(STR_HINT_SELECT), T(row == R_TEST ? STR_HINT_SEND : STR_HINT_EDIT), T(STR_HINT_BACK)};
				hint_bar(b, l, 3);
			} else {
				const char *b[] = {"updown", "leftright", "circle"};
				const char *l[] = {T(STR_HINT_SELECT), T(STR_HINT_CHANGE), T(STR_HINT_BACK)};
				hint_bar(b, l, 3);
			}
			g_drawn = 2;
			g_backdrop_shown = 0;
		}
		flip_frame();
		int p = pad_pressed();
		if (p) g_redraw = 2;
		if (p & B_UP) row = (row + NROWS - 1) % NROWS;
		if (p & B_DOWN) row = (row + 1) % NROWS;
		if (p & B_CIRCLE) return;
		if ((p & B_CROSS) && row == R_ADDR) {
			char url[512];
			snprintf(url, sizeof(url), "%s", g_webhook[0] ? g_webhook : "http://");
			if (osk_input(T(STR_WH_ADDRESS_Q), url, sizeof(url))) set_webhook(!strcmp(url, "http://") ? "" : url);
			pad_pressed();
			continue;
		}
		if ((p & B_CROSS) && row == R_HDR) { webhook_headers_screen(); pad_pressed(); g_redraw = 2; continue; }
		if ((p & B_CROSS) && row == R_TEST) { test_status = test_webhook(); continue; }
		int d = (p & B_RIGHT) ? 1 : (p & B_LEFT) ? -1 : 0;
		if ((p & B_CROSS) && row >= R_EV && row < R_PROG) d = 1;   // X toggles an event too
		if (!d) continue;
		if (row == R_HDR) {
			g_webhook_hdr_on = !g_webhook_hdr_on;
		} else if (row >= R_EV && row < R_PROG) {
			g_webhook_mask ^= 1 << ev[row - R_EV];
		} else if (row == R_PROG) {
			// Off, 1, 2, 5, 10, 15, 25, 50 %
			int k = (g_webhook_mask >> WH_RIP_PROGRESS) & 1 ? 1 : 0;
			if (k) { int j = 0; while (j < 7 && wh_steps[j] != g_webhook_step) j++; k = j + 1; }
			k = (k + (d > 0 ? 1 : 7)) % 8;
			if (k == 0) g_webhook_mask &= ~(1 << WH_RIP_PROGRESS);
			else { g_webhook_mask |= 1 << WH_RIP_PROGRESS; g_webhook_step = wh_steps[k - 1]; }
		} else continue;
		cfg_save();
	}
}

static void settings_screen()
{
	// edit locally; each change is applied and saved at once, O goes back
	char outdir[64];
	snprintf(outdir, sizeof(outdir), "%s", g_outdir);
	int retries = g_retries, speed = g_speed, eject = g_eject, autostart = g_autostart, redump = g_redump, row = 0;
	int remote = g_remote, webhook_on = g_webhook_on, check = g_check, whstep = g_webhook_step;
	const int NROWS = 10, VIS = 9;   // the list scrolls when it is longer than VIS rows
	int first = 0;
	char rdver[64] = "";
	time_t rdwhen = 0;
	int rdcount = rd_local_info(rdver, sizeof(rdver), &rdwhen);
	static const int steps[] = {1, 2, 4, 8, 16, 32};
	pad_pressed();
	g_redraw = 2;
	while (!exit_requested) {
		if (g_redraw > 0) {
		g_redraw = 0;
		g_drawn = 2;
		draw_backdrop();
		ui_text(UI_BODY, ui_w() / 2, ui_h() / 10 + ui_line(UI_TITLE) + ui_line(UI_BODY) / 2, 0xA8B0C8, T(STR_SETTINGS_TITLE), UI_CENTER);
		char vals[11][96];
		char save_dir[64]; snprintf(save_dir, sizeof(save_dir), "%s", g_outdir);
		snprintf(g_outdir, sizeof(g_outdir), "%s", outdir);
		target_name(outdir, vals[0], sizeof(vals[0]));
		if (g_disc_state == DISC_READY && g_disc_mb && dest_free_mb(outdir) && dest_free_mb(outdir) < (uint64_t)g_disc_mb + 2) {
			char t[96];
			snprintf(t, sizeof(t), "(!) %s", vals[0]);   // the disc in the drive does not fit here
			snprintf(vals[0], sizeof(vals[0]), "%s", t);
		}
		snprintf(g_outdir, sizeof(g_outdir), "%s", save_dir);
		snprintf(vals[1], sizeof(vals[1]), "%d", retries);
		if (speed) snprintf(vals[2], sizeof(vals[2]), "%d\xC3\x97", speed_x[speed]); else snprintf(vals[2], sizeof(vals[2]), "%s", T(STR_SPEED_MAX));
		snprintf(vals[3], sizeof(vals[3]), "%s", eject ? T(STR_ON) : T(STR_OFF));
		snprintf(vals[4], sizeof(vals[4]), "%s", autostart ? T(STR_ON) : T(STR_OFF));
		snprintf(vals[5], sizeof(vals[5]), "%s", redump == RD_ONLINE ? T(STR_RD_ONLINE) : redump == RD_LOCAL ? T(STR_RD_LOCAL) : T(STR_OFF));
		if (rdcount > 0) {
			// Redump's version (date) and when this copy was downloaded (date and time)
			char date[16], ver[24], when[32];
			snprintf(date, sizeof(date), "%.10s", rdver);
			fmt_rd_version(date, ver, sizeof(ver));
			fmt_local(rdwhen, when, sizeof(when));
			snprintf(vals[6], sizeof(vals[6]), T(STR_RD_COPY_VALUE), ver, when);
		} else {
			snprintf(vals[6], sizeof(vals[6]), "%s", T(STR_RD_MISSING));
		}
		char ip[20];
		remote_ip(ip, sizeof(ip));
		if (remote) snprintf(vals[7], sizeof(vals[7]), T(STR_REMOTE_ON), ip, REMOTE_PORT);
		else snprintf(vals[7], sizeof(vals[7]), "%s", T(STR_OFF));
		if (!webhook_on) snprintf(vals[8], sizeof(vals[8]), "%s", T(STR_OFF));
		else if (!g_webhook[0]) snprintf(vals[8], sizeof(vals[8]), "%s", T(STR_WEBHOOK_NOURL));
		else {
			char host[96];
			webhook_host(g_webhook, host, sizeof(host));
			int ne = 0;
			for (int i = 0; i < WH_COUNT; i++) ne += (g_webhook_mask >> i) & 1;
			snprintf(vals[8], sizeof(vals[8]), T(STR_WEBHOOK_ON_N), host, ne);
		}
		snprintf(vals[9], sizeof(vals[9]), "%s", check == CHECK_OFF ? T(STR_OFF) : check == CHECK_ON ? T(STR_CHECK_ON) : T(STR_CHECK_BITPERFECT));
		const char *labels[10] = {T(STR_L_TARGET), T(STR_L_RETRIES), T(STR_L_SPEED), T(STR_L_EJECT), T(STR_L_AUTOSTART), T(STR_L_REDUMP), T(STR_L_RDCOPY),
		                          T(STR_L_REMOTE), T(STR_L_WEBHOOK), T(STR_L_CHECK)};
		if (row < first) first = row;
		if (row >= first + VIS) first = row - VIS + 1;
		// first row below the "Settings" subtitle (drawn at h/10 + title + body/2), then nine rows
		int top = ui_h() / 10 + ui_line(UI_TITLE) + ui_line(UI_BODY) * 2, lh = ui_line(UI_BODY) * 17 / 10, left = ui_w() / 6, right = ui_w() * 5 / 6;
		for (int r = first; r < NROWS && r < first + VIS; r++) {
			int y = top + (r - first) * lh;
			if (r == row) ui_rect(left - ui_w() / 60, y - lh / 4, right - left + ui_w() / 30, lh * 3 / 4 + lh / 4, 0x703D6FD9);
			ui_text(UI_BODY, left, y, 0xFFFFFF, labels[r]);
			char v[128];
			snprintf(v, sizeof(v), r == row && r != 6 ? "‹ %s ›" : "%s", vals[r]);   // row 6 is an action
			ui_text(UI_BODY, right, y, r == row ? 0xFFFFFF : 0xC8D0E0, v, UI_RIGHT);
		}
		{   // all settings live in one file that can also be edited over FTP
			char foot[200];
			snprintf(foot, sizeof(foot), T(STR_WH_CFG_PATH), CFG_PATH);
			footer_note(foot);
		}
		if (row == 8) {
			const char *b[] = {"updown", "leftright", "cross", "triangle", "circle"};
			const char *l[] = {T(STR_HINT_SELECT), T(STR_HINT_CHANGE), T(STR_HINT_SHOW), T(STR_HINT_DEFAULTS), T(STR_HINT_BACK)};
			hint_bar(b, l, 5);
		} else {
			const char *b[] = {"updown", row == 6 ? "cross" : "leftright", "triangle", "circle"};
			const char *l[] = {T(STR_HINT_SELECT), row == 6 ? T(STR_HINT_DOWNLOAD) : T(STR_HINT_CHANGE), T(STR_HINT_DEFAULTS), T(STR_HINT_BACK)};
			hint_bar(b, l, 4);
		}
		}
		flip_frame();
		int p = pad_pressed();
		if (p) g_redraw = 2;
		if (p & B_UP) row = (row + NROWS - 1) % NROWS;
		if (p & B_DOWN) row = (row + 1) % NROWS;
		if ((p & B_CROSS) && row == 8) {
			webhook_settings_screen();
			pad_pressed();
			g_redraw = 2;
			continue;
		}
		if ((p & B_CROSS) && row == 6) {
			redump_download_screen();
			rdcount = rd_local_info(rdver, sizeof(rdver), &rdwhen);
			pad_pressed();
			g_redraw = 2;
			continue;
		}
		int d = (p & B_RIGHT) ? 1 : (p & B_LEFT) ? -1 : 0;
		if (row == 6) d = 0;
		int reset = 0;
		if (p & B_TRIANGLE) {
			reset = own_dialog(T(STR_RESET_Q), 1) == 1;
			pad_pressed();
			g_redraw = 2;
			if (reset) {   // defaults; the webhook address and the Redump copy are kept
				snprintf(outdir, sizeof(outdir), "/dev_hdd0/PSXISO");
				retries = 4; speed = 0; eject = 1; autostart = 0; redump = RD_OFF;
				if (remote) remote_set(0);
				remote = 0; webhook_on = 0; check = CHECK_BITPERFECT; whstep = 5;   // step: web page only
			}
		}
		if (d && !reset) {
			if (row == 0) {
				char save_dir2[64]; snprintf(save_dir2, sizeof(save_dir2), "%s", g_outdir);
				snprintf(g_outdir, sizeof(g_outdir), "%s", outdir);
				next_target(d);
				snprintf(outdir, sizeof(outdir), "%s", g_outdir);
				snprintf(g_outdir, sizeof(g_outdir), "%s", save_dir2);
			} else if (row == 1) {
				int k = 0; while (k < 6 && steps[k] != retries) k++;
				retries = steps[(k + (d > 0 ? 1 : 5)) % 6];
			} else if (row == 2) {
				// right = faster: 2x, 4x, 8x, max (index 3 ... 0)
				if (d > 0 && speed > 0) speed--;
				if (d < 0 && speed < 3) speed++;
			} else if (row == 3) {
				eject = !eject;
			} else if (row == 4) {
				autostart = !autostart;
			} else if (row == 5) {
				redump = (redump + (d > 0 ? 1 : 2)) % 3;
			} else if (row == 7) {
				remote = !remote;
				remote_set(remote);
				if (remote) notify_url(STR_REMOTE_HELP, "");
			} else if (row == 8) {
				webhook_on = !webhook_on;
			} else if (row == 9) {
				check = (check + (d > 0 ? 1 : 2)) % 3;
			}
		}
		if (d || reset) {
			// every change is applied and saved immediately
			snprintf(g_outdir, sizeof(g_outdir), "%s", outdir);
			g_retries = retries; g_speed = speed; g_eject = eject; g_autostart = autostart; g_redump = redump; g_remote = remote; g_webhook_on = webhook_on; g_check = check; g_webhook_step = whstep;
			OUT_DIR = g_outdir;
			cfg_save();
			logf("settings saved: outdir=%s retries=%d speed=%d eject=%d autostart=%d redump=%d remote=%d webhook=%d",
			     g_outdir, g_retries, g_speed, g_eject, g_autostart, g_redump, g_remote, g_webhook_on);
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
	fire_event("disc_ejected");
	sys_storage_close(g_fd);
	g_disc_state = DISC_NONE;
}

// Home screen. Returns 1 = rip the disc in the drive, 2 = rip a freshly inserted disc, 0 = exit.
static volatile int g_insert_pending = 0;   // a disc was inserted on the result screen (auto-rip off)
static int home_screen()
{
	disc_inserted = g_insert_pending;   // treat it as freshly inserted: identify (and report) it
	g_insert_pending = 0;
	g_redraw = 2;
	pad_pressed();   // swallow buttons still held from a previous screen
	if (g_disc_state != DISC_READY && !g_ident_busy && !disc_inserted) identify_start(0);
	int last_state = -1, inserted = 0, last_rd = -1;
	while (!exit_requested) {
		if (g_rd_seq != last_rd) { last_rd = g_rd_seq; g_redraw = 2; }
		if (disc_inserted) { disc_inserted = 0; inserted = 1; while (g_ident_busy) usleep(10000); identify_start(1); }
		if (!disc_present) { if (g_disc_state != DISC_NONE && !g_ident_busy) g_disc_state = DISC_NONE; inserted = 0; }
		int st = g_disc_state;
		if (st != last_state) { last_state = st; g_redraw = 2; }
		// automatic rip only for a disc not ripped to this destination yet: never overwrite one
		if (st == DISC_READY && inserted && g_autostart && !g_prev_rip) return 2;
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
				// group 1: the disc (title, serial and size, Redump name) close together
				ui_text(UI_TITLE, ui_w() / 2, y - ui_line(UI_BODY), 0xFFFFFF, g_disc_title, UI_CENTER);
				ui_text(UI_BODY, ui_w() / 2, y - ui_line(UI_BODY) + ui_line(UI_TITLE) * 9 / 10, 0xA8B0C8, info, UI_CENTER);
			} else {
				const char *msg = st == DISC_READING ? T(STR_HOME_READING) : st == DISC_ERROR ? T(STR_HOME_NOTPSX) : T(STR_HOME_INSERT);
				ui_text(UI_BODY, ui_w() / 2, y, 0xFFFFFF, msg, UI_CENTER);
			}
			if (g_webhook_on && g_webhook[0]) {   // just a hint that webhooks are on; details in the settings
				int ne = 0;
				for (int i = 0; i < WH_COUNT; i++) ne += (g_webhook_mask >> i) & 1;
				char wl[64];
				snprintf(wl, sizeof(wl), T(STR_HOME_WEBHOOK_N), ne);
				footer_note(wl);
			}
			int ls = ui_line(UI_SMALL), lb = ui_line(UI_BODY);
			int ly = y - lb + ui_line(UI_TITLE) * 9 / 10 + lb;   // just under serial and size
			if (st == DISC_READY && g_disc_rd[0]) { ui_text(UI_SMALL, ui_w() / 2, ly, 0x9AD0A8, g_disc_rd, UI_CENTER); ly += ls; }
			// group 2: already ripped here? one line, the "known errors" part in yellow as a warning
			if (st == DISC_READY && g_prev_rip) {
				ly += lb;
				char p2[96];
				if (g_repair_n) snprintf(p2, sizeof(p2), T(STR_HERE_ERRORS), g_repair_n);
				else snprintf(p2, sizeof(p2), "%s", T(STR_HERE_CLEAN));
				const char *p1 = T(STR_ALREADY_HERE);
				int w1 = ui_text_width(UI_SMALL, p1), w2 = ui_text_width(UI_SMALL, p2), x0 = (ui_w() - w1 - w2) / 2;
				ui_text(UI_SMALL, x0, ly, 0x8FA8D8, p1);
				ui_text(UI_SMALL, x0 + w1, ly, g_repair_n ? 0xF0C060 : 0x8FA8D8, p2);
				ly += ls;
			}
			// group 3: where it goes
			int sy = ly + lb;
			ui_text(UI_SMALL, ui_w() / 2, sy, 0x8890A8, saving, UI_CENTER);
			if (st == DISC_READY && g_space_short && !g_prev_rip) {   // will not fit (a re-rip replaces the old image)
				char sl[200];
				snprintf(sl, sizeof(sl), T(STR_NO_SPACE), g_disc_mb, (unsigned)(g_disc_mb + 2 - g_space_short));
				ui_text(UI_SMALL, ui_w() / 2, sy + ls * 13 / 10, 0xFF8A80, sl, UI_CENTER);
			}
			if (st == DISC_READY && g_prev_rip && g_repair_n) {   // ripped here with known errors: square repairs
				const char *b[] = {"cross", "square", "triangle", "start", "circle"};
				const char *l[] = {T(STR_HINT_NEWRIP), T(STR_HINT_REPAIR), T(STR_HINT_EJECT), T(STR_HINT_SETTINGS), T(STR_HINT_EXIT)};
				hint_bar(b, l, 5);
			} else if (st == DISC_READY && g_prev_rip) {   // ripped here without known errors: nothing to repair
				const char *b[] = {"cross", "triangle", "start", "circle"};
				const char *l[] = {T(STR_HINT_NEWRIP), T(STR_HINT_EJECT), T(STR_HINT_SETTINGS), T(STR_HINT_EXIT)};
				hint_bar(b, l, 4);
			} else if (st == DISC_READY) {
				const char *b[] = {"cross", "triangle", "start", "circle"};
				const char *l[] = {T(STR_HINT_RIP), T(STR_HINT_EJECT), T(STR_HINT_SETTINGS), T(STR_HINT_EXIT)};
				hint_bar(b, l, 4);
			} else if (st == DISC_ERROR) {
				const char *b[] = {"triangle", "start", "circle"};
				const char *l[] = {T(STR_HINT_EJECT), T(STR_HINT_SETTINGS), T(STR_HINT_EXIT)};
				hint_bar(b, l, 3);
			} else {
				const char *b[] = {"start", "circle"};
				const char *l[] = {T(STR_HINT_SETTINGS), T(STR_HINT_EXIT)};
				hint_bar(b, l, 2);
			}
		}
		flip_frame();
		int p = pad_pressed();
		if ((p & B_CROSS) && st == DISC_READY) {
			int shown_prev = g_prev_rip;
			prev_rip_check();   // the destination may have changed since the disc was identified
			if (g_prev_rip != shown_prev) { g_redraw = 2; continue; }   // the screen was out of date: show it first
			// would run out of space half way (a new rip over an old one replaces it, so that fits)
			if (g_space_short && !g_prev_rip) {
				char m[256];
				snprintf(m, sizeof(m), T(STR_NO_SPACE), g_disc_mb, (unsigned)(g_disc_mb + 2 - g_space_short));
				own_dialog(m, 0);
				g_redraw = 2;
				continue;
			}
			return inserted ? 2 : 1;   // rip, also over an existing rip (X is always a rip)
		}
		if ((p & B_SQUARE) && st == DISC_READY && g_prev_rip && g_repair_n) {   // repair the sectors the log lists
			int shown_prev = g_prev_rip;
			prev_rip_check();
			if (g_prev_rip != shown_prev || !g_repair_n) { g_redraw = 2; continue; }
			return 3;
		}
		if ((p & B_TRIANGLE) && (st == DISC_READY || st == DISC_ERROR)) { eject_disc(); g_redraw = 2; }
		if (p & B_START) {
			int old = g_redump;
			settings_screen();
			if (g_redump != old && g_disc_state == DISC_READY && !g_ident_busy) lookup_start();
			if (g_disc_state == DISC_READY && !g_ident_busy) prev_rip_check();   // destination may have changed
			g_redraw = 2;
		}
		if (p & B_CIRCLE) return 0;
	}
	return 0;
}

// ---------------------------------------------------------------- one disc
static char g_done_msg[768];   // summary of the last successful rip
// The same summary for the own result screen: coloured lines
struct ResLine { char text[240]; uint32_t col; };
static ResLine g_res[8];
static int g_nres = 0;
static char g_res_title[160], g_res_path[300];
static void res_add(uint32_t col, const char *fmt, ...)
{
	if (g_nres >= 8) return;
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(g_res[g_nres].text, sizeof(g_res[g_nres].text), fmt, ap);
	va_end(ap);
	g_res[g_nres++].col = col;
}

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

// ---------------------------------------------------------------- remote view and webhooks
enum { ST_HOME, ST_RIPPING };
static volatile int g_state = ST_HOME;
static time_t g_rip_t0;

static const char *rd_result_str()
{
	switch (g_rd_result) {
	case 1: return "verified";
	case 0: return "no_match";
	case -1: return "unchecked";
	case -2: return "not_found";
	default: return "off";
	}
}

// Fields shared by the status page and the webhooks (appended to an open JSON object).
static void json_fields(char *js, size_t n)
{
	size_t o;
	o = strlen(js); snprintf(js + o, n - o, "\"title\":"); json_str(js, n, g_state_title);
	o = strlen(js); snprintf(js + o, n - o, ",\"serial\":"); json_str(js, n, g_state_serial);
	o = strlen(js); snprintf(js + o, n - o, ",\"sectors_done\":%u,\"sectors_total\":%u,\"unreadable\":%u,\"fixed\":%u,\"bad\":%u",
	                         g_readpos > g_done ? g_readpos : g_done, g_total, g_readerrs, g_recovered, g_badsect);
	o = strlen(js); snprintf(js + o, n - o, ",\"redump\":\"%s\",\"redump_name\":", rd_result_str()); json_str(js, n, g_rd_result_name);
}

static int g_state_busy() { return g_state == ST_RIPPING || g_ident_busy || g_repairing || lv1_patched; }

static int status_json(char *js, int n)
{
	uint32_t pos = g_readpos > g_done ? g_readpos : g_done;
	// busy: the drive is in use or the lv1 patch is on; never stop or reinstall the app then
	int busy = g_state == ST_RIPPING || g_ident_busy || g_repairing || lv1_patched;
	snprintf(js, n, "{\"state\":\"%s\",\"busy\":%s,\"percent\":%u,", g_state == ST_RIPPING ? "ripping" : "idle", busy ? "true" : "false",
	         g_state == ST_RIPPING && g_total ? (unsigned)((uint64_t)pos * 100 / g_total) : 0);
	if (g_state != ST_RIPPING) {   // idle: describe the disc on the home screen
		size_t o = strlen(js);
		snprintf(js + o, n - o, "\"disc\":");
		json_str(js, n, g_disc_state == DISC_READY ? g_disc_title : "");
		o = strlen(js); snprintf(js + o, n - o, ",");
	}
	json_fields(js, n);
	size_t o = strlen(js);
	snprintf(js + o, n - o, "}");
	return (int)strlen(js);
}


static void set_webhook(const char *url)
{
	snprintf(g_webhook, sizeof(g_webhook), "%s", url);
	cfg_save();
	char host[160];
	webhook_host(g_webhook, host, sizeof(host));
	logf("webhook address set (%s)", g_webhook[0] ? host : "none");
	g_redraw = 2;
}

static void event_json(char *js, size_t n, const char *event)
{
	uint32_t pos = g_readpos > g_done ? g_readpos : g_done;
	snprintf(js, n, "{\"event\":\"%s\",\"app\":\"PSX Ripper\",\"percent\":%u,", event,
	         g_total ? (unsigned)((uint64_t)pos * 100 / g_total) : 0);
	json_fields(js, n);
	size_t o = strlen(js);
	snprintf(js + o, n - o, ",\"seconds\":%ld,\"path\":", g_rip_t0 ? (long)(time(NULL) - g_rip_t0) : 0L);
	char bin[300];
	snprintf(bin, sizeof(bin), "%s.bin", g_base);
	json_str(js, n, strcmp(event, "rip_finished") ? "" : bin);
	o = strlen(js);
	snprintf(js + o, n - o, "}");
}

static int test_webhook()
{
	char js[1024];
	event_json(js, sizeof(js), "test");
	char hdrs[1024];
	webhook_headers(hdrs, sizeof(hdrs));
	int st = webhook_post(g_webhook, js, hdrs);
	logf("webhook test -> %d", st);
	return st;
}

static void fire_event(const char *event)
{
	int idx = webhook_event_index(event);
	if (!g_webhook_on || !g_webhook[0] || idx < 0 || !((g_webhook_mask >> idx) & 1)) return;
	char js[1024];
	event_json(js, sizeof(js), event);
	char hdrs[1024];
	webhook_headers(hdrs, sizeof(hdrs));
	webhook_send_async(g_webhook, js, hdrs);
}

static void write_riplog();
static int rip_disc_inner(int fresh, int autorip)
{
	char msg[768];
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

	g_rd_result = -3; g_rd_result_name[0] = 0;
	g_rip_t0 = time(NULL);
	g_state = ST_RIPPING;
	snprintf(g_state_title, sizeof(g_state_title), "%s", name);
	snprintf(g_state_serial, sizeof(g_state_serial), "%s", g_serial);
	fire_event("rip_started");
	if (!strncmp(OUT_DIR, "exfat", 5)) { VfVolume v[8]; vf_scan(v, 8); }   // (re)mount exFAT drives
	if (vf_mkdir(OUT_DIR) && !vf_exists(OUT_DIR)) logf("cannot create %s", OUT_DIR);
	g_done = 0; g_readpos = 0; g_total = g_leadout; g_abort = 0; g_finished = 0;

	{
		// SET CD SPEED: maximum read speed
		uint16_t kbs = g_speed ? (uint16_t)(176 * speed_x[g_speed]) : 0xFFFF;
		uint8_t cdb[12] = {0xBB, 0, (uint8_t)(kbs >> 8), (uint8_t)kbs, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0};
		logf("set cd speed %u KB/s -> 0x%x", kbs, atapi(cdb, NULL, 0, ATAPI_NON_DATA_PROTO));
	}
	sys_ppu_thread_t tid;
	sysThreadCreate(&tid, rip_thread, NULL, 1000, 0x10000, THREAD_JOINABLE, (char *)"psxrip");

	char ripmsg[256];
	snprintf(ripmsg, sizeof(ripmsg), T(STR_RIPPING), name, g_serial[0] ? g_serial : T(STR_UNKNOWN));
	pad_pressed();
	uint32_t shown = 101, shown_pos = 0, shown_lba = 0;
	int shown_try = -1, shown_abort = 0;
	Eta eta;
	eta_start(&eta);
	uint64_t shown_ms = 0;
	int shown_ev = -1;
	uint32_t sent_pct = 0;   // last percentage sent as rip_progress
	while (!g_finished) {
		if ((pad_pressed() & B_CIRCLE) || exit_requested) g_abort = 1;
		uint32_t pos = g_readpos > g_done ? g_readpos : g_done;
		uint32_t pct = (uint32_t)((uint64_t)pos * 100 / (g_total ? g_total : 1));
		if (pct >= sent_pct + (uint32_t)g_webhook_step && pct < 100 && !g_phase) {   // every g_webhook_step percent
			sent_pct = pct - pct % g_webhook_step;
			fire_event("rip_progress");
		}
		int rmax = g_rr_max, rtry = rmax ? g_rr_try : 0;
		uint32_t rlba = g_rr_lba;
		// redraw on a new percentage, every 256 sectors, or when the re-read state changes
		if (pct != shown || pos / 256 != shown_pos / 256 || rtry != shown_try || (rmax && rlba != shown_lba) || g_abort != shown_abort ||
		    now_ms() - shown_ms >= 1000 || g_ev_seq != shown_ev) {
			shown_ms = now_ms();
			shown_ev = g_ev_seq;
			char pm[96], note[200] = "";
			snprintf(pm, sizeof(pm), T(STR_PROGRESS), pos, g_total);
			if (g_phase == 3) snprintf(note, sizeof(note), "%s", T(STR_FINISHING));
			else if (g_pass2_total) snprintf(note, sizeof(note), T(g_phase == 1 ? STR_VERIFY_PASS : STR_PASS2), g_pass2_done < g_pass2_total ? g_pass2_done + 1 : g_pass2_total, g_pass2_total);
			if (rmax) snprintf(note + strlen(note), sizeof(note) - strlen(note), T(STR_REREAD), rlba, rtry, rmax);
			if (g_readerrs || g_recovered) {
				size_t o = strlen(note);
				snprintf(note + o, sizeof(note) - o, T(STR_READ_STATS), o ? " · " : "", g_readerrs, g_recovered);
			}
			char tl[96];
			// during the double-check and the slow re-reads the estimate follows that phase
			static int eta_phase = 0;
			if (g_phase != eta_phase) { eta_phase = g_phase; uint64_t t0 = eta.t0; eta_start(&eta); eta.t0 = t0; }
			if (g_pass2_total && g_phase) eta_line(&eta, g_pass2_done, g_pass2_total, tl, sizeof(tl));
			else eta_line(&eta, pos, g_total, tl, sizeof(tl));
			progress_draw(ripmsg, pm, pct, note, g_abort, tl);
			shown = pct; shown_pos = pos; shown_try = rtry; shown_lba = rlba; shown_abort = g_abort;
		}
		flip_frame();
	}
	u64 rv;
	sysThreadJoin(tid, &rv);
	if (!g_result) {   // LibCrypt check and Redump run here: say so instead of a frozen last frame
		char pm[96];
		snprintf(pm, sizeof(pm), T(STR_PROGRESS), g_total, g_total);
		progress_draw(ripmsg, pm, 100, T(STR_FINISHING), 0, " ");
		flip_frame(); flip_frame();
	}
	int is_pal = g_is_pal;
	// re-reading LibCrypt candidates only matters for discs on the LibCrypt list
	if (!g_result && is_pal && g_qcal_done && g_nlc > 0 && g_nlc <= MAX_LC && g_serial[0] && is_libcrypt_disc(g_serial)) {
		uint8_t *tmp = (uint8_t *)memalign(128, 4096);
		verify_lc_candidates(tmp);
		free(tmp);
	}
	if (!g_result && !g_headless && g_eject) {
		// eject so the next disc can go straight in (START STOP UNIT, LoEj)
		uint8_t cdb[12] = {0x1B, 0, 0, 0, 0x02, 0, 0, 0, 0, 0, 0, 0};
		logf("eject -> 0x%x", atapi(cdb, NULL, 0, ATAPI_NON_DATA_PROTO));
		fire_event("disc_ejected");
	}
	sys_storage_close(g_fd);
	lv1_storage_unpatch();   // done with the drive: never keep the patch while a dialog waits

	if (g_result) {
		snprintf(msg, sizeof(msg), "%s.bin.part", g_base);
		vf_unlink(msg);
		g_state = ST_HOME;
		fire_event(g_result == 1 ? "rip_cancelled" : "rip_failed");
		tell(g_result == 1 ? T(STR_ABORTED) : T(STR_CREATE_FAILED));
		return 0;
	}

	{
		char part[300], bin[300];
		snprintf(part, sizeof(part), "%s.bin.part", g_base);
		snprintf(bin, sizeof(bin), "%s.bin", g_base);
		vf_unlink(bin);
		if (vf_rename(part, bin)) logf("rename %s failed", part);
	}
	const char *slash = strrchr(g_base, '/');
	char binname[256];
	snprintf(binname, sizeof(binname), "%s.bin", slash + 1);
	write_cue(binname);
	{
		char mpath[300];
		snprintf(mpath, sizeof(mpath), "%s.md5", g_base);
		VFile *mf = vf_open(mpath, "wb");
		if (mf) {
			for (int t = 0; t < g_ntracks; t++) vf_printf(mf, "%s  track %02d\n", g_track_md5[t], g_tracks[t].num);
			vf_close(mf);
		}
	}

	char rdinfo[RD_NAME + 80] = "";
	if (g_redump != RD_OFF) redump_verify(rdinfo, sizeof(rdinfo));

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
	char lcline[480] = "";
	if (lcinfo[0]) snprintf(lcline, sizeof(lcline), "\n%s", lcinfo);
	if (rdinfo[0]) snprintf(lcline + strlen(lcline), sizeof(lcline) - strlen(lcline), "\n%s", rdinfo);
	if (g_nsuspect) {
		snprintf(lcline + strlen(lcline), sizeof(lcline) - strlen(lcline), "\n");
		snprintf(lcline + strlen(lcline), sizeof(lcline) - strlen(lcline), T(STR_CONFIRMED), g_nsuspect, g_confirmed);
	}
	if (g_badsect) {
		snprintf(lcline + strlen(lcline), sizeof(lcline) - strlen(lcline), "\n");
		snprintf(lcline + strlen(lcline), sizeof(lcline) - strlen(lcline), T(STR_BAD_SECTORS), g_badsect);
	}
	logf("sector check: %u bad, %u fixed by re-reading; %u sectors without EDC compared, %u read differently",
	     g_badsect, g_recovered, g_secure, g_unstable);
	snprintf(msg, sizeof(msg), T(STR_DONE),
	         g_base, lcline, g_readerrs);
	// result screen
	g_nres = 0;
	snprintf(g_res_title, sizeof(g_res_title), "%s  ·  %s", name, g_serial[0] ? g_serial : T(STR_UNKNOWN));
	snprintf(g_res_path, sizeof(g_res_path), "%s.bin", g_base);
	if (rdinfo[0]) res_add(g_rd_result == 1 ? 0x7CE0A0 : g_rd_result == 0 ? 0xFF8A80 : 0xA8B0C8, "%s", rdinfo);
	if (lcinfo[0]) res_add(0xE8E8F0, "%s", lcinfo);
	if (g_secure) res_add(g_unstable ? 0xF0C060 : 0xA8B0C8, T(STR_VERIFY_RESULT), g_secure, g_unstable);
	if (g_nsuspect && g_confirmed == g_nsuspect) res_add(0x7CE0A0, T(STR_FIXED_ALL_CONFIRMED), g_nsuspect);
	else if (g_nsuspect) res_add(0xF0C060, T(STR_CONFIRMED), g_nsuspect, g_confirmed);
	if (g_badsect) res_add(0xFF8A80, T(STR_BAD_SECTORS), g_badsect);
	res_add(g_readerrs ? 0xFF8A80 : 0xA8B0C8, T(STR_UNREADABLE_N), g_readerrs);
	{
		char d[16];
		fmt_dur(d, sizeof(d), (long)(time(NULL) - g_rip_t0));
		res_add(0xA8B0C8, T(STR_TOTAL_TIME), d);
	}
	write_riplog();
	logf("done: %s errs=%u", g_base, g_readerrs);
	snprintf(g_done_msg, sizeof(g_done_msg), "%s", msg);
	g_state = ST_HOME;
	fire_event("rip_finished");
	return 1;
}

static int rip_disc(int fresh, int autorip)
{
	while (g_ident_busy) usleep(10000);   // the identify thread patches/unpatches lv1 itself
	int ok = rip_disc_inner(fresh, autorip);
	lv1_storage_unpatch();
	return ok;
}

// Rip log next to the image (<name>.log): what was read and which sectors are still wrong, so a
// later "repair" (after cleaning the disc) can re-read just those sectors.
static void write_riplog()
{
	char path[320];
	snprintf(path, sizeof(path), "%s.log", g_base);
	VFile *f = vf_open(path, "wb");
	if (!f) return;
	time_t now = time(NULL);
	struct tm *g = gmtime(&now);
	vf_printf(f, "PSX Ripper rip log\n");
	vf_printf(f, "title=%s\nserial=%s\n", g_res_title, g_serial);
	if (g) vf_printf(f, "date=%04d-%02d-%02d %02d:%02d UTC\n", g->tm_year + 1900, g->tm_mon + 1, g->tm_mday, g->tm_hour, g->tm_min);
	vf_printf(f, "sectors=%u\ncheck=%s\nretries=%d\n", g_leadout, g_check == CHECK_OFF ? "off" : g_check == CHECK_ON ? "on" : "bitperfect", g_retries);
	for (int t = 0; t < g_ntracks; t++) vf_printf(f, "track=%02d start=%u md5=%s\n", g_tracks[t].num, g_tstart[t], g_track_md5[t]);
	vf_printf(f, "redump=%s%s%s\n", rd_result_str(), g_rd_result_name[0] ? " " : "", g_rd_result_name);
	vf_printf(f, "unreadable_count=%u\nbad_count=%u\nfixed_count=%u\n", g_readerrs, g_badsect, g_recovered);
	for (uint32_t k = 0; k < g_nhard; k++) {
		if (g_hard[k] & HARD_RESOLVED) continue;
		vf_printf(f, "%s=%u\n", (g_hard[k] >> 31) ? "bad" : "unreadable", g_hard[k] & 0x3FFFFFFF);
	}
	for (uint32_t k = 0; k < g_nsuspect; k++) vf_printf(f, "uncertain=%u %s\n", g_suspect[k], g_suspect_ok[k] ? "confirmed" : "unconfirmed");
	vf_close(f);
	logf("rip log written: %s", path);
}

// Repairs a previous rip from its log: re-reads only the sectors that are still wrong (with the
// same confirmation as the second pass) and writes the ones that now read correctly into the
// existing image. The disc in the drive must be the same one (same name and serial).
static void repair_thread(void *arg)
{
	int patched = second_pass();
	vf_close(g_out);
	char bin[320];
	snprintf(bin, sizeof(bin), "%s.bin", g_base);
	if (patched) md5_from_file(bin);
	g_result = g_abort ? 1 : 0;
	g_finished = 1;
	sysThreadExit(0);
}

static int repair_disc_inner();
static int repair_disc()
{
	g_repairing = 1;
	int r = repair_disc_inner();
	lv1_storage_unpatch();   // whatever happened
	g_repairing = 0;
	return r;
}

static int repair_disc_inner()
{
	while (g_ident_busy) usleep(10000);
	int err = 0, code = 0;
	if (prepare_disc(&err, &code)) { lv1_storage_unpatch(); return 0; }
	char path[320];
	snprintf(path, sizeof(path), "%s.log", g_base);
	VFile *lf = vf_open(path, "rb");
	static char buf[65536];
	int n = lf ? (int)vf_read(lf, buf, sizeof(buf) - 1) : 0;
	if (lf) vf_close(lf);
	buf[n > 0 ? n : 0] = 0;
	snprintf(path, sizeof(path), "%s.bin", g_base);
	g_out = vf_open(path, "r+b");
	if (!g_out || !n) { if (g_out) vf_close(g_out); sys_storage_close(g_fd); lv1_storage_unpatch(); tell(T(STR_CREATE_FAILED)); return 0; }
	if (!riplog_open_count()) {
		// nothing listed: the previous rip stands as it is
		vf_close(g_out);
		sys_storage_close(g_fd);
		lv1_storage_unpatch();
		g_nres = 0;
		snprintf(g_res_title, sizeof(g_res_title), "%s  ·  %s", g_name, g_serial);
		snprintf(g_res_path, sizeof(g_res_path), "%s.bin", g_base);
		const char *rd = strstr(buf, "redump=");
		if (rd) {   // "redump=verified Alundra (Europe)" -> the same line as after a rip
			char code[16] = "", name[RD_NAME] = "";
			sscanf(rd + 7, "%15[^ \n] %159[^\n]", code, name);
			if (!strcmp(code, "verified")) res_add(0x7CE0A0, T(STR_RD_VERIFIED), name);
			else if (!strcmp(code, "no_match")) res_add(0xFF8A80, "%s", T(STR_RD_NOMATCH));
			else if (!strcmp(code, "not_found")) res_add(0xA8B0C8, "%s", T(STR_RD_NOTFOUND));
			else if (!strcmp(code, "unchecked")) res_add(0xA8B0C8, "%s", T(STR_RD_UNCHECKED));
		}
		res_add(0x7CE0A0, "%s", T(STR_NOTHING_TO_REPAIR));
		logf("repair %s: nothing to repair", g_base);
		snprintf(g_state_title, sizeof(g_state_title), "%s", g_name);
		snprintf(g_state_serial, sizeof(g_state_serial), "%s", g_serial);
		fire_event("repair_finished");
		return 1;
	}

	// rebuild the state the second pass works on
	edc_init(); ecc_init();
	g_nhard = 0; g_nsuspect = 0; g_confirmed = 0; g_changed = 0; g_readerrs = 0; g_badsect = 0; g_recovered = 0;
	g_pass2_total = 0; g_pass2_done = 0; g_abort = 0; g_finished = 0; g_rd_result = -3; g_rd_result_name[0] = 0;
	tstart_init();
	for (char *l = strtok(buf, "\n"); l; l = strtok(NULL, "\n")) {
		unsigned v, tn, st;
		if (sscanf(l, "unreadable=%u", &v) == 1) { note_hard(v, 0); g_readerrs++; }
		else if (sscanf(l, "bad=%u", &v) == 1) { note_hard(v, 1); g_badsect++; }
		else if (sscanf(l, "uncertain=%u", &v) == 1) {
			uint8_t sec[RAW_SIZE];
			if (vf_seek(g_out, (uint64_t)v * RAW_SIZE) == 0 && vf_read(g_out, sec, RAW_SIZE) == RAW_SIZE) {
				note_suspect(v, sec);
				if (!strstr(l, "unconfirmed")) g_suspect_ok[g_nsuspect - 1] = 1, g_confirmed++;
			}
		}
		else if (sscanf(l, "track=%u start=%u", &tn, &st) == 2) {
			for (int t = 0; t < g_ntracks; t++) if (g_tracks[t].num == (int)tn) g_tstart[t] = st;
		}
	}
	// confirmed sectors need no new reads: keep only the unconfirmed ones for the pass
	uint32_t keep = 0;
	for (uint32_t k = 0; k < g_nsuspect; k++)
		if (!g_suspect_ok[k]) {
			g_suspect[keep] = g_suspect[k];
			memmove(g_suspect_data + keep * RAW_SIZE, g_suspect_data + k * RAW_SIZE, RAW_SIZE);
			g_suspect_ok[keep++] = 0;
		}
	g_nsuspect = keep; g_confirmed = 0;
	logf("repair %s: %u failed and %u uncertain sector(s)", g_base, g_nhard, g_nsuspect);

	g_state = ST_RIPPING;
	snprintf(g_state_title, sizeof(g_state_title), "%s", g_name);
	snprintf(g_state_serial, sizeof(g_state_serial), "%s", g_serial);
	g_rip_t0 = time(NULL);
	g_done = g_total = g_leadout;
	int check = g_check; g_check = CHECK_BITPERFECT;   // the repair always confirms
	sys_ppu_thread_t tid;
	sysThreadCreate(&tid, repair_thread, NULL, 1000, 0x10000, THREAD_JOINABLE, (char *)"psxrepair");
	char title[256];
	snprintf(title, sizeof(title), T(STR_REPAIRING), g_name, g_serial);
	pad_pressed();
	uint32_t shown = ~0u; int shown_try = -1, shown_abort = 0, shown_ev = -1;
	g_nev = 0;
	Eta eta;
	eta_start(&eta);
	uint64_t shown_ms = 0;
	while (!g_finished) {
		if ((pad_pressed() & B_CIRCLE) || exit_requested) g_abort = 1;
		uint32_t tot = g_pass2_total ? g_pass2_total : 1, done = g_pass2_done;
		int rtry = g_rr_max ? g_rr_try : 0;
		if (done != shown || rtry != shown_try || g_abort != shown_abort || g_ev_seq != shown_ev || now_ms() - shown_ms >= 1000) {
			shown_ev = g_ev_seq; shown_ms = now_ms();
			char pm[96], note[200] = "", tl[96];
			eta_line(&eta, done, g_pass2_total, tl, sizeof(tl));
			snprintf(pm, sizeof(pm), T(STR_PROGRESS), done, g_pass2_total);
			if (g_rr_max) snprintf(note, sizeof(note), T(STR_REREAD), g_rr_lba, rtry, g_rr_max);
			progress_draw(title, pm, (uint32_t)((uint64_t)done * 100 / tot), note, g_abort, tl);
			shown = done; shown_try = rtry; shown_abort = g_abort;
		}
		flip_frame();
	}
	u64 rv;
	sysThreadJoin(tid, &rv);
	g_check = check;
	if (g_eject) { uint8_t cdb[12] = {0x1B, 0, 0, 0, 0x02, 0, 0, 0, 0, 0, 0, 0}; atapi(cdb, NULL, 0, ATAPI_NON_DATA_PROTO); }
	sys_storage_close(g_fd);
	lv1_storage_unpatch();
	for (int t = 0; t < g_ntracks; t++) md5_hex(g_track_md5bin[t], g_track_md5[t]);   // fresh when patched
	char rdinfo[RD_NAME + 80] = "";
	if (g_redump != RD_OFF) redump_verify(rdinfo, sizeof(rdinfo));
	// results, the .md5 file and the log as after a rip
	g_nres = 0;
	snprintf(g_res_title, sizeof(g_res_title), "%s  ·  %s", g_name, g_serial);
	snprintf(g_res_path, sizeof(g_res_path), "%s.bin", g_base);
	if (rdinfo[0]) res_add(g_rd_result == 1 ? 0x7CE0A0 : g_rd_result == 0 ? 0xFF8A80 : 0xA8B0C8, "%s", rdinfo);
	res_add(0xE8E8F0, T(STR_REPAIRED), g_recovered + g_changed, g_pass2_total);
	if (g_badsect) res_add(0xFF8A80, T(STR_BAD_SECTORS), g_badsect);
	res_add(g_readerrs ? 0xFF8A80 : 0xA8B0C8, T(STR_UNREADABLE_N), g_readerrs);
	{
		char mpath[300];
		snprintf(mpath, sizeof(mpath), "%s.md5", g_base);
		VFile *mf = vf_open(mpath, "wb");
		if (mf) { for (int t = 0; t < g_ntracks; t++) vf_printf(mf, "%s  track %02d\n", g_track_md5[t], g_tracks[t].num); vf_close(mf); }
	}
	write_riplog();
	logf("repair done: %u recovered, %u changed, %u unreadable, %u bad", g_recovered, g_changed, g_readerrs, g_badsect);
	g_state = ST_HOME;
	fire_event("repair_finished");
	g_repair_n = 0;
	return 1;
}

// Text with line breaks and word wrap, centred; returns the y below it.
static int draw_wrapped(UiFont f, const char *text, int y, uint32_t col)
{
	int maxw = ui_w() * 3 / 4, lh = ui_line(f) * 12 / 10;
	char line[256];
	const char *p = text;
	while (*p) {
		size_t len = strcspn(p, "\n"), o = 0;
		const char *end = p + len;
		while (p < end) {
			// take words while they fit
			const char *q = p, *cut = NULL;
			o = 0;
			while (q < end) {
				const char *w = q;
				while (w < end && *w != ' ') w++;
				size_t add = (size_t)(w - q) + (o ? 1 : 0);
				if (o + add >= sizeof(line)) break;
				char tmp[256];
				memcpy(tmp, line, o);
				if (o) tmp[o] = ' ';
				memcpy(tmp + o + (o ? 1 : 0), q, w - q);
				tmp[o + add] = 0;
				if (o && ui_text_width(f, tmp) > maxw) break;
				memcpy(line, tmp, o + add + 1);
				o += add;
				cut = w;
				q = w < end ? w + 1 : w;
			}
			if (!cut) { cut = end; }   // a single word longer than the line
			line[o] = 0;
			ui_text(f, ui_w() / 2, y, col, line, UI_CENTER);
			y += lh;
			p = cut < end ? cut + 1 : end;
		}
		if (!len) y += lh;   // empty line
		p = *end ? end + 1 : end;
	}
	return y;
}

// Message (X OK) or question (X Yes, O No) in the app's own style. Returns 1 = OK/yes, 2 = no.
static int own_dialog(const char *msg, int yesno)
{
	pad_pressed();
	g_redraw = 2;
	while (!exit_requested) {
		if (g_redraw > 0) {
			g_redraw = 0;
			draw_backdrop();
			draw_wrapped(UI_BODY, msg, ui_h() / 3, 0xE8E8F0);
			if (yesno) {
				const char *b[] = {"cross", "circle"};
				const char *l[] = {T(STR_HINT_YES), T(STR_HINT_NO)};
				hint_bar(b, l, 2);
			} else {
				const char *b[] = {"cross"};
				const char *l[] = {T(STR_HINT_OK)};
				hint_bar(b, l, 1);
			}
			g_drawn = 2;
			g_backdrop_shown = 0;
		}
		flip_frame();
		int p = pad_pressed();
		if (p & B_CROSS) return 1;
		if (yesno && (p & B_CIRCLE)) return 2;
	}
	return 2;
}

// Own result screen after a rip (instead of the system dialog). Returns 0 for O or a new disc:
// the start screen then identifies it (and rips it when "rip immediately" is on and it is not
// ripped to this destination yet).
static void eject_disc();
static int result_screen()
{
	int ejected = !disc_present;   // from the system's disc callback: is the disc still in the drive?
	disc_inserted = 0;
	pad_pressed();
	g_redraw = 2;
	while (!exit_requested) {
		if (g_redraw > 0) {
			g_redraw = 0;
			draw_backdrop();
			int lb = ui_line(UI_BODY), ls = ui_line(UI_SMALL);
			int y = ui_h() / 10 + ui_line(UI_TITLE) + lb;
			ui_text(UI_TITLE, ui_w() / 2, y, 0x7CE0A0, T(STR_RES_DONE), UI_CENTER);
			y += ui_line(UI_TITLE) + lb / 2;
			ui_text(UI_BODY, ui_w() / 2, y, 0xFFFFFF, g_res_title, UI_CENTER);
			y += lb + ls / 2;
			ui_text(UI_SMALL, ui_w() / 2, y, 0x8890A8, g_res_path, UI_CENTER);
			y += ls * 2;
			for (int i = 0; i < g_nres; i++) {
				ui_text(UI_BODY, ui_w() / 2, y, g_res[i].col, g_res[i].text, UI_CENTER);
				y += lb * 13 / 10;
			}
			y += lb / 2;
			if (g_autostart)   // only then does a new disc start a rip from here
				ui_text(UI_SMALL, ui_w() / 2, y, 0xA8B0C8, T(ejected ? STR_RES_NEXT : STR_RES_NEXT_NOEJECT), UI_CENTER);
			if (ejected) {
				const char *b[] = {"circle"};
				const char *l[] = {T(STR_HINT_BACK)};
				hint_bar(b, l, 1);
			} else {   // the disc is still in the drive: offer to eject it
				const char *b[] = {"triangle", "circle"};
				const char *l[] = {T(STR_HINT_EJECT), T(STR_HINT_BACK)};
				hint_bar(b, l, 2);
			}
			g_drawn = 2;
			g_backdrop_shown = 0;
		}
		flip_frame();
		if (disc_inserted) {   // always via the home screen: it identifies the disc, rips it when
			g_insert_pending = 1;  // "rip immediately" is on and it is not ripped here yet, else waits
			return 0;
		}
		int p = pad_pressed();
		if ((p & B_TRIANGLE) && !ejected) { eject_disc(); ejected = 1; disc_inserted = 0; g_redraw = 2; }
		if (p & B_CIRCLE) return 0;
	}
	return 0;
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
	// start-up checks: config, lv1 state after an earlier crash, USB file systems
	char warn[400] = "";
	if (!cfg_load()) { mkdir("/dev_hdd0/tmp", 0777); cfg_save(); logf("startup: no config, defaults written"); }   // editable over FTP
	{
		int lv = lv1_startup_check();
		if (lv == 1) snprintf(warn + strlen(warn), sizeof(warn) - strlen(warn), "%s\n", T(STR_START_LV1_FIXED));
		else if (lv == -1) snprintf(warn + strlen(warn), sizeof(warn) - strlen(warn), "%s\n", T(STR_START_LV1_REBOOT));
	}
	{
		VfVolume v[8];
		int nv = vf_scan(v, 8);
		for (int i = 0; i < nv && i < 8; i++) logf("exFAT drive on USB port %d (%llu MB free)", v[i].port, (unsigned long long)v[i].free_mb);
	}
	{
		static const RemoteHooks hooks = {status_json, debug_cmd};
		remote_init(&hooks);
		if (g_remote) remote_set(1);
	}
	OUT_DIR = g_outdir;
	{ struct stat gs; ui_guides = stat("/dev_hdd0/tmp/psxrip.guides", &gs) == 0; }
#ifdef PRIVATE_SBI
	install_missing_sbis();
#endif

	logf("---- PSX Ripper start ----");
	if (warn[0]) logf("startup checks: %s", warn);
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
	if (warn[0]) own_dialog(warn, 0);   // what the start-up checks found and fixed
	{ struct stat ds; g_debug_on = stat("/dev_hdd0/tmp/psxrip.debug", &ds) == 0; if (g_debug_on) { logf("debug commands on (/dbg)"); usbodd_init(); } }
	{ struct stat ps; if (stat("/dev_hdd0/tmp/psxrip.usbprobe", &ps) == 0) { logf("usbprobe: start"); usb_optical_probe(); logf("usbprobe: done"); } }
	sysDiscRegisterDiscChangeCallback(disc_eject_cb, disc_insert_cb);
	int autorip = 0;   // set after a successful rip: the next inserted disc is ripped without asking
	while (!exit_requested) {
		int fresh;
		if (autorip) {
			fresh = 1;
		} else {
			int a = home_screen();
			if (exit_requested || a == 0) break;
			if (a == 3) {
				if (repair_disc() && result_screen()) autorip = 1;
				g_disc_state = DISC_NONE;
				continue;
			}
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

		// auto mode: show the result and wait for the next disc; O goes back to the start screen
		if (result_screen()) autorip = 1;
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
