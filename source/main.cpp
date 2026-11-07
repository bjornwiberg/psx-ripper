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
#include "version.h"
#include "about.h"   // APP_VERSION, from version.txt (written into the build directory by the Makefile)
#include "ui.h"
#include "md5.h"
#include "redump.h"
#include "remote.h"
#include "mcp.h"
#include "webhook.h"
#include "vfs.h"
#include "usbodd.h"
#include "audit.h"
#include <sys/mutex.h>

#ifndef LOG_PATH   // the host tests point it elsewhere
#define LOG_PATH   "/dev_hdd0/tmp/psxrip.log"
#endif
// Output locations; the mount test redirects both so a mounted image is never overwritten
static const char *SBI_DIR = "/dev_hdd0/tmp/sbi";
static int g_mounttest = 0;   // flag psxrip.mounttest: rip the image Cobra has mounted (development aid)

// ---------------------------------------------------------------- settings (saved in CFG_PATH)
#ifndef CFG_PATH   // the host tests point it elsewhere
#define CFG_PATH "/dev_hdd0/tmp/psxrip.cfg"
#endif
static char g_outdir[64] = "/dev_hdd0/PSXISO";
static int g_redump = RD_OFF;      // verification against Redump: off, online or local copy
// Remote control (Settings → Remote control): the web server on port 8090 runs while any is on
static int g_remote = 0;           // remote view: the stream and the pad page
static int g_mcp = 0;              // MCP (POST /mcp): an agent reads and controls the app
static int g_status_json = 0;      // /status.json
static int g_cfg_status_seen;      // cfg_load: the file had status_json= (else it is migrated)
enum { CHECK_OFF, CHECK_ON, CHECK_BITPERFECT };

// ---- the values the settings offer, in the order right/left steps through them, with their labels:
// the screens step through and show these, and MCP's list_settings / set_setting offer and take
// exactly these (the SETTINGS table further down). A value and its label: literal text, or a text
// id (its %d is the value), or else the number.
struct SetOpt { int v; StrId id; const char *lit; };
static const SetOpt o_speed[] = {{3, STR_COUNT, "2\xC3\x97"}, {2, STR_COUNT, "4\xC3\x97"}, {1, STR_COUNT, "8\xC3\x97"}, {0, STR_SPEED_MAX, NULL}};   // right = faster
static const SetOpt o_retries[] = {{1, STR_COUNT, NULL}, {2, STR_COUNT, NULL}, {4, STR_COUNT, NULL}, {8, STR_COUNT, NULL}, {16, STR_COUNT, NULL}, {32, STR_COUNT, NULL}};
static const SetOpt o_check[] = {{CHECK_OFF, STR_OFF, NULL}, {CHECK_ON, STR_CHECK_ON, NULL}, {CHECK_BITPERFECT, STR_CHECK_BITPERFECT, NULL}};
static const SetOpt o_onoff[] = {{0, STR_OFF, NULL}, {1, STR_ON, NULL}};
static const SetOpt o_redump[] = {{RD_OFF, STR_OFF, NULL}, {RD_ONLINE, STR_RD_ONLINE, NULL}, {RD_LOCAL, STR_RD_LOCAL, NULL}};
static const SetOpt o_progress[] = {{0, STR_OFF, NULL}, {1, STR_WH_PROGRESS_N, NULL}, {2, STR_WH_PROGRESS_N, NULL}, {5, STR_WH_PROGRESS_N, NULL},
                                    {10, STR_WH_PROGRESS_N, NULL}, {15, STR_WH_PROGRESS_N, NULL}, {25, STR_WH_PROGRESS_N, NULL}, {50, STR_WH_PROGRESS_N, NULL}};
static const SetOpt o_audit_mb[] = {{1, STR_AUDIT_MB_VAL, NULL}, {2, STR_AUDIT_MB_VAL, NULL}, {4, STR_AUDIT_MB_VAL, NULL}, {8, STR_AUDIT_MB_VAL, NULL},
                                    {16, STR_AUDIT_MB_VAL, NULL}, {32, STR_AUDIT_MB_VAL, NULL}, {64, STR_AUDIT_MB_VAL, NULL}, {128, STR_AUDIT_MB_VAL, NULL},
                                    {256, STR_AUDIT_MB_VAL, NULL}};
#define OPTS(a) a, (int)(sizeof(a) / sizeof(a[0]))
typedef const char *(*Tr)(StrId);   // a text in a language: T (the console's), T_en, T_sv
static void opt_label(const SetOpt &o, Tr tr, char *out, size_t n)
{
	if (o.lit) snprintf(out, n, "%s", o.lit);
	else if (o.id != STR_COUNT) snprintf(out, n, tr(o.id), o.v);
	else snprintf(out, n, "%d", o.v);
}
static int opt_index(const SetOpt *o, int n, int v)   // -1: not one of them
{
	for (int i = 0; i < n; i++) if (o[i].v == v) return i;
	return -1;
}
// v's label in the console's language (the number when it is not one of them)
static const char *opt_text(const SetOpt *o, int n, int v, char *out, size_t sz)
{
	int i = opt_index(o, n, v);
	if (i >= 0) opt_label(o[i], T, out, sz); else snprintf(out, sz, "%d", v);
	return out;
}
// The value d steps (right +1, left -1) from cur, round: the last's next is the first. A value that
// is not one of them goes to the first (right) or the last (left).
static int opt_wrap(const SetOpt *o, int n, int cur, int d)
{
	int i = opt_index(o, n, cur);
	if (i < 0) i = n;
	return o[(i + (d > 0 ? 1 : n - 1)) % n].v;
}
// The same without going round: it stops at the first and the last (a value that is not one of
// them counts as the last).
static int opt_clamp(const SetOpt *o, int n, int cur, int d)
{
	int i = opt_index(o, n, cur);
	if (i < 0) i = n - 1;
	i += d > 0 ? 1 : -1;
	return o[i < 0 ? 0 : i >= n ? n - 1 : i].v;
}
// A drive's own value: -1 ("standard") before the values, round (another value goes to "standard").
static int opt_std(const SetOpt *o, int n, int cur, int d)
{
	int i = cur == -1 ? 0 : opt_index(o, n, cur) + 1;   // not one of them: 0, the standard
	i = (i + (d > 0 ? 1 : n)) % (n + 1);
	return i ? o[i - 1].v : -1;
}
static int g_webhook_on = 0;       // send webhooks
static char g_webhook[512] = "";   // webhook address (http://...); may contain a secret id, never logged
static int g_webhook_mask = WH_DEFAULT_MASK;   // which WEBHOOK_EVENTS are sent
#define WH_HEADERS 3
static char g_webhook_hdr[WH_HEADERS][256];    // extra headers "Name: value" (often a secret: never logged or shown)
static int g_webhook_hdr_on = 0;               // send them (off keeps them stored)
// File name of a rip: %T title (Redump's name without the language list, else the app's title;
// with "(Disc 1)" for a disc of several), %R region, %D disc ID, %N Redump's whole name. Text in [ ] does not show in webMAN's game list.
static char g_name_pat[96] = "%T [%D]";
enum { SRC_BD, SRC_USB };
static char g_outdir_usb[8][64];               // destination per USB drive, by its port ("" = the hard disk)
// Drive settings: the standard (Settings, START) and a drive's own values (SELECT on its card),
// where -1 means "standard".
struct DriveCfg { int set, retries, speed, eject, autostart, check; };
static DriveCfg g_std = {1, 4, 0, 1, 0, CHECK_BITPERFECT};   // the standard, from the old keys retries=, speed=, ...
static DriveCfg g_bdcfg = {0, -1, -1, -1, -1, -1};   // the PS3 drive's own values (drive_bd=)
static DriveCfg g_usbcfg[8];                   // a USB drive's own values, by its port (drive_usbN=)
static int std_or(int v, int std) { return v >= 0 ? v : std; }

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
static int g_webhook_step = 5;                 // rip_progress every this many percent
static const int speed_x[4] = {0, 8, 4, 2};

// One card per drive on the home screen: card 0 = the PS3's drive, card 1 + n = USB drive n (usbodd).
// Identify, rip, repair and eject work on one card at a time, g_card: its drive gets the commands,
// its destination is OUT_DIR, and the disc globals below (g_disc_state, ...) are its fields.
enum { DISC_NONE, DISC_READING, DISC_READY, DISC_ERROR };
#define NCARDS (1 + USBODD_DRIVES)
struct Card {
	volatile int usb_dead;   // USB: the drive has stopped answering (several commands in a row failed)
	volatile int present, inserted;   // a disc in the drive / just put in, not handled yet
	volatile int state;               // DISC_*
	int tried;                        // identification ran since the disc went in
	int fresh;                        // put in while the app runs: auto-rip, disc_identified
	char title[128], serial[16];
	uint32_t mb;
	char rd[160];                     // Redump line, "" = none
	volatile int prev_rip, repair_n, space_short;
	volatile int recheck;             // the destination changed: look again for an earlier rip there
	volatile int pend_cfg;            // CF_*: config the card takes when its job ends (an undo changed it meanwhile)
	char base[256];                   // destination + file name without extension (g_base of the card)
	char outdir[64];                  // chosen destination
	char dest[64];                    // used: the chosen one, or the hard disk while it is out
	int retries, speed, eject, autostart, check;   // the drive's settings
	volatile int job;                 // a rip, check or repair runs on this card (its own thread)
	int repairing;                    // that job is a repair: progress is the re-read sectors
	char res[200];                    // last rip: one line, coloured res_col
	uint32_t res_col;
	int res_bad;                     // the last rip left unreadable or bad sectors (it needs a repair)
	int res_rd;                      // the last rip against Redump: 1 matches, 0 does not, -1 not checked, -2 no line (off, failed)
	char res_disc[160];               // which disc the last rip was ("Motorhead (SLUS_007.17)")
	char res_key[160];                // that disc's ID (its title when it has none): is it the one in the drive?
	int res_failed;                   // it was cancelled or failed: an earlier rip there is untouched
	char rd_done[160];                // Redump result as text (verified / no match), for the log and webhooks
	uint32_t rd_done_col;
	char rd_name[RD_NAME];            // Redump's name of the disc ("Alundra (Europe)"), "" = not one known
	char fname[200];                  // file name a rip gets (pattern), without .bin
	char exist[256];                  // an earlier rip of this disc at the destination (base path), "" = none
	int rd_state;                     // the image here vs Redump: 1 verified, 0 no match, -1 not known (not ripped)
	uint64_t eta_t0, eta_tw; uint32_t eta_pw, eta_p0; double eta_rate;   // time left of the running rip
	int eta_phase;
	int pg_scroll, pg_max;            // the page: pixels scrolled down, and how far it can (L2/R2)
	uint32_t audit_job;               // the running job's id in the audit log, 0 = none
	AuditSrc job_src;                 // what started it (pad, remote, app)
	char audit_disc[96];              // its disc key
	char rd_before[RD_NAME + 16];     // the rip's Redump line before the job ("unchecked" when none)
};
static Card g_cards[NCARDS];

static void card_defaults(Card &k) { k.retries = k.speed = k.eject = k.autostart = k.check = -1; }   // all standard
static void card_from(Card &k, const DriveCfg &d) { k.retries = d.retries; k.speed = d.speed; k.eject = d.eject; k.autostart = d.autostart; k.check = d.check; }

// The card's settings into the config: the PS3 drive's in the old keys, a USB drive's by its port.
static void card_store(int c)
{
	const Card &k = g_cards[c];
	DriveCfg d = {1, k.retries, k.speed, k.eject, k.autostart, k.check};
	if (c == 0) { g_bdcfg = d; return; }
	int port = usbodd_port(c - 1);
	if (port >= 0 && port < 8) g_usbcfg[port] = d;
}
static volatile int g_card = 0;       // changes only while no identification, rip or repair runs
// The audit's image access: the app's own file layer (it also reaches exFAT drives).
static void *au_open(const char *p, const char *m) { return vf_open(p, m); }
static size_t au_read(void *f, void *p, size_t n) { return vf_read((VFile *)f, p, n); }
static size_t au_write(void *f, const void *p, size_t n) { return vf_write((VFile *)f, p, n); }
static int au_seek(void *f, uint64_t pos) { return vf_seek((VFile *)f, pos); }
static int au_close(void *f) { return vf_close((VFile *)f); }
static const AuditIo vf_io = {au_open, au_read, au_write, au_seek, au_close};

// Where the last button press came from (set by pad_pressed): the source of the changes made after it.
static AuditSrc g_input_src = AU_PAD;
static int g_audit_mb = 256;   // size cap of the audit's sector files, in MB
static void audit_apply_cap() { audit_set_cap((uint64_t)g_audit_mb << 20); }

// One config line ("key=value") into the settings. Returns 1 when it was a known key with a usable
// value, else 0 (nothing changed).
static int cfg_apply_line(const char *line)
{
	if (!strncmp(line, "outdir=", 7)) {
		if (line[7] != '/' && strncmp(line + 7, "exfat", 5)) return 0;   // also exFAT drives
		snprintf(g_outdir, sizeof(g_outdir), "%s", line + 7);
	}
	else if (!strncmp(line, "retries=", 8)) { int r = atoi(line + 8); if (r < 1 || r > 64) return 0; g_std.retries = r; }
	else if (!strncmp(line, "speed=", 6)) { int v = atoi(line + 6); if (v < 0 || v > 3) return 0; g_std.speed = v; }
	else if (!strncmp(line, "eject=", 6)) g_std.eject = atoi(line + 6) ? 1 : 0;
	else if (!strncmp(line, "autostart=", 10)) g_std.autostart = atoi(line + 10) ? 1 : 0;
	else if (!strncmp(line, "redump=", 7)) { int v = atoi(line + 7); if (v < RD_OFF || v > RD_LOCAL) return 0; g_redump = v; }
	else if (!strncmp(line, "remote=", 7)) g_remote = atoi(line + 7) ? 1 : 0;
	else if (!strncmp(line, "mcp=", 4)) g_mcp = atoi(line + 4) ? 1 : 0;
	else if (!strncmp(line, "status_json=", 12)) { g_status_json = atoi(line + 12) ? 1 : 0; g_cfg_status_seen = 1; }
	else if (!strncmp(line, "check=", 6)) { int v = atoi(line + 6); if (v < CHECK_OFF || v > CHECK_BITPERFECT) return 0; g_std.check = v; }
	else if (!strncmp(line, "webhook_on=", 11)) g_webhook_on = atoi(line + 11) ? 1 : 0;
	else if (!strncmp(line, "webhook=", 8)) snprintf(g_webhook, sizeof(g_webhook), "%s", line + 8);
	else if (!strncmp(line, "webhook_events=", 15)) g_webhook_mask = atoi(line + 15) & ((1 << WH_COUNT) - 1);
	else if (!strncmp(line, "webhook_headers_on=", 19)) g_webhook_hdr_on = atoi(line + 19) ? 1 : 0;
	else if (!strncmp(line, "webhook_header", 14) && line[14] >= '1' && line[14] <= '0' + WH_HEADERS && line[15] == '=') {
		snprintf(g_webhook_hdr[line[14] - '1'], sizeof(g_webhook_hdr[0]), "%s", line + 16);
		clean_header(g_webhook_hdr[line[14] - '1']);
	}
	else if (!strncmp(line, "outdir_usb", 10) && line[10] >= '0' && line[10] <= '7' && line[11] == '=') {   // destination of the USB drive on port N
		if (line[12] != '/' && strncmp(line + 12, "exfat", 5)) return 0;
		snprintf(g_outdir_usb[line[10] - '0'], sizeof(g_outdir_usb[0]), "%s", line + 12);
	}
	else if ((!strncmp(line, "drive_usb", 9) && line[9] >= '0' && line[9] <= '7' && line[10] == '=') || !strncmp(line, "drive_bd=", 9)) {
		// drive_bd= / drive_usbN=retries,speed,eject,autostart,check; -1 = the standard
		DriveCfg c = {1, -1, -1, -1, -1, -1};
		const char *v = strchr(line, '=') + 1;
		if (sscanf(v, "%d,%d,%d,%d,%d", &c.retries, &c.speed, &c.eject, &c.autostart, &c.check) == 5 &&
		    (c.retries == -1 || (c.retries >= 1 && c.retries <= 64)) && c.speed >= -1 && c.speed <= 3 && c.eject >= -1 && c.eject <= 1 &&
		    c.autostart >= -1 && c.autostart <= 1 && c.check >= -1 && c.check <= CHECK_BITPERFECT) {
			if (line[6] == 'b') g_bdcfg = c; else g_usbcfg[line[9] - '0'] = c;
		} else return 0;
	}
	else if (!strncmp(line, "name_pattern=", 13) && line[13]) snprintf(g_name_pat, sizeof(g_name_pat), "%s", line + 13);
	else if (!strncmp(line, "webhook_step=", 13)) {
		int v = atoi(line + 13);   // a step of the progress row (its 0 is "off", not a step)
		if (v <= 0 || opt_index(OPTS(o_progress), v) < 0) return 0;
		g_webhook_step = v;
	}
	else if (!strncmp(line, "audit_mb=", 9)) {
		int v = atoi(line + 9);   // anything but a listed size is the default
		g_audit_mb = opt_index(OPTS(o_audit_mb), v) >= 0 ? v : 256;
		audit_apply_cap();
	}
	else return 0;
	return 1;
}

// Returns 0 when there is no config file yet.
static int cfg_load()
{
	FILE *f = fopen(CFG_PATH, "r");
	if (!f) return 0;
	char line[640];
	g_cfg_status_seen = 0;
	while (fgets(line, sizeof(line), f)) {
		line[strcspn(line, "\r\n")] = 0;
		cfg_apply_line(line);
	}
	fclose(f);
	if (!g_cfg_status_seen) g_status_json = g_remote;   // a config from before the switches: the remote view had /status.json
	return 1;
}

// The config text as last written; every line that differs after the next save is an audit row.
static char g_cfg_last[4096];
static bool g_cfg_primed = false;   // the first save after start only fills g_cfg_last

// The webhook address may hold a secret id and the headers often hold a token: neither goes into the log.
static bool cfg_key_secret(const char *key) { return !strcmp(key, "webhook") || (!strncmp(key, "webhook_header", 14) && key[14] >= '1' && key[14] <= '9' && !key[15]); }

// The value of line `key` in text (up to the next newline) into out; false when text has no such line.
static bool cfg_find(const char *text, const char *key, char *out, size_t n)
{
	size_t kl = strlen(key);
	for (const char *p = text; *p; ) {
		const char *e = strchr(p, '\n');
		size_t len = e ? (size_t)(e - p) : strlen(p);
		if (len > kl && !strncmp(p, key, kl) && p[kl] == '=') {
			size_t vl = len - kl - 1;
			if (vl >= n) vl = n - 1;
			memcpy(out, p + kl + 1, vl);
			out[vl] = 0;
			return true;
		}
		p += len + (e ? 1 : 0);
	}
	return false;
}

// Logs the lines of `now` that differ from `was` (and lines that vanished) as setting rows.
// The address and header lines can hold a secret: they are logged as set / cleared, never as text.
static void cfg_audit_diff(AuditSrc src, const char *was, const char *now)
{
	char key[48], a[512], b[512];
	for (int pass = 0; pass < 2; pass++) {
		const char *t = pass ? was : now;   // pass 1 finds the lines only `was` had
		for (const char *p = t; *p; ) {
			const char *e = strchr(p, '\n'), *eq = (const char *)memchr(p, '=', e ? (size_t)(e - p) : strlen(p));
			if (eq && (size_t)(eq - p) < sizeof(key)) {
				memcpy(key, p, eq - p); key[eq - p] = 0;
				bool in_was = cfg_find(was, key, a, sizeof(a)), in_now = cfg_find(now, key, b, sizeof(b));
				if (!in_was) a[0] = 0;
				if (!in_now) b[0] = 0;
				if ((pass == 0 || !in_now) && (in_was != in_now || strcmp(a, b))) {
					if (cfg_key_secret(key)) {
						snprintf(a, sizeof(a), "%s", a[0] ? "set" : "cleared");
						snprintf(b, sizeof(b), "%s", b[0] ? "set" : "cleared");
						audit_setting_private(src, key, a, b);
					} else audit_setting(src, key, a, b);
				}
			}
			p += (e ? (size_t)(e - p) + 1 : strlen(p));
		}
	}
}

// The config text, as it is written to the file.
static void cfg_text(char *txt, size_t cap)
{
	size_t n = 0;
	auto put = [&](const char *fmt, ...) {
		va_list ap;
		va_start(ap, fmt);
		if (n < cap) n += vsnprintf(txt + n, cap - n, fmt, ap);
		va_end(ap);
		if (n >= cap) n = cap - 1;
	};
	const DriveCfg &k = g_std;   // the standard drive settings (the globals follow the card in use)
	put("outdir=%s\nretries=%d\nspeed=%d\neject=%d\nautostart=%d\nredump=%d\nremote=%d\nwebhook_on=%d\nwebhook=%s\ncheck=%d\nwebhook_events=%d\nwebhook_step=%d\n",
	    g_outdir, k.retries, k.speed, k.eject, k.autostart, g_redump, g_remote, g_webhook_on, g_webhook, k.check, g_webhook_mask, g_webhook_step);
	if (g_bdcfg.set)
		put("drive_bd=%d,%d,%d,%d,%d\n", g_bdcfg.retries, g_bdcfg.speed, g_bdcfg.eject, g_bdcfg.autostart, g_bdcfg.check);
	for (int i = 0; i < 8; i++) if (g_outdir_usb[i][0]) put("outdir_usb%d=%s\n", i, g_outdir_usb[i]);
	for (int i = 0; i < 8; i++)
		if (g_usbcfg[i].set)
			put("drive_usb%d=%d,%d,%d,%d,%d\n", i, g_usbcfg[i].retries, g_usbcfg[i].speed, g_usbcfg[i].eject, g_usbcfg[i].autostart, g_usbcfg[i].check);
	put("name_pattern=%s\n", g_name_pat);
	put("webhook_headers_on=%d\n", g_webhook_hdr_on);
	for (int i = 0; i < WH_HEADERS; i++) put("webhook_header%d=%s\n", i + 1, g_webhook_hdr[i]);
	put("audit_mb=%d\n", g_audit_mb);
	put("mcp=%d\nstatus_json=%d\n", g_mcp, g_status_json);
}

// At start: the file may have been edited (over FTP) while the app was off. Every setting whose
// value differs from the last one the audit log knows gets an `app` row, so an undo compares with
// the real value. A setting the log never saw has nothing to differ from.
static void cfg_audit_boot()
{
	static char keys[96][48], vals[96][192];
	int n = audit_setting_values(keys, vals, 96);
	char v[512];
	for (int i = 0; i < n; i++) {
		if (!cfg_find(g_cfg_last, keys[i], v, sizeof(v))) v[0] = 0;   // a line that is gone: empty
		if (cfg_key_secret(keys[i])) {
			const char *now = v[0] ? "set" : "cleared";
			if (strcmp(vals[i], now)) audit_setting_private(AU_APP, keys[i], vals[i], now);
		} else if (strncmp(vals[i], v, sizeof(vals[0]) - 1)) audit_setting(AU_APP, keys[i], vals[i], v);   // the log keeps 191 characters
	}
}

// At start, after the config was read: the text the next save is compared with, so that the first
// real change after a boot is logged too; and the log learns the values the file has now.
static void cfg_prime()
{
	cfg_text(g_cfg_last, sizeof(g_cfg_last));
	g_cfg_primed = true;
	cfg_audit_boot();
}

// Settings change on the UI thread (the screens, the audit's undo) and on MCP's thread. A change
// (the value, its side effects and the save) holds this lock, so two never mix in one save and each
// audit row gets the source that made it. Recursive: a change holds it across its cfg_save.
static sys_mutex_t g_cfg_mx;
struct CfgLock { CfgLock() { sysMutexLock(g_cfg_mx, 0); } ~CfgLock() { sysMutexUnlock(g_cfg_mx); } };
static volatile int g_cfg_gen = 0;   // counts the saves: an open screen redraws when it changes

static void cfg_save_src(AuditSrc src)
{
	CfgLock lock;
	g_cfg_gen++;
	FILE *f = fopen(CFG_PATH, "w");
	if (!f) return;
	char txt[sizeof(g_cfg_last)];
	cfg_text(txt, sizeof(txt));
	fputs(txt, f);
	fclose(f);
	if (g_cfg_primed) cfg_audit_diff(src, g_cfg_last, txt);
	snprintf(g_cfg_last, sizeof(g_cfg_last), "%s", txt);
	g_cfg_primed = true;
}
static void cfg_save() { cfg_save_src(g_input_src); }

static void setting_applied(const char *key);   // below: what the screens do besides changing the value

// The web server's paths after a switch changed (the server runs while any is on).
static void remote_apply() { remote_set((g_remote ? REMOTE_VIEW : 0) | (g_mcp ? REMOTE_MCP : 0) | (g_status_json ? REMOTE_STATUS : 0)); }

// The audit's undo of a setting row: the value goes through the config parser like a line in the
// file, and the save records the change as an undo.
static int set_setting_hook(const char *key, const char *value)
{
	if (cfg_key_secret(key)) return -1;   // its row holds only set / cleared: there is no text to put back
	CfgLock lock;
	char line[700];
	snprintf(line, sizeof(line), "%s=%s", key, value);
	if (!cfg_apply_line(line)) return -1;
	setting_applied(key);
	cfg_save_src(AU_UNDO);
	return 0;
}

#define RAW_SIZE   2352
#define SUBQ_SIZE  16
#define CHUNK      1024            // sectors per READ CD (same as IRISMAN)
#define MAX_TRACKS 99
#define MAX_LC     512             // max mismatching Q sectors we keep

// ---------------------------------------------------------------- per-drive context
// Everything one identification or rip of one drive uses lives in its context: a card's drive can
// rip while another one identifies or rips. A thread working for a card sets t_card; the UI and
// other threads use the card the screen acts on (g_card). The names below are macros for the
// context of the current thread, so the ripping code reads as before.
#define SECT_Q (RAW_SIZE + SUBQ_SIZE)
#define MAX_HARD 8192
#define MAX_SUSPECT 512
#define NEV 20   // events kept per rip (the page shows what fits, L2/R2 scroll)
enum { ST_HOME, ST_RIPPING };
struct Track {
	int num;
	int ctrl;        // 0x4 bit set = data
	uint32_t lba;
};
struct LcEntry { uint32_t lba; uint8_t q[10]; };
struct RipEvent { char text[120]; uint32_t col; };
struct ResLine { char text[240]; uint32_t col; };
struct Ctx {
	// the drive
	int g_fd;
	uint32_t g_sc;                 // atapi syscall that worked
	uint8_t *g_cmdbuf;             // aligned command transfer buffer
	uint8_t *g_sec;                // 128-byte aligned sector buffer
	int g_use, g_usb;              // SRC_BD / SRC_USB, usbodd drive number
	int g_read_mode;               // 0 = syscall 604/6 args, 1 = syscall 616/7 args
	uint8_t g_er_page[12];
	int g_er_valid;
	int g_retries, g_speed, g_eject, g_autostart, g_check;   // the drive's settings (standard or its own)
	const char *OUT_DIR;           // destination
	// the disc
	Track g_tracks[MAX_TRACKS + 1];
	int g_ntracks;
	uint32_t g_leadout;
	char g_serial[16];             // "SLUS_005.53"
	char g_volid[33];
	char g_publisher[129];   // the ISO volume's publisher ("KONAMI"), for discs whose ID is wrong
	char g_name[128];              // title (or volume id)
	char g_base[256];              // "/dev_hdd0/PSXISO/NAME [SERIAL]"
	// Q and LibCrypt
	LcEntry g_lc[MAX_LC];
	int g_nlc;
	uint32_t g_qzero, g_qchecked;
	int g_is_pal;                  // LibCrypt is only checked on PAL discs
	int g_qoff, g_qcal_done;
	int g_qhist[5];                // delta -2..+2
	// the rip
	volatile uint32_t g_done, g_total;
	volatile uint32_t g_readpos;   // reader position inside a slow (damaged) chunk
	volatile uint32_t g_rr_lba;
	volatile int g_rr_try, g_rr_max;
	uint32_t g_hard[MAX_HARD];
	uint32_t g_nhard;
	volatile uint32_t g_pass2_total, g_pass2_done;
	uint32_t g_suspect[MAX_SUSPECT];
	uint8_t *g_suspect_data;       // MAX_SUSPECT * RAW_SIZE
	uint32_t g_nsuspect;
	uint8_t g_suspect_ok[MAX_SUSPECT];   // confirmed in the second pass
	volatile uint32_t g_confirmed, g_changed;
	volatile int g_abort, g_finished, g_result;
	volatile int g_skip;           // square: leave the re-reads, keep what the rip has (repair later)
	volatile uint32_t g_readerrs;
	Md5 g_md5[MAX_TRACKS + 1];
	struct { uint32_t lba; int t; Md5 m; } g_snap[512];   // MD5 midstates while writing, one per buffer
	int g_nsnap;
	uint32_t g_patch_min;          // first sector written again after the first pass (UINT32_MAX: none)
	char g_track_md5[MAX_TRACKS + 1][33];
	uint8_t g_track_md5bin[MAX_TRACKS + 1][16];
	volatile uint32_t g_tstart[MAX_TRACKS + 1];
	uint8_t *g_bufs[2];
	volatile uint32_t g_buf_len[2];   // sectors in buffer, 0 = empty
	volatile uint32_t g_buf_end[2];   // lba after the last sector in buffer
	volatile int g_reader_done, g_write_failed;
	VFile *g_out;
	volatile uint32_t g_badsect, g_recovered;
	volatile uint32_t g_unstable, g_secure;   // sectors that read differently / sectors compared
	uint8_t *g_vbuf;               // second read of a run, CHUNK sectors
	uint32_t *g_uv_lba;
	uint64_t *g_uv_hash;
	uint32_t g_nuv;
	volatile int g_phase;          // 0 reading, 1 verify pass, 2 second pass, 3 finishing
	RipEvent g_ev[NEV];
	volatile int g_nev, g_ev_seq;
	// Redump
	RdDisc g_rd_discs[4];          // online results, reused to verify the rip
	int g_rd_ndiscs;               // -1 = no online lookup for the current disc
	char g_rd_serial[16];          // serial the online results belong to
	char g_id_serial[16];
	uint64_t g_id_total;
	int g_id_ntracks;
	int g_rd_result;               // last verification: 1 verified, 0 no match, -1 unchecked, -2 not found, -3 off
	char g_rd_result_name[RD_NAME];
	// state and result
	char g_state_title[128], g_state_serial[16];
	volatile int g_state;          // ST_HOME / ST_RIPPING
	time_t g_rip_t0;
	char g_done_msg[768];          // summary of the last successful rip
	ResLine g_res[8];
	int g_nres;
	char g_res_title[160], g_res_path[300];
	// scratch buffers (were function statics)
	uint8_t er_b[64] __attribute__((aligned(128)));
	uint8_t er_w[20] __attribute__((aligned(128)));
	uint8_t ecc_t[RAW_SIZE];
	uint8_t cr_prev[SECT_Q] __attribute__((aligned(128)));
	uint8_t vp_sec[RAW_SIZE] __attribute__((aligned(128)));
	uint8_t sp_sec[RAW_SIZE] __attribute__((aligned(128)));
	uint8_t rt_dead[CHUNK];
	uint32_t rt_last_bad;
	char log_buf[65536];
};
static Ctx g_ctx[NCARDS];
static __thread int t_card = -1;   // card the current thread works for, -1 = the one the screen acts on
static int g_parallel = 1;          // thread-local storage works: cards may rip at the same time
static inline int cur_card() { return t_card >= 0 ? t_card : g_card; }
static void ctx_init(int c)
{
	Ctx &x = g_ctx[c];
	x.g_fd = -1; x.g_sc = 616; x.g_rd_ndiscs = -1; x.g_rd_result = -3; x.g_state = ST_HOME;
	x.g_use = c ? SRC_USB : SRC_BD; x.g_usb = c ? c - 1 : 0;
	x.OUT_DIR = "/dev_hdd0/PSXISO";
	x.g_cmdbuf = (uint8_t *)memalign(128, 4096);
	x.g_sec = (uint8_t *)memalign(128, 4096);
	x.g_retries = 4; x.g_eject = 1; x.g_check = CHECK_BITPERFECT;
}
#define CX (g_ctx[cur_card()])
#define g_fd (CX.g_fd)
#define g_sc (CX.g_sc)
#define g_cmdbuf (CX.g_cmdbuf)
#define g_sec (CX.g_sec)
#define g_use (CX.g_use)
#define g_usb (CX.g_usb)
#define g_read_mode (CX.g_read_mode)
#define g_er_page (CX.g_er_page)
#define g_er_valid (CX.g_er_valid)
#define g_retries (CX.g_retries)
#define g_speed (CX.g_speed)
#define g_eject (CX.g_eject)
#define g_autostart (CX.g_autostart)
#define g_check (CX.g_check)
#define OUT_DIR (CX.OUT_DIR)
#define g_tracks (CX.g_tracks)
#define g_ntracks (CX.g_ntracks)
#define g_leadout (CX.g_leadout)
#define g_serial (CX.g_serial)
#define g_volid (CX.g_volid)
#define g_publisher (CX.g_publisher)
#define g_name (CX.g_name)
#define g_base (CX.g_base)
#define g_lc (CX.g_lc)
#define g_nlc (CX.g_nlc)
#define g_qzero (CX.g_qzero)
#define g_qchecked (CX.g_qchecked)
#define g_is_pal (CX.g_is_pal)
#define g_qoff (CX.g_qoff)
#define g_qcal_done (CX.g_qcal_done)
#define g_qhist (CX.g_qhist)
#define g_done (CX.g_done)
#define g_total (CX.g_total)
#define g_readpos (CX.g_readpos)
#define g_rr_lba (CX.g_rr_lba)
#define g_rr_try (CX.g_rr_try)
#define g_rr_max (CX.g_rr_max)
#define g_hard (CX.g_hard)
#define g_nhard (CX.g_nhard)
#define g_pass2_total (CX.g_pass2_total)
#define g_pass2_done (CX.g_pass2_done)
#define g_suspect (CX.g_suspect)
#define g_suspect_data (CX.g_suspect_data)
#define g_nsuspect (CX.g_nsuspect)
#define g_suspect_ok (CX.g_suspect_ok)
#define g_confirmed (CX.g_confirmed)
#define g_changed (CX.g_changed)
#define g_abort (CX.g_abort)
#define g_skip (CX.g_skip)
#define g_finished (CX.g_finished)
#define g_result (CX.g_result)
#define g_readerrs (CX.g_readerrs)
#define g_md5 (CX.g_md5)
#define g_snap (CX.g_snap)
#define g_nsnap (CX.g_nsnap)
#define g_patch_min (CX.g_patch_min)
#define g_track_md5 (CX.g_track_md5)
#define g_track_md5bin (CX.g_track_md5bin)
#define g_tstart (CX.g_tstart)
#define g_bufs (CX.g_bufs)
#define g_buf_len (CX.g_buf_len)
#define g_buf_end (CX.g_buf_end)
#define g_reader_done (CX.g_reader_done)
#define g_write_failed (CX.g_write_failed)
#define g_out (CX.g_out)
#define g_badsect (CX.g_badsect)
#define g_recovered (CX.g_recovered)
#define g_unstable (CX.g_unstable)
#define g_secure (CX.g_secure)
#define g_vbuf (CX.g_vbuf)
#define g_uv_lba (CX.g_uv_lba)
#define g_uv_hash (CX.g_uv_hash)
#define g_nuv (CX.g_nuv)
#define g_phase (CX.g_phase)
#define g_ev (CX.g_ev)
#define g_nev (CX.g_nev)
#define g_ev_seq (CX.g_ev_seq)
#define g_rd_discs (CX.g_rd_discs)
#define g_rd_ndiscs (CX.g_rd_ndiscs)
#define g_rd_serial (CX.g_rd_serial)
#define g_id_serial (CX.g_id_serial)
#define g_id_total (CX.g_id_total)
#define g_id_ntracks (CX.g_id_ntracks)
#define g_rd_result (CX.g_rd_result)
#define g_rd_result_name (CX.g_rd_result_name)
#define g_state_title (CX.g_state_title)
#define g_state_serial (CX.g_state_serial)
#define g_state (CX.g_state)
#define g_rip_t0 (CX.g_rip_t0)
#define g_done_msg (CX.g_done_msg)
#define g_res (CX.g_res)
#define g_nres (CX.g_nres)
#define g_res_title (CX.g_res_title)
#define g_res_path (CX.g_res_path)

// Card c's context from any thread: the field names are macros for the current thread's context,
// so these switch the thread's card for a moment.
struct OnCard { int keep; OnCard(int c) : keep(t_card) { t_card = c; } ~OnCard() { t_card = keep; } };
static int ctx_ripping(int c) { OnCard o(c); return g_state == ST_RIPPING; }
static int any_ripping() { for (int c = 0; c < NCARDS; c++) if (ctx_ripping(c)) return 1; return 0; }

#define disc_inserted (g_cards[cur_card()].inserted)
#define disc_present (g_cards[cur_card()].present)


// ---------------------------------------------------------------- logging
static void logf(const char *fmt, ...)
{
	FILE *f = fopen(LOG_PATH, "a");
	if (!f) return;
	if (t_card >= 0) fprintf(f, "[%d] ", t_card);   // a card's thread: which drive
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

// ---------------------------------------------------------------- dialogs
extern "C" {
static volatile int exit_requested = 0;
static volatile int g_osk_event = 0;   // last on-screen keyboard event (SYSUTIL_OSK_*)
// The system's own layers (PS button menu, dialogs) draw over our picture: while one is up and
// right after it closes, the finished canvas is copied to the screen again.
static volatile int g_sys_overlay = 0;   // 1 = a system layer is up, 2 = it just closed
static void sysutil_callback(u64 status, u64 param, void *usrdata)
{
	if (status == SYSUTIL_EXIT_GAME) exit_requested = 1;
	if (status == SYSUTIL_MENU_OPEN || status == SYSUTIL_DRAW_BEGIN) g_sys_overlay = 1;
	if (status == SYSUTIL_MENU_CLOSE || status == SYSUTIL_DRAW_END) g_sys_overlay = 2;
	if (status >= SYSUTIL_OSK_LOADED && status <= SYSUTIL_OSK_INPUT_CANCELED) g_osk_event = (int)status;
}

// the system's disc callbacks are about its own drive only
static void disc_eject_cb(void) { g_cards[0].inserted = 0; g_cards[0].present = 0; }
static void disc_insert_cb(u32 type, char *title) { g_cards[0].inserted = 1; g_cards[0].present = 1; }

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
	{   // the title, and under it the version, small and grey (a local build's is long: 1.0.2-20261103.0835)
		// the title moves up so that both end where the title alone did: the screens below keep their layout
		int y = ui_h() / 10 - ui_line(UI_SMALL) * 3 / 4;
		ui_text(UI_TITLE, ui_w() / 2, y, 0xFFFFFF, "PSX RiPPER", UI_CENTER);
		ui_text(UI_SMALL, ui_w() / 2, y + ui_line(UI_TITLE) - ui_line(UI_SMALL) / 4, 0x8890A8, "v" APP_VERSION, UI_CENTER);
		// an agent may be controlling the app: always in sight, at the left of the title
		if (g_mcp) ui_text(UI_SMALL, ui_w() / 6, y + (ui_line(UI_TITLE) - ui_line(UI_SMALL)) / 2, 0xF0C060, "[MCP]");
	}
}

// Frames still to (re)draw: content is drawn into both buffers once, then only flipped.
static int g_redraw = 2;          // canvas needs to be redrawn
static int g_drawn = 0;           // canvas changed: copy it to both RSX buffers on the next flips
static int g_backdrop_shown = 0;

static void present()
{
	if (g_sys_overlay) {   // repaint both buffers once the system layer is gone
		g_drawn = 2;
		if (g_sys_overlay == 2) g_sys_overlay = 0;
	}
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
static void own_dialog_draw(const char *msg, int yesno);   // one frame of it (the UI tests draw it)
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

// ATAPI packets are SCSI commands: with the USB drive selected they go over our own USB driver.
// The SuperDrive is an ATAPI drive behind a USB bridge: it only answers 12-byte packets.
static int cdb_len(const uint8_t *) { return 12; }

static int atapi(const uint8_t *cdb, uint8_t *buf, uint32_t len, uint32_t proto)
{
	if (g_use == SRC_USB) return usbodd_cmd(g_usb, cdb, cdb_len(cdb), buf, len, 1);
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
static uint8_t bcd(int v);
static uint32_t edc(const uint8_t *p, int n);
static void edc_init();
static int debug_cmd(const char *q, char *out, int n)
{
	char v[80], v2[600], opbuf[16];
	if (!g_debug_on) { snprintf(out, n, "debug mode off (create /dev_hdd0/tmp/psxrip.debug)\n"); return 0; }
	const char *op = qget(q, "op", opbuf, sizeof(opbuf));   // own buffer: v is reused below
	if (op && !strcmp(op, "hdrscan")) {   // a finished image: is every sector where its header says, and intact?
		// hdrscan&path=/dev_hdd0/PSXISO/x.bin (%xx decoded)&n=<sectors of the data track>; reads the file only
		char path[300];
		qget(q, "path", v2, sizeof(v2));
		size_t o = 0;
		for (const char *c = v2; *c && o < sizeof(path) - 1; c++) {
			if (*c == '%' && c[1] && c[2]) { char h[3] = {c[1], c[2], 0}; path[o++] = (char)strtol(h, NULL, 16); c += 2; }
			else path[o++] = *c == '+' ? ' ' : *c;
		}
		path[o] = 0;
		uint32_t lim = qget(q, "n", v, sizeof(v)) ? strtoul(v, NULL, 0) : 0xFFFFFFFF;
		FILE *f = fopen(path, "rb");
		if (!f) { snprintf(out, n, "cannot open %s\n", path); return 0; }
		edc_init();   // the table is otherwise made when a rip starts
		static uint8_t sec[RAW_SIZE];
		static const uint8_t sync[12] = {0, 255, 255, 255, 255, 255, 255, 255, 255, 255, 255, 0};
		uint32_t i = 0, nsync = 0, naddr = 0, nedc = 0, nsub = 0, nzero_edc = 0, nmode[4] = {0, 0, 0, 0}, shown = 0;
		snprintf(out, n, "hdrscan %s\n", path);
		for (; i < lim && fread(sec, 1, RAW_SIZE, f) == RAW_SIZE; i++) {
			const char *bad = 0;
			char why[96] = "";
			if (memcmp(sec, sync, 12)) { nsync++; bad = "sync"; }
			else {
				uint32_t a = i + 150;
				if (sec[12] != bcd(a / 4500) || sec[13] != bcd((a / 75) % 60) || sec[14] != bcd(a % 75)) {
					naddr++; bad = "address";
					auto ub = [](uint8_t b) { return (b >> 4) * 10 + (b & 15); };
					snprintf(why, sizeof(why), " header says lba %d", ub(sec[12]) * 4500 + ub(sec[13]) * 75 + ub(sec[14]) - 150);
				}
				nmode[sec[15] & 3]++;
				if (sec[15] == 2 && memcmp(sec + 16, sec + 20, 4)) { nsub++; if (!bad) bad = "subheader copies differ"; }
				uint32_t calc = 0, stored = 0; const uint8_t *st = 0;
				if (sec[15] == 1) { calc = edc(sec, 2064); st = sec + 2064; }
				else if (sec[15] == 2 && (sec[18] & 0x20)) { calc = edc(sec + 16, 2332); st = sec + 2348; }
				else if (sec[15] == 2) { calc = edc(sec + 16, 2056); st = sec + 2072; }
				if (st) {
					stored = st[0] | st[1] << 8 | st[2] << 16 | (uint32_t)st[3] << 24;
					if (sec[15] == 2 && (sec[18] & 0x20) && !stored) nzero_edc++;
					else if (stored != calc) { nedc++; if (!bad) bad = "EDC"; }
				}
			}
			if (bad && shown < 16) {
				shown++;
				size_t k = strlen(out);
				snprintf(out + k, n - k, "  lba %u: %s%s  [%02x %02x %02x %02x | %02x %02x %02x %02x]\n", i, bad, why, sec[12], sec[13], sec[14], sec[15], sec[16], sec[17], sec[18], sec[19]);
			}
		}
		fclose(f);
		size_t k = strlen(out);
		snprintf(out + k, n - k, "%u sectors: sync %u, address %u, EDC %u, subheader %u; mode0 %u mode1 %u mode2 %u; form 2 without EDC %u\n",
		         i, nsync, naddr, nedc, nsub, nmode[0], nmode[1], nmode[2], nzero_edc);
		return 0;
	}
	if (g_state_busy()) { snprintf(out, n, "busy: the app is using the drive\n"); return 0; }
	if (!op) { snprintf(out, n, "ops: list | usb | usbscsi&cdb=hex&len=&dir=in|out[&drive=n] | scsi&id=&sc=616|604&proto=0|1|3&dir=0|1&cdb=hex&len= | devcmd&id=&cmd=&in=hex&len= | read&id=&lba=\n"); return 0; }
	if (!strcmp(op, "usb")) { usbodd_status(out, n); return 0; }
	int dv = qget(q, "drive", v, sizeof(v)) ? atoi(v) : g_usb;   // which SuperDrive
	if (!strcmp(op, "usbreset")) { snprintf(out, n, "bulk-only reset -> %d\n", usbodd_reset(dv)); return 0; }
	if (!strcmp(op, "usbbench")) {   // speed: n sectors from lba, per command, READ CD with/without Q or READ(10)
		uint32_t lba = qget(q, "lba", v, sizeof(v)) ? strtoul(v, NULL, 0) : 20000;
		int cnt = qget(q, "n", v, sizeof(v)) ? atoi(v) : 2000;
		int per = qget(q, "per", v, sizeof(v)) ? atoi(v) : 27;
		int sub = qget(q, "sub", v, sizeof(v)) ? atoi(v) : 2;         // 0 none, 2 formatted Q; -1 = READ(10) user data
		int sp = qget(q, "speed", v, sizeof(v)) ? atoi(v) : -2;       // KB/s for SET CD SPEED first (-2 = leave)
		int blk = sub < 0 ? 2048 : sub == 2 ? RAW_SIZE + SUBQ_SIZE : RAW_SIZE;
		out[0] = 0;
		if (per < 1) per = 1;
		if (per > USBODD_MAX / blk) per = USBODD_MAX / blk;
		static uint8_t data[USBODD_MAX];
		if (sp != -2) {
			uint8_t c[12] = {0xBB, 0, (uint8_t)(sp >> 8), (uint8_t)sp, 0xFF, 0xFF, 0, 0, 0, 0, 0, 0};
			snprintf(out, n, "set cd speed %d -> 0x%x\n", sp, usbodd_cmd(dv, c, 12, NULL, 0, 0));
		}
		struct timeval tv0, tv1;
		gettimeofday(&tv0, NULL);
		int errs = 0, done = 0;
		while (done < cnt) {
			int k = cnt - done < per ? cnt - done : per;
			uint32_t l = lba + done;
			uint8_t c[12] = {0};
			if (sub < 0) { c[0] = 0x28; c[2] = l >> 24; c[3] = l >> 16; c[4] = l >> 8; c[5] = l; c[7] = k >> 8; c[8] = k; }
			else { c[0] = 0xBE; c[2] = l >> 24; c[3] = l >> 16; c[4] = l >> 8; c[5] = l; c[6] = k >> 16; c[7] = k >> 8; c[8] = k; c[9] = 0xF8; c[10] = sub; }
			if (usbodd_cmd(dv, c, 12, data, k * blk, 1)) errs++;
			done += k;
		}
		gettimeofday(&tv1, NULL);
		uint64_t ms = (uint64_t)(tv1.tv_sec - tv0.tv_sec) * 1000 + (tv1.tv_usec - tv0.tv_usec) / 1000;
		size_t o = strlen(out);
		snprintf(out + o, n - o, "%d sectors from %u, %d per command, sub %d: %llu ms, %.0f KB/s (%.1fx), %d errors\n", cnt, lba, per, sub,
		         (unsigned long long)ms, ms ? cnt * 2352.0 / 1.024 / ms : 0.0, ms ? cnt * 1000.0 / 75 / ms : 0.0, errs);
		return 0;
	}
	if (!strcmp(op, "usbscsi")) {   // SCSI to the USB optical drive over our own Bulk-Only Transport
		uint8_t cdb[16] = {0};
		int cl = unhex(qget(q, "cdb", v2, sizeof(v2)) ? v2 : "", cdb, 16);
		int len = qget(q, "len", v, sizeof(v)) ? atoi(v) : 0;
		int din = !qget(q, "dir", v, sizeof(v)) || strcmp(v, "out");
		static uint8_t data[65536];
		memset(data, 0, sizeof(data));
		int st = -1, got = 0;
		char lg[600] = "";
		int r = usbodd_scsi(dv, cdb, cl ? cl : 12, data, len < 0 || len > 65536 ? 0 : len, din, &st, &got, lg, sizeof(lg));
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
	if (g_use == SRC_USB) return usbodd_cmd(g_usb, cdb, cdb_len(cdb), buf, len, 0);
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

static int er_read(uint8_t page[12])
{
	uint8_t (&b)[64] = CX.er_b;
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
	uint8_t (&b)[20] = CX.er_w;
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


static uint32_t be32(const uint8_t *p) { return (p[0] << 24) | (p[1] << 16) | (p[2] << 8) | p[3]; }

static void tstart_init();
static int read_toc()
{
	uint8_t cdb[12] = {0x43, 0x00, 0x00, 0, 0, 0, 0x01, 0x03, 0x24, 0, 0, 0};  // LBA, format 0, from track 1, alloc 804
	memset(g_cmdbuf, 0, 0x324);
	int ret = -1;
	// probe which syscall/proto combination the drive accepts
	const uint32_t scs[2] = {616, 604};
	const uint32_t protos[2] = {ATAPI_PIO_DATA_IN_PROTO, ATAPI_DMA_PROTO};
	if (g_use == SRC_USB) { ret = atapi(cdb, g_cmdbuf, 0x324, 0); logf("READ TOC (USB) -> 0x%x", ret); }
	for (int s = 0; s < 2 && ret && g_use == SRC_BD; s++)
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
	tstart_init();   // the pregap starts of this disc, not of the one read before it
	return (g_ntracks > 0 && g_leadout > 0) ? 0 : -2;
}

// Reads `count` raw sectors, each 2352 bytes followed by 16 bytes formatted Q,
// the same way IRISMAN does (syscall 604, one ATAPI block per sector, DMA).

// The first audio track after the data track: where its pregap starts (INDEX 00) and INDEX 01.
// 0 when the disc has no audio track after a data track.
static uint32_t audio_pregap(uint32_t *index1)
{
	for (int t = 1; t < g_ntracks; t++)
		if (!(g_tracks[t].ctrl & 0x4) && (g_tracks[t - 1].ctrl & 0x4)) {
			uint32_t l = g_tracks[t].lba, s = g_tstart[t];
			*index1 = l;
			// the pregap start from the subchannel, when it belongs to this disc: within 10 s before
			// INDEX 01 (a value left from another disc, read with the same drive, made identify read
			// SYSTEM.CNF as audio); else the usual 2 s
			if (s < l && s + 750 >= l) return s;
			return l >= 150 ? l - 150 : 0;
		}
	return 0;
}

static int sector_zero(const uint8_t *p)
{
	for (int i = 0; i < RAW_SIZE; i++) if (p[i]) return 0;
	return 1;
}

// A job stopped because its disc left the drive (not because O was pressed): set by the read,
// cleared when a job starts.
static volatile int g_disc_lost[NCARDS];

// READ CD with a chosen subchannel selection: 0 = none, 1 = raw P-W (96 bytes), 2 = formatted Q (16 bytes)
static int read_cd_sub(uint32_t lba, uint32_t count, uint8_t *buf, uint8_t sub, uint32_t blk)
{
	if (g_use == SRC_USB) {   // one USB command moves at most USBODD_MAX bytes: split the read
		// The 2 s pregap between the data track and the first audio track: a SuperDrive refuses every
		// sector there when asked for "any sector type" (the subchannel says audio, the drive does not
		// agree), so a rip spent an hour re-reading 150 sectors and filled them with silence. Asked
		// for CD-DA it reads them. Audio sectors are read as CD-DA from that pregap on; a read never
		// spans the boundary, so data sectors are never read as audio.
		uint32_t idx1 = 0, ap = audio_pregap(&idx1);
		(void)idx1;
		uint32_t per = USBODD_MAX / blk;
		for (uint32_t done = 0; done < count; ) {
			uint32_t n = count - done < per ? count - done : per, l = lba + done;
			int audio = ap && l >= ap;
			if (ap && l < ap && l + n > ap) n = ap - l;
			uint8_t c[12] = {0xBE, (uint8_t)(audio ? 0x04 : 0), (uint8_t)(l >> 24), (uint8_t)(l >> 16), (uint8_t)(l >> 8), (uint8_t)l,
			                 (uint8_t)(n >> 16), (uint8_t)(n >> 8), (uint8_t)n, 0xF8, sub, 0};
			int r = usbodd_cmd(g_usb, c, 12, buf + (size_t)done * blk, n * blk, 1);
			// the disc is gone (taken out, or the SuperDrive pushed out a disc it could not read):
			// stop the job instead of "re-reading" a thousand sectors that all fail at once
			if (r) {   // what the drive said about a failed read: when it changes, and every 100th
				static int last[NCARDS], cnt[NCARDS];
				int c = cur_card();
				if (r != last[c] || ++cnt[c] % 100 == 0) { logf("read %u x%u failed: 0x%x", l, n, r); last[c] = r; cnt[c] = 0; }
			}
			if (((uint32_t)r & 0xFF000000u) == 0x8D000000u && (r >> 16 & 0x0F) == 0x02 && (r >> 8 & 0xFF) == 0x3A && !g_abort) {
				g_abort = 1;
				g_disc_lost[cur_card()] = 1;   // not cancelled: the end says so (disc_lost)
				logf("read %u: no disc in the drive (medium not present), stopping", l);
			}
			if (r) return r;
			if (usbodd_last_got(g_usb) != (int)(n * blk)) return -1;   // fewer sectors than asked for
			done += n;
		}
		return 0;
	}
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

// The first sector of the first audio track's pregap, right after the data track: both the PS3's
// drive and a SuperDrive hand out junk for it (a few hundred stray bytes, different per drive), while
// the disc has silence there, as in Redump (Tomba! 2: zeroing it gives Redump's track MD5). When the
// sector after it is silent, it is stored as silence. A pregap with real sound in it is left alone.
// `buf` holds `count` sectors of 2352 + 16 (Q) bytes starting at `lba`.
static void pregap_transition(uint32_t lba, uint32_t count, uint8_t *buf)
{
	uint32_t idx1 = 0, ap = audio_pregap(&idx1);
	if (!ap || ap < lba || ap >= lba + count || ap + 1 >= idx1) return;
	uint8_t *s0 = buf + (size_t)(ap - lba) * SECT_Q;
	if (sector_zero(s0)) return;
	int next_silent;
	if (ap + 1 < lba + count) next_silent = sector_zero(s0 + SECT_Q);
	else {
		uint8_t *nb = (uint8_t *)memalign(128, SECT_Q);
		next_silent = nb && read_cd_sub(ap + 1, 1, nb, 0x02, SECT_Q) == 0 && sector_zero(nb);
		free(nb);
	}
	if (!next_silent) return;
	memset(s0, 0, RAW_SIZE);
	logf("sector %u: first pregap sector of the first audio track read as junk, stored as silence", ap);
}

static int q_needed(uint32_t lba, uint32_t count);
static int read_cd_q(uint32_t lba, uint32_t count, uint8_t *buf);
static int read_cd(uint32_t lba, uint32_t count, uint8_t *buf)
{
	int r = read_cd_q(lba, count, buf);
	if (r == 0 && g_ntracks > 1) pregap_transition(lba, count, buf);
	return r;
}

static int read_cd_q(uint32_t lba, uint32_t count, uint8_t *buf)
{
	// A USB SuperDrive reads Q at about 2x but without it at 5-8x: read Q only where the ripper looks
	// at it, elsewhere the Q bytes are left zero (counted as "no Q", like a drive that gives none)
	if (g_use == SRC_USB && !q_needed(lba, count)) {
		int r = read_cd_sub(lba, count, buf, 0x00, RAW_SIZE);
		if (r) return r;
		for (int i = (int)count - 1; i >= 0; i--) {   // 2352-byte sectors -> 2368 with Q, from the end, in place
			memmove(buf + (size_t)i * SECT_Q, buf + (size_t)i * RAW_SIZE, RAW_SIZE);
			memset(buf + (size_t)i * SECT_Q + RAW_SIZE, 0, SUBQ_SIZE);
		}
		return 0;
	}
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
	if (g_use == SRC_USB) {
		memset(g_sec, 0, 4096);
		int r = read_cd(16, 1, g_sec);
		logf("READ CD probe (USB) -> 0x%x [%02x %02x .. %02x %02x]", r, g_sec[0], g_sec[1], g_sec[15], g_sec[24]);
		return r;
	}
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


// The disc ID out of SYSTEM.CNF: "BOOT = cdrom:\\SLUS_005.53;1" -> "SLUS_005.53". Some discs write
// it in lower case ("cdrom:\\scus_943.51;1", Wipeout XL): matched either way, stored in upper case.
// Empty when there is none.
static void serial_from_cnf(const char *cnf, char *out)
{
	out[0] = 0;
	for (const char *p = cnf; *p; p++) {
		int ok = 1;
		for (int i = 0; i < 11 && ok; i++) {
			char c = p[i];
			if (i < 4) ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
			else if (i == 4) ok = c == '_';
			else if (i == 8) ok = c == '.';
			else ok = c >= '0' && c <= '9';
		}
		if (!ok) continue;
		for (int i = 0; i < 11; i++) out[i] = (p[i] >= 'a' && p[i] <= 'z') ? p[i] - 32 : p[i];
		out[11] = 0;
		return;
	}
}

// The disc ID out of the volume name, for the earliest discs that have no SYSTEM.CNF (they boot
// PSX.EXE; Gokujou Parodius Da! Deluxe Pack, SLPS-00002, says "SLPS00002"): four letters and five
// digits, with or without "_" / "-" between them -> "SLPS_000.02". Empty when it is not one.
static void serial_from_volume(const char *vol, char *out)
{
	out[0] = 0;
	char c[9];
	int n = 0;
	for (const char *p = vol; *p; p++) {
		if (n == 4 && (*p == '_' || *p == '-')) continue;
		if (n == 9) return;   // longer than an ID
		c[n++] = *p;
	}
	if (n != 9) return;
	for (int i = 0; i < 9; i++) {
		int ok = i < 4 ? ((c[i] >= 'A' && c[i] <= 'Z') || (c[i] >= 'a' && c[i] <= 'z')) : (c[i] >= '0' && c[i] <= '9');
		if (!ok) return;
	}
	for (int i = 0; i < 4; i++) out[i] = (c[i] >= 'a' && c[i] <= 'z') ? c[i] - 32 : c[i];
	out[4] = '_'; out[5] = c[4]; out[6] = c[5]; out[7] = c[6]; out[8] = '.'; out[9] = c[7]; out[10] = c[8]; out[11] = 0;
}

static void detect_disc_id()
{
	uint8_t pvd[2048], dir[2048], cnf[2048];
	g_serial[0] = 0;
	g_volid[0] = 0;
	g_publisher[0] = 0;
	if (read_user_sector(16, pvd) || memcmp(pvd + 1, "CD001", 5)) { logf("no ISO9660 PVD"); return; }

	memcpy(g_volid, pvd + 40, 32);
	g_volid[32] = 0;
	for (int i = 31; i >= 0 && (g_volid[i] == ' ' || g_volid[i] == 0); i--) g_volid[i] = 0;
	memcpy(g_publisher, pvd + 318, 128);
	g_publisher[128] = 0;
	for (int i = 127; i >= 0 && (g_publisher[i] == ' ' || g_publisher[i] == 0); i--) g_publisher[i] = 0;

	uint32_t root = pvd[158] | (pvd[159] << 8) | (pvd[160] << 16) | (pvd[161] << 24);  // root extent, little-endian copy
	logf("volume=%s root=%u", g_volid, root);
	if (read_user_sector(root, dir)) { logf("root dir read failed"); return; }
	uint32_t lba, size;
	if (find_file(dir, 2048, "SYSTEM.CNF", &lba, &size) || read_user_sector(lba, cnf)) {
		serial_from_volume(g_volid, g_serial);   // the earliest discs boot PSX.EXE and have none
		logf("SYSTEM.CNF not found (vol=%s), serial from the volume name: %s", g_volid, g_serial[0] ? g_serial : "none");
		return;
	}
	cnf[2047] = 0;
	serial_from_cnf((const char *)cnf, g_serial);
	logf("volume=%s serial=%s publisher=%s", g_volid, g_serial, g_publisher);
}

// ---------------------------------------------------------------- serial -> title
#include "titles.inc"

// Discs pressed with another game's ID in SYSTEM.CNF: the ID and the volume's publisher pick the
// right title (the ID alone is the other game in the database). NULL when the disc is not one of them.
static const char *title_override(const char *serial, const char *publisher)
{
	static const struct { const char *serial, *publisher, *title; } t[] = {
		// Castlevania Chronicles (USA) is SLUS-01384, but Konami wrote SLUS_013.48 (Tennis) on the
		// disc, in SYSTEM.CNF and the volume name (volume set "CASTLEVAINA", publisher KONAMI)
		{"SLUS_013.48", "KONAMI", "Castlevania Chronicles"},
	};
	for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++)
		if (!strcmp(serial, t[i].serial) && !strcmp(publisher, t[i].publisher)) return t[i].title;
	return NULL;
}

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


static int bcd2(uint8_t b) { return (b >> 4) * 10 + (b & 0x0F); }

// Many drives return the Q subchannel of a neighbouring sector (typically +1). The offset is
// learned from the first sectors (LibCrypt sectors live far beyond them) and then applied.
#define QCAL_SECTORS 512
#define LC_LBA_MIN 13000
#define LC_LBA_MAX 45000


// Sectors whose Q the ripper uses: the calibration at the start, the LibCrypt area of a PAL disc and
// the 10 s before each later track (pregap starts).
static int q_needed(uint32_t lba, uint32_t count)
{
	uint32_t end = lba + count;
	if (lba < QCAL_SECTORS + 2) return 1;
	if (g_is_pal && end > LC_LBA_MIN - 2 && lba < LC_LBA_MAX + 2) return 1;
	for (int t = 1; t < g_ntracks; t++) {
		uint32_t l = g_tracks[t].lba;
		if (end + 760 > l && lba < l + 2) return 1;
	}
	return 0;
}

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
// sector being re-read right now (shown on the progress screen); g_rr_max = 0 when not re-reading
// Sectors that failed in the first pass (unreadable, or bit 31 set: wrong EDC). They get a second
// pass at low speed once the whole disc has been read, the way data recovery tools work.

static void note_hard(uint32_t lba, int edc_bad)
{
	if (g_nhard < MAX_HARD) g_hard[g_nhard++] = lba | (edc_bad ? 0x80000000u : 0);
}

// Sectors that were wrong at first but fixed in the first pass (EDC fixed by a re-read, or two
// reads that disagreed): they are confirmed in the second pass. Their data is kept to see whether
// the confirmation changes them.
#define HARD_RESOLVED 0x40000000u          // g_hard flag: read correctly in the second pass

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
static int g_ripped = 0;    // discs ripped this session
static int g_headless = 0;  // remote test mode: no dialogs, no eject, exit after one rip
#define AUTO_FLAG "/dev_hdd0/tmp/psxrip.auto"

static int track_is_audio(uint32_t lba)
{
	for (int t = g_ntracks - 1; t >= 0; t--)
		if (lba >= g_tracks[t].lba) return !(g_tracks[t].ctrl & 0x4);
	return 0;
}

// Double buffering: the reader thread fills one buffer from the drive while the writer
// thread flushes the other to the hard disk, so drive and disk work in parallel.
// MD5 per track, computed by the writer thread (Redump hashes each track separately)
// First sector of each track as Redump splits a disc: tracks after the first start at their
// pregap (INDEX 00), not at INDEX 01 from the TOC. Starts with the usual 2 s pregap after a data
// track and is corrected from the Q subchannel (absolute time + track number) while reading.

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


// Sector check (data track 1): sync pattern, header address and EDC. The drive can hand out a
// sector with uncorrected errors and still report success, so the reader re-reads such sectors.
static uint32_t g_edc_tab[256];

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
	uint8_t (&t)[RAW_SIZE] = CX.ecc_t;
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
// The first pass only records a fingerprint of each sector without EDC; a verify pass after it
// re-reads them in long runs at full speed (no seeking back and forth while ripping).
#define MAX_UV (256 * 1024)

// Recent events for the progress screen ("Sector 125454 fixed by re-reading"), newest last.
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
	t_card = (int)(long)arg;
	for (int k = 0;; k ^= 1) {
		while (!g_buf_len[k] && !(g_reader_done && !g_buf_len[k ^ 1] && !g_buf_len[k])) usleep(500);
		if (!g_buf_len[k]) break;   // reader done and nothing left
		__sync_synchronize();
		uint32_t n = g_buf_len[k];
		if (vf_write(g_out, g_bufs[k], (size_t)RAW_SIZE * n) != (size_t)RAW_SIZE * n) g_write_failed = 1;
		// feed each sector range to the MD5 of the track it belongs to
		uint32_t first = g_buf_end[k] - n;
		{   // midstate before this buffer: a later patch re-reads only from here, not the whole image
			int t0 = g_ntracks - 1;
			while (t0 > 0 && first < g_tstart[t0]) t0--;
			if (g_nsnap < 512) { g_snap[g_nsnap].lba = first; g_snap[g_nsnap].t = t0; g_snap[g_nsnap].m = g_md5[t0]; g_nsnap++; }
		}
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
	uint8_t (&prev)[SECT_Q] = CX.cr_prev;
	int streak = 0;
	for (int t = 0; t < max_reads && !g_abort && !g_skip; t++) {
		g_rr_try = t + 1;
		if (read_cd(lba, 1, g_sec) || (need_edc && check_sector(lba, g_sec))) { streak = 0; continue; }
		if (streak && !memcmp(prev, g_sec, RAW_SIZE)) streak++;
		else { streak = 1; memcpy(prev, g_sec, RAW_SIZE); }
		if (streak == 3) { memcpy(out, prev, RAW_SIZE); return 1; }
	}
	return 0;
}

// A repair: the tracks' MD5s from the earlier rip's log, and which tracks the repair wrote to. Only
// those are hashed again (a whole image took minutes; a repaired pregap sector is in a small track).
static uint8_t g_md5_known[NCARDS][MAX_TRACKS + 1], g_md5_dirty[NCARDS][MAX_TRACKS + 1];
// sectors hashed of how many while MD5s are recomputed (the page shows it); 0 total = not now
static volatile uint32_t g_md5_done[NCARDS], g_md5_total[NCARDS];

static int track_of(uint32_t lba)
{
	int t = 0;
	while (t + 1 < g_ntracks && lba >= g_tstart[t + 1]) t++;
	return t;
}

// The audit's key of the card's disc: its ID (its title when it has none) and its size in sectors.
static void disc_key(char *out, size_t n)
{
	const Card &k = g_cards[cur_card()];
	snprintf(out, n, "%s:%u", k.serial[0] ? k.serial : k.title, (unsigned)g_leadout);
}

// Writes one sector into the image (g_out) and logs it in the audit log with the bytes it replaces.
// Old bytes that cannot be read count as a failed write: no sector is changed without them in the
// log (a rip opens its file w+b so they can be read back). Returns 0 when written.
static int write_sector_audited(uint32_t lba, const uint8_t *sec)
{
	uint8_t old[RAW_SIZE];
	uint64_t pos = (uint64_t)lba * RAW_SIZE;
	if (vf_seek(g_out, pos) || vf_read(g_out, old, RAW_SIZE) != RAW_SIZE) {
		logf("sector %u: its old bytes cannot be read, not written", lba);
		return -1;
	}
	if (vf_seek(g_out, pos) || vf_write(g_out, sec, RAW_SIZE) != RAW_SIZE) return -1;
	uint32_t job = g_cards[cur_card()].audit_job;
	if (job) {   // 0: not started as a job (headless test mode), nothing to log it under
		char file[300];
		snprintf(file, sizeof(file), "%s.bin", g_base);   // a rip's .part becomes this file
		audit_sector(job, file, lba, old, sec);
	}
	return 0;
}

// a sector written into the image: its track's MD5 must be computed again
static void note_patched(uint32_t lba)
{
	if (lba < g_patch_min) g_patch_min = lba;
	g_md5_dirty[cur_card()][track_of(lba)] = 1;
}

// Verify pass (bit-perfect mode): re-reads the sectors without EDC in long runs at full speed and
// compares them with their first-pass fingerprint. A sector that differs is read until three reads
// agree and written into the image. Returns the number of sectors written.
static int verify_pass()
{
	if (!g_nuv || g_abort || g_write_failed) return 0;
	g_phase = 1;
	g_pass2_total = g_nuv; g_pass2_done = 0;
	rip_event(0xA8B0C8, STR_EV_VERIFY_START, g_nuv);   // why this step: the drive cannot vouch for these sectors
	uint64_t t0 = now_ms();
	int patched = 0, reads = g_retries * 3 + 3;
	uint8_t (&sec)[RAW_SIZE] = CX.vp_sec;
	for (uint32_t k = 0; k < g_nuv && !g_abort && !g_skip; ) {
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
					if (write_sector_audited(lba, sec) == 0) { patched++; g_changed++; note_patched(lba); }
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
	uint8_t (&sec)[RAW_SIZE] = CX.sp_sec;
	time_t t0 = time(NULL);
	for (uint32_t k = 0; k < total && !g_abort && !g_skip; k++) {
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
					if (write_sector_audited(lba, sec) == 0) { patched++; note_patched(lba); }
					logf("second pass: sector %u confirmed with different data, image updated", lba);
					rip_event(0x7CE0A0, STR_EV_DIFFER, lba);
				} else {
					rip_event(0x7CE0A0, STR_EV_CONFIRMED, lba);   // the fix from the first pass holds
				}
			} else {
				logf("second pass: sector %u could not be confirmed", lba);
			}
		} else if (ok && write_sector_audited(lba, sec) == 0) {
			patched++;
			note_patched(lba);
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
	     patched, g_confirmed, g_nsuspect, g_changed, (long)(time(NULL) - t0), g_skip ? " (skipped with square)" : "");
	return patched;
}

// Only the written tracks, when every other track's MD5 is known. 1 = done, 0 = hash the whole image.
static int md5_dirty_tracks(VFile *f, uint8_t *b)
{
	int c = cur_card(), any = 0;
	uint64_t todo = 0, done = 0;
	for (int t = 0; t < g_ntracks; t++) {
		if (!g_md5_dirty[c][t] && !g_md5_known[c][t]) return 0;
		uint32_t end = t + 1 < g_ntracks ? g_tstart[t + 1] : g_leadout;
		if (g_md5_dirty[c][t]) { todo += end - g_tstart[t]; any = 1; }
	}
	if (!any) return 0;
	g_md5_total[c] = (uint32_t)todo; g_md5_done[c] = 0;
	for (int t = 0; t < g_ntracks; t++) {
		if (!g_md5_dirty[c][t]) continue;
		uint32_t lba = g_tstart[t], end = t + 1 < g_ntracks ? g_tstart[t + 1] : g_leadout;
		Md5 m;
		md5_init(&m);
		if (vf_seek(f, (uint64_t)lba * RAW_SIZE)) return 0;
		while (lba < end) {
			uint32_t n = end - lba < 256 ? end - lba : 256;
			if (vf_read(f, b, (size_t)RAW_SIZE * n) != (size_t)RAW_SIZE * n) return 0;
			md5_update(&m, b, (size_t)n * RAW_SIZE);
			lba += n; done += n;
			g_md5_done[c] = (uint32_t)done;
		}
		md5_final(&m, g_track_md5bin[t]);
		logf("md5 of track %d recomputed (%u sectors), the other tracks' from the log", g_tracks[t].num, end - g_tstart[t]);
	}
	return 1;
}

// Recomputes the per-track MD5s from the finished image (after the second pass patched it).
static void md5_from_file(const char *path)
{
	VFile *f = vf_open(path, "rb");
	if (!f) return;
	g_phase = 3;   // the page says "Recomputing MD5" with the percentage
	{
		uint8_t *b = (uint8_t *)malloc(256 * RAW_SIZE);
		int ok = b && md5_dirty_tracks(f, b);
		free(b);
		if (ok) { vf_close(f); g_md5_total[cur_card()] = 0; return; }
	}
	g_md5_total[cur_card()] = g_leadout; g_md5_done[cur_card()] = 0;
	uint8_t *b = (uint8_t *)malloc(256 * RAW_SIZE);
	uint32_t lba = 0;
	int t = 0;
	md5_init(&g_md5[0]);
	// a rip that streamed its MD5s: start at the last midstate before the first patched sector; the
	// tracks before it keep their streamed MD5 (nothing in them changed)
	int si = -1;
	for (int i = 0; i < g_nsnap; i++) if (g_snap[i].lba <= g_patch_min) si = i;
	if (si >= 0 && g_patch_min != 0xFFFFFFFFu && vf_seek(f, (uint64_t)g_snap[si].lba * RAW_SIZE) == 0) {
		lba = g_snap[si].lba; t = g_snap[si].t;
		for (int k = 0; k < t; k++) md5_final(&g_md5[k], g_track_md5bin[k]);   // whole, unchanged
		g_md5[t] = g_snap[si].m;
		logf("md5 from sector %u (first patched %u), not from the start", lba, g_patch_min);
	} else if (lba == 0) vf_seek(f, 0);
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
		g_md5_done[cur_card()] = lba;
	}
	while (t < g_ntracks) { md5_final(&g_md5[t], g_track_md5bin[t]); if (++t < g_ntracks) md5_init(&g_md5[t]); }
	free(b);
	vf_close(f);
	g_md5_total[cur_card()] = 0;
	logf("md5 recomputed from %s", path);
}

static void rip_thread(void *arg)
{
	t_card = (int)(long)arg;
	char path[300];
	snprintf(path, sizeof(path), "%s.bin.part", g_base);   // renamed to .bin when complete
	g_out = vf_open(path, "w+b");   // w+b: the passes after the first read a sector back before writing it again
	if (!g_out) { logf("cannot create %s", path); g_result = -1; g_finished = 1; sysThreadExit(0); }

	g_bufs[0] = (uint8_t *)memalign(128, CHUNK * SECT_Q);
	g_bufs[1] = (uint8_t *)memalign(128, CHUNK * SECT_Q);
	g_buf_len[0] = g_buf_len[1] = 0;
	for (int t = 0; t < g_ntracks; t++) md5_init(&g_md5[t]);
	g_nsnap = 0; g_patch_min = 0xFFFFFFFFu;
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
	sysThreadCreate(&wtid, writer_thread, (void *)(long)cur_card(), 1000, 0x10000, THREAD_JOINABLE, (char *)"psxwrite");

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
		uint8_t (&dead)[CHUNK] = CX.rt_dead;   // sectors the drive could not read at all (zero-filled)
		memset(dead, 0, n);
		for (int tries = 0; tries < g_retries && ret; tries++) ret = read_cd(lba, n, buf);
		if (ret) {
			// Fall back to reads of 16 sectors; only a 16-sector piece that fails is read sector by
			// sector. Inside a damaged area (an unreadable sector within the last 256) each sector
			// gets one try, otherwise a scratch that hits once per revolution costs hours.
			// Cancelling is checked per sector.
			uint32_t &last_bad = CX.rt_last_bad;
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
#define g_space_short (g_cards[cur_card()].space_short)   // MB missing, 0 = fits or unknown
static void space_check(uint32_t disc_mb)
{
	uint64_t fr = dest_free_mb(OUT_DIR);
	g_space_short = (fr && fr < (uint64_t)disc_mb + 2) ? (int)(disc_mb + 2 - fr) : 0;
}


// The destination as "Hard disk · PSXISO" (drive and folder) and its free space in GB.
// tr: the texts' language (T = the console's; MCP asks for English and Swedish)
static void dest_parts(const char *dir, char *out, size_t n, unsigned *gb, Tr tr = T, int io = 1)   // io 0: no free-space query
{
	int port;
	uint64_t mb = 0;
	char label[48];
	const char *folder = strchr(dir + 1, '/');
	if (sscanf(dir, "exfat%1d:", &port) == 1) {
		if (io) vf_free_mb(dir, &mb);
		snprintf(label, sizeof(label), tr(STR_DEST_EXFAT), port);
		folder = strchr(dir, '/');
	} else {
		char dev[16];
		snprintf(dev, sizeof(dev), "%.12s", dir);
		char *sl = strchr(dev + 1, '/');
		if (sl) *sl = 0;
		if (!io || disk_free(dev, &mb)) mb = 0;
		if (sscanf(dir, "/dev_usb%3d", &port) == 1) snprintf(label, sizeof(label), tr(STR_DEST_USB), port);
		else snprintf(label, sizeof(label), "%s", tr(STR_DEST_HDD));
	}
	snprintf(out, n, "%s · %s", label, folder ? folder + 1 : "");
	*gb = (unsigned)(mb / 1024);
}

// A file name that fits maxw in font: whole, or cut in the middle so its end (disc ID, .bin) shows.
static void fit_middle(UiFont font, const char *in, char *out, size_t n, int maxw)
{
	snprintf(out, n, "%s", in);
	int len = (int)strlen(in), keep = len;
	while (ui_text_width(font, out) > maxw && keep > 4) {
		keep--;
		int head = keep / 2 + keep % 2, tail = keep / 2;
		while (head > 0 && ((unsigned char)in[head] & 0xC0) == 0x80) head--;   // whole UTF-8 characters
		int ts = len - tail;
		while (ts < len && ((unsigned char)in[ts] & 0xC0) == 0x80) ts++;
		snprintf(out, n, "%.*s…%s", head, in, in + ts);
	}
}

// The destinations there are, in the order left/right steps through them: the hard disk, then
// each USB port's drive (FAT32 or exFAT). Returns the count.
static int dest_list(char (*cands)[32])
{
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
	return nc;
}

// Destination next to the current one in the list hard disk, USB 0, USB 1, ... (by port; a FAT32
// drive is mounted by the PS3 as /dev_usbNNN, an exFAT drive is reached through FatFs).
// dir = +1 forward, -1 back.
static void next_target(char *outdir, int dir)
{
	char cands[9][32];
	int nc = dest_list(cands);
	int cur = 0;
	for (int i = 0; i < nc; i++) if (!strcmp(cands[i], outdir)) cur = i;   // not there any more: from the hard disk
	snprintf(outdir, 64, "%s", cands[(cur + (dir < 0 ? nc - 1 : 1)) % nc]);
}

// ---------------------------------------------------------------- disc identification for the home screen
static int prepare_disc(int *err, int *code);
#define g_disc_state (g_cards[cur_card()].state)
static volatile int g_ident_busy = 0;
static volatile int g_ident_card = -1;   // card being identified
static volatile int g_repairing = 0;   // a repair reads the drive
#define g_disc_title (g_cards[cur_card()].title)
#define g_disc_serial (g_cards[cur_card()].serial)
#define g_disc_mb (g_cards[cur_card()].mb)
#define g_repair_n (g_cards[cur_card()].repair_n)   // sectors a previous rip of this disc still has wrong (from its log)
#define g_prev_rip (g_cards[cur_card()].prev_rip)   // the destination already has <name>.bin for this disc

// Region of a disc: from Redump's name ("Croc 2 (Europe) (En,Fr)" -> "Europe"), else from the
// disc ID's prefix.
static void region_of(const char *rdname, const char *serial, char *out, size_t n)
{
	out[0] = 0;
	const char *a = rdname ? strchr(rdname, '(') : NULL, *b = a ? strchr(a, ')') : NULL;
	if (a && b && b > a + 1) { snprintf(out, n, "%.*s", (int)(b - a - 1), a + 1); return; }
	if (!serial || strlen(serial) < 4) return;
	if (serial[2] == 'E') snprintf(out, n, "Europe");                       // SLES, SCES, SLED, SCED
	else if (serial[2] == 'U') snprintf(out, n, "USA");                     // SLUS, SCUS
	else if (serial[2] == 'P' || (serial[0] == 'S' && serial[1] == 'I')) snprintf(out, n, "Japan");   // SLPS, SCPS, SLPM, SIPS
}

// The file name for a pattern; illegal characters replaced, empty brackets and double spaces gone.
static void name_from(const char *pat, const char *rdname, const char *title, const char *serial, char *out, size_t n)
{
	char t[160], r[64], b[300], dk[32] = "";
	// a disc of several: "(Disc 1)" in Redump's name, else in the app's own title list (GameDB-PSX)
	const char *dp = rdname && rdname[0] ? strstr(rdname, "(Disc ") : NULL;
	if (!dp) dp = strstr(title, "(Disc ");
	if (dp) { const char *e = strchr(dp, ')'); if (e) snprintf(dk, sizeof(dk), "%.*s", (int)(e - dp - 1), dp + 1); }
	// the title: up to the first bracket (Redump's name or the app's), then its disc
	const char *src = rdname && rdname[0] ? rdname : title;
	const char *q = strstr(src, " (");
	snprintf(t, sizeof(t), "%.*s", q ? (int)(q - src) : (int)strlen(src), src);
	if (dk[0]) snprintf(t + strlen(t), sizeof(t) - strlen(t), " (%s)", dk);
	region_of(rdname, serial, r, sizeof(r));
	size_t o = 0;
	for (const char *p = pat; *p && o < sizeof(b) - 1; p++) {
		const char *ins = NULL;
		if (*p == '%' && p[1]) {
			p++;
			ins = *p == 'T' ? t : *p == 'R' ? r : *p == 'D' ? serial : *p == 'N' ? (rdname && rdname[0] ? rdname : title) : *p == '%' ? "%" : "";
		}
		if (ins) o += snprintf(b + o, sizeof(b) - o, "%s", ins);
		else b[o++] = *p;
		if (o >= sizeof(b)) o = sizeof(b) - 1;
	}
	b[o] = 0;
	// "()" / "[]" left over from a missing region or ID, then spaces
	for (char *e; (e = strstr(b, "()")) || (e = strstr(b, "[]")); ) memmove(e, e + 2, strlen(e + 2) + 1);
	o = 0;
	for (size_t i = 0; b[i]; i++) {
		if (b[i] == ' ' && (o == 0 || b[o - 1] == ' ')) continue;
		b[o++] = b[i];
	}
	while (o && b[o - 1] == ' ') o--;
	b[o] = 0;
	if (!b[0]) snprintf(b, sizeof(b), "%s", title[0] ? title : "PSX DISC");
	sanitize(b);
	snprintf(out, n, "%s", b);
}

// How webMAN shows a file in its game list: without the text in [ ] (and the spaces around it).
static void listed_name(const char *fname, char *out, size_t n)
{
	size_t o = 0;
	int depth = 0;
	for (const char *p = fname; *p && o < n - 1; p++) {
		if (*p == '[') { depth++; continue; }
		if (*p == ']') { if (depth) depth--; continue; }
		if (!depth) out[o++] = *p;
	}
	out[o] = 0;
	char *w = out;   // spaces: single, none at the ends
	for (char *r = out; *r; r++) if (!(*r == ' ' && (w == out || w[-1] == ' '))) *w++ = *r;
	while (w > out && w[-1] == ' ') w--;
	*w = 0;
}

// Disc ID inside an image: ISO 9660 primary volume descriptor -> root directory -> SYSTEM.CNF ->
// "BOOT = cdrom:\SLES_020.88;1". For images without a rip log (older rips, other tools).
static int bin_serial(const char *path, char *serial, size_t n)
{
	VFile *f = vf_open(path, "rb");
	if (!f) return -1;
	uint8_t sec[RAW_SIZE];
	int ok = -1;
	auto user = [&](uint32_t lba) -> const uint8_t * {   // a sector's 2048 user bytes
		if (vf_seek(f, (uint64_t)lba * RAW_SIZE) || vf_read(f, sec, RAW_SIZE) != RAW_SIZE) return NULL;
		return sec + (sec[15] == 2 ? 24 : 16);
	};
	const uint8_t *pvd = user(16);
	if (pvd && !memcmp(pvd + 1, "CD001", 5)) {
		uint32_t root = pvd[158] | pvd[159] << 8 | pvd[160] << 16 | (uint32_t)pvd[161] << 24;
		const uint8_t *d = user(root);
		uint32_t cnf = 0;
		for (int o = 0; d && o < 2048 && d[o]; o += d[o]) {
			int nl = d[o + 32];
			if (nl >= 10 && !memcmp(d + o + 33, "SYSTEM.CNF", 10)) { cnf = d[o + 2] | d[o + 3] << 8 | d[o + 4] << 16 | (uint32_t)d[o + 5] << 24; break; }
		}
		const uint8_t *c = cnf ? user(cnf) : NULL;
		if (c) {
			char txt[2049];
			memcpy(txt, c, 2048); txt[2048] = 0;
			char *bt = strstr(txt, "BOOT");
			char *semi = bt ? strchr(bt, ';') : NULL;
			if (bt && semi) {
				char *st = semi;
				while (st > bt && st[-1] != '\\' && st[-1] != ':' && st[-1] != ' ' && st[-1] != '=') st--;
				if (semi - st >= 9 && semi - st < (long)n) { snprintf(serial, n, "%.*s", (int)(semi - st), st); ok = 0; }
			}
		}
	}
	vf_close(f);
	return ok;
}

// Earlier rips at a destination by disc ID, so one is found whatever its file name: the rip log
// says serial=, an image without a log is read. Built once per destination, again after a rip.
#define IDX_MAX 400
struct RipIdx { char dir[64]; int n, stale; struct { char serial[16]; char name[200]; } e[IDX_MAX]; };
static RipIdx g_idx[2];
static sys_mutex_t g_idx_mx;
static void idx_add(const char *name, void *ctx)
{
	RipIdx *x = (RipIdx *)ctx;
	size_t l = strlen(name);
	if (l < 5 || strcasecmp(name + l - 4, ".bin") || x->n >= IDX_MAX) return;
	char base[200], path[320], serial[16] = "";
	snprintf(base, sizeof(base), "%.*s", (int)(l - 4), name);
	snprintf(path, sizeof(path), "%s/%s.log", x->dir, base);
	VFile *f = vf_open(path, "rb");
	if (f) {
		char b[1024];
		int k = (int)vf_read(f, b, sizeof(b) - 1);
		vf_close(f);
		b[k > 0 ? k : 0] = 0;
		const char *sv = strstr(b, "\nserial=");
		if (sv) sscanf(sv + 8, "%15[^\n]", serial);
	}
	if (!serial[0]) {
		snprintf(path, sizeof(path), "%s/%s", x->dir, name);
		bin_serial(path, serial, sizeof(serial));
	}
	if (!serial[0]) return;
	snprintf(x->e[x->n].serial, sizeof(x->e[0].serial), "%s", serial);
	snprintf(x->e[x->n].name, sizeof(x->e[0].name), "%s", base);
	x->n++;
}
static void idx_stale() { for (int i = 0; i < 2; i++) g_idx[i].stale = 1; }
// Base path (dir/name) of an earlier rip of serial in dir, "" when there is none.
static void idx_find(const char *dir, const char *serial, char *out, size_t n)
{
	out[0] = 0;
	sysMutexLock(g_idx_mx, 0);
	RipIdx *x = !strcmp(g_idx[0].dir, dir) ? &g_idx[0] : !strcmp(g_idx[1].dir, dir) ? &g_idx[1] : NULL;
	if (!x || x->stale) {
		if (!x) { x = &g_idx[1]; g_idx[1] = g_idx[0]; x = &g_idx[0]; }   // keep the last two destinations
		snprintf(x->dir, sizeof(x->dir), "%s", dir);
		x->n = 0; x->stale = 0;
		if (vf_list(dir, idx_add, x) < 0) {   // a path the PS3 serves itself
			DIR *d = opendir(dir);
			if (d) {
				struct dirent *de;
				while ((de = readdir(d))) if (de->d_name[0] != '.') idx_add(de->d_name, x);
				closedir(d);
			}
		}
		logf("rip index %s: %d image(s)", dir, x->n);
	}
	for (int i = 0; i < x->n; i++)
		if (!strcmp(x->e[i].serial, serial)) { snprintf(out, n, "%s/%s", dir, x->e[i].name); break; }
	sysMutexUnlock(g_idx_mx);
}

// Counts the sectors in <g_base>.log that are still wrong (unreadable, bad, unconfirmed).
// An earlier rip (before 1.0.3) may hold the drive's junk in the first pregap sector of track 2
// (see pregap_transition): that sector not silent, the one after it silent. Returns that sector,
// 0 when the image is fine. `log` is the rip's log, `bin` its open image.
static uint32_t pregap_junk_in(VFile *bin, const char *log)
{
	const char *t2 = strstr(log, "track=02 start=");
	unsigned s = 0;
	if (!t2 || sscanf(t2, "track=02 start=%u", &s) != 1 || !s) return 0;
	static uint8_t sec[NCARDS][2 * RAW_SIZE];
	uint8_t *b = sec[cur_card()];
	if (vf_seek(bin, (uint64_t)s * RAW_SIZE) || vf_read(bin, b, 2 * RAW_SIZE) != 2 * RAW_SIZE) return 0;
	return !sector_zero(b) && sector_zero(b + RAW_SIZE) ? s : 0;
}

static int riplog_open_count()
{
	char path[320];
	snprintf(path, sizeof(path), "%s.log", g_base);
	snprintf(path + strlen(path) - 4, 8, ".bin");
	if (!vf_exists(path)) return 0;
	snprintf(path, sizeof(path), "%s.log", g_base);
	VFile *f = vf_open(path, "rb");
	if (!f) return 0;
	char (&buf)[65536] = CX.log_buf;
	int n = (int)vf_read(f, buf, sizeof(buf) - 1), cnt = 0;
	vf_close(f);
	buf[n > 0 ? n : 0] = 0;
	{   // and the junk sector of an older rip, which its log does not list
		snprintf(path, sizeof(path), "%s.bin", g_base);
		VFile *bf = vf_open(path, "rb");
		if (bf) { if (pregap_junk_in(bf, buf)) cnt++; vf_close(bf); }
	}
	for (char *l = strtok(buf, "\n"); l; l = strtok(NULL, "\n"))
		if (!strncmp(l, "unreadable=", 11) || !strncmp(l, "bad=", 4) || (!strncmp(l, "uncertain=", 10) && strstr(l, "unconfirmed")))
			cnt++;
	return cnt;
}

// Redump lookup for the home screen; runs after identification (network only, no drive access).
#define g_disc_rd (g_cards[cur_card()].rd)  // line shown on the home screen, empty = none
static volatile int g_rd_seq = 0;      // bumped when g_disc_rd changes
static sys_mutex_t g_rd_mx;            // one lookup at a time (redump.cpp uses static buffers); made in main()
static void rd_lock() { sysMutexLock(g_rd_mx, 0); }
static void rd_unlock() { sysMutexUnlock(g_rd_mx); }

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
	Card &k = g_cards[cur_card()];   // one disc it can be: its Redump name is the card's title
	if (hits == 1 && !k.rd_name[0]) snprintf(k.rd_name, sizeof(k.rd_name), "%s", names[0]);
	rd_set(line);
}

// What Redump says about the identified disc: by size (local copy) or by serial (online).
static void redump_lookup(const char *serial, uint64_t total, int ntracks)
{
	rd_lock();
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
	rd_unlock();
}

// Re-run the lookup for the disc already shown (the Redump setting was changed).
static void lookup_thread(void *arg)
{
	t_card = (int)(long)arg;
	rd_set("");
	if (g_redump != RD_OFF) redump_lookup(g_id_serial, g_id_total, g_id_ntracks);
	sysThreadExit(0);
}

static void lookup_start()
{
	sys_ppu_thread_t t;
	sysThreadCreate(&t, lookup_thread, (void *)(long)cur_card(), 1000, 0x10000, 0, (char *)"psxlookup");
}

// Compares the track MD5s of the finished rip with Redump. Fills `out` with the result line.
static void redump_verify(char *out, size_t n)
{
	char name[RD_NAME] = "";
	int r;   // 1 = verified, 0 = no match, -1 = could not check, -2 = serial unknown to Redump
	rd_lock();
	if (g_redump == RD_LOCAL) {
		r = rd_local_verify(g_track_md5bin, g_ntracks, name, sizeof(name));
	} else if (!g_serial[0]) {
		r = -1;
	} else {
		if (g_rd_ndiscs < 0 || strcmp(g_rd_serial, g_serial)) {
			g_rd_ndiscs = rd_online(g_serial, g_rd_discs, 4);
			snprintf(g_rd_serial, sizeof(g_rd_serial), "%s", g_serial);
		}
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
	rd_unlock();
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
	Card &k = g_cards[cur_card()];
	// the file a rip gets: the name pattern at this card's destination
	if (g_serial[0]) {
		name_from(g_name_pat, k.rd_name, g_name, g_serial, k.fname, sizeof(k.fname));
		snprintf(g_base, sizeof(g_base), "%s/%s", OUT_DIR, k.fname);
	} else {
		const char *slash = strrchr(g_base, '/');
		snprintf(k.fname, sizeof(k.fname), "%s", slash ? slash + 1 : g_name);
		snprintf(g_base, sizeof(g_base), "%s/%s", OUT_DIR, k.fname);
	}
	if (!strncmp(OUT_DIR, "exfat", 5)) { VfVolume v[8]; vf_scan(v, 8); }   // mounted before looking
	// an earlier rip of this disc here: under this name, or under another one (found by disc ID)
	char bp[320];
	snprintf(bp, sizeof(bp), "%s.bin", g_base);
	k.exist[0] = 0;
	if (vf_exists(bp)) snprintf(k.exist, sizeof(k.exist), "%s", g_base);
	else if (g_serial[0]) idx_find(OUT_DIR, g_serial, k.exist, sizeof(k.exist));
	g_prev_rip = k.exist[0] != 0;
	char keep[256];
	snprintf(keep, sizeof(keep), "%s", g_base);
	if (g_prev_rip) snprintf(g_base, sizeof(g_base), "%s", k.exist);   // its log
	g_repair_n = g_prev_rip ? riplog_open_count() : 0;
	// how that image compared with Redump, from its log ("redump=verified Alundra (Europe)"): the
	// card's title icon says it, also for a rip from an earlier session
	k.rd_done[0] = 0;
	k.rd_state = -1;
	if (g_prev_rip) {
		char lp[320];
		snprintf(lp, sizeof(lp), "%s.log", g_base);
		VFile *lf = vf_open(lp, "rb");
		if (lf) {
			char b[2048];
			int n = (int)vf_read(lf, b, sizeof(b) - 1);
			vf_close(lf);
			b[n > 0 ? n : 0] = 0;
			const char *rd = strstr(b, "redump=");
			char code[16] = "", name[RD_NAME] = "";
			if (rd && sscanf(rd + 7, "%15[^ \n] %159[^\n]", code, name) >= 1) {
				if (!strcmp(code, "verified")) {
					snprintf(k.rd_done, sizeof(k.rd_done), T(STR_RD_CARD_OK), name); k.rd_done_col = 0x7CE0A0;
					k.rd_state = 1;
					if (name[0]) snprintf(k.rd_name, sizeof(k.rd_name), "%s", name);   // the verified name
				}
				else if (!strcmp(code, "no_match")) { snprintf(k.rd_done, sizeof(k.rd_done), "%s", T(STR_RD_NOMATCH)); k.rd_done_col = 0xFF8A80; k.rd_state = 0; }
			}
		}
	}
	if (g_prev_rip && k.rd_state == 1 && strcmp(keep, g_base)) {
		// a verified rip under another name: Redump's verified name may change the new name
		name_from(g_name_pat, k.rd_name, g_name, g_serial, k.fname, sizeof(k.fname));
		snprintf(keep, sizeof(keep), "%s/%s", OUT_DIR, k.fname);
	}
	snprintf(g_base, sizeof(g_base), "%s", keep);   // a new rip writes the pattern's name
	space_check(g_disc_mb ? g_disc_mb : (uint32_t)((uint64_t)g_leadout * RAW_SIZE / (1024 * 1024)));
}

static void fire_event(const char *event);
static void identify_thread(void *arg)
{
	t_card = (int)((long)arg >> 8);
	g_cards[t_card].rd_name[0] = 0;
	long fresh = (long)arg & 0xFF;
	if (fresh) usleep(2000 * 1000);   // freshly inserted: let the drive spin up
	int err = 0, code = 0;
	int r = prepare_disc(&err, &code);
	if (r == 0) sys_storage_close(g_fd);
	// Undo the lv1 patch straight away: if the app is then killed (webMAN exit, XMB quit) the
	// hypervisor would stay patched and the next system storage operation (pkg install) hangs.
	if (g_use == SRC_BD) lv1_storage_unpatch();
	rd_set("");
	if (r == 0) {
		snprintf(g_disc_title, sizeof(g_disc_title), "%s", g_name);
		snprintf(g_disc_serial, sizeof(g_disc_serial), "%s", g_serial[0] ? g_serial : T(STR_UNKNOWN));
		g_disc_mb = (uint32_t)((uint64_t)g_leadout * RAW_SIZE / (1024 * 1024));
		prev_rip_check();
		snprintf(g_cards[cur_card()].base, sizeof(g_cards[0].base), "%s", g_base);
		snprintf(g_state_title, sizeof(g_state_title), "%s", g_name);
		snprintf(g_state_serial, sizeof(g_state_serial), "%s", g_serial);
		// only for a disc inserted while the app runs; a disc already in the drive at start is part
		// of app_started, and the home screen identifying the same disc again sends nothing
		if (fresh) fire_event("disc_identified");
		if (g_repair_n) logf("previous rip has %d sector(s) to repair", g_repair_n);
		__sync_synchronize();
		g_disc_state = DISC_READY;
	} else {
		g_disc_state = (r == -2) ? DISC_NONE : DISC_ERROR;
		if (r != -2 && fresh) {   // a disc was inserted but is not a readable PS1 disc
			g_state_title[0] = 0; g_state_serial[0] = 0;
			fire_event("disc_not_identified");
		}
	}
	logf("identify -> %d (state %d)", r, g_disc_state);
	snprintf(g_id_serial, sizeof(g_id_serial), "%s", g_serial);
	g_id_total = (uint64_t)g_leadout * RAW_SIZE;
	g_id_ntracks = g_ntracks;
	// the lookup writes this card's Redump line: the next identification (another card) waits for it
	if (r == 0 && g_redump != RD_OFF) redump_lookup(g_id_serial, g_id_total, g_id_ntracks);
	if (r == 0) { prev_rip_check(); snprintf(g_cards[t_card].base, sizeof(g_cards[0].base), "%s", g_base); }   // Redump's name
	g_ident_busy = 0;
	sysThreadExit(0);
}

static void identify_start(int fresh)
{
	g_ident_card = cur_card();
	if (g_ident_busy) return;
	g_ident_busy = 1;
	g_disc_state = DISC_READING;
	sys_ppu_thread_t t;
	sysThreadCreate(&t, identify_thread, (void *)(long)(fresh | cur_card() << 8), 1000, 0x10000, 0, (char *)"psxident");
}

// ---------------------------------------------------------------- own screens (home, settings)
enum { B_CROSS = 1, B_CIRCLE = 2, B_TRIANGLE = 4, B_UP = 8, B_DOWN = 16, B_LEFT = 32, B_RIGHT = 64, B_START = 128, B_SQUARE = 256, B_SELECT = 512, B_L1 = 1024, B_R1 = 2048, B_L2 = 4096, B_R2 = 8192 };

// Buttons newly pressed since the last call, over all connected pads. Each pad keeps its own
// last state: ioPadGetData only reports changes, so a quiet pad must not resurrect another pad's buttons.
// Buttons pressed on the remote view's pad (its server thread), taken by the next pad_pressed().
static volatile int g_remote_btn = 0;
// What the screen shows, for MCP's get_state: each screen's loop says where it is every frame; a
// dialog, the on-screen keyboard and the hint bar's buttons are noted where they are drawn.
enum { SCR_HOME, SCR_SETTINGS, SCR_DRIVE_SETTINGS, SCR_FILENAME, SCR_WEBHOOKS, SCR_WEBHOOK_HEADERS, SCR_ABOUT, SCR_AUDIT_LOG,
       SCR_REMOTE_CONTROL, SCR_REDUMP_DOWNLOAD, SCR_RESULT };
static const char *const SCR_NAMES[] = {"home", "settings", "drive_settings", "filename", "webhooks", "webhook_headers", "about", "audit_log",
                                        "remote_control", "redump_download", "result"};
#define HINTS 8
struct UiState {
	int scr, row, card;          // the screen, its focused row (-1 none), its drive (card, -1 none)
	int tab;                     // the home screen's tab (card)
	int dlg_open, dlg_q;         // the app's own dialog is up (a question: yes/no)
	char dlg_text[400];
	int osk;                     // the system's on-screen keyboard is up
	int hint_n;                  // the hint bar last drawn: button and what it does
	char hint_b[HINTS][12], hint_l[HINTS][64];
};
// Written by the UI thread, read whole by MCP's get_state under a short lock (never a screen with
// another screen's row). The lock is a leaf: nothing else is taken while it is held.
static UiState g_ui = {SCR_HOME, -1, -1, 0, 0, 0, "", 0, 0, {}, {}};
static sys_mutex_t g_ui_mx;
struct UiLock { UiLock() { sysMutexLock(g_ui_mx, 0); } ~UiLock() { sysMutexUnlock(g_ui_mx); } };
static void ui_at(int scr, int row = -1, int card = -1) { UiLock lock; g_ui.scr = scr; g_ui.row = row; g_ui.card = card; }
static void ui_snapshot(UiState &s) { UiLock lock; s = g_ui; }

static int remote_press(const char *b)
{
	static const struct { const char *n; int bit; } map[] = {
		{"cross", B_CROSS}, {"circle", B_CIRCLE}, {"triangle", B_TRIANGLE}, {"square", B_SQUARE},
		{"up", B_UP}, {"down", B_DOWN}, {"left", B_LEFT}, {"right", B_RIGHT},
		{"start", B_START}, {"select", B_SELECT}, {"l1", B_L1}, {"r1", B_R1}, {"l2", B_L2}, {"r2", B_R2}};
	for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++)
		if (!strcmp(b, map[i].n)) { __sync_fetch_and_or(&g_remote_btn, map[i].bit); logf("remote pad: %s", b); return 0; }
	return -1;
}

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
				if (pd.BTN_L1) b |= B_L1;
				if (pd.BTN_R1) b |= B_R1;
				if (pd.BTN_L2) b |= B_L2;
				if (pd.BTN_R2) b |= B_R2;
				pad_state[i] = b;
			}
			now |= pad_state[i];
		}
	}
	int remote = __sync_lock_test_and_set(&g_remote_btn, 0);
	int pressed = (now & ~last) | remote;   // and the web page's pad
	if (pressed) g_input_src = remote ? AU_REMOTE : AU_PAD;
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

static void hint_bar(const char *const *buttons, const char *const *labels, int n);
// The bottom row of a page that may scroll: L2/R2 go in second when it does.
static void hint_bar_pg(const char *const *buttons, const char *const *labels, int n, int scrolls)
{
	if (!scrolls) { hint_bar(buttons, labels, n); return; }
	const char *b[8], *l[8];
	int m = 0;
	for (int i = 0; i < n && m < 7; i++) {
		b[m] = buttons[i]; l[m++] = labels[i];
		if (i == 0) { b[m] = "l2r2"; l[m++] = T(STR_HINT_EVENTS); }
	}
	hint_bar(b, l, m);
}
static void hint_bar(const char *const *buttons, const char *const *labels, int n)
{
	{
		UiLock lock;
		for (int i = 0; i < n && i < HINTS; i++) {
			snprintf(g_ui.hint_b[i], sizeof(g_ui.hint_b[0]), "%s", buttons[i]);
			snprintf(g_ui.hint_l[i], sizeof(g_ui.hint_l[0]), "%s", labels[i]);
		}
		g_ui.hint_n = n < HINTS ? n : HINTS;
	}
	int total = 0, gap = ui_w() / 24, sum = 0;
	for (int i = 0; i < n; i++) sum += ui_hint_width(buttons[i], labels[i]);
	// a long row: the gaps shrink so it stays on the screen
	if (n > 1 && sum + gap * (n - 1) > ui_w() * 94 / 100) gap = (ui_w() * 94 / 100 - sum) / (n - 1);
	if (gap < ui_w() / 120) gap = ui_w() / 120;
	total = sum + gap * (n - 1);
	int x = (ui_w() - total) / 2, y = hint_y();
	for (int i = 0; i < n; i++) x += ui_hint(x, y, buttons[i], labels[i]) + gap;
}

static void cards_take_pending();
static void flip_frame()
{
	sysUtilCheckCallback();
	cards_take_pending();   // a drive's settings changed over MCP
	static int seen_gen = -1;   // a setting changed elsewhere (MCP): the screen shows it
	if (g_cfg_gen != seen_gen) { seen_gen = g_cfg_gen; g_redraw = 2; }
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
	if (secs < 0) secs = 0;
	if (secs > 99 * 3600 + 3599) secs = 99 * 3600 + 3599;   // "99:59:59" at most: it always fits
	int h = (int)(secs / 3600), m = (int)(secs / 60 % 60), sc = (int)(secs % 60);
	if (h) snprintf(out, n, "%d:%02d:%02d", h, m, sc);
	else snprintf(out, n, "%d:%02d", m, sc);
}

// "Time 2:14 · about 1:35 left · 2.6 MB/s": speed over the last ~20 s (the drive is faster on the
// outer part of the disc and slow in damaged areas), remaining time from that speed. Runs in the
// card's context (OnCard): the main read, or the pass after it (verify, re-reads, repair).
static void eta_pos(uint32_t *pos, uint32_t *total)
{
	if (g_pass2_total && g_phase) { *pos = g_pass2_done; *total = g_pass2_total; return; }
	*pos = g_readpos > g_done ? g_readpos : g_done;
	*total = g_total;
}

// Sampled every frame for every ripping card, not only the one on screen: a tab opened later
// would otherwise start from nothing (no time counted, "1 second left").
static void eta_tick(int c)
{
	Card &k = g_cards[c];
	if (!k.job) return;
	OnCard o(c);
	uint32_t pos, total;
	eta_pos(&pos, &total);
	uint64_t t = now_ms();
	if (!k.eta_t0 || g_phase != k.eta_phase) {   // a new job or a new pass: start measuring here
		k.eta_t0 = k.eta_tw = t; k.eta_pw = k.eta_p0 = pos; k.eta_rate = 0; k.eta_phase = g_phase;
		return;
	}
	if (t - k.eta_tw >= 2000) {   // sample every 2 s, smooth over ~20 s
		double r = pos >= k.eta_pw ? (double)(pos - k.eta_pw) * 1000.0 / (double)(t - k.eta_tw) : 0;
		k.eta_rate = k.eta_rate > 0 ? k.eta_rate * 0.9 + r * 0.1 : r;
		k.eta_tw = t; k.eta_pw = pos;
	}
}

static void eta_text(const Card &k, char *out, size_t n)
{
	uint32_t pos, total;
	eta_pos(&pos, &total);
	char el[16], left[16];
	fmt_dur(el, sizeof(el), g_rip_t0 ? (long)(time(NULL) - g_rip_t0) : 0);   // since the job started
	// the reading speed (sectors/s) over the last ~20 s; the re-read and verify passes, which may
	// take a minute per sector, use the average since the pass started instead
	uint64_t ms = now_ms() - k.eta_t0;
	double rate = k.eta_rate;
	if ((g_phase || rate <= 1) && pos > k.eta_p0 && ms > 6000) rate = (double)(pos - k.eta_p0) * 1000.0 / (double)ms;
	if (k.eta_t0 && rate > 0 && ms > 6000 && total > pos && (g_phase || rate > 1) && (total - pos) / rate < 100.0 * 3600) {
		fmt_dur(left, sizeof(left), (long)((total - pos) / rate));
		unsigned kbs = (unsigned)(rate * RAW_SIZE / 1024);
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
	rd_lock();
	g_dl_result = rd_download(&g_dl_done, &g_dl_total, &g_dl_phase, &g_dl_abort);
	rd_unlock();
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
		ui_at(SCR_REDUMP_DOWNLOAD);
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
	{ UiLock lock; g_ui.osk = 1; }
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
	{ UiLock lock; g_ui.osk = 0; }
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
		ui_at(SCR_WEBHOOK_HEADERS, row);
		flip_frame();
		int p = pad_pressed();
		if (p) g_redraw = 2;
		if (p & B_UP) row = (row + WH_HEADERS - 1) % WH_HEADERS;
		if (p & B_DOWN) row = (row + 1) % WH_HEADERS;
		if (p & B_R2) row = WH_HEADERS - 1;   // L2/R2: first / last
		if (p & B_L2) row = 0;
		if (p & B_CIRCLE) return;
		if (p & B_CROSS) {
			char h[256];
			snprintf(h, sizeof(h), "%s", g_webhook_hdr[row]);
			if (osk_input(T(STR_WH_HEADER_Q), h, sizeof(h))) {
				CfgLock lock;
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
			{   // more rows above / below: arrows at the right edge, as in the settings (L2/R2 jump a page)
				int asz = ui_line(UI_SMALL) * 2 / 3, ax = right + ui_w() / 40;
				if (first > 0) ui_icon("up", ax, top + (lh * 3 / 4 - asz) / 2 - lh / 8, asz, 0xC8D0E0);
				if (first + VIS < NROWS) ui_icon("down", ax, top + (VIS - 1) * lh + (lh * 3 / 4 - asz) / 2 - lh / 8, asz, 0xC8D0E0);
			}
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
					if (g_webhook_hdr_on) snprintf(val, sizeof(val), T(STR_WH_HEADERS_ON), nh); else opt_text(OPTS(o_onoff), 0, val, sizeof(val));
				} else if (r < R_PROG) {
					int i = ev[r - R_EV];
					snprintf(label, sizeof(label), "%s", T(wh_label(i)));
					opt_text(OPTS(o_onoff), (g_webhook_mask >> i) & 1, val, sizeof(val));
				} else if (r == R_PROG) {
					snprintf(label, sizeof(label), "%s", T(STR_WH_PROGRESS));
					opt_text(OPTS(o_progress), (g_webhook_mask >> WH_RIP_PROGRESS) & 1 ? g_webhook_step : 0, val, sizeof(val));
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
				const char *b[] = {"updown", "l2r2", "leftright", "cross", "circle"};
				const char *l[] = {T(STR_HINT_SELECT), T(STR_HINT_PAGE), T(STR_HINT_CHANGE), T(STR_HINT_EDIT), T(STR_HINT_BACK)};
				hint_bar(b, l, 5);
			} else if (row == R_ADDR || row == R_TEST) {
				const char *b[] = {"updown", "l2r2", "cross", "circle"};
				const char *l[] = {T(STR_HINT_SELECT), T(STR_HINT_PAGE), T(row == R_TEST ? STR_HINT_SEND : STR_HINT_EDIT), T(STR_HINT_BACK)};
				hint_bar(b, l, 4);
			} else {
				const char *b[] = {"updown", "l2r2", "leftright", "circle"};
				const char *l[] = {T(STR_HINT_SELECT), T(STR_HINT_PAGE), T(STR_HINT_CHANGE), T(STR_HINT_BACK)};
				hint_bar(b, l, 4);
			}
			g_drawn = 2;
			g_backdrop_shown = 0;
		}
		ui_at(SCR_WEBHOOKS, row);
		flip_frame();
		int p = pad_pressed();
		if (p) g_redraw = 2;
		if (p & B_UP) row = (row + NROWS - 1) % NROWS;
		if (p & B_DOWN) row = (row + 1) % NROWS;
		if (p & B_R2) row = row + VIS < NROWS ? row + VIS : NROWS - 1;   // a page down / up
		if (p & B_L2) row = row >= VIS ? row - VIS : 0;
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
		CfgLock lock;
		if (row == R_HDR) {
			g_webhook_hdr_on = opt_wrap(OPTS(o_onoff), g_webhook_hdr_on, d);
		} else if (row >= R_EV && row < R_PROG) {
			int b = ev[row - R_EV];
			if (opt_wrap(OPTS(o_onoff), (g_webhook_mask >> b) & 1, d)) g_webhook_mask |= 1 << b; else g_webhook_mask &= ~(1 << b);
		} else if (row == R_PROG) {   // off, or on every this many percent
			int k = opt_wrap(OPTS(o_progress), (g_webhook_mask >> WH_RIP_PROGRESS) & 1 ? g_webhook_step : 0, d);
			if (k == 0) g_webhook_mask &= ~(1 << WH_RIP_PROGRESS);
			else { g_webhook_mask |= 1 << WH_RIP_PROGRESS; g_webhook_step = k; }
		} else continue;
		cfg_save();
	}
}

// Is a destination still there? The hard disk always is; USB drives can be pulled out.
static int dest_available(const char *dir)
{
	if (!strncmp(dir, "exfat", 5)) {
		VfVolume v[8];
		int n = vf_scan(v, 8);
		for (int i = 0; i < n && i < 8; i++) if (!strncmp(dir, v[i].root, strlen(v[i].root))) return 1;
		return 0;
	}
	if (!strncmp(dir, "/dev_usb", 8)) {
		char dev[16];
		snprintf(dev, sizeof(dev), "%.11s", dir);   // "/dev_usb001"
		struct stat st;
		return stat(dev, &st) == 0;
	}
	return 1;
}
static const char *HDD_DIR = "/dev_hdd0/PSXISO";

// "Internal drive", "USB 1 · Apple SuperDrive": like the destinations, by port. drv = usbodd drive, -1 = the PS3's.
static void source_name(int drv, char *out, size_t n)
{
	if (drv < 0) { snprintf(out, n, "%s", T(STR_SRC_BD)); return; }
	const char *m = usbodd_name(drv)[0] ? usbodd_name(drv) : "CD/DVD";
	if (usbodd_port(drv) >= 0) snprintf(out, n, T(STR_SRC_USB), usbodd_port(drv), m);
	else snprintf(out, n, T(STR_SRC_USB_NOPORT), m);
}

// Text for a drive setting: its own value, or "Standard (value)".
static void drive_val(char *out, size_t n, int own, int std, const char *(*fmt)(int, char *, size_t))
{
	char v[64];
	if (own >= 0) { snprintf(out, n, "%s", fmt(own, v, sizeof(v))); return; }
	char sv[64];
	snprintf(out, n, T(STR_STD_VALUE), fmt(std, sv, sizeof(sv)));
}
static const char *fmt_speed(int v, char *b, size_t n) { return opt_text(OPTS(o_speed), v, b, n); }
static const char *fmt_int(int v, char *b, size_t n) { snprintf(b, n, "%d", v); return b; }
static const char *fmt_onoff(int v, char *b, size_t n) { return opt_text(OPTS(o_onoff), v ? 1 : 0, b, n); }
static const char *fmt_check(int v, char *b, size_t n) { return opt_text(OPTS(o_check), v, b, n); }


static int card_list(int *out);
static int drive_column(int c, int right, int y, const char *idl);
static void ui_text_fit(UiFont f, int x, int y, uint32_t col, const char *text, int maxw, UiAlign al = UI_LEFT);

// File name patterns ready to pick; anything else is the own pattern.
static const char *FN_PRESETS[] = {"%T [%D]", "%T (%R)", "%T (%R) [%D]", "%N"};
static char g_name_own[96] = "";   // the last own pattern, kept while a ready-made one is picked
static void fn_label(const char *pat, char *out, size_t n)
{
	static const StrId lbl[] = {STR_FN_P1, STR_FN_P2, STR_FN_P3, STR_FN_P4};
	for (int i = 0; i < 4; i++) if (!strcmp(pat, FN_PRESETS[i])) { snprintf(out, n, "%s", T(lbl[i])); return; }
	snprintf(out, n, T(STR_FN_OWN), pat);
}
static void fn_step(int d)
{
	int cur = 4;
	for (int i = 0; i < 4; i++) if (!strcmp(g_name_pat, FN_PRESETS[i])) cur = i;
	if (cur == 4) snprintf(g_name_own, sizeof(g_name_own), "%s", g_name_pat);
	int nx = (cur + (d > 0 ? 1 : 4)) % 5;
	if (nx == 4 && !g_name_own[0]) nx = d > 0 ? 0 : 3;   // no own pattern yet: X on the row makes one
	snprintf(g_name_pat, sizeof(g_name_pat), "%s", nx < 4 ? FN_PRESETS[nx] : g_name_own);
}

// The file name view: the pattern as parts, a preview for the discs in the drives (file name and
// how webMAN lists it), parts to add with X, square removes the last one, triangle types freely.
// A section heading in a view: body size, light blue, a thin line under it. Returns the height.
static int section_head(int L, int R, int y, const char *text)
{
	int lb = ui_line(UI_BODY);
	ui_text(UI_BODY, L, y, 0x8FB0FF, text);
	ui_rect(L, y + lb + lb / 8, R - L, 2, 0x50FFFFFF);
	return lb + lb / 3;
}

static void filename_screen()
{
	char pat[96];
	snprintf(pat, sizeof(pat), "%s", g_name_pat);
	static const char *parts[] = {"%T", "%R", "%D", "%N", " ", "(", ")", "[", "]", "-"};
	static const StrId plbl[] = {STR_FN_B_TITLE, STR_FN_B_REGION, STR_FN_B_ID, STR_FN_B_NAME, STR_FN_B_SPACE};
	const int NP = 10, NL = 5;   // the first NL parts have a name
	int sel = 0;
	// the pattern as parts ("%T" or one character) and a cursor between them: L1/R1 move it, X puts
	// a part in at the cursor, square removes the part before it
	auto part_starts = [&](int *st) {   // offsets where each part starts; returns the count
		int n = 0;
		for (int i = 0; pat[i]; i++) { st[n++] = i; if (pat[i] == '%' && pat[i + 1]) i++; }
		st[n] = (int)strlen(pat);
		return n;
	};
	int cur;
	{ int st[100]; cur = part_starts(st); }   // at the end
	uint64_t blink0 = now_ms();
	char was[96];   // the saved pattern this view started from: changed elsewhere (MCP), an unedited view takes it
	snprintf(was, sizeof(was), "%s", g_name_pat);
	pad_pressed();
	g_redraw = 2;
	while (!exit_requested) {
		if (strcmp(was, g_name_pat)) {
			if (!strcmp(pat, was)) { snprintf(pat, sizeof(pat), "%s", g_name_pat); int s2[100]; cur = part_starts(s2); }
			snprintf(was, sizeof(was), "%s", g_name_pat);
		}
		if (g_redraw > 0) {
			g_redraw = 0;
			g_drawn = 2;
			draw_backdrop();
			int lb = ui_line(UI_BODY), ls = ui_line(UI_SMALL), L = ui_w() / 6, R = ui_w() * 5 / 6;
			ui_text(UI_BODY, ui_w() / 2, ui_h() / 10 + ui_line(UI_TITLE) + lb / 2, 0xA8B0C8, T(STR_L_FILENAME), UI_CENTER);
			int y = ui_h() / 10 + ui_line(UI_TITLE) + lb * 3 / 2;
			// the pattern: variables as blue chips, other characters as they are (a space as a dot)
			y += section_head(L, R, y, T(STR_FN_PATTERN));
			int x = L, pad = ui_w() / 120, cx = L, pi = 0;
			bool on = ((now_ms() - blink0) / 500) % 2 == 0;   // the cursor blinks
			for (const char *q = pat; *q; q++, pi++) {
				if (pi == cur) cx = x;
				if (*q == '%' && q[1]) {
					q++;
					int k = *q == 'T' ? 0 : *q == 'R' ? 1 : *q == 'D' ? 2 : *q == 'N' ? 3 : -1;
					const char *w = k >= 0 ? T(plbl[k]) : "%";
					int tw = ui_text_width(UI_BODY, w);
					ui_rect(x, y - lb / 8, tw + pad * 2, lb + lb / 4, 0xFF3D6FD9);
					ui_text(UI_BODY, x + pad, y, 0xFFFFFF, w);
					x += tw + pad * 3;
				} else {
					char c[2] = {*q == ' ' ? (char)0 : *q, 0};
					const char *w = *q == ' ' ? "·" : c;
					x += ui_text(UI_BODY, x, y, 0xC8D0E0, w) + pad;
				}
			}
			if (cur >= pi) cx = x;   // at the end
			if (on) ui_rect(cx - 3, y - lb / 8, 3, lb + lb / 4, 0xFFFFFFFF);   // where the next part goes
			y += lb + ls;
			// parts to add
			y += section_head(L, R, y, T(STR_FN_ADD));
			x = L;
			for (int i = 0; i < NP; i++) {
				const char *w = i < NL ? T(plbl[i]) : parts[i];
				int tw = ui_text_width(UI_SMALL, w);
				ui_rect(x, y - ls / 8, tw + pad * 3, ls + ls / 4, i == sel ? 0xFF3D6FD9 : 0xFF404668);
				ui_text(UI_SMALL, x + pad * 3 / 2, y, i == sel ? 0xFFFFFF : 0xC8D0E0, w);
				x += tw + pad * 4;
			}
			y += ls * 2;
			ui_text_fit(UI_SMALL, L, y, 0x8FA8D8, T(STR_FN_NOTE), R - L);
			y += ls + ls / 2;
			// preview: each disc in a drive, else an example
			y += section_head(L, R, y, T(STR_FN_PREVIEW));
			int list[NCARDS], nc = card_list(list), shown = 0;
			for (int i = 0; i <= nc; i++) {
				const char *rn, *tt, *sr, *dn = "";
				char drv[96];
				if (i < nc) {
					Card &k = g_cards[list[i]];
					if (k.state != DISC_READY) continue;
					rn = k.rd_name; tt = k.title; sr = k.serial;
					source_name(list[i] ? list[i] - 1 : -1, drv, sizeof(drv));
					dn = drv;
				} else {
					if (shown) break;
					rn = "Croc 2 (Europe) (En,Fr,De,Es,It,Nl,Sv)"; tt = "Croc 2"; sr = "SLES_020.88";
				}
				char fn[200], fb[220], ln[200], lv[260];
				name_from(pat, rn, tt, sr, fn, sizeof(fn));
				snprintf(fb, sizeof(fb), "%s.bin", fn);
				listed_name(fn, ln, sizeof(ln));
				snprintf(lv, sizeof(lv), T(STR_FN_LISTED), ln);
				// the whole name if it fits; else smaller, and if even that is too long cut in the middle,
				// so the start and the end (disc ID, .bin) always show
				{
					int room = R - L - ui_w() / 5;
					if (ui_text_width(UI_BODY, fb) <= room) ui_text(UI_BODY, L, y, 0xFFFFFF, fb);
					else {
						char mid[240];
						snprintf(mid, sizeof(mid), "%s", fb);
						int n = (int)strlen(fb), keep = n / 2;
						while (ui_text_width(UI_SMALL, mid) > room && keep > 4) {
							keep--;
							int head = keep / 2 + keep % 2, tail = keep / 2;
							while (head > 0 && ((unsigned char)fb[head] & 0xC0) == 0x80) head--;   // whole UTF-8 characters
							int ts = n - tail;
							while (ts < n && ((unsigned char)fb[ts] & 0xC0) == 0x80) ts++;
							snprintf(mid, sizeof(mid), "%.*s…%s", head, fb, fb + ts);
						}
						ui_text(UI_SMALL, L, y + (lb - ls) / 2, 0xFFFFFF, mid);
					}
				}
				int cw = dn[0] ? drive_column(list[i], R, y, sr) + ui_w() / 60 : 0;   // like on the cards, ID without size
				ui_text_fit(UI_SMALL, L + ui_w() / 60, y + lb, 0x8FA8D8, lv, R - L - ui_w() / 60 - cw);
				y += lb + ls * 2 + ls / 3;
				shown++;
			}
			const char *b[] = {"l1r1", "leftright", "cross", "square", "triangle", "circle"};
			const char *l[] = {T(STR_HINT_CURSOR), T(STR_HINT_PICK_PART), T(STR_HINT_ADD), T(STR_HINT_DEL_LAST), T(STR_HINT_TYPE), T(STR_HINT_BACK)};
			hint_bar(b, l, 6);
		}
		ui_at(SCR_FILENAME);
		flip_frame();
		{ static uint64_t last = 0; if (now_ms() - last >= 250) { last = now_ms(); g_redraw = 2; } }   // the blinking cursor
		int p = pad_pressed();
		if (p) { g_redraw = 2; blink0 = now_ms(); }   // the cursor shows at once after a key
		int st[100], np = part_starts(st);
		if (cur > np) cur = np;
		if (p & B_RIGHT) sel = (sel + 1) % NP;
		if (p & B_LEFT) sel = (sel + NP - 1) % NP;
		if ((p & B_R1) && cur < np) cur++;
		if ((p & B_L1) && cur > 0) cur--;
		if ((p & B_CROSS) && strlen(pat) + 3 < sizeof(pat)) {   // in at the cursor
			int at = st[cur];
			char rest[96];
			snprintf(rest, sizeof(rest), "%s", pat + at);
			snprintf(pat + at, sizeof(pat) - at, "%s%s", parts[sel], rest);
			cur++;
		}
		if ((p & B_SQUARE) && cur > 0) {   // the part before the cursor
			int a0 = st[cur - 1], a1 = st[cur];
			memmove(pat + a0, pat + a1, strlen(pat + a1) + 1);
			cur--;
		}
		if (p & B_TRIANGLE) {
			char e[96];
			snprintf(e, sizeof(e), "%s", pat);
			if (osk_input(T(STR_FN_OSK_Q), e, sizeof(e))) { snprintf(pat, sizeof(pat), "%s", e); int s2[100]; cur = part_starts(s2); }
			pad_pressed();
		}
		if (p & B_CIRCLE) {   // back; a changed pattern is saved only when the question says yes
			const char *np = pat[0] ? pat : FN_PRESETS[0];   // an empty pattern is the first ready-made one
			if (!strcmp(np, g_name_pat)) return;
			int a = own_dialog(T(STR_FN_SAVE_Q), 1);
			pad_pressed();
			g_redraw = 2;
			if (a != 1) return;   // no: the pattern stays as it was
			CfgLock lock;
			snprintf(g_name_pat, sizeof(g_name_pat), "%s", np);
			cfg_save();
			for (int c = 0; c < NCARDS; c++) g_cards[c].recheck = 1;   // the cards show the new names
			logf("file name pattern: %s", g_name_pat);
			return;
		}
	}
}

// A list of settings rows with the focused one highlighted (the settings screens share it).
static void settings_rows(const char *subtitle, const char *const *labels, char (*vals)[96], int nrows, int row, unsigned actions)   // actions: bit n = row n is opened with X, no ‹ ›
{
	ui_text(UI_BODY, ui_w() / 2, ui_h() / 10 + ui_line(UI_TITLE) + ui_line(UI_BODY) / 2, 0xA8B0C8, subtitle, UI_CENTER);
	// first row below the subtitle (drawn at h/10 + title + body/2)
	int top = ui_h() / 10 + ui_line(UI_TITLE) + ui_line(UI_BODY) * 2, lh = ui_line(UI_BODY) * 17 / 10, left = ui_w() / 6, right = ui_w() * 5 / 6;
	// at most VIS rows fit above the footer: a longer list scrolls with the focused row
	const int VIS = 9;
	int first = row < VIS ? 0 : row - VIS + 1;
	// more rows above / below: small arrows at the right edge (L2/R2 jump a page)
	int asz = ui_line(UI_SMALL) * 2 / 3, ax = right + ui_w() / 40;
	if (first > 0) ui_icon("up", ax, top + (lh * 3 / 4 - asz) / 2 - lh / 8, asz, 0xC8D0E0);
	if (first + VIS < nrows) ui_icon("down", ax, top + (VIS - 1) * lh + (lh * 3 / 4 - asz) / 2 - lh / 8, asz, 0xC8D0E0);
	for (int r = first; r < nrows && r < first + VIS; r++) {
		int y = top + (r - first) * lh;
		if (r == row) ui_rect(left - ui_w() / 60, y - lh / 4, right - left + ui_w() / 30, lh * 3 / 4 + lh / 4, 0x703D6FD9);
		int lw = ui_text(UI_BODY, left, y, 0xFFFFFF, labels[r]);
		char v[128];
		snprintf(v, sizeof(v), r == row && !((actions >> r) & 1) ? "‹ %s ›" : "%s", vals[r]);
		ui_text_fit(UI_BODY, right, y, r == row ? 0xFFFFFF : 0xC8D0E0, v, right - left - lw - ui_w() / 40, UI_RIGHT);   // never over the label
	}
	char foot[240], cp[200];   // the one file all settings live in (it can be edited over FTP)
	snprintf(cp, sizeof(cp), T(STR_WH_CFG_PATH), CFG_PATH);
	snprintf(foot, sizeof(foot), "%s", cp);   // (the version is in the title)
	footer_note(foot);
}

// About (the last row of the settings): what the app is, who made it, where the source is.
static int draw_wrapped(UiFont f, const char *text, int y, uint32_t col);
static void about_draw()
{
	draw_backdrop();
	int lb = ui_line(UI_BODY), ls = ui_line(UI_SMALL);
	int y = ui_h() / 10 + ui_line(UI_TITLE) + lb / 2;
	ui_text(UI_BODY, ui_w() / 2, y, 0xA8B0C8, T(STR_L_ABOUT), UI_CENTER);
	y += lb * 2;
	y = draw_wrapped(UI_BODY, T(STR_ABOUT_DESC), y, 0xE8ECF8) + lb;
	char line[200];
	snprintf(line, sizeof(line), T(STR_ABOUT_BY), APP_AUTHOR, APP_EMAIL);
	ui_text(UI_BODY, ui_w() / 2, y, 0xFFFFFF, line, UI_CENTER);
	y += lb + ls / 2;
	snprintf(line, sizeof(line), T(STR_ABOUT_SOURCE), APP_URL);
	ui_text(UI_BODY, ui_w() / 2, y, 0x9AD0A8, line, UI_CENTER);
	y += lb * 3 / 2;
	ui_text(UI_BODY, ui_w() / 2, y, 0xA8B0C8, T(STR_ABOUT_BUILT), UI_CENTER);   // the libraries and our own driver
	y += lb + ls / 3;
	y = draw_wrapped(UI_SMALL, T(STR_ABOUT_LIBS), y, 0xC8D0E0) + ls / 2;
	ui_text(UI_SMALL, ui_w() / 2, y, 0x8890A8, T(STR_ABOUT_LICENSE), UI_CENTER);
	y += ls + ls / 3;
	ui_text(UI_SMALL, ui_w() / 2, y, 0x8890A8, T(STR_ABOUT_SONY), UI_CENTER);
	const char *b[] = {"circle"};
	const char *l[] = {T(STR_HINT_BACK)};
	hint_bar(b, l, 1);
}

static void about_screen()
{
	pad_pressed();
	g_redraw = 2;
	while (!exit_requested) {
		if (g_redraw > 0) {
			g_redraw = 0;
			about_draw();
			g_drawn = 2;
		}
		ui_at(SCR_ABOUT);
		flip_frame();
		int p = pad_pressed();
		if (p & (B_CIRCLE | B_CROSS)) return;
	}
}

// ---------------------------------------------------------------- the audit log (Settings → Audit log)
// Four levels: the domains, then the discs (rips) or the events (settings), a disc's events, an
// event's rows. X opens, O goes back, triangle undoes the focused item after a question. The lists
// are read from the log when a level is entered and after an undo, never per frame.
#define AV_PAGE 9     // rows a page shows
#define AV_MAX 128    // discs and events a level lists (the newest)
struct AuditView {
	int level;               // 0 domains, 1 discs (rips) or events (settings), 2 a disc's events, 3 an event's rows
	AuditDom dom;
	char disc[96], disc_name[96];   // the disc of levels 2 and 3
	AuditRow ev;             // level 3: the event
	int row, first, n;       // focused entry, first one shown, entries loaded
	int base, more;          // level 3: index of av_rows[0] in the event; rows follow the loaded ones
	int empty;               // level 0: nothing in the log yet
	int back[4];             // the focused entry of each level, for going back
	int undo_ok;             // the focused disc or job: an undo would undo something (the core's exact answer)
	int undo_redo;           // and that undo would be a redo (all its changes are undone)
	AuditReason no_undo;     // and if not, why
	char why_key[100];       // the last refused undo (its row id or disc) and the reason
	AuditReason why;
};
static char av_keys[AV_MAX][96], av_names[AV_MAX][96];
static uint32_t av_last[AV_MAX];
static AuditRow av_rows[AV_MAX];
static const AuditDom av_doms[] = {AU_RIPS, AU_SETTINGS};

static int av_count(const AuditView &v) { return v.level == 3 ? v.base + v.n : v.n; }

static const AuditRow *audit_focused(const AuditView &v)
{
	if (v.level == 0 || (v.level == 1 && v.dom == AU_RIPS)) return NULL;
	int i = v.level == 3 ? v.row - v.base : v.row;
	return i >= 0 && i < v.n ? &av_rows[i] : NULL;
}

// Level 3: the two pages from the one `from` is on, and one row more to know whether others follow
// (a repair can have thousands; each row shown is checked against the image).
static void av_page(AuditView &v, int from)
{
	v.base = from / AV_PAGE * AV_PAGE;
	int n = audit_rows(v.ev.job, av_rows, v.base, 2 * AV_PAGE + 1);
	v.more = n > 2 * AV_PAGE;
	v.n = v.more ? 2 * AV_PAGE : n;
}

// Whether undoing the focused disc or job would undo anything, and if not why (the core applies
// every guard the undo does). Asked when the focus moves, not per frame.
static void av_probe(AuditView &v)
{
	v.undo_ok = v.undo_redo = 0;
	memset(&v.no_undo, 0, sizeof(v.no_undo));
	const AuditRow *e = audit_focused(v);
	if (v.level == 1 && v.dom == AU_RIPS && v.row < v.n) v.undo_ok = audit_disc_reversible(av_keys[v.row], &v.no_undo, &v.undo_redo);
	else if (v.level == 2 && e && !strcmp(e->kind, "job")) v.undo_ok = audit_job_reversible(e->job, &v.no_undo, &v.undo_redo);
}

// The focused row on the screen, the right page loaded.
static void av_fix(AuditView &v)
{
	if (v.row < v.first) v.first = v.row;
	if (v.row >= v.first + AV_PAGE) v.first = v.row - AV_PAGE + 1;
	if (v.level == 3) {
		if (v.first < v.base || (v.first + AV_PAGE > v.base + v.n && v.more)) av_page(v, v.first);
		if (v.row >= v.base + v.n) v.row = v.base + v.n > 0 ? v.base + v.n - 1 : 0;   // the end
		if (v.first > v.row) v.first = v.row;
	}
	av_probe(v);
}

static void av_load(AuditView &v)
{
	if (v.level == 0) {
		AuditRow one;
		v.n = 2;
		v.empty = audit_discs(NULL, NULL, NULL, NULL, 1) == 0 && audit_events(AU_SETTINGS, NULL, &one, 1) == 0;
	} else if (v.level == 1 && v.dom == AU_RIPS) v.n = audit_discs(av_keys, av_names, NULL, av_last, AV_MAX);
	else if (v.level < 3) v.n = audit_events(v.dom, v.level == 2 ? v.disc : NULL, av_rows, AV_MAX);
	else { av_page(v, v.first); return; }
	if (v.row >= v.n) v.row = v.n ? v.n - 1 : 0;
}

static void audit_view_init(AuditView &v)
{
	memset(&v, 0, sizeof(v));
	v.dom = AU_RIPS;
	av_load(v);
	av_fix(v);
}

static void audit_nav(AuditView &v, int p)
{
	int cnt = av_count(v);
	if (!cnt) return;
	if (v.level < 3) {   // the whole list is loaded: up and down go round
		if (p & B_UP) v.row = (v.row + cnt - 1) % cnt;
		if (p & B_DOWN) v.row = (v.row + 1) % cnt;
		if (p & B_R2) v.row = v.row + AV_PAGE < cnt ? v.row + AV_PAGE : cnt - 1;
	} else {   // an event's rows are read a page at a time: the end is known once it is reached
		if ((p & B_UP) && v.row > 0) v.row--;
		if (p & B_DOWN) v.row++;
		if (p & B_R2) v.row += AV_PAGE;
	}
	if (p & B_L2) v.row = v.row >= AV_PAGE ? v.row - AV_PAGE : 0;
	av_fix(v);
}

static int av_can_open(const AuditView &v)
{
	if (v.level == 0) return !v.empty;
	if (v.level == 1) return v.dom == AU_RIPS && v.n > 0;
	const AuditRow *r = audit_focused(v);
	return v.level == 2 && r && !strcmp(r->kind, "job");
}

// X: one level down.
static void audit_open(AuditView &v)
{
	if (!av_can_open(v)) return;
	if (v.level == 0) v.dom = av_doms[v.row];
	else if (v.level == 1) {
		snprintf(v.disc, sizeof(v.disc), "%s", av_keys[v.row]);
		snprintf(v.disc_name, sizeof(v.disc_name), "%s", av_names[v.row]);
	} else v.ev = *audit_focused(v);
	v.back[v.level++] = v.row;
	v.row = v.first = v.base = v.n = v.more = 0;
	v.why_key[0] = 0;
	av_load(v);
	av_fix(v);
}

// O: one level up; 0 at the top (the screen closes).
static int audit_back(AuditView &v)
{
	if (v.level == 0) return 0;
	v.row = v.back[--v.level];
	v.first = 0;
	v.why_key[0] = 0;
	av_load(v);
	av_fix(v);
	return 1;
}

// Triangle is offered on: a disc (all its kept changes), a job (its sectors) and a row that can be undone.
static int audit_can_undo(const AuditView &v)
{
	if (v.level == 1 && v.dom == AU_RIPS) return v.n > 0 && v.undo_ok == 1;
	const AuditRow *r = audit_focused(v);
	if (!r) return 0;
	if (v.level == 2 && !strcmp(r->kind, "job")) return v.undo_ok == 1;
	return r->reversible;
}
// The undo would apply undone changes again (a disc or job all undone, or an undo row): △ says Redo.
static int audit_is_redo(const AuditView &v)
{
	if (v.level == 1 && v.dom == AU_RIPS) return v.undo_redo;
	const AuditRow *r = audit_focused(v);
	if (!r) return 0;
	if (v.level == 2 && !strcmp(r->kind, "job")) return v.undo_redo;
	return r->redo;
}

static void av_focus_key(const AuditView &v, char *out, size_t n)
{
	const AuditRow *r = audit_focused(v);
	if (v.level == 1 && v.dom == AU_RIPS && v.row < v.n) snprintf(out, n, "%s", av_keys[v.row]);
	else if (r) snprintf(out, n, "%u", (unsigned)r->id);
	else out[0] = 0;
}

static const char *av_src(AuditSrc s)
{
	switch (s) {
	case AU_PAD: return T(STR_AUDIT_SRC_PAD);
	case AU_REMOTE: return T(STR_AUDIT_SRC_REMOTE);
	case AU_APP: return T(STR_AUDIT_SRC_APP);
	case AU_MCP: return "MCP";
	case AU_UNDO: return T(STR_AUDIT_SRC_UNDO);
	}
	return "";
}

// A config key as the settings name it, and a value as they show it.
static const char *av_key_label(const char *key)
{
	static const struct { const char *key; StrId s; } names[] = {
		{"speed", STR_L_SPEED}, {"retries", STR_L_RETRIES}, {"check", STR_L_CHECK}, {"eject", STR_L_EJECT},
		{"autostart", STR_L_AUTOSTART}, {"name_pattern", STR_L_FILENAME}, {"redump", STR_L_REDUMP}, {"remote", STR_L_REMOTE},
		{"webhook_on", STR_L_WEBHOOK}, {"webhook", STR_WH_ADDRESS}, {"outdir", STR_L_TARGET}, {"audit_mb", STR_L_AUDIT_MB}};
	for (auto &k : names) if (!strcmp(key, k.key)) return T(k.s);
	if (!strncmp(key, "webhook_header", 14)) return T(STR_WH_HEADERS);
	return key;
}
static void av_setting_val(const char *key, const char *val, char *out, size_t n)
{
	int v = atoi(val);
	if (!val[0]) out[0] = 0;
	else if (!strcmp(key, "speed") && v >= 0 && v <= 3) fmt_speed(v, out, n);
	else if (!strcmp(key, "check") && v >= CHECK_OFF && v <= CHECK_BITPERFECT) fmt_check(v, out, n);
	else if (!strcmp(key, "eject") || !strcmp(key, "autostart") || !strcmp(key, "remote") || !strcmp(key, "webhook_on") || !strcmp(key, "webhook_headers_on"))
		fmt_onoff(v, out, n);
	else if (!strcmp(key, "redump")) snprintf(out, n, "%s", v == RD_ONLINE ? T(STR_RD_ONLINE) : v == RD_LOCAL ? T(STR_RD_LOCAL) : T(STR_OFF));
	else if (!strcmp(key, "audit_mb")) snprintf(out, n, T(STR_AUDIT_MB_VAL), v);
	else if (!strcmp(key, "name_pattern")) fn_label(val, out, n);
	else snprintf(out, n, "%s", val);
}

// The core's reason as words in the UI's language.
static void av_reason(const AuditReason &rs, char *out, size_t n)
{
	switch (rs.code) {
	case AW_NONE: out[0] = 0; break;
	case AW_ALREADY: snprintf(out, n, "%s", T(STR_AW_ALREADY)); break;
	case AW_RECORD: snprintf(out, n, "%s", T(STR_AW_RECORD)); break;
	case AW_SECRET: snprintf(out, n, "%s", T(STR_AW_SECRET)); break;
	case AW_SETTING_CHANGED: snprintf(out, n, T(STR_AW_SETTING_CHANGED), av_key_label(rs.what)); break;
	case AW_SECTOR_CHANGED: snprintf(out, n, T(STR_AW_SECTOR_CHANGED), (unsigned)rs.lba); break;
	case AW_DROPPED: snprintf(out, n, "%s", T(STR_AW_DROPPED)); break;
	case AW_UNFINISHED: snprintf(out, n, "%s", T(STR_AW_UNFINISHED)); break;
	case AW_RUNNING: case AW_JOB_ON_DISC: snprintf(out, n, "%s", T(STR_AW_RUNNING)); break;
	case AW_CANNOT_OPEN: snprintf(out, n, T(STR_AW_CANNOT_OPEN), rs.file); break;
	case AW_CANNOT_WRITE: snprintf(out, n, T(STR_AW_CANNOT_WRITE), rs.file); break;
	case AW_LOG_WRITE: snprintf(out, n, "%s", T(STR_AW_LOG_WRITE)); break;
	case AW_NOT_APPLIED: case AW_NO_HOOK: snprintf(out, n, T(STR_AW_NOT_APPLIED), av_key_label(rs.what)); break;
	case AW_NOTHING: snprintf(out, n, "%s", T(STR_AW_NOTHING)); break;
	case AW_NO_ROW: case AW_NO_DISC: snprintf(out, n, "%s", T(STR_AW_NOT_FOUND)); break;
	case AW_REPLACED: snprintf(out, n, "%s", T(STR_AW_REPLACED)); break;
	case AW_STOPPED: snprintf(out, n, "%s", T(STR_AW_STOPPED)); break;
	default: snprintf(out, n, "%s", T(STR_AUDIT_NO_UNDO)); break;
	}
}
// A row's own verdict (from the query) as a reason.
static AuditReason av_row_reason(const AuditRow &r)
{
	AuditReason rs;
	memset(&rs, 0, sizeof(rs));
	rs.code = r.why;
	rs.row = r.id; rs.job = r.why_job; rs.lba = r.lba;
	snprintf(rs.what, sizeof(rs.what), "%.95s", r.key[0] ? r.key : r.what);   // a config key: short (an undo row: its setting)
	const char *slash = strrchr(r.file, '/');
	snprintf(rs.file, sizeof(rs.file), "%.95s", slash ? slash + 1 : r.file);
	return rs;
}

// What a row did, in words.
static void av_row_label(const AuditRow &r, char *out, size_t n)
{
	if (!strcmp(r.kind, "job"))
		snprintf(out, n, "%s", T(!strcmp(r.what, "repair") ? STR_HINT_REPAIR : !strcmp(r.what, "check") ? STR_HINT_CHECK : STR_HINT_RIP));
	else if (!strcmp(r.kind, "sector")) snprintf(out, n, T(STR_AUDIT_SECTOR), (unsigned)r.lba);
	else if (!strcmp(r.kind, "setting")) {
		if (r.norev) snprintf(out, n, "%s", av_key_label(r.what));   // a secret: only set / cleared was logged
		else {
			char a[96], b[96];
			av_setting_val(r.what, r.before, a, sizeof(a));
			av_setting_val(r.what, r.after, b, sizeof(b));
			snprintf(out, n, "%s: %s → %s", av_key_label(r.what), a, b);
		}
	}
	else if (!strcmp(r.kind, "result"))
		snprintf(out, n, "%s", T(!strcmp(r.after, "finished") ? STR_TAB_DONE : !strcmp(r.after, "cancelled") ? STR_TAB_CANCELLED :
		                         !strcmp(r.after, "disc_lost") ? STR_WH_SHORT_10 : STR_TAB_FAILED));
	else if (!strcmp(r.kind, "redump")) {
		if (!strncmp(r.after, "verified", 8)) snprintf(out, n, T(STR_RD_CARD_OK), r.after[8] ? r.after + 9 : "");
		else snprintf(out, n, "%s", T(!strcmp(r.after, "no_match") ? STR_RD_NOMATCH : !strcmp(r.after, "not_found") ? STR_RD_NOTFOUND : STR_RD_UNCHECKED));
	}
	else if (!strcmp(r.kind, "undo")) snprintf(out, n, T(STR_AUDIT_UNDO_ROW), r.what);
	else snprintf(out, n, "%s %.100s", r.kind, r.what);   // a kind this screen does not know yet
	if (r.undone) {
		size_t l = strlen(out);
		snprintf(out + l, n - l, " %s", T(STR_AUDIT_UNDONE));
	}
}

// Entry i of the level: its label, and the value at the right (when, and where it came from).
static void audit_label(const AuditView &v, int i, char *out, size_t n)
{
	out[0] = 0;
	if (v.level == 0) snprintf(out, n, "%s", T(av_doms[i] == AU_RIPS ? STR_AUDIT_DOM_RIPS : STR_AUDIT_DOM_SETTINGS));
	else if (v.level == 1 && v.dom == AU_RIPS) { if (i < v.n) snprintf(out, n, "%s", av_names[i]); }
	else {
		int k = v.level == 3 ? i - v.base : i;
		if (k >= 0 && k < v.n) av_row_label(av_rows[k], out, n);
	}
}
static void av_value(const AuditView &v, int i, char *out, size_t n)
{
	out[0] = 0;
	if (v.level == 0) return;
	if (v.level == 1 && v.dom == AU_RIPS) { fmt_local(av_last[i], out, n); return; }
	int k = v.level == 3 ? i - v.base : i;
	if (k < 0 || k >= v.n) return;
	char when[32];
	fmt_local(av_rows[k].t, when, sizeof(when));
	snprintf(out, n, "%s · %s", when, av_src(av_rows[k].src));
}

// The audit screen's undo is running: the recheck of a rip shows a wait screen (recheck_hook; not
// in the UI tests, which have no screen to flip).
static int g_check_ui;
static char g_wait_name[128];

// Undoes the focused item; the list is read again and the focus stays on it. 0 = done, else why[]
// says why not (English, for the log) and v.why has the reason for the screen.
static int audit_undo(AuditView &v, char *why, size_t n)
{
	const AuditRow *r = audit_focused(v);
	char key[100];
	av_focus_key(v, key, sizeof(key));
	why[0] = 0;
	int rc = -1;
	AuditReason rs;
	memset(&rs, 0, sizeof(rs));
	if (v.level == 1 && v.dom == AU_RIPS) rc = audit_undo_disc_since(key, 0, why, n, &rs);
	else if (r && v.level == 2 && !strcmp(r->kind, "job")) rc = audit_undo_job(r->job, why, n, &rs);
	else if (r) rc = audit_undo_row(r->id, why, n, &rs);
	if (rc) logf("audit undo of %s refused: %s", key, why);   // the English text, for the log
	snprintf(v.why_key, sizeof(v.why_key), "%s", rc ? key : "");
	v.why = rs;
	av_load(v);
	if (v.level == 1 || v.level == 2) {   // newer rows come first: find the item again
		char k2[100];
		int keep = v.row;
		for (v.row = 0; v.row < v.n; v.row++) { av_focus_key(v, k2, sizeof(k2)); if (!strcmp(k2, key)) break; }
		if (v.row >= v.n) v.row = keep < v.n ? keep : 0;
	}
	av_fix(v);
	return rc;
}

static void audit_draw(const AuditView &v)
{
	draw_backdrop();
	int ls = ui_line(UI_SMALL);
	char sub[240], lb[160];
	if (v.level == 0) snprintf(sub, sizeof(sub), "%s", T(STR_L_AUDIT));
	else if (v.level == 1) snprintf(sub, sizeof(sub), "%s · %s", T(STR_L_AUDIT), T(v.dom == AU_RIPS ? STR_AUDIT_DOM_RIPS : STR_AUDIT_DOM_SETTINGS));
	else if (v.level == 2) snprintf(sub, sizeof(sub), "%s", v.disc_name);
	else { av_row_label(v.ev, lb, sizeof(lb)); snprintf(sub, sizeof(sub), "%s · %s", lb, v.disc_name); }
	int top = ui_h() / 10 + ui_line(UI_TITLE) + ui_line(UI_BODY) * 2, lh = ui_line(UI_BODY) * 17 / 10;
	int left = ui_w() / 6, right = ui_w() * 5 / 6;
	ui_text_fit(UI_BODY, ui_w() / 2, ui_h() / 10 + ui_line(UI_TITLE) + ui_line(UI_BODY) / 2, 0xA8B0C8, sub, right - left, UI_CENTER);
	int cnt = av_count(v);
	if ((v.level == 0 && v.empty) || !cnt) {
		ui_text(UI_BODY, ui_w() / 2, ui_h() / 3, 0xC8D0E0, T(STR_AUDIT_EMPTY), UI_CENTER);
		const char *b[] = {"circle"};
		const char *l[] = {T(STR_HINT_BACK)};
		hint_bar(b, l, 1);
		return;
	}
	{   // more rows above / below: arrows at the right edge, as in the settings (L2/R2 jump a page)
		int asz = ls * 2 / 3, ax = right + ui_w() / 40;
		if (v.first > 0) ui_icon("up", ax, top + (lh * 3 / 4 - asz) / 2 - lh / 8, asz, 0xC8D0E0);
		if (v.first + AV_PAGE < cnt || v.more) ui_icon("down", ax, top + (AV_PAGE - 1) * lh + (lh * 3 / 4 - asz) / 2 - lh / 8, asz, 0xC8D0E0);
	}
	for (int i = v.first; i < cnt && i < v.first + AV_PAGE; i++) {
		int y = top + (i - v.first) * lh;
		char val[96];
		audit_label(v, i, lb, sizeof(lb));
		av_value(v, i, val, sizeof(val));
		if (i == v.row) ui_rect(left - ui_w() / 60, y - lh / 4, right - left + ui_w() / 30, lh, 0x703D6FD9);
		// when and from where: a detail, small, on the label's baseline
		int vy = y + ui_line(UI_BODY) - ui_line(UI_SMALL) - ui_line(UI_BODY) / 12;
		int vw = val[0] ? ui_text(UI_SMALL, right, vy, i == v.row ? 0xFFFFFF : 0xA8B0C8, val, UI_RIGHT) + ui_w() / 40 : 0;
		ui_text_fit(UI_BODY, left, y, 0xFFFFFF, lb, right - left - vw);   // never over the value
	}
	// the focused item's details, small: why it cannot be undone, or the file a sector is in
	char det[300] = "", key[100];
	const AuditRow *r = audit_focused(v);
	av_focus_key(v, key, sizeof(key));
	char rtx[200];
	if (v.why_key[0] && !strcmp(v.why_key, key)) { av_reason(v.why, rtx, sizeof(rtx)); snprintf(det, sizeof(det), T(STR_AUDIT_REFUSED), rtx); }
	else if (!audit_can_undo(v) && (v.level == 1 && v.dom == AU_RIPS ? v.n > 0 : r && !strcmp(r->kind, "job") && v.level == 2))
		av_reason(v.no_undo, det, sizeof(det));   // a disc or a job: the core's answer
	else if (r && (!r->undone || r->redo) && !r->reversible) av_reason(av_row_reason(*r), det, sizeof(det));   // an undone row: why no redo
	else if (r && (!strcmp(r->kind, "sector") || (!strcmp(r->kind, "undo") && r->file[0]))) { const char *s = strrchr(r->file, '/'); snprintf(det, sizeof(det), "%s", s ? s + 1 : r->file); }
	if (det[0]) footer_note(det);
	const char *b[5], *l[5];
	int m = 0;
	b[m] = "updown"; l[m++] = T(STR_HINT_SELECT);
	if (cnt > AV_PAGE || v.more) { b[m] = "l2r2"; l[m++] = T(STR_HINT_PAGE); }
	if (av_can_open(v)) { b[m] = "cross"; l[m++] = T(STR_HINT_OPEN); }
	if (audit_can_undo(v)) { b[m] = "triangle"; l[m++] = T(audit_is_redo(v) ? STR_HINT_REDO : STR_HINT_UNDO); }
	b[m] = "circle"; l[m++] = T(STR_HINT_BACK);
	hint_bar(b, l, m);
}

// The wait while an undone rip is checked again (it reads the whole image), or while a big undo
// writes its sectors back (STR_AUDIT_UNDOING): its name and how far.
static void audit_wait_draw(const char *name, uint32_t mb, StrId what = STR_AUDIT_WAIT)
{
	draw_backdrop();
	ui_text(UI_BODY, ui_w() / 2, ui_h() / 10 + ui_line(UI_TITLE) + ui_line(UI_BODY) / 2, 0xA8B0C8, T(STR_L_AUDIT), UI_CENTER);
	char t[300];
	snprintf(t, sizeof(t), T(what), name);
	int y = draw_wrapped(UI_BODY, t, ui_h() / 3, 0xE8E8F0);
	if (mb) {
		snprintf(t, sizeof(t), T(STR_AUDIT_MB_VAL), (int)mb);
		ui_text(UI_SMALL, ui_w() / 2, y + ui_line(UI_SMALL) / 2, 0xA8B0C8, t, UI_CENTER);
	}
}
static void audit_wait_tick(uint32_t sectors)
{
	audit_wait_draw(g_wait_name, (uint32_t)((uint64_t)sectors * RAW_SIZE >> 20));
	g_drawn = 2;
	g_backdrop_shown = 0;
	flip_frame();   // both buffers
	flip_frame();
}
// What the wait of a big undo says: undoing, or redoing (applying undone changes again).
static StrId audit_wait_what(int redo) { return redo ? STR_AUDIT_REDOING : STR_AUDIT_UNDOING; }
static int g_wait_redo;   // the audit screen's undo running now is a redo

// The audit's undo of a job or disc, every AUDIT_TICK sectors: the screen says how far (the audit
// screen only) and the system's callbacks run; closing the app stops the undo.
static int audit_undo_tick(uint32_t done, uint32_t total)
{
	(void)total;
	if (g_check_ui) {
		audit_wait_draw(g_wait_name, (uint32_t)((uint64_t)done * RAW_SIZE >> 20), audit_wait_what(g_wait_redo));
		g_drawn = 2;
		g_backdrop_shown = 0;
		flip_frame();
	} else sysUtilCheckCallback();
	return exit_requested;
}

static void audit_screen()
{
	AuditView v;
	audit_view_init(v);
	pad_pressed();
	g_redraw = 2;
	while (!exit_requested) {
		if (g_redraw > 0) {
			g_redraw = 0;
			audit_draw(v);
			g_drawn = 2;
			g_backdrop_shown = 0;
		}
		ui_at(SCR_AUDIT_LOG, v.row);
		flip_frame();
		int p = pad_pressed();
		if (p) g_redraw = 2;
		if (p & (B_UP | B_DOWN | B_L2 | B_R2)) audit_nav(v, p);
		if (p & B_CROSS) audit_open(v);
		if ((p & B_TRIANGLE) && audit_can_undo(v)) {
			char label[160], q[256];
			audit_label(v, v.row, label, sizeof(label));
			int disc = v.level == 1 && v.dom == AU_RIPS, redo = audit_is_redo(v);   // a disc: all of it
			snprintf(q, sizeof(q), T(redo ? (disc ? STR_AUDIT_REDO_DISC_Q : STR_AUDIT_REDO_Q) : (disc ? STR_AUDIT_UNDO_DISC_Q : STR_AUDIT_UNDO_Q)), label);
			if (own_dialog(q, 1) == 1) {
				char why[200];
				snprintf(g_wait_name, sizeof(g_wait_name), "%s", label);   // a big undo shows it while it writes
				g_check_ui = 1;
				g_wait_redo = redo;
				int rc = audit_undo(v, why, sizeof(why));
				g_check_ui = g_wait_redo = 0;
				if (rc != 0) {
					char m[300], rtx[200];
					av_reason(v.why, rtx, sizeof(rtx));
					snprintf(m, sizeof(m), T(STR_AUDIT_REFUSED), rtx);
					own_dialog(m, 0);
				}
			}
			pad_pressed();
			g_redraw = 2;
		}
		if ((p & B_CIRCLE) && !audit_back(v)) return;
	}
}

// Remote control (a row of the settings): the web server's three switches, each with where it
// answers. One frame of it (the UI tests draw it).
static void remote_control_draw(int row)
{
	draw_backdrop();
	ui_text(UI_BODY, ui_w() / 2, ui_h() / 10 + ui_line(UI_TITLE) + ui_line(UI_BODY) / 2, 0xA8B0C8, T(STR_L_REMOTE), UI_CENTER);
	int top = ui_h() / 10 + ui_line(UI_TITLE) + ui_line(UI_BODY) * 2, lh = ui_line(UI_BODY) * 17 / 10;
	int left = ui_w() / 6, right = ui_w() * 5 / 6;
	const StrId labels[3] = {STR_L_REMOTE_VIEW, STR_L_MCP, STR_L_STATUS_JSON}, help[3] = {STR_RC_VIEW_HELP, STR_RC_MCP_HELP, STR_RC_STATUS_HELP};
	const int on[3] = {g_remote, g_mcp, g_status_json};
	for (int r = 0; r < 3; r++) {
		int y = top + r * lh;
		char v[64];
		char t[48];
		snprintf(v, sizeof(v), r == row ? "‹ %s ›" : "%s", opt_text(OPTS(o_onoff), on[r] ? 1 : 0, t, sizeof(t)));
		if (r == row) ui_rect(left - ui_w() / 60, y - lh / 4, right - left + ui_w() / 30, lh, 0x703D6FD9);
		ui_text(UI_BODY, left, y, 0xFFFFFF, T(labels[r]));
		ui_text(UI_BODY, right, y, r == row ? 0xFFFFFF : 0xC8D0E0, v, UI_RIGHT);
	}
	char ip[20], det[200];
	remote_ip(ip, sizeof(ip));
	snprintf(det, sizeof(det), T(help[row]), ip, REMOTE_PORT);
	footer_note(det);
	const char *b[] = {"updown", "leftright", "circle"};
	const char *l[] = {T(STR_HINT_SELECT), T(STR_HINT_CHANGE), T(STR_HINT_BACK)};
	hint_bar(b, l, 3);
}

static void remote_control_screen()
{
	int row = 0;
	const int NROWS = 3;
	pad_pressed();
	g_redraw = 2;
	while (!exit_requested) {
		if (g_redraw > 0) {
			g_redraw = 0;
			remote_control_draw(row);
			g_drawn = 2;
			g_backdrop_shown = 0;
		}
		ui_at(SCR_REMOTE_CONTROL, row);
		flip_frame();
		int p = pad_pressed();
		if (p) g_redraw = 2;
		if (p & B_UP) row = (row + NROWS - 1) % NROWS;
		if (p & B_DOWN) row = (row + 1) % NROWS;
		if (p & B_R2) row = NROWS - 1;   // all rows show: L2/R2 go to the first / last
		if (p & B_L2) row = 0;
		if (p & B_CIRCLE) return;
		if (!(p & (B_LEFT | B_RIGHT | B_CROSS))) continue;   // X switches too
		CfgLock lock;
		int &v = row == 0 ? g_remote : row == 1 ? g_mcp : g_status_json;
		v = opt_wrap(OPTS(o_onoff), v, (p & B_LEFT) ? -1 : 1);
		remote_apply();
		cfg_save();   // the audit log gets the change
		logf("remote control: view=%d mcp=%d status_json=%d", g_remote, g_mcp, g_status_json);
		if (row == 0 && g_remote) notify_url(STR_REMOTE_HELP, "");
	}
}

// Settings (START): the standard for the drives (speed, re-reads, check, eject, rip at once; a
// drive can have its own values, SELECT on its card), Redump, the Redump copy, remote control, webhooks.
// One frame of the settings (START): the rows with their values and the buttons of the focused
// row. (The UI tests on the host draw it.)
static void settings_draw(int row, const DriveCfg &sd, int redump, int remote, int webhook_on, int rdcount, const char *rdver, time_t rdwhen)
{
	const int NROWS = 13;
	draw_backdrop();
	char vals[13][96];
	fn_label(g_name_pat, vals[5], sizeof(vals[5]));
	fmt_speed(sd.speed, vals[0], sizeof(vals[0]));
	fmt_int(sd.retries, vals[1], sizeof(vals[1]));
	fmt_check(sd.check, vals[2], sizeof(vals[2]));
	fmt_onoff(sd.eject, vals[3], sizeof(vals[3]));
	fmt_onoff(sd.autostart, vals[4], sizeof(vals[4]));
	opt_text(OPTS(o_redump), redump, vals[6], sizeof(vals[6]));
	if (rdcount > 0) {
		// Redump's version (date) and when this copy was downloaded (date and time)
		char date[16], ver[24], when[32];
		snprintf(date, sizeof(date), "%.10s", rdver);
		fmt_rd_version(date, ver, sizeof(ver));
		fmt_local(rdwhen, when, sizeof(when));
		snprintf(vals[7], sizeof(vals[7]), T(STR_RD_COPY_VALUE), ver, when);
	} else {
		snprintf(vals[7], sizeof(vals[7]), "%s", T(STR_RD_MISSING));
	}
	char ip[20];
	remote_ip(ip, sizeof(ip));
	if (remote) snprintf(vals[8], sizeof(vals[8]), T(STR_REMOTE_ON), ip, REMOTE_PORT);
	else snprintf(vals[8], sizeof(vals[8]), "%s", T(STR_OFF));
	if (!webhook_on) snprintf(vals[9], sizeof(vals[9]), "%s", T(STR_OFF));
	else if (!g_webhook[0]) snprintf(vals[9], sizeof(vals[9]), "%s", T(STR_WEBHOOK_NOURL));
	else {
		char host[96];
		webhook_host(g_webhook, host, sizeof(host));
		int ne = 0;
		for (int i = 0; i < WH_COUNT; i++) ne += (g_webhook_mask >> i) & 1;
		snprintf(vals[9], sizeof(vals[9]), T(STR_WEBHOOK_ON_N), host, ne);
	}
	vals[10][0] = 0;   // the audit log: X opens it
	opt_text(OPTS(o_audit_mb), g_audit_mb, vals[11], sizeof(vals[11]));
	snprintf(vals[12], sizeof(vals[12]), "v%s", APP_VERSION);
	const char *labels[13] = {T(STR_L_SPEED), T(STR_L_RETRIES), T(STR_L_CHECK), T(STR_L_EJECT), T(STR_L_AUTOSTART), T(STR_L_FILENAME),
	                         T(STR_L_REDUMP), T(STR_L_RDCOPY), T(STR_L_REMOTE), T(STR_L_WEBHOOK), T(STR_L_AUDIT), T(STR_L_AUDIT_MB), T(STR_L_ABOUT)};
	settings_rows(T(STR_SETTINGS_TITLE), labels, vals, NROWS, row, 1u << 7 | 1u << 8 | 1u << 10 | 1u << 12);   // the Redump download, remote control, the audit log and About are actions
	if (row == 8 || row == 10 || row == 12) {   // remote control, the audit log and About: X opens them
		const char *b[] = {"updown", "l2r2", "cross", "circle"};
		const char *l[] = {T(STR_HINT_SELECT), T(STR_HINT_PAGE), T(STR_HINT_OPEN), T(STR_HINT_BACK)};
		hint_bar(b, l, 4);
	} else if (row == 9 || row == 5) {   // webhooks and the file name: X opens their own view
		const char *b[] = {"updown", "l2r2", "leftright", "cross", "triangle", "circle"};
		const char *l[] = {T(STR_HINT_SELECT), T(STR_HINT_PAGE), T(STR_HINT_CHANGE), T(row == 5 ? STR_HINT_EDIT : STR_HINT_SHOW), T(STR_HINT_DEFAULTS), T(STR_HINT_BACK)};
		hint_bar(b, l, 6);
	} else {
		const char *b[] = {"updown", "l2r2", row == 7 ? "cross" : "leftright", "triangle", "circle"};
		const char *l[] = {T(STR_HINT_SELECT), T(STR_HINT_PAGE), row == 7 ? T(STR_HINT_DOWNLOAD) : T(STR_HINT_CHANGE), T(STR_HINT_DEFAULTS), T(STR_HINT_BACK)};
		hint_bar(b, l, 5);
	}
}

static void settings_screen()
{
	// the values shown are the settings themselves (no copies): a change over MCP shows at once
	int row = 0;
	const int NROWS = 13;   // the last row opens About
	char rdver[64] = "";
	time_t rdwhen = 0;
	int rdcount = rd_local_info(rdver, sizeof(rdver), &rdwhen);
	pad_pressed();
	g_redraw = 2;
	while (!exit_requested) {
		if (g_redraw > 0) {
			g_redraw = 0;
			g_drawn = 2;
			settings_draw(row, g_std, g_redump, g_remote || g_mcp || g_status_json, g_webhook_on, rdcount, rdver, rdwhen);
		}
		ui_at(SCR_SETTINGS, row);
		flip_frame();
		int p = pad_pressed();
		if (p) g_redraw = 2;
		if (p & B_UP) row = (row + NROWS - 1) % NROWS;
		if (p & B_DOWN) row = (row + 1) % NROWS;
		if (p & B_R2) row = row + 9 < NROWS ? row + 9 : NROWS - 1;   // a page down / up (9 rows show)
		if (p & B_L2) row = row >= 9 ? row - 9 : 0;
		if ((p & B_CROSS) && row == 5) {
			filename_screen();
			pad_pressed();
			g_redraw = 2;
			continue;
		}
		if ((p & B_CROSS) && row == 12) {
			about_screen();
			pad_pressed();
			g_redraw = 2;
			continue;
		}
		if ((p & B_CROSS) && row == 10) {
			audit_screen();
			pad_pressed();
			g_redraw = 2;
			continue;
		}
		if ((p & B_CROSS) && row == 8) {
			remote_control_screen();
			pad_pressed();
			g_redraw = 2;
			continue;
		}
		if ((p & B_CROSS) && row == 9) {
			webhook_settings_screen();
			pad_pressed();
			g_redraw = 2;
			continue;
		}
		if ((p & B_CROSS) && row == 7) {
			redump_download_screen();
			rdcount = rd_local_info(rdver, sizeof(rdver), &rdwhen);
			pad_pressed();
			g_redraw = 2;
			continue;
		}
		int d = (p & B_RIGHT) ? 1 : (p & B_LEFT) ? -1 : 0;
		if (row == 7 || row == 8 || row == 10 || row == 12) d = 0;
		int reset = 0;
		if (p & B_TRIANGLE) {
			reset = own_dialog(T(STR_RESET_Q), 1) == 1;
			pad_pressed();
			g_redraw = 2;
		}
		if (!d && !reset) { if (p & B_CIRCLE) return; continue; }
		{   // every change is applied and saved immediately
			CfgLock lock;
			DriveCfg &sd = g_std;
			if (reset) {   // defaults; the webhook address and the Redump copy are kept
				DriveCfg st = {1, 4, 0, 1, 0, CHECK_BITPERFECT};
				snprintf(g_name_pat, sizeof(g_name_pat), "%s", FN_PRESETS[0]);
				sd = st;
				g_redump = RD_OFF;
				g_remote = g_mcp = g_status_json = 0;
				remote_apply();
				g_webhook_on = 0; g_webhook_step = 5;
				g_audit_mb = 256;
			} else if (row == 0) {   // right = faster (2x, 4x, 8x, max), no wrap-around
				sd.speed = opt_clamp(OPTS(o_speed), sd.speed, d);
			} else if (row == 1) {
				sd.retries = opt_wrap(OPTS(o_retries), sd.retries, d);
			} else if (row == 2) {
				sd.check = opt_wrap(OPTS(o_check), sd.check, d);
			} else if (row == 3) {
				sd.eject = opt_wrap(OPTS(o_onoff), sd.eject, d);
			} else if (row == 4) {
				sd.autostart = opt_wrap(OPTS(o_onoff), sd.autostart, d);
			} else if (row == 5) {   // the ready-made patterns, and the own one
				fn_step(d);
			} else if (row == 6) {
				g_redump = opt_wrap(OPTS(o_redump), g_redump, d);
			} else if (row == 9) {
				g_webhook_on = opt_wrap(OPTS(o_onoff), g_webhook_on, d);
			} else if (row == 11) {   // right = more; no wrap-around: 256 to 1 MB would drop the kept sectors
				g_audit_mb = opt_clamp(OPTS(o_audit_mb), g_audit_mb, d);
			}
			for (int c = 0; c < NCARDS; c++) g_cards[c].recheck = 1;   // names and destinations may have changed
			cfg_save();
			audit_apply_cap();   // a smaller size drops the oldest kept sectors at once
			logf("settings saved: standard speed=%d retries=%d check=%d eject=%d autostart=%d, redump=%d remote=%d webhook=%d audit_mb=%d",
			     sd.speed, sd.retries, sd.check, sd.eject, sd.autostart, g_redump, g_remote, g_webhook_on, g_audit_mb);
		}
		if (p & B_CIRCLE) return;
	}
}

// Settings of one drive (SELECT on its card): read speed, re-reads, sector check, eject after a
// rip, rip a disc at once. Each is "standard" (Settings) unless set here for this drive. Saved per
// drive: the PS3's as drive_bd=, a USB drive by its port as drive_usbN=.
static void drive_settings_screen(int c)
{
	Card &k = g_cards[c];
	int row = 0;
	const int NROWS = 5;
	pad_pressed();
	g_redraw = 2;
	while (!exit_requested) {
		if (g_redraw > 0) {
			g_redraw = 0;
			g_drawn = 2;
			draw_backdrop();
			char vals[5][96], name[96];
			source_name(c ? c - 1 : -1, name, sizeof(name));
			drive_val(vals[0], sizeof(vals[0]), k.speed, g_std.speed, fmt_speed);
			drive_val(vals[1], sizeof(vals[1]), k.retries, g_std.retries, fmt_int);
			drive_val(vals[2], sizeof(vals[2]), k.check, g_std.check, fmt_check);
			drive_val(vals[3], sizeof(vals[3]), k.eject, g_std.eject, fmt_onoff);
			drive_val(vals[4], sizeof(vals[4]), k.autostart, g_std.autostart, fmt_onoff);
			const char *labels[5] = {T(STR_L_SPEED), T(STR_L_RETRIES), T(STR_L_CHECK), T(STR_L_EJECT), T(STR_L_AUTOSTART)};
			settings_rows(name, labels, vals, NROWS, row, 0);
			const char *b[] = {"updown", "leftright", "triangle", "circle"};
			const char *l[] = {T(STR_HINT_SELECT), T(STR_HINT_CHANGE), T(STR_HINT_STANDARD), T(STR_HINT_BACK)};
			hint_bar(b, l, 4);
		}
		ui_at(SCR_DRIVE_SETTINGS, row, c);
		flip_frame();
		int p = pad_pressed();
		if (p) g_redraw = 2;
		if (p & B_UP) row = (row + NROWS - 1) % NROWS;
		if (p & B_DOWN) row = (row + 1) % NROWS;
		if (p & B_R2) row = NROWS - 1;   // all rows show: L2/R2 go to the first / last
		if (p & B_L2) row = 0;
		int d = (p & B_RIGHT) ? 1 : (p & B_LEFT) ? -1 : 0;
		if (!d && !(p & B_TRIANGLE)) { if (p & B_CIRCLE) return; continue; }
		int reset = 0;
		CfgLock lock;
		if (p & B_TRIANGLE) { card_defaults(k); reset = 1; }   // all back to the standard
		if (d) {
			if (row == 0) k.speed = opt_std(OPTS(o_speed), k.speed, d);   // right = faster
			else if (row == 1) k.retries = opt_std(OPTS(o_retries), k.retries, d);
			else if (row == 2) k.check = opt_std(OPTS(o_check), k.check, d);
			else if (row == 3) k.eject = opt_std(OPTS(o_onoff), k.eject, d);
			else if (row == 4) k.autostart = opt_std(OPTS(o_onoff), k.autostart, d);
		}
		if (d || reset) {
			card_store(c);
			cfg_save();
			logf("drive %d settings: speed=%d retries=%d check=%d eject=%d autostart=%d (-1 = standard)", c, k.speed, k.retries, k.check, k.eject, k.autostart);
		}
		if (p & B_CIRCLE) return;
	}
}

// Ejects the disc (START STOP UNIT with LoEj).
static void eject_disc()
{
	while (g_ident_busy) usleep(10000);
	if (g_use == SRC_USB) {
		logf("eject: card %d -> usb drive %d (port %d)", cur_card(), g_usb, usbodd_port(g_usb));
		usbodd_eject(g_usb);
		fire_event("disc_ejected");
		g_disc_state = DISC_NONE;
		disc_present = 0;
		return;
	}
	if (sys_storage_open(BD_DEVICE, &g_fd)) return;
	uint8_t cdb[12] = {0x1B, 0, 0, 0, 0x02, 0, 0, 0, 0, 0, 0, 0};
	logf("eject (home) -> 0x%x", atapi(cdb, NULL, 0, ATAPI_NON_DATA_PROTO));
	fire_event("disc_ejected");
	sys_storage_close(g_fd);
	g_disc_state = DISC_NONE;
}

static int card_busy(int c)
{
	return ctx_ripping(c) || g_cards[c].job || (g_ident_busy && g_ident_card == c) || (g_repairing && g_card == c);
}

// Makes card c the one the drive commands, OUT_DIR and the disc globals refer to. Only while idle.
static void select_card(int c)
{
	g_card = c;
	g_use = c ? SRC_USB : SRC_BD;
	if (c) g_usb = c - 1;
	if (!g_cards[c].dest[0]) snprintf(g_cards[c].dest, sizeof(g_cards[0].dest), "%s", HDD_DIR);
	OUT_DIR = g_cards[c].dest;
	const Card &k = g_cards[c];   // the rip reads these
	g_retries = std_or(k.retries, g_std.retries); g_speed = std_or(k.speed, g_std.speed); g_eject = std_or(k.eject, g_std.eject);
	g_autostart = std_or(k.autostart, g_std.autostart); g_check = std_or(k.check, g_std.check);
	if (g_cards[c].base[0]) snprintf(g_base, sizeof(g_base), "%s", g_cards[c].base);
}

// The cards there are, in screen order: the PS3's drive, then the USB drives by port.
static int card_list(int *out)
{
	int n = 0;
	out[n++] = 0;
	for (int port = -1; port < 8; port++)
		for (int i = 0; i < USBODD_DRIVES; i++)
			if (usbodd_attached(i) && usbodd_port(i) == port) out[n++] = 1 + i;
	return n;
}

// A destination was chosen for card c: remember it (per USB port for the USB drives) and save.
static void card_apply_outdir(int c, const char *dir)
{
	Card &k = g_cards[c];
	// dir may be one of these buffers (the undo passes the global): never copy one onto itself
	auto put = [dir](char *to, size_t n) { if (to != dir) snprintf(to, n, "%s", dir); };
	put(k.outdir, sizeof(k.outdir));
	put(k.dest, sizeof(k.dest));
	if (c == 0) put(g_outdir, sizeof(g_outdir));
	else if (usbodd_port(c - 1) >= 0) put(g_outdir_usb[usbodd_port(c - 1)], sizeof(g_outdir_usb[0]));
	k.recheck = 1;
}
static void card_set_outdir(int c, const char *dir)
{
	CfgLock lock;
	card_apply_outdir(c, dir);
	cfg_save();
	logf("card %d: destination %s", c, dir);
}

enum { CF_OUTDIR = 1, CF_DRIVE = 2 };

// Card c takes its destination (CF_OUTDIR) and drive settings (CF_DRIVE) from the config. A card
// with a job keeps them until the job ends, as the screens do (they apply to the next job).
static void card_take_cfg(int c, int what)
{
	Card &k = g_cards[c];
	if (card_busy(c)) { k.pend_cfg |= what; k.recheck = 1; return; }
	k.pend_cfg &= ~what;
	int port = c ? usbodd_port(c - 1) : 0;
	if (c && (port < 0 || port > 7)) return;
	if (what & CF_DRIVE) card_from(k, c ? g_usbcfg[port] : g_bdcfg);
	if (what & CF_OUTDIR) card_apply_outdir(c, !c ? g_outdir : g_outdir_usb[port][0] ? g_outdir_usb[port] : HDD_DIR);
}

// The cards whose job ended take what changed meanwhile, and every card what MCP changed (each
// frame, on the UI thread: a job starts only there, so it never sees a half-written card).
static void cards_take_pending()
{
	for (int c = 0; c < NCARDS; c++)
		if (g_cards[c].pend_cfg && !card_busy(c)) { CfgLock lock; card_take_cfg(c, g_cards[c].pend_cfg); }
}

// After the audit's undo changed a setting: the same follow-ups as the settings screens (the web
// view, the cards' own copies of the drive settings and destinations, the cards' re-check).
static void setting_applied(const char *key)
{
	if (!strcmp(key, "remote") || !strcmp(key, "mcp") || !strcmp(key, "status_json")) remote_apply();
	else if (!strcmp(key, "drive_bd")) card_take_cfg(0, CF_DRIVE);
	else if (!strncmp(key, "drive_usb", 9) && key[9] >= '0' && key[9] <= '7' && !key[10]) {
		for (int c = 1; c < NCARDS; c++) if (usbodd_port(c - 1) == key[9] - '0') card_take_cfg(c, CF_DRIVE);
	}
	else if (!strcmp(key, "outdir")) card_take_cfg(0, CF_OUTDIR);
	else if (!strncmp(key, "outdir_usb", 10) && key[10] >= '0' && key[10] <= '7' && !key[11]) {
		for (int c = 1; c < NCARDS; c++) if (usbodd_port(c - 1) == key[10] - '0') card_take_cfg(c, CF_OUTDIR);
	}
	for (int c = 0; c < NCARDS; c++) g_cards[c].recheck = 1;   // names and destinations may have changed
}

// The system reports discs only for its own drive and nothing about USB drives coming and going.
// A thread looks every two seconds: USB optical drives (model and port when they connect, unlock
// so the slot takes a disc, disc in and out) and each card's destination (the chosen one when it
// is there, else the hard disk). A drive the app is reading from is left alone.
static void usb_presence_thread(void *arg)
{
	int was[USBODD_DRIVES];   // disc in USB drive n: -1 unknown, 0 no, 1 yes
	int fails[USBODD_DRIVES];        // commands in a row that got no answer at all
	for (int i = 0; i < USBODD_DRIVES; i++) { was[i] = -1; fails[i] = 0; }
	while (!exit_requested) {
		usleep(2000 * 1000);
		for (int i = 0; i < USBODD_DRIVES; i++) {
			Card &k = g_cards[1 + i];
			if (!usbodd_attached(i)) {
				fails[i] = 0; k.usb_dead = 0;
				if (was[i] != -1 || k.present) { k.present = 0; k.inserted = 0; was[i] = -1; g_redraw = 2; }
				continue;
			}
			if (card_busy(1 + i)) {   // in use: its own commands tell. Every 10 s, for the log only:
				// is the disc still in it (a SuperDrive has pushed out a disc during a repair)
				static int tick[USBODD_DRIVES], seen[USBODD_DRIVES];
				if (++tick[i] % 5 == 0) {
					const uint8_t tur[6] = {0};
					int r = usbodd_cmd(i, tur, 6, NULL, 0, 0);
					int gone = r != -1 && (r >> 16 & 0x0F) == 0x02 && (r >> 8 & 0xFF) == 0x3A;
					if (gone != seen[i]) { seen[i] = gone; logf("usb drive %d (port %d) during a job: %s (0x%x)", i, usbodd_port(i), gone ? "NO DISC" : "disc in it", r); }
				}
				was[i] = -1;
				continue;
			}
			if (!usbodd_name(i)[0]) {   // just connected: model, port and the destination chosen for that port
				usbodd_identify(i);
				int port = usbodd_port(i);
				const char *od = port >= 0 && g_outdir_usb[port][0] ? g_outdir_usb[port] : HDD_DIR;
				snprintf(k.outdir, sizeof(k.outdir), "%s", od);
				k.dest[0] = 0;
				card_defaults(k);
				if (port >= 0 && port < 8 && g_usbcfg[port].set) card_from(k, g_usbcfg[port]);
				k.state = DISC_NONE; k.tried = 0; k.base[0] = 0;
				logf("usb drive %d: %s, port %d", i, usbodd_name(i), port);
				g_redraw = 2;
			}
			const uint8_t tur[6] = {0};
			int r = usbodd_cmd(i, tur, 6, NULL, 0, 0);
			fails[i] = r == -1 ? fails[i] + 1 : 0;   // -1: no answer (the driver already reset the pipes)
			int dead = fails[i] >= 3;
			if (dead != k.usb_dead) { k.usb_dead = dead; g_redraw = 2; logf("usb drive %d: %s", i, dead ? "does not answer" : "answers again"); }
			// no answer, or unit attention (the drive was reset or the medium changed and it says so
			// once): ask again next time, it tells nothing about the disc
			int sk = r == -1 ? -1 : (r >> 16 & 0x0F);
			if (r == -1 || sk == 0x06) continue;
			// a disc that is still spinning up (not ready, becoming ready) counts as present
			int now = !(sk == 0x02 && (r >> 8 & 0xFF) == 0x3A);
			if (!now && was[i] != 0) usbodd_unlock(i);   // without it the slot does not take a disc
			// the first answer after the drive connected (or a job on it ended) tells whether a disc
			// was already in it: part of the start, not an insert. The SuperDrive only takes a disc
			// when it is unlocked, which the app does, so any disc that appears after an empty answer
			// was put in, and starts a rip when the drive's settings say so.
			if (was[i] == 0 && now) { k.present = 1; k.inserted = 1; logf("usb drive %d (port %d): disc inserted", i, usbodd_port(i)); }
			if (was[i] == 1 && !now) { k.present = 0; k.inserted = 0; logf("usb drive %d (port %d): disc removed", i, usbodd_port(i)); }
			if (was[i] == -1) { k.present = now; logf("usb drive %d (port %d): %s", i, usbodd_port(i), now ? "a disc is in it" : "empty"); }
			if (was[i] != now) g_redraw = 2;
			was[i] = now;
		}
		for (int c = 0; c < NCARDS; c++) {
			Card &k = g_cards[c];
			if (!k.outdir[0] || card_busy(c)) continue;   // a rip writes there: leave it be
			const char *dest = dest_available(k.outdir) ? k.outdir : HDD_DIR;
			if (strcmp(dest, k.dest)) {
				snprintf(k.dest, sizeof(k.dest), "%s", dest);
				logf("card %d: destination now %s", c, dest);
				k.recheck = 1;   // "already ripped here", free space
				g_redraw = 2;
			}
		}
	}
	sysThreadExit(0);
}

// One card: drive, disc, what is known about it here, and where it goes. Returns its height.
// Text that must stay within maxw pixels: cut at a character and ended with "…".
static void ui_text_fit(UiFont f, int x, int y, uint32_t col, const char *text, int maxw, UiAlign al)
{
	if (ui_text_width(f, text) <= maxw) { ui_text(f, x, y, col, text, al); return; }
	char b[256];
	snprintf(b, sizeof(b), "%s", text);
	int n = (int)strlen(b);
	while (n > 0) {
		do n--; while (n > 0 && ((unsigned char)b[n] & 0xC0) == 0x80);   // back one UTF-8 character
		snprintf(b + n, sizeof(b) - n, "…");
		if (ui_text_width(f, b) <= maxw) break;
	}
	ui_text(f, x, y, col, b, al);
}

// The right column of a card (and of the file name preview): the drive (icon + "USB 0" /
// "Internal drive") level with a body line at y, its model under it in small type, then the disc
// (disc icon + idl). Right-aligned at right; returns the width it takes.
static int drive_column(int c, int right, int y, const char *idl)
{
	int lb = ui_line(UI_BODY), ls = ui_line(UI_SMALL);
	char drive[96], model[48] = "";
	if (c && usbodd_port(c - 1) >= 0) snprintf(drive, sizeof(drive), T(STR_SRC_USB_PORT), usbodd_port(c - 1));
	else if (c) snprintf(drive, sizeof(drive), "USB");
	else snprintf(drive, sizeof(drive), "%s", T(STR_SRC_BD));
	if (c) snprintf(model, sizeof(model), "%s", usbodd_name(c - 1)[0] ? usbodd_name(c - 1) : "CD/DVD");
	int isz = ls * 3 / 4, igap = ls / 3, tw = ui_text_width(UI_SMALL, drive);
	int ty = y + (lb - ls) / 2;
	ui_icon(c ? "usb" : "drive", right - tw - igap - isz, ty + (ls - isz) / 2, isz, 0x8890A8);
	ui_text(UI_SMALL, right - tw, ty, 0x8890A8, drive);
	// the model: there, but small (it matters less than the drive and the disc)
	if (model[0]) ui_text(UI_TINY, right, y + lb + (ls - ui_line(UI_TINY)) / 2, 0x7078A0, model, UI_RIGHT);
	int iw = 0;
	if (idl && idl[0]) {
		iw = ui_text_width(UI_SMALL, idl) + isz + igap;
		ui_icon("disc", right - iw, y + lb + ls + (ls - isz) / 2, isz, 0x8890A8);
		ui_text(UI_SMALL, right, y + lb + ls, 0x8890A8, idl, UI_RIGHT);
	}
	int w = tw + isz + igap, mw = ui_text_width(UI_TINY, model);
	if (mw > w) w = mw;
	if (iw > w) w = iw;
	return w;
}


// Tabs: one per drive, with a short status, the selected one blue. They scroll sideways when
// there are more than fit. Returns the height used.
static int draw_tabs(const int *list, int nc, int sel, int y)
{
	int ls = ui_line(UI_SMALL), L = ui_w() / 8, R = ui_w() * 7 / 8;
	int pad = ls * 2 / 3, isz = ls * 3 / 4, igap = ls / 3, gap = ls / 2, th = ls * 2;
	// L1/R1 reminder on the right: "L1 ◂ ▸ R1" (the arrows drawn, the small font has no such glyphs)
	int asz0 = ls / 2, kg = ls / 3, w1 = ui_text_width(UI_SMALL, "L1"), w2 = ui_text_width(UI_SMALL, "R1");
	int kw = w1 + kg + asz0 + kg + asz0 + kg + w2, kx = R - kw, ky = y + (th - ls) / 2;
	ui_text(UI_SMALL, kx, ky, 0x8890A8, "L1");
	ui_icon("left", kx + w1 + kg, ky + (ls - asz0) / 2, asz0, 0x8890A8);
	ui_icon("right", kx + w1 + kg * 2 + asz0, ky + (ls - asz0) / 2, asz0, 0x8890A8);
	ui_text(UI_SMALL, kx + kw - w2, ky, 0x8890A8, "R1");
	int room = R - L - kw - gap * 2;
	char name[NCARDS][32], stt[NCARDS][24];
	uint32_t scol[NCARDS];
	int w[NCARDS], warn[NCARDS], si = 0;
	for (int i = 0; i < nc; i++) {
		int c = list[i];
		Card &k = g_cards[c];
		if (list[i] == sel) si = i;
		if (c == 0) snprintf(name[i], sizeof(name[0]), "%s", T(STR_TAB_INTERNAL));
		else if (usbodd_port(c - 1) >= 0) snprintf(name[i], sizeof(name[0]), T(STR_SRC_USB_PORT), usbodd_port(c - 1));
		else snprintf(name[i], sizeof(name[0]), "USB");
		scol[i] = 0x8890A8;
		if (k.job) {
			OnCard o(c);
			uint32_t pos = g_readpos > g_done ? g_readpos : g_done;
			uint32_t pm = g_total ? (uint32_t)((uint64_t)(pos > g_total ? g_total : pos) * 1000 / g_total) : 0;   // per mille, as on the page
			if (g_pass2_total && (k.repairing || g_phase == 1 || g_phase == 2)) pm = (uint32_t)((uint64_t)g_pass2_done * 1000 / g_pass2_total);
			if (g_phase == 3 && g_md5_total[c]) pm = (uint32_t)((uint64_t)g_md5_done[c] * 1000 / g_md5_total[c]);
			if (pm > 1000) pm = 1000;
			if (pm < 100) snprintf(stt[i], sizeof(stt[0]), T(STR_PCT_DEC), pm / 10, pm % 10);
			else snprintf(stt[i], sizeof(stt[0]), "%u %%", pm / 10);
			scol[i] = 0xC8E05A;
		} else if (k.res[0] && (k.state == DISC_READY || k.state == DISC_NONE)) {
			snprintf(stt[i], sizeof(stt[0]), "%s", T(k.res_failed == 2 ? STR_TAB_CANCELLED : k.res_failed ? STR_TAB_FAILED : STR_TAB_DONE));
			scol[i] = k.res_failed ? 0xF0C060 : k.res_col;
		} else if (k.state == DISC_READING) { snprintf(stt[i], sizeof(stt[0]), "%s", T(STR_TAB_READING)); }
		else if (k.state == DISC_READY) { snprintf(stt[i], sizeof(stt[0]), "%s", T(STR_TAB_READY)); scol[i] = 0xC8D0E0; }
		else if (k.state == DISC_ERROR) { snprintf(stt[i], sizeof(stt[0]), "%s", T(STR_TAB_NOTPSX)); scol[i] = 0xFF8A80; }
		else if (list[i] && k.usb_dead) { snprintf(stt[i], sizeof(stt[0]), "%s", T(STR_TAB_NORESP)); scol[i] = 0xF0C060; }
		else snprintf(stt[i], sizeof(stt[0]), "%s", T(STR_TAB_EMPTY));
		w[i] = pad + isz + igap + ui_text_width(UI_SMALL, name[i]) + gap + ui_text_width(UI_SMALL, stt[i]) + pad;
		// the page's Redump icon on the tab too, for the disc here (or, with none, the last rip): which
		// discs to put aside and try again later shows without opening each tab
		// 1 medal (matches), 2 warning (does not, or sectors left to repair), 3 question (not checked)
		warn[i] = 0;
		if (k.job) { OnCard o(c); warn[i] = g_readerrs ? 2 : k.rd_state == 1 ? 1 : 3; }   // ripping: ?, the warning from the first sector it could not read
		else {
			if (k.state == DISC_READY && k.prev_rip) warn[i] = k.repair_n > 0 || k.rd_state == 0 ? 2 : k.rd_state == 1 ? 1 : 3;
			else if (k.res[0] && !k.res_failed && (k.state == DISC_READY || k.state == DISC_NONE) && k.res_rd >= -1)
				warn[i] = k.res_bad || k.res_rd == 0 ? 2 : k.res_rd == 1 ? 1 : 3;
		}
		if (warn[i]) w[i] += igap + isz;
	}
	// the first tab shown: as far left as possible with the selected one still in view
	int first = 0;
	for (;;) {
		int x = 0;
		for (int i = first; i <= si; i++) x += w[i] + gap;
		if (x - gap <= room || first == si) break;
		first++;
	}
	int x = L, last = first - 1;
	for (int i = first; i < nc; i++) {
		if (x + w[i] > L + room) break;
		bool on = list[i] == sel;
		ui_rect(x, y, w[i], th, on ? 0xFF3D6FD9 : 0x30FFFFFF);
		int tx = x + pad;
		ui_icon(list[i] ? "usb" : "drive", tx, y + (th - isz) / 2, isz, on ? 0xFFFFFF : 0x8890A8);
		tx += isz + igap;
		tx += ui_text(UI_SMALL, tx, y + (th - ls) / 2, on ? 0xFFFFFF : 0xC8D0E0, name[i]) + gap;
		tx += ui_text(UI_SMALL, tx, y + (th - ls) / 2, scol[i], stt[i]);
		if (warn[i]) ui_icon(warn[i] == 1 ? "medal" : warn[i] == 2 ? "warn" : "question", tx + igap, y + (th - isz) / 2, isz,
		                     warn[i] == 1 ? 0xE8C870 : warn[i] == 2 ? 0xFF8A80 : 0x8890A8);   // the page's icons and colours
		if (g_cards[list[i]].job) {   // ripping: a thin bar along the bottom of the tab
			OnCard o(list[i]);
			uint32_t pos = g_readpos > g_done ? g_readpos : g_done;
			int bw = g_total ? (int)((uint64_t)w[i] * (pos > g_total ? g_total : pos) / g_total) : 0;
			if (g_pass2_total && (g_cards[list[i]].repairing || g_phase == 1 || g_phase == 2)) bw = (int)((uint64_t)w[i] * g_pass2_done / g_pass2_total);
			int bh = ui_line(UI_SMALL) / 5;   // subtle: about 6 px at 1080p
			ui_rect(x, y + th - bh, w[i], bh, 0x40FFFFFF);
			ui_rect(x, y + th - bh, bw, bh, 0xFFC8E05A);
		}
		x += w[i] + gap;
		last = i;
	}
	int asz = ls / 2;   // more tabs than fit: small arrows at the row's ends
	if (first > 0) ui_icon("left", L - asz - gap / 2, y + (th - asz) / 2, asz, 0xC8D0E0);
	if (last < nc - 1) ui_icon("right", x, y + (th - asz) / 2, asz, 0xC8D0E0);
	ui_rect(L, y + th + ls / 2, R - L, 2, 0x50FFFFFF);
	return th + ls;
}

// One drive's page under the tabs: title (Redump's name) with the Redump icon | drive column;
// the destination; ripping: a wide bar, percentage and time, what it does, the latest events;
// not ripping: what is known about the disc here, how the last rip went, the destination to pick.
static int draw_page_body(int c, int y)
{
	const int ytop = y;
	Card &k = g_cards[c];
	int lb = ui_line(UI_BODY), ls = ui_line(UI_SMALL), L = ui_w() / 8, R = ui_w() * 7 / 8;
	int st = k.state, known = k.job || st == DISC_READY, gap = ui_w() / 60;
	char idl[64] = "";
	if (known) snprintf(idl, sizeof(idl), "%s  ·  %u MB", k.serial, k.mb);
	int rw = drive_column(c, R, y, idl) + gap;
	int lw = R - L - rw;
	if (known) {
		const char *title = k.rd_name[0] ? k.rd_name : k.title;
		int rs = k.rd_state;
		if (k.job) { OnCard o(c); if (g_readerrs) rs = 0; }   // ripping: a sector it could not read turns the question into a warning
		const char *icon = rs == 1 ? "medal" : rs == 0 ? "warn" : "question";
		uint32_t icol = rs == 1 ? 0xE8C870 : rs == 0 ? 0xFF8A80 : 0x8890A8;
		int bsz = lb * 3 / 4, igap = ls / 3, room = lw - bsz - igap;
		int w = ui_text_width(UI_BODY, title);
		if (w > room) w = room;
		ui_text_fit(UI_BODY, L, y, 0xFFFFFF, title, room);
		ui_icon(icon, L + w + igap, y + (lb - bsz) / 2, bsz, icol);
	} else {
		const char *msg = st == DISC_READING ? T(c ? STR_SRC_SPINUP : STR_HOME_READING) : st == DISC_ERROR ? T(STR_HOME_NOTPSX) : c && k.usb_dead ? T(STR_USB_NORESP) : T(STR_HOME_INSERT);
		ui_text_fit(UI_BODY, L, y, 0xFFFFFF, msg, lw);
	}
	y += lb + ls / 2;
	// the destination: "Destination: Hard disk · PSXISO" with the free space on the right, under it the file name
	auto dest_block = [&](int yy, int arrows, uint32_t col) {
		char dp[120], m[200], gbs[48], fl[260], fn[260];
		unsigned gb = 0;
		dest_parts(k.dest[0] ? k.dest : HDD_DIR, dp, sizeof(dp), &gb);
		snprintf(m, sizeof(m), arrows ? "‹ %s%s ›" : "%s%s", arrows ? "" : T(STR_DEST_LABEL), dp);   // ←→ where this drive saves
		snprintf(gbs, sizeof(gbs), T(STR_DEST_FREE), gb);
		int gw = ui_text_width(UI_SMALL, gbs);   // the free space: small, on the right
		ui_text(UI_SMALL, R, yy + (lb - ls) / 2, 0x8890A8, gbs, UI_RIGHT);
		ui_text_fit(UI_BODY, L, yy, col, m, R - L - gw - ui_w() / 40);
		yy += lb;
		if (known && k.fname[0]) {
			snprintf(fl, sizeof(fl), "%s.bin", k.fname);
			// small, the label grey and the name light so it stands out
			int pw = ui_text_width(UI_SMALL, T(STR_FILE_LABEL));
			fit_middle(UI_SMALL, fl, fn, sizeof(fn), R - L - pw);
			ui_text(UI_SMALL, L, yy, 0x8890A8, T(STR_FILE_LABEL));
			ui_text(UI_SMALL, L + pw, yy, 0xE8ECF8, fn);
			yy += ls + ls / 3;
		}
		return yy;
	};
	if (k.job) {
		{ int under = ytop + lb + ls * 2 + ls / 2; if (y < under) y = under; }   // below the right column
		y = dest_block(y, 0, 0x8890A8) + ls;
		OnCard o(c);
		uint32_t pos = g_readpos > g_done ? g_readpos : g_done, tot = g_total ? g_total : 1;
		// per mille: under 10 % the page shows a decimal (6 of 1570 re-read is 0.4 %, not 0 %)
		uint32_t pm = (uint32_t)((uint64_t)pos * 1000 / tot);
		int inpass = g_pass2_total && (k.repairing || g_phase == 1 || g_phase == 2);   // the verify pass, the re-reads, a repair: their own progress
		if (inpass) pm = (uint32_t)((uint64_t)g_pass2_done * 1000 / g_pass2_total);
		if (g_phase == 3 && g_md5_total[c]) pm = (uint32_t)((uint64_t)g_md5_done[c] * 1000 / g_md5_total[c]);   // MD5s being recomputed
		if (pm > 1000) pm = 1000;
		uint32_t pct = pm / 10;
		int bh = ls * 2 / 3;
		ui_rect(L, y, R - L, bh, 0x50FFFFFF);
		ui_rect(L, y, (int)((uint64_t)(R - L) * pm / 1000), bh, 0xFFC8E05A);
		y += bh + ls / 2;
		char tl[96], pc[140];
		eta_text(k, tl, sizeof(tl));
		// percentage and time on the right of the phase line (the tab shows the percentage too)
		char pcs[24];
		if (pm < 100) snprintf(pcs, sizeof(pcs), T(STR_PCT_DEC), pm / 10, pm % 10);
		else snprintf(pcs, sizeof(pcs), "%u %%", pct);
		snprintf(pc, sizeof(pc), "%s  ·  %s", pcs, tl);
		int pcw = ui_text_width(UI_BODY, pc);
		ui_text(UI_BODY, R, y, 0xC8D0E0, pc, UI_RIGHT);
		// what it does: the phase, a re-read, the counts
		char ph[160];
		if (g_abort) snprintf(ph, sizeof(ph), "%s", T(STR_CANCELLING));
		else if (g_skip && (g_phase == 1 || g_phase == 2)) snprintf(ph, sizeof(ph), "%s", T(STR_SKIPPING));
		else if (g_phase == 4) snprintf(ph, sizeof(ph), T(STR_CHECKING), pos, g_total);
		else if (g_phase == 3 && g_md5_total[c]) snprintf(ph, sizeof(ph), "%s", T(STR_MD5_PROGRESS));   // the percentage is on the right
		else if (g_phase == 3) snprintf(ph, sizeof(ph), "%s", T(STR_FINISHING));
		else if (g_pass2_total) snprintf(ph, sizeof(ph), "%s", T(g_phase == 1 ? STR_VERIFY_PASS : STR_PASS2));   // the count goes on the line below
		else snprintf(ph, sizeof(ph), T(STR_PROGRESS), pos, g_total);
		for (size_t e = strlen(ph); e > 0 && (ph[e - 1] == ' ' || ph[e - 1] == ':'); e--) ph[e - 1] = 0;   // "(4 of 279):" alone
		ui_text_fit(UI_BODY, L, y, 0xFFFFFF, ph, R - L - pcw - ui_w() / 40);
		y += lb;
		{   // details in the small type: which sector of the pass, and the slow re-read going on
			char det[220] = "", rr[160];
			if (inpass && g_phase != 4) snprintf(det, sizeof(det), T(STR_PASS_COUNT), g_pass2_done < g_pass2_total ? g_pass2_done + 1 : g_pass2_total, g_pass2_total);
			if (!g_abort && g_rr_max) {
				snprintf(rr, sizeof(rr), T(STR_REREAD), g_rr_lba, g_rr_try, g_rr_max);
				const char *r0 = rr;
				if (!det[0]) while (*r0 == ' ' || *r0 == '\xc2' || *r0 == '\xb7') r0++;   // the re-read text starts with " · " when it follows another
				snprintf(det + strlen(det), sizeof(det) - strlen(det), "%s%s", det[0] && r0 == rr && rr[0] != ' ' ? "  ·  " : "", r0);
			}
			if (det[0]) ui_text_fit(UI_SMALL, L, y, g_rr_max ? 0xF0C060 : 0xC8D0E0, det, R - L);
		}
		y += ls + ls / 3;
		if (g_phase != 4) {   // the check reads the file, not the disc: no read counts then
			char cnt[160] = "";
			snprintf(cnt, sizeof(cnt), T(STR_READ_STATS), "", g_readerrs, g_recovered);
			ui_text_fit(UI_SMALL, L, y, g_readerrs ? 0xF0C060 : 0xC8D0E0, cnt, R - L);
		}
		y += ls * 3 / 2;
		// the events, newest first; a longer list makes the page scroll (L2/R2)
		if (g_nev > 0) {
			y += section_head(L, R, y, T(STR_EVENTS));
			int rh = ls + ls / 4;
			for (int i = g_nev - 1; i >= 0; i--) { ui_text_fit(UI_SMALL, L, y, g_ev[i].col, g_ev[i].text, R - L); y += rh; }
		}
		return y;
	}
	// not ripping: the disc here, then where it goes; headed groups with room between them, below
	// the right column (drive, model, disc)
	{ int under = ytop + lb + ls * 2 + ls / 2; if (y < under) y = under; }
	y += section_head(L, R, y, T(STR_SEC_DISC));
	const int ydest = y + lb + (ls + ls / 3) * 4 + lb;   // Mål always here: the page does not jump between tabs
	if (st == DISC_READY && k.prev_rip) {   // already ripped here: how it stands, in words
		char as[300], line[400];
		const char *en = strrchr(k.exist, '/');
		en = en ? en + 1 : k.exist;
		const char *p1 = T(STR_ALREADY_HERE);
		if (strcmp(en, k.fname)) { snprintf(as, sizeof(as), T(STR_ALREADY_AS), en); p1 = as; }
		const char *p2 = k.rd_state == 1 ? T(STR_HERE_VERIFIED) : k.rd_state == 0 ? T(STR_HERE_NOMATCH) : T(STR_HERE_UNCHECKED);
		uint32_t c2 = k.rd_state == 0 ? 0xFF8A80 : 0x8FA8D8;
		snprintf(line, sizeof(line), "%s", p1);
		int w1 = ui_text_width(UI_BODY, line);
		ui_text_fit(UI_BODY, L, y, 0x8FA8D8, line, R - L);
		if (w1 < R - L) ui_text_fit(UI_BODY, L + w1, y, c2, p2, R - L - w1);
		y += lb + ls / 3;
		if (k.repair_n) {   // details in the small type, like "Senaste rippningen"
			char e[120];
			snprintf(e, sizeof(e), T(STR_KNOWN_ERRORS), k.repair_n);
			ui_text_fit(UI_SMALL, L, y, 0xF0C060, e, R - L);
			y += ls + ls / 3;
			char tip[200];   // what square does about them, right under them
			snprintf(tip, sizeof(tip), T(STR_REPAIR_TIP), k.repair_n);
			ui_text_fit(UI_SMALL, L, y, 0x8890A8, tip, R - L);
			y += ls + ls / 3;
		}
		if (k.rd_state < 0 && !k.repair_n) {   // what the check does (with known errors square repairs instead)
			ui_text_fit(UI_SMALL, L, y, 0x8890A8, T(STR_CHECK_TIP), R - L);
			y += ls + ls / 3;
		}
	} else if (st == DISC_READY) {
		ui_text_fit(UI_BODY, L, y, 0x8FA8D8, T(STR_NOT_RIPPED_HERE), R - L);
		y += lb + ls / 3;
	} else if (c && k.usb_dead) {   // what to do about a drive that has stopped answering
		ui_text_fit(UI_SMALL, L, y, 0xF0C060, T(STR_USB_NORESP_TIP), R - L);
		y += ls + ls / 3;
	}
	// the last rip on this drive: with no disc in it, or with that same disc; another disc's page
	// does not show it (it would read as if it were about the disc in the drive)
	int same = st == DISC_READY && !strcmp(k.res_key, k.serial[0] ? k.serial : k.title);
	if (k.res[0] && (st == DISC_NONE || same)) {
		char last[420];   // which disc it was too: after an eject the page shows none
		snprintf(last, sizeof(last), T(STR_LAST_RIP), k.res_disc, k.res);
		ui_text_fit(UI_SMALL, L, y, k.res_col, last, R - L);
		y += ls + ls / 3;
		if (k.res_rd >= -1) {   // and how it compared with Redump, with the tab's icon
			const char *icon = k.res_rd == 1 ? "medal" : k.res_rd == 0 ? "warn" : "question";
			uint32_t col = k.res_rd == 1 ? 0x7CE0A0 : k.res_rd == 0 ? 0xFF8A80 : 0x8890A8;
			uint32_t icol = k.res_rd == 1 ? 0xE8C870 : col;
			int isz = ls * 3 / 4, igap = ls / 3;
			ui_icon(icon, L, y + (ls - isz) / 2, isz, icol);
			ui_text_fit(UI_SMALL, L + isz + igap, y, col, T(k.res_rd == 1 ? STR_LAST_RD_OK : k.res_rd == 0 ? STR_LAST_RD_NO : STR_LAST_RD_NONE), R - L - isz - igap);
			y += ls + ls / 3;
		}
	}
	if (st == DISC_READY && k.space_short && !k.prev_rip) {
		char sl[200];
		snprintf(sl, sizeof(sl), T(STR_NO_SPACE), k.mb, (unsigned)(k.mb + 2 - k.space_short));
		ui_text_fit(UI_SMALL, L, y, 0xFF8A80, sl, R - L);
		y += ls + ls / 3;
	}
	y = y + lb > ydest ? y + lb : ydest;
	y += section_head(L, R, y, T(STR_SEC_DEST));
	y = dest_block(y, 1, 0xC8D0E0) + ls;
	return y;
}

// The page between the tabs and the general row. Taller than that, it scrolls with L2/R2:
// what is outside is not drawn, and arrows at the right edge, at the top and the bottom, say there
// is more, as in the settings.
static void draw_page(int c, int y)
{
	Card &k = g_cards[c];
	int ls = ui_line(UI_SMALL), bottom = hint_y() - ui_line(UI_BODY) * 3 / 2 - ls / 4;
	if (k.pg_scroll > k.pg_max) k.pg_scroll = k.pg_max;
	if (k.pg_scroll < 0) k.pg_scroll = 0;
	ui_clip(y, bottom);
	int end = draw_page_body(c, y - k.pg_scroll) + k.pg_scroll;
	ui_clip(0, 0);
	k.pg_max = end - ls > bottom ? end - ls - bottom : 0;   // the last line's spacing need not show
	if (k.pg_scroll > k.pg_max) k.pg_scroll = k.pg_max;
	int asz = ls * 2 / 3, ax = ui_w() * 7 / 8 + ui_w() / 40;
	if (k.pg_scroll > 0) ui_icon("up", ax, y, asz, 0xC8D0E0);
	if (k.pg_scroll < k.pg_max) ui_icon("down", ax, bottom - asz, asz, 0xC8D0E0);
}

static void rip_job_start(int c, AuditSrc src, int fresh);
static void check_job_start(int c, AuditSrc src);
static void repair_job_start(int c, AuditSrc src);


// One frame of the home screen: tabs, the selected drive's page, the general row and the buttons
// of the selected drive. (The UI tests on the host draw it for their scenarios.)
static void home_draw(const int *list, int nc, int sel)
{
	Card &f = g_cards[sel];
	int st = f.state;
	draw_backdrop();
	int py = ui_h() / 10 + ui_line(UI_TITLE) + ui_line(UI_BODY);
	if (nc > 1) py += draw_tabs(list, nc, sel, py);   // one drive: no tabs, just its page
	else py += ui_line(UI_BODY);
	draw_page(sel, py);
	// above the bottom row, the general things: webhooks on the left, settings and exit on the right
	int gy = hint_y() - ui_line(UI_BODY) * 3 / 2;
	if (g_webhook_on && g_webhook[0]) {   // just a hint that webhooks are on; details in the settings
		int ne = 0;
		for (int i = 0; i < WH_COUNT; i++) ne += (g_webhook_mask >> i) & 1;
		char wl[64];
		snprintf(wl, sizeof(wl), T(STR_HOME_WEBHOOK_N), ne);
		ui_text(UI_SMALL, ui_w() / 8, gy + (ui_line(UI_BODY) - ui_line(UI_SMALL)) / 2, 0x6870A0, wl);
	}
	{
		int anyjob = 0;
		for (int i = 0; i < nc; i++) anyjob |= g_cards[list[i]].job;
		int w1 = ui_hint_width("start", T(STR_HINT_SETTINGS)), w2 = anyjob ? 0 : ui_hint_width("circle", T(STR_HINT_EXIT)), gap = ui_w() / 40;
		int x = ui_w() * 7 / 8 - w1 - (w2 ? gap + w2 : 0);
		x += ui_hint(x, gy, "start", T(STR_HINT_SETTINGS)) + gap;
		if (w2) ui_hint(x, gy, "circle", T(STR_HINT_EXIT));   // O on a ripping card cancels its rip instead
	}
	// the bottom row: what the buttons do on the focused card
	int rereads = 0;   // the rip is in its re-reads: square leaves them
	if (f.job) { OnCard o(sel); rereads = (g_phase == 1 || g_phase == 2) && !g_skip && !g_abort; }
	int evs = f.pg_max > 0;   // the page scrolls with L2/R2
	if (f.job && rereads) {
		const char *b[] = {"square", "l2r2", "circle"};
		const char *l[] = {T(STR_HINT_SKIP), T(STR_HINT_EVENTS), T(STR_HINT_CANCEL_RIP)};
		const char *b2[] = {"square", "circle"};
		const char *l2[] = {T(STR_HINT_SKIP), T(STR_HINT_CANCEL_RIP)};
		if (evs) hint_bar(b, l, 3); else hint_bar(b2, l2, 2);
	} else if (f.job) {
		int checking;
		{ OnCard o(sel); checking = g_phase == 4; }
		const char *b[] = {"l2r2", "circle"};
		const char *l[] = {T(STR_HINT_EVENTS), T(checking ? STR_HINT_CANCEL_CHECK : STR_HINT_CANCEL_RIP)};
		if (evs) hint_bar(b, l, 2); else hint_bar(b + 1, l + 1, 1);
	} else if (st == DISC_READY && f.prev_rip && f.repair_n) {   // ripped here with known errors: square repairs
		const char *b[] = {"cross", "square", "triangle", "select"};
		const char *l[] = {T(STR_HINT_NEWRIP), T(STR_HINT_REPAIR), T(STR_HINT_EJECT), T(STR_HINT_DRIVE)};
		hint_bar_pg(b, l, 4, evs);
	} else if (st == DISC_READY && f.prev_rip && f.rd_state < 0) {   // ripped here, not checked: square checks the file
		const char *b[] = {"cross", "square", "triangle", "select"};
		const char *l[] = {T(STR_HINT_NEWRIP), T(STR_HINT_CHECK), T(STR_HINT_EJECT), T(STR_HINT_DRIVE)};
		hint_bar_pg(b, l, 4, evs);
	} else if (st == DISC_READY) {
		const char *b[] = {"cross", "triangle", "select"};
		const char *l[] = {T(f.prev_rip ? STR_HINT_NEWRIP : STR_HINT_RIP), T(STR_HINT_EJECT), T(STR_HINT_DRIVE)};
		hint_bar_pg(b, l, 3, evs);
	} else if (st == DISC_ERROR) {
		const char *b[] = {"triangle", "select"};
		const char *l[] = {T(STR_HINT_EJECT), T(STR_HINT_DRIVE)};
		hint_bar_pg(b, l, 2, evs);
	} else if (sel > 0) {   // a USB drive without a disc it knows: it may still hold one
		const char *b[] = {"triangle", "select"};
		const char *l[] = {T(STR_HINT_EJECT), T(STR_HINT_DRIVE)};
		hint_bar_pg(b, l, 2, evs);
	} else {
		const char *b[] = {"select"};
		const char *l[] = {T(STR_HINT_DRIVE)};
		hint_bar_pg(b, l, 1, evs);
	}
}

// Home screen: a card per drive. Up/down picks a card; left/right its destination; X, square and
// triangle act on it. Returns 1 = rip the disc in g_card's drive, 2 = rip a freshly inserted disc,
// 3 = repair, 0 = exit.
static volatile int g_insert_pending = 0;   // a disc was inserted on the result screen (auto-rip off)
static int home_screen()
{
	if (g_insert_pending) g_cards[g_card].inserted = 1;   // treat it as freshly inserted: identify (and report) it
	g_insert_pending = 0;
	g_redraw = 2;
	pad_pressed();   // swallow buttons still held from a previous screen
	static int sel = 0;
	int last_rd = -1, last_sig = -1;
	while (!exit_requested) {
		int list[NCARDS], nc = card_list(list);
		int si = 0;
		for (int i = 0; i < nc; i++) if (list[i] == sel) si = i;
		sel = list[si];
		if (g_rd_seq != last_rd) { last_rd = g_rd_seq; g_redraw = 2; }
		// disc events and identification, one drive at a time
		if (!g_ident_busy && !g_repairing) {
			cards_take_pending();   // an undo changed a busy card's destination or drive settings
			for (int i = 0; i < nc; i++) {
				Card &k = g_cards[list[i]];
				if (k.job) continue;   // ripping: the rip has the drive
				if (k.inserted) { k.inserted = 0; k.tried = 0; k.fresh = 1; k.state = DISC_NONE; k.base[0] = 0; k.rd_done[0] = 0; k.rd_name[0] = 0; k.rd_state = -1; }   // the last rip's line stays (it names its disc)
				if (!k.present) { if (k.state != DISC_NONE) k.state = DISC_NONE; k.tried = 0; k.fresh = 0; continue; }
				if (!k.tried) { k.tried = 1; select_card(list[i]); identify_start(k.fresh); break; }
				if (k.recheck) { k.recheck = 0; if (k.state == DISC_READY) { select_card(list[i]); prev_rip_check(); } g_redraw = 2; }
			}
		}
		// app_started: once every drive there at start has been looked at (USB drives attach and spin
		// up first), with all the discs; at most two minutes after the start
		static int started_sent = 0;
		static uint64_t t_start = 0;
		if (!t_start) t_start = now_ms();
		if (!started_sent && !g_ident_busy && now_ms() - t_start > 5000) {
			int pending = 0;
			for (int i = 0; i < nc; i++) if (g_cards[list[i]].present && !g_cards[list[i]].tried) pending = 1;
			if (!pending || now_ms() - t_start > 120000) {
				started_sent = 1;
				int c0 = 0;   // title/serial of the first disc, for receivers that only read those
				for (int i = nc - 1; i >= 0; i--) if (g_cards[list[i]].state == DISC_READY) c0 = list[i];
				select_card(c0);
				if (g_cards[c0].state == DISC_READY) {
					snprintf(g_state_title, sizeof(g_state_title), "%s", g_cards[c0].title);
					snprintf(g_state_serial, sizeof(g_state_serial), "%s", g_cards[c0].serial);
				} else { g_state_title[0] = 0; g_state_serial[0] = 0; }
				fire_event("app_started");
			}
		}
		int sig = nc, jobs = 0;
		for (int i = 0; i < nc; i++) { sig = sig * 7 + g_cards[list[i]].state * 2 + g_cards[list[i]].prev_rip + g_cards[list[i]].job * 3; jobs += g_cards[list[i]].job; }
		if (sig != last_sig) { last_sig = sig; g_redraw = 2; }
		static uint64_t last_prog = 0;   // ripping cards: their progress twice a second
		if (jobs && now_ms() - last_prog >= 500) { last_prog = now_ms(); g_redraw = 2; }
		// automatic start for a disc just put in: a rip when it is not ripped to this destination yet
		// (never overwrite one), a repair when its earlier rip here has known errors (re-reads only
		// those sectors, so it never makes the rip worse); else nothing, the tab says how it stands
		if (!g_ident_busy)
			for (int i = 0; i < nc; i++) {
				Card &k = g_cards[list[i]];
				if (k.job || !std_or(k.autostart, g_std.autostart) || k.state != DISC_READY || !k.fresh) continue;
				k.fresh = 0;
				select_card(list[i]);
				if (!k.prev_rip) { rip_job_start(list[i], AU_APP, 1); continue; }
				if (k.repair_n) {
					prev_rip_check();   // the log as it is now
					if (g_prev_rip && g_repair_n) { logf("auto repair: %d sector(s)", g_repair_n); repair_job_start(list[i], AU_APP); }
				}
			}
		{   // Redump was switched (the settings, an undo, MCP): look the disc up again
			static int seen_rd = -1;
			if (seen_rd < 0) seen_rd = g_redump;
			if (g_redump != seen_rd && !g_ident_busy) {
				seen_rd = g_redump;
				if (g_disc_state == DISC_READY) lookup_start();
			}
		}
		for (int i = 0; i < nc; i++) eta_tick(list[i]);   // every tab's time left, also those not shown
		Card &f = g_cards[sel];
		int st = f.state;
		g_backdrop_shown = 0;   // a dialog opened later must redraw its backdrop
		if (g_redraw > 0) {
			g_redraw = 0;
			g_drawn = 2;
			home_draw(list, nc, sel);
		}
		{ UiLock lock; g_ui.tab = sel; }
		ui_at(SCR_HOME, -1, sel);
		flip_frame();
		int p = pad_pressed();
		if ((p & B_UP) && nc > 1) { sel = list[(si + nc - 1) % nc]; g_redraw = 2; }   // also up/down change the tab
		if ((p & B_DOWN) && nc > 1) { sel = list[(si + 1) % nc]; g_redraw = 2; }
		if ((p & B_R1) && nc > 1) { sel = list[(si + 1) % nc]; g_redraw = 2; }   // the next / previous tab
		if ((p & B_L1) && nc > 1) { sel = list[(si + nc - 1) % nc]; g_redraw = 2; }
		if (f.pg_max > 0 && (p & (B_L2 | B_R2))) {   // a page taller than the screen: L2/R2 scroll it (up / down)
			int step = (ui_line(UI_SMALL) * 5 / 4) * 4;   // four event rows
			if (p & B_L2) f.pg_scroll -= step;
			if (p & B_R2) f.pg_scroll += step;
			g_redraw = 2;
			p &= ~(B_L2 | B_R2);
		}
		if ((p & B_R2) && nc > 1) { sel = list[nc - 1]; g_redraw = 2; }   // the last / first tab
		if ((p & B_L2) && nc > 1) { sel = list[0]; g_redraw = 2; }
		if (f.job) {   // a ripping card: only cancel, and the general settings (they apply to the next rips)
			if (p & B_CIRCLE) {
				char m[300];
				snprintf(m, sizeof(m), T(STR_CANCEL_RIP_Q), f.title);
				if (own_dialog(m, 1) == 1 && f.job) { OnCard o(sel); g_abort = 1; }
				pad_pressed();
				g_redraw = 2;
				continue;
			}
			if (p & B_SQUARE) {   // leave the re-reads: the rip finishes with what it has, the log lists the rest
				OnCard o(sel);
				if (g_phase == 1 || g_phase == 2) { g_skip = 1; logf("re-reads skipped (square)"); g_redraw = 2; }
			}
			p &= B_START;   // its own settings were taken when the rip started: not while it runs
		}
		if (p & (B_LEFT | B_RIGHT)) {   // where this drive saves
			char od[64];
			snprintf(od, sizeof(od), "%s", f.dest[0] ? f.dest : HDD_DIR);
			next_target(od, (p & B_RIGHT) ? 1 : -1);
			card_set_outdir(sel, od);
			g_redraw = 2;
		}
		if ((p & (B_CROSS | B_SQUARE | B_TRIANGLE)) && (st == DISC_READY || st == DISC_ERROR)) {
			while (g_ident_busy && g_ident_card == sel) usleep(10000);
			select_card(sel);
		}
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
			int fr = f.fresh;
			f.fresh = 0;
			rip_job_start(sel, g_input_src, fr);   // rip, also over an existing rip (X is always a rip); the card shows it
			g_redraw = 2;
			continue;
		}
		if ((p & B_SQUARE) && st == DISC_READY && f.prev_rip && !f.repair_n && f.rd_state < 0 && !f.job) {   // check the file
			check_job_start(sel, g_input_src);
			g_redraw = 2;
			continue;
		}
		if ((p & B_SQUARE) && st == DISC_READY && f.prev_rip && f.repair_n) {   // repair the sectors the log lists
			int shown_prev = g_prev_rip;
			prev_rip_check();
			if (g_prev_rip != shown_prev || !g_repair_n) { g_redraw = 2; continue; }
			repair_job_start(sel, g_input_src);   // on its tab, like a rip; the other drives go on
			g_redraw = 2;
			continue;
		}
		if ((p & B_TRIANGLE) && !f.job && (st == DISC_READY || st == DISC_ERROR || sel > 0)) {   // a USB drive: also when it has not seen a disc
			while (g_ident_busy && g_ident_card == sel) usleep(10000);
			logf("eject pressed on tab %d (%s)", sel, sel ? "USB" : "internal");
			select_card(sel);
			eject_disc();   // the last rip's line stays: with auto-eject it is the only trace of how it went
			g_redraw = 2;
		}
		if (p & B_SELECT) {   // this drive's settings
			drive_settings_screen(sel);
			pad_pressed();
			g_redraw = 2;
			continue;
		}
		if (p & B_START) {
			settings_screen();
			for (int i = 0; i < NCARDS; i++) g_cards[i].recheck = 1;   // destinations may have been reset
			g_redraw = 2;
		}
		if ((p & B_CIRCLE) && !jobs) return 0;   // exit only when nothing rips (a ripping tab's O cancels its rip)
	}
	return 0;
}

// ---------------------------------------------------------------- one disc
// The same summary for the own result screen: coloured lines
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
	if (!g_mounttest && g_use == SRC_BD) webman_unmount();

	int ret;
	if (g_use == SRC_USB) {   // no lv2 device and no lv1 patch: our own driver talks to the drive
		g_fd = -1;
		if (!usbodd_attached(g_usb)) { *err = STR_OPEN_FAILED; *code = -1; return -2; }
		usbodd_unlock(g_usb);          // harmless when it is already unlocked; lets the slot take a disc
		ret = usbodd_ready(g_usb, 90000);
		logf("usb drive ready -> %d", ret);
		if (ret) { *err = STR_TOC_FAILED; *code = ret; return -2; }
	} else {
		ret = sys_storage_open(BD_DEVICE, &g_fd);
		logf("storage_open -> 0x%x fd=%d", ret, g_fd);
		if (ret) { *err = STR_OPEN_FAILED; *code = ret; return -1; }
	}

	ret = read_toc();
	if (ret) { sys_storage_close(g_fd); *err = STR_TOC_FAILED; *code = ret; return -2; }   // -2: no (readable) disc

	ret = probe_read();
	if (ret && g_use == SRC_BD && lv1_storage_patch() == 0) {
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
	const char *title = g_serial[0] ? title_override(g_serial, g_publisher) : NULL;
	if (!title && g_serial[0]) title = lookup_title(g_serial);
	snprintf(name, sizeof(g_name), "%s", title ? title : (g_volid[0] ? g_volid : "PSX DISC"));
	logf("title=%s", title ? title : "(not in database)");
	sanitize(name);
	if (g_serial[0]) {
		char fn[200];
		name_from(g_name_pat, g_cards[cur_card()].rd_name, name, g_serial, fn, sizeof(fn));
		snprintf(g_base, sizeof(g_base), "%s/%s", OUT_DIR, fn);
	}
	else
		snprintf(g_base, sizeof(g_base), "%s/%s %lu", OUT_DIR, name, (unsigned long)time(NULL));

	g_is_pal = g_serial[0] && g_serial[2] == 'E';   // SLES/SCES/SLED/SCED
	return 0;
}

// ---------------------------------------------------------------- remote view and webhooks

static const char *rd_code(int r)
{
	switch (r) {
	case 1: return "verified";
	case 0: return "no_match";
	case -1: return "unchecked";
	case -2: return "not_found";
	default: return "off";
	}
}
static const char *rd_result_str() { return rd_code(g_rd_result); }

// ---------------------------------------------------------------- jobs in the audit log

// The Redump line of a rip's log ("verified Alundra (Europe)"), "unchecked" when there is none.
static void redump_line_of(const char *base, char *out, size_t n)
{
	snprintf(out, n, "unchecked");
	if (!base[0]) return;
	char p[320], b[4096];
	snprintf(p, sizeof(p), "%s.log", base);
	VFile *f = vf_open(p, "rb");
	if (!f) return;
	int got = (int)vf_read(f, b, sizeof(b) - 1);
	vf_close(f);
	b[got > 0 ? got : 0] = 0;
	const char *rd = strstr(b, "redump=");
	if (!rd) return;
	rd += 7;
	int len = (int)strcspn(rd, "\r\n");
	if (len) snprintf(out, n, "%.*s", len, rd);
}

// A job on card c begins: its row in the audit log with what started it, the disc key and the
// rip's name (a rip: the file it makes; a repair or check: the rip it works on).
static void audit_card_begin(int c, AuditSrc src, const char *what)
{
	Card &k = g_cards[c];
	OnCard o(c);
	disc_key(k.audit_disc, sizeof(k.audit_disc));
	const char *slash = strrchr(k.exist, '/');
	const char *name = !strcmp(what, "rip") || !k.exist[0] ? k.fname : slash ? slash + 1 : k.exist;
	redump_line_of(k.exist, k.rd_before, sizeof(k.rd_before));
	k.job_src = src;
	k.audit_job = audit_job_begin(src, k.audit_disc, name, what);
}

// Every way out of a job ends it here, or the audit keeps it running and refuses undo on its disc.
// A finished rip names the image it made: older rows on that file are then no longer undone.
static void audit_card_end(int c, const char *result, const char *image = NULL)
{
	Card &k = g_cards[c];
	if (k.audit_job) audit_job_end(k.audit_job, result, image);
	k.audit_job = 0;
}

// The job's Redump result (g_rd_result) against the rip's line before it.
static void audit_card_redump(int c)
{
	Card &k = g_cards[c];
	if (!k.audit_job) return;
	OnCard o(c);
	char after[RD_NAME + 16];
	snprintf(after, sizeof(after), "%s%s%s", rd_result_str(), g_rd_result_name[0] ? " " : "", g_rd_result_name);
	audit_note(k.audit_job, AU_RIPS, k.audit_disc, "redump", "", k.rd_before, after);
}

// How a rip or a repair ended (card context): ok is what rip_disc / repair_disc returned.
static const char *rip_result(int ok)
{
	return ok ? "finished" : g_disc_lost[cur_card()] ? "disc_lost" : g_abort ? "cancelled" : "failed";
}
static const char *repair_result(int ok)
{
	return g_disc_lost[cur_card()] ? "disc_lost" : !ok ? "failed" : g_abort ? "cancelled" : "finished";
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

static int g_state_busy() { return any_ripping() || g_ident_busy || g_repairing || lv1_patched; }

// A drive as the tools name it: "internal", or "usb" with its port as the tabs show it ("usb0");
// "usb" alone for a USB drive whose port is not known (no tool can name it).
static void drive_id(int c, char *out, size_t n)
{
	if (!c) snprintf(out, n, "internal");
	else if (usbodd_port(c - 1) >= 0) snprintf(out, n, "usb%d", usbodd_port(c - 1));
	else snprintf(out, n, "usb");
}
// The card of a drive id, -1 when no such drive is there.
static int drive_card(const char *id)
{
	int list[NCARDS], nc = card_list(list);
	char d[16];
	for (int i = 0; i < nc; i++) { drive_id(list[i], d, sizeof(d)); if (!strcmp(d, id) && strcmp(d, "usb")) return list[i]; }
	return -1;
}

static void drives_json(char *js, int n);
// /status.json: the app, and every drive (internal first, then the USB drives by port) with its
// disc, its job and its last rip. busy at the top: a drive is in use or the lv1 patch is on, never
// stop or reinstall the app then (tools/deploy.sh reads it).
static int status_json(char *js, int n)
{
	int list[NCARDS], nc = card_list(list), anyjob = 0;
	for (int i = 0; i < nc; i++) anyjob |= g_cards[list[i]].job;
	snprintf(js, n, "{\"app\":\"PSX RiPPER\",\"version\":\"%s\",\"busy\":%s,\"state\":\"%s\",",
	         APP_VERSION, g_state_busy() ? "true" : "false", anyjob ? "ripping" : "idle");
	drives_json(js, n);
	size_t o = strlen(js);
	snprintf(js + o, n - o, "}");
	return (int)strlen(js);
}

// "drives":[...] appended to js: every drive (internal first, then the USB drives by port) with its
// disc, its job and its last rip (status.json and MCP).
static void drives_json(char *js, int n)
{
	int list[NCARDS], nc = card_list(list);
	auto put = [&](const char *fmt, ...) {
		size_t o = strlen(js);
		va_list ap; va_start(ap, fmt); vsnprintf(js + o, n - o, fmt, ap); va_end(ap);
	};
	auto str = [&](const char *key, const char *v) { put(",\"%s\":", key); json_str(js, n, v); };
	auto rd = [](int st) { return st == 1 ? "verified" : st == 0 ? "no_match" : "unchecked"; };
	put("\"drives\":[");
	for (int i = 0; i < nc; i++) {
		int c = list[i];
		Card &k = g_cards[c];
		const char *state = k.job ? (k.repairing ? "repairing" : "ripping") : k.state == DISC_READY ? "ready" : k.state == DISC_READING ? "reading" :
		                    k.state == DISC_ERROR ? "not_psx" : c && k.usb_dead ? "no_answer" : "empty";
		char id[16];
		drive_id(c, id, sizeof(id));
		put("%s{\"drive\":\"%s\",\"id\":\"%s\"", i ? "," : "", c ? "usb" : "internal", id);   // internal, or usb with its port: as the tabs
		if (c) put(",\"port\":%d", usbodd_port(c - 1));
		str("model", c ? usbodd_name(c - 1) : "PS3 Blu-ray drive");
		put(",\"state\":\"%s\"", state);
		int known = k.job || k.state == DISC_READY;
		if (known) {
			str("title", k.title);
			str("serial", k.serial);
			put(",\"size_mb\":%u", k.mb);
			str("redump_name", k.rd_name);
			put(",\"redump\":\"%s\",\"already_ripped\":%s,\"known_errors\":%d", rd(k.rd_state), k.prev_rip ? "true" : "false", k.repair_n);
			str("destination", k.dest[0] ? k.dest : HDD_DIR);
			str("file", k.fname);
		}
		if (k.job) {   // what the job does now
			OnCard o(c);
			uint32_t pos = g_readpos > g_done ? g_readpos : g_done;
			int inpass = g_pass2_total && (k.repairing || g_phase == 1 || g_phase == 2);
			const char *phase = k.repairing ? "repairing" : g_phase == 1 ? "verifying" : g_phase == 2 ? "rereading" : g_phase == 3 ? "finishing" : g_phase == 4 ? "checking" : "reading";
			unsigned pct = inpass ? (unsigned)((uint64_t)g_pass2_done * 100 / g_pass2_total) : g_total ? (unsigned)((uint64_t)(pos > g_total ? g_total : pos) * 100 / g_total) : 0;
			put(",\"job\":{\"phase\":\"%s\",\"percent\":%u,\"sectors_done\":%u,\"sectors_total\":%u,\"pass_done\":%u,\"pass_total\":%u,\"unreadable\":%u,\"fixed\":%u,\"bad\":%u}",
			    phase, pct, pos, g_total, g_pass2_done, g_pass2_total, g_readerrs, g_recovered, g_badsect);
		}
		if (k.res[0]) {   // the last rip, repair or check on this drive (stays after the eject)
			put(",\"last\":{\"result\":\"%s\"", k.res_failed == 2 ? "cancelled" : k.res_failed ? "failed" : "done");
			str("disc", k.res_disc);
			str("summary", k.res);
			if (k.res_rd >= -1) put(",\"redump\":\"%s\"", rd(k.res_rd));
			put(",\"needs_repair\":%s}", k.res_bad ? "true" : "false");
		}
		put("}");
	}
	put("]");
}


static void set_webhook(const char *url)
{
	CfgLock lock;
	snprintf(g_webhook, sizeof(g_webhook), "%s", url);
	cfg_save();
	char host[160];
	webhook_host(g_webhook, host, sizeof(host));
	logf("webhook address set (%s)", g_webhook[0] ? host : "none");
	g_redraw = 2;
}

// ---------------------------------------------------------------- MCP (mcp.h): the app's side
// The hooks run on the web server's thread, one request at a time. The reading tools only read;
// set_setting changes a setting as the screens do, under the config lock. Every answer is valid
// UTF-8 and uses at most half its buffer: mcp_handle escapes it into a buffer of the same size.

// A JSON text being written into a bounded buffer; `over` once something did not fit.
struct Js { char *p; int n, o; bool over; };
static Js js_begin(char *out, int n)
{
	int lim = (n - 200) / 2;
	if (lim < 1) lim = 1;
	out[0] = 0;
	return Js{out, lim, 0, false};
}
static void js_put(Js &j, const char *fmt, ...)
{
	if (j.over) return;
	va_list ap;
	va_start(ap, fmt);
	int k = vsnprintf(j.p + j.o, j.n - j.o, fmt, ap);
	va_end(ap);
	if (k < 0 || k >= j.n - j.o) { j.over = true; j.p[j.o] = 0; return; }
	j.o += k;
}
// The length of the UTF-8 character at p, 0 when the bytes there are not one (overlong, a
// surrogate, cut short, ...).
static int utf8_seq(const unsigned char *p)
{
	unsigned c = p[0], cp, min;
	int n;
	if (c < 0x80) return 1;
	if (c >= 0xC2 && c <= 0xDF) { n = 2; cp = c & 0x1F; min = 0x80; }
	else if ((c & 0xF0) == 0xE0) { n = 3; cp = c & 0x0F; min = 0x800; }
	else if (c >= 0xF0 && c <= 0xF4) { n = 4; cp = c & 0x07; min = 0x10000; }
	else return 0;
	for (int i = 1; i < n; i++) {   // a NUL ends it here: never read past the string
		if ((p[i] & 0xC0) != 0x80) return 0;
		cp = cp << 6 | (p[i] & 0x3F);
	}
	return cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF) ? 0 : n;
}
// Bytes that are not UTF-8 (a disc's own text, a log line) become '?', in place.
static void utf8_fix(char *s)
{
	for (unsigned char *p = (unsigned char *)s; *p; ) {
		int k = utf8_seq(p);
		if (k) p += k; else *p++ = '?';
	}
}
// s as a JSON string: quoted and escaped, bytes that are not UTF-8 as '?'.
static void js_str(Js &j, const char *s)
{
	if (j.over) return;
	int o = j.o;
	auto room = [&](int k) { if (o + k + 2 > j.n) { j.over = true; j.p[j.o] = 0; return false; } return true; };
	if (!room(1)) return;
	j.p[o++] = '"';
	for (const unsigned char *p = (const unsigned char *)s; *p; ) {
		int k = utf8_seq(p);
		if (!k) { if (!room(1)) return; j.p[o++] = '?'; p++; continue; }
		if (*p == '"' || *p == '\\') { if (!room(2)) return; j.p[o++] = '\\'; j.p[o++] = *p++; continue; }
		if (*p < 0x20) { if (!room(6)) return; o += snprintf(j.p + o, 7, "\\u%04x", *p++); continue; }
		if (!room(k)) return;
		while (k--) j.p[o++] = *p++;
	}
	j.p[o++] = '"';
	j.p[o] = 0;
	j.o = o;
}
// The drives (status.json's) appended at the end.
static void js_drives(Js &j)
{
	if (j.over) return;
	drives_json(j.p, j.n);
	j.o = (int)strlen(j.p);
	if (j.o >= j.n - 2) j.over = true;   // cut short
}
static int js_end(Js &j, char *out, int n)
{
	if (j.over) { snprintf(out, n, "The answer is too large."); return -1; }
	utf8_fix(out);   // the drives' texts come from the discs
	return 0;
}

static int mcp_na(char *out, int n) { snprintf(out, n, "not available yet"); return -1; }
static int mcp_na_ui(const char *, const char *, char *out, int n) { return mcp_na(out, n); }

// get_state: the screen, its row, the tab, an open dialog or keyboard, the hint bar, the drives.
static int mcp_state(char *out, int n)
{
	Js j = js_begin(out, n);
	UiState u;   // one picture of the screen
	ui_snapshot(u);
	int scr = u.scr, row = u.row, card = u.card, dlg = u.dlg_open;
	if (scr < 0 || scr >= (int)(sizeof(SCR_NAMES) / sizeof(SCR_NAMES[0]))) scr = SCR_HOME;
	char id[16];
	js_put(j, "{\"screen\":\"%s\"", dlg ? "dialog" : SCR_NAMES[scr]);
	if (row >= 0) js_put(j, ",\"row\":%d", row); else js_put(j, ",\"row\":null");
	drive_id(u.tab, id, sizeof(id));
	js_put(j, ",\"tab\":\"%s\"", id);
	if (scr == SCR_DRIVE_SETTINGS && card >= 0) { drive_id(card, id, sizeof(id)); js_put(j, ",\"drive\":\"%s\"", id); }
	if (dlg) {
		js_put(j, ",\"dialog\":{\"text\":");
		js_str(j, u.dlg_text);
		js_put(j, ",\"question\":%s,\"over\":\"%s\"}", u.dlg_q ? "true" : "false", SCR_NAMES[scr]);
	} else js_put(j, ",\"dialog\":null");
	js_put(j, ",\"keyboard\":%s,\"buttons\":[", u.osk ? "true" : "false");
	for (int i = 0; i < u.hint_n && i < HINTS; i++) {
		js_put(j, "%s{\"button\":", i ? "," : "");
		js_str(j, u.hint_b[i]);
		js_put(j, ",\"label\":");
		js_str(j, u.hint_l[i]);
		js_put(j, "}");
	}
	js_put(j, "],\"busy\":%s,", g_state_busy() ? "true" : "false");
	js_drives(j);
	js_put(j, "}");
	return js_end(j, out, n);
}

// get_drives: status.json's drives.
static int mcp_drives(char *out, int n)
{
	Js j = js_begin(out, n);
	js_put(j, "{\"busy\":%s,", g_state_busy() ? "true" : "false");
	js_drives(j);
	js_put(j, "}");
	return js_end(j, out, n);
}

// ---- the settings table for list_settings and set_setting: its values are the option arrays the
// screens step through (o_speed, ... at the top), so MCP offers and takes exactly what the pad can
enum { SK_LIST, SK_TEXT, SK_SECRET, SK_DEST };   // one of a list, free text, a secret (never shown), a destination there is
enum { SC_STD, SC_BOTH, SC_DRIVE };              // the standard only, the standard and each drive, each drive only
enum { F_SPEED, F_RETRIES, F_CHECK, F_EJECT, F_AUTOSTART, F_DEST, F_NAME, F_REDUMP, F_REMOTE, F_MCP, F_STATUS, F_WH_ON, F_WH_URL,
       F_WH_HDR_ON, F_WH_HDR, F_WH_EVENT, F_WH_PROGRESS, F_AUDIT_MB };
struct SettingDef {
	const char *key;        // the config key where the value is one (else it is shown as its parts are saved)
	StrId group, label;     // label, as "group · label" when there is a group (STR_COUNT: none); a %d in it is arg + 1
	int kind, scope, field, arg;
	const SetOpt *opts;
	int nopts;
	int max;                // free text: at most this many bytes
};
#define NO_OPTS NULL, 0
// In the order of the screens: Settings (with the drives' own values), Remote control, Webhooks.
// A new setting is a row here (and a case in setting_int / setting_apply when it is not a plain
// config key with a number); its screen steps through the same option array, and ui_test's parity
// check needs its screen row.
static const SettingDef SETTINGS[] = {
	{"speed", STR_COUNT, STR_L_SPEED, SK_LIST, SC_BOTH, F_SPEED, 0, OPTS(o_speed), 0},
	{"retries", STR_COUNT, STR_L_RETRIES, SK_LIST, SC_BOTH, F_RETRIES, 0, OPTS(o_retries), 0},
	{"check", STR_COUNT, STR_L_CHECK, SK_LIST, SC_BOTH, F_CHECK, 0, OPTS(o_check), 0},
	{"eject", STR_COUNT, STR_L_EJECT, SK_LIST, SC_BOTH, F_EJECT, 0, OPTS(o_onoff), 0},
	{"autostart", STR_COUNT, STR_L_AUTOSTART, SK_LIST, SC_BOTH, F_AUTOSTART, 0, OPTS(o_onoff), 0},
	{"destination", STR_COUNT, STR_SEC_DEST, SK_DEST, SC_DRIVE, F_DEST, 0, NO_OPTS, 0},
	{"name_pattern", STR_COUNT, STR_L_FILENAME, SK_TEXT, SC_STD, F_NAME, 0, NO_OPTS, (int)sizeof(g_name_pat) - 1},
	{"redump", STR_COUNT, STR_L_REDUMP, SK_LIST, SC_STD, F_REDUMP, 0, OPTS(o_redump), 0},
	{"remote", STR_L_REMOTE, STR_L_REMOTE_VIEW, SK_LIST, SC_STD, F_REMOTE, 0, OPTS(o_onoff), 0},
	{"mcp", STR_L_REMOTE, STR_L_MCP, SK_LIST, SC_STD, F_MCP, 0, OPTS(o_onoff), 0},
	{"status_json", STR_L_REMOTE, STR_L_STATUS_JSON, SK_LIST, SC_STD, F_STATUS, 0, OPTS(o_onoff), 0},
	{"webhook_on", STR_COUNT, STR_L_WEBHOOK, SK_LIST, SC_STD, F_WH_ON, 0, OPTS(o_onoff), 0},
	{"webhook", STR_L_WEBHOOK, STR_WH_ADDRESS, SK_SECRET, SC_STD, F_WH_URL, 0, NO_OPTS, (int)sizeof(g_webhook) - 1},
	{"webhook_headers_on", STR_L_WEBHOOK, STR_WH_HEADERS, SK_LIST, SC_STD, F_WH_HDR_ON, 0, OPTS(o_onoff), 0},
	{"webhook_header1", STR_L_WEBHOOK, STR_WH_HEADER_N, SK_SECRET, SC_STD, F_WH_HDR, 0, NO_OPTS, (int)sizeof(g_webhook_hdr[0]) - 1},
	{"webhook_header2", STR_L_WEBHOOK, STR_WH_HEADER_N, SK_SECRET, SC_STD, F_WH_HDR, 1, NO_OPTS, (int)sizeof(g_webhook_hdr[0]) - 1},
	{"webhook_header3", STR_L_WEBHOOK, STR_WH_HEADER_N, SK_SECRET, SC_STD, F_WH_HDR, 2, NO_OPTS, (int)sizeof(g_webhook_hdr[0]) - 1},
	{"webhook_event_disc_identified", STR_L_WEBHOOK, STR_WH_EV_0, SK_LIST, SC_STD, F_WH_EVENT, WH_DISC_IDENTIFIED, OPTS(o_onoff), 0},
	{"webhook_event_rip_started", STR_L_WEBHOOK, STR_WH_EV_1, SK_LIST, SC_STD, F_WH_EVENT, WH_RIP_STARTED, OPTS(o_onoff), 0},
	{"webhook_event_rip_finished", STR_L_WEBHOOK, STR_WH_EV_3, SK_LIST, SC_STD, F_WH_EVENT, WH_RIP_FINISHED, OPTS(o_onoff), 0},
	{"webhook_event_rip_cancelled", STR_L_WEBHOOK, STR_WH_EV_4, SK_LIST, SC_STD, F_WH_EVENT, WH_RIP_CANCELLED, OPTS(o_onoff), 0},
	{"webhook_event_rip_failed", STR_L_WEBHOOK, STR_WH_EV_5, SK_LIST, SC_STD, F_WH_EVENT, WH_RIP_FAILED, OPTS(o_onoff), 0},
	{"webhook_event_repair_finished", STR_L_WEBHOOK, STR_WH_EV_6, SK_LIST, SC_STD, F_WH_EVENT, WH_REPAIR_FINISHED, OPTS(o_onoff), 0},
	{"webhook_event_disc_ejected", STR_L_WEBHOOK, STR_WH_EV_7, SK_LIST, SC_STD, F_WH_EVENT, WH_DISC_EJECTED, OPTS(o_onoff), 0},
	{"webhook_event_disc_not_identified", STR_L_WEBHOOK, STR_WH_EV_8, SK_LIST, SC_STD, F_WH_EVENT, WH_DISC_NOT_IDENTIFIED, OPTS(o_onoff), 0},
	{"webhook_event_app_started", STR_L_WEBHOOK, STR_WH_EV_9, SK_LIST, SC_STD, F_WH_EVENT, WH_APP_STARTED, OPTS(o_onoff), 0},
	{"webhook_event_disc_lost", STR_L_WEBHOOK, STR_WH_EV_10, SK_LIST, SC_STD, F_WH_EVENT, WH_DISC_LOST, OPTS(o_onoff), 0},
	{"webhook_progress", STR_L_WEBHOOK, STR_WH_PROGRESS, SK_LIST, SC_STD, F_WH_PROGRESS, 0, OPTS(o_progress), 0},
	{"audit_mb", STR_COUNT, STR_L_AUDIT_MB, SK_LIST, SC_STD, F_AUDIT_MB, 0, OPTS(o_audit_mb), 0},
};
#define NSETTINGS ((int)(sizeof(SETTINGS) / sizeof(SETTINGS[0])))

static void setting_label(const SettingDef &d, Tr tr, char *out, size_t n)
{
	char l[160];
	snprintf(l, sizeof(l), tr(d.label), d.arg + 1);   // "Header %d"
	if (d.group != STR_COUNT) snprintf(out, n, "%s · %s", tr(d.group), l);
	else snprintf(out, n, "%s", l);
}
// A drive's own settings and destination as saved: what its card takes (on the UI thread) and its
// next job uses. NULL: a USB drive without a port.
static DriveCfg *card_cfg(int c)
{
	if (!c) return &g_bdcfg;
	int port = usbodd_port(c - 1);
	return port >= 0 && port < 8 ? &g_usbcfg[port] : NULL;
}
static const char *card_dest_cfg(int c)
{
	if (!c) return g_outdir;
	int port = usbodd_port(c - 1);
	return port >= 0 && port < 8 && g_outdir_usb[port][0] ? g_outdir_usb[port] : HDD_DIR;
}
static int *drive_field(DriveCfg &d, int field)
{
	switch (field) {
	case F_SPEED: return &d.speed;
	case F_RETRIES: return &d.retries;
	case F_CHECK: return &d.check;
	case F_EJECT: return &d.eject;
	case F_AUTOSTART: return &d.autostart;
	}
	return NULL;
}
// A list setting's value: the standard's (card -1) or a drive's own as saved (-1 = the standard).
static int setting_int(const SettingDef &d, int card)
{
	if (card >= 0) {
		DriveCfg *cf = card_cfg(card), none = {0, -1, -1, -1, -1, -1};
		int *v = drive_field(cf && cf->set ? *cf : none, d.field);
		return v ? *v : -1;
	}
	switch (d.field) {
	case F_SPEED: return g_std.speed;
	case F_RETRIES: return g_std.retries;
	case F_CHECK: return g_std.check;
	case F_EJECT: return g_std.eject;
	case F_AUTOSTART: return g_std.autostart;
	case F_REDUMP: return g_redump;
	case F_REMOTE: return g_remote;
	case F_MCP: return g_mcp;
	case F_STATUS: return g_status_json;
	case F_WH_ON: return g_webhook_on;
	case F_WH_HDR_ON: return g_webhook_hdr_on;
	case F_WH_EVENT: return (g_webhook_mask >> d.arg) & 1;
	case F_WH_PROGRESS: return (g_webhook_mask >> WH_RIP_PROGRESS) & 1 ? g_webhook_step : 0;
	case F_AUDIT_MB: return g_audit_mb;
	}
	return 0;
}
// A destination's label: with its free space when it is there (a free-space query), else "not connected"
static void dest_label(const char *dir, Tr tr, char *out, size_t n, int there = 1)
{
	char parts[96], fr[48];
	unsigned gb = 0;
	dest_parts(dir, parts, sizeof(parts), &gb, tr, there);
	if (there) snprintf(fr, sizeof(fr), tr(STR_DEST_FREE), gb);
	else snprintf(fr, sizeof(fr), "%s", tr(STR_DEST_NOT_CONNECTED));
	snprintf(out, n, "%s · %s", parts, fr);
}
// The destinations there are with their labels in English and Swedish: found before the config
// lock is taken (a drive scan and a free-space query per destination). NULL: no memory.
struct DestOpts { int n; char dir[9][32], en[9][160], sv[9][160]; };
static DestOpts *dest_opts()
{
	DestOpts *o = (DestOpts *)malloc(sizeof(DestOpts));
	if (!o) return NULL;
	o->n = dest_list(o->dir);
	for (int i = 0; i < o->n; i++) {
		dest_label(o->dir[i], T_en, o->en[i], sizeof(o->en[0]));
		dest_label(o->dir[i], T_sv, o->sv[i], sizeof(o->sv[0]));
	}
	return o;
}
static int dest_offered(const DestOpts *o, const char *dir)
{
	for (int i = 0; o && i < o->n; i++) if (!strcmp(o->dir[i], dir)) return 1;
	return 0;
}
// A value's label in both languages: "label":{"en":...,"sv":...}
static void js_labels(Js &j, const char *en, const char *sv)
{
	js_put(j, "\"label\":{\"en\":");
	js_str(j, en);
	js_put(j, ",\"sv\":");
	js_str(j, sv);
	js_put(j, "}");
}
// One setting as list_settings shows it, in the standard (card -1) or for a drive. Holds no lock of
// its own and does no I/O: the destinations come from dest_opts.
static void setting_json(Js &j, const SettingDef &d, int card, const DestOpts *dests)
{
	char sc[16], a[200], b[200];
	if (card < 0) snprintf(sc, sizeof(sc), "standard"); else drive_id(card, sc, sizeof(sc));
	js_put(j, "{\"key\":");
	js_str(j, d.key);
	js_put(j, ",\"scope\":");
	js_str(j, sc);
	setting_label(d, T_en, a, sizeof(a));
	setting_label(d, T_sv, b, sizeof(b));
	js_put(j, ",");
	js_labels(j, a, b);
	js_put(j, ",\"secret\":%s", d.kind == SK_SECRET ? "true" : "false");
	if (d.kind == SK_SECRET) {   // only whether it is set: the value may hold a token
		int set = d.field == F_WH_URL ? g_webhook[0] != 0 : g_webhook_hdr[d.arg][0] != 0;
		js_put(j, ",\"is_set\":%s,\"values\":null}", set ? "true" : "false");
		return;
	}
	if (d.kind == SK_TEXT) {
		js_put(j, ",\"value\":");
		js_str(j, g_name_pat);
		js_put(j, ",\"values\":null}");
		return;
	}
	if (d.kind == SK_DEST) {
		const char *cur = card_dest_cfg(card);
		js_put(j, ",\"value\":");
		js_str(j, cur);
		js_put(j, ",\"values\":[");
		int nd = dests ? dests->n : 0;
		for (int i = 0; i < nd; i++) {
			js_put(j, "%s{\"value\":", i ? "," : "");
			js_str(j, dests->dir[i]);
			js_put(j, ",");
			js_labels(j, dests->en[i], dests->sv[i]);
			js_put(j, "}");
		}
		if (!dest_offered(dests, cur)) {   // chosen before, not plugged in now: still the value
			js_put(j, "%s{\"value\":", nd ? "," : "");
			js_str(j, cur);
			js_put(j, ",");
			dest_label(cur, T_en, a, sizeof(a), 0);
			dest_label(cur, T_sv, b, sizeof(b), 0);
			js_labels(j, a, b);
			js_put(j, "}");
		}
		js_put(j, "]}");
		return;
	}
	js_put(j, ",\"value\":\"%d\",\"values\":[", setting_int(d, card));
	if (card >= 0) {   // a drive: first "standard", with the standard's value
		int sv = setting_int(d, -1);
		char va[100] = "", vb[100] = "";
		snprintf(va, sizeof(va), "%d", sv); snprintf(vb, sizeof(vb), "%d", sv);
		for (int i = 0; i < d.nopts; i++) if (d.opts[i].v == sv) { opt_label(d.opts[i], T_en, va, sizeof(va)); opt_label(d.opts[i], T_sv, vb, sizeof(vb)); }
		snprintf(a, sizeof(a), T_en(STR_STD_VALUE), va);
		snprintf(b, sizeof(b), T_sv(STR_STD_VALUE), vb);
		js_put(j, "{\"value\":\"-1\",");
		js_labels(j, a, b);
		js_put(j, "},");
	}
	for (int i = 0; i < d.nopts; i++) {
		opt_label(d.opts[i], T_en, a, sizeof(a));
		opt_label(d.opts[i], T_sv, b, sizeof(b));
		js_put(j, "%s{\"value\":\"%d\",", i ? "," : "", d.opts[i].v);
		js_labels(j, a, b);
		js_put(j, "}");
	}
	js_put(j, "]}");
}

// list_settings: the standard's settings, then each drive's own.
static int mcp_list_settings(char *out, int n)
{
	DestOpts *dests = dest_opts();   // the drives are scanned before the lock: a rip's writes need the file layer
	if (!dests) { snprintf(out, n, "Out of memory."); return -1; }
	Js j = js_begin(out, n);
	CfgLock lock;   // one consistent picture of the values
	js_put(j, "{\"settings\":[");
	int first = 1;
	for (int i = 0; i < NSETTINGS; i++) {
		if (SETTINGS[i].scope == SC_DRIVE) continue;
		js_put(j, first ? "" : ",");
		first = 0;
		setting_json(j, SETTINGS[i], -1, dests);
	}
	int list[NCARDS], nc = card_list(list);
	for (int c = 0; c < nc; c++) {
		char id[16];
		drive_id(list[c], id, sizeof(id));
		if (!strcmp(id, "usb")) continue;   // no port: no tool can name it
		for (int i = 0; i < NSETTINGS; i++) {
			if (SETTINGS[i].scope == SC_STD) continue;
			js_put(j, ",");
			setting_json(j, SETTINGS[i], list[c], dests);
		}
	}
	js_put(j, "]}");
	free(dests);
	return js_end(j, out, n);
}

// The allowed values of a list setting as text for a refusal: "3 (2×), 2 (4×), ..."
static void allowed_text(const SettingDef &d, int card, char *out, size_t n)
{
	size_t o = snprintf(out, n, "%s", card >= 0 ? "-1 (standard)" : "");
	for (int i = 0; i < d.nopts && o < n; i++) {
		char l[100];
		opt_label(d.opts[i], T_en, l, sizeof(l));
		o += snprintf(out + o, n - o, "%s%d (%s)", o ? ", " : "", d.opts[i].v, l);
	}
}

// Changes setting d (of a drive: card >= 0) to value, as its screen would: the same values, the
// same follow-ups, saved with source mcp. Holds the config lock. Non-zero with the reason in why.
// A drive's card takes its new values on the UI thread (flip_frame), never here: a job may start there.
static int setting_apply(const SettingDef &d, int card, const char *value, const DestOpts *dests, char *why, size_t wn)
{
	int v = 0;
	if (d.kind == SK_LIST) {
		int ok = card >= 0 && !strcmp(value, "-1");
		if (ok) v = -1;
		for (int i = 0; i < d.nopts && !ok; i++) {
			char t[16];
			snprintf(t, sizeof(t), "%d", d.opts[i].v);
			if (!strcmp(t, value)) { ok = 1; v = d.opts[i].v; }
		}
		if (!ok) {
			char al[600];
			allowed_text(d, card, al, sizeof(al));
			snprintf(why, wn, "Not an allowed value for %s: \"%.40s\". Allowed: %s.", d.key, value, al);
			return -1;
		}
	} else if (d.kind == SK_TEXT || d.kind == SK_SECRET) {
		if ((int)strlen(value) > d.max) {
			snprintf(why, wn, "Too long: %s takes at most %d bytes.", d.key, d.max);
			return -1;
		}
		for (const unsigned char *p = (const unsigned char *)value; *p; p++)
			if (*p < 0x20 || *p == 0x7F) {   // the on-screen keyboard cannot type one (never the value: it may be a secret)
				snprintf(why, wn, T_en(STR_SET_CTRL_CHAR), d.key);
				return -1;
			}
	}
	CfgLock lock;
	if (d.kind == SK_DEST && !dest_offered(dests, value) && strcmp(value, card_dest_cfg(card))) {
		char al[400] = "";
		for (int i = 0; dests && i < dests->n; i++) snprintf(al + strlen(al), sizeof(al) - strlen(al), "%s%s", i ? ", " : "", dests->dir[i]);
		snprintf(why, wn, "Not a destination that is there now: \"%.60s\". Allowed: %s.", value, al);
		return -1;
	}
	if (card >= 0) {   // as the drive's settings (SELECT) and the home screen's left/right do
		Card &k = g_cards[card];
		char id[16];
		drive_id(card, id, sizeof(id));
		DriveCfg *cf = card_cfg(card);
		if (card_busy(card)) {
			snprintf(why, wn, "The drive %s is busy: a rip, repair or check runs on it. Its settings can change when the job has ended.", id);
			return -1;
		}
		if (!cf) { snprintf(why, wn, "The drive %s has no port: its settings cannot be saved.", id); return -1; }
		if (d.kind == SK_DEST) {
			if (!card) snprintf(g_outdir, sizeof(g_outdir), "%s", value);
			else snprintf(g_outdir_usb[usbodd_port(card - 1)], sizeof(g_outdir_usb[0]), "%s", value);
			__sync_fetch_and_or(&k.pend_cfg, CF_OUTDIR);   // the card takes it on the UI thread
		} else {
			DriveCfg nd = {1, -1, -1, -1, -1, -1};
			if (cf->set) nd = *cf;
			nd.set = 1;
			*drive_field(nd, d.field) = v;
			*cf = nd;
			__sync_fetch_and_or(&k.pend_cfg, CF_DRIVE);
		}
		cfg_save_src(AU_MCP);
		logf("mcp: drive %s: %s=%s", id, d.key, value);
		return 0;
	}
	char line[700];
	int mask = g_webhook_mask, was_remote = g_remote;
	switch (d.field) {
	case F_WH_EVENT:
		snprintf(line, sizeof(line), "webhook_events=%d", v ? mask | 1 << d.arg : mask & ~(1 << d.arg));
		break;
	case F_WH_PROGRESS:   // off, or on with its step: two config keys, one save
		if (v) { snprintf(line, sizeof(line), "webhook_step=%d", v); cfg_apply_line(line); }
		snprintf(line, sizeof(line), "webhook_events=%d", v ? mask | 1 << WH_RIP_PROGRESS : mask & ~(1 << WH_RIP_PROGRESS));
		break;
	case F_NAME:   // an empty pattern is the first ready-made one, as on the screen
		snprintf(line, sizeof(line), "name_pattern=%s", value[0] ? value : FN_PRESETS[0]);
		break;
	case F_WH_URL:   // the keyboard's "http://" alone is no address
		snprintf(line, sizeof(line), "webhook=%s", strcmp(value, "http://") ? value : "");
		break;
	case F_WH_HDR:
		if (value[0] && !strchr(value, ':')) { snprintf(why, wn, "A header is \"Name: value\" (or empty to clear it)."); return -1; }
		snprintf(line, sizeof(line), "webhook_header%d=%s", d.arg + 1, value);
		break;
	default:
		snprintf(line, sizeof(line), "%s=%d", d.key, v);
		break;
	}
	if (!cfg_apply_line(line)) { snprintf(why, wn, "The value was not taken."); return -1; }
	char key[48];
	snprintf(key, sizeof(key), "%.*s", (int)strcspn(line, "="), line);
	setting_applied(key);
	cfg_save_src(AU_MCP);
	if (d.kind == SK_SECRET) logf("mcp: setting %s %s", d.key, value[0] ? "set" : "cleared");   // never the value
	else logf("mcp: setting %s=%s", d.key, value);
	if (d.field == F_REMOTE && g_remote && !was_remote) notify_url(STR_REMOTE_HELP, "");   // as the menu does when it turns the view on
	return 0;
}

// set_setting: one setting of the standard (drive NULL) or of a drive; the answer is the setting
// as list_settings shows it now.
static int mcp_set_setting(const char *key, const char *value, const char *drive, char *out, int n)
{
	const SettingDef *d = NULL;
	for (int i = 0; i < NSETTINGS && !d; i++) if (!strcmp(SETTINGS[i].key, key)) d = &SETTINGS[i];
	if (!d) { snprintf(out, n, "Unknown setting \"%s\"; list_settings lists them.", key); return -1; }
	int card = -1;
	if (drive) {
		if (d->scope == SC_STD) { snprintf(out, n, "%s has no value per drive: leave out drive.", key); return -1; }
		if ((card = drive_card(drive)) < 0) { snprintf(out, n, "No drive %s is connected; get_drives lists the drives.", drive); return -1; }
	} else if (d->scope == SC_DRIVE) {
		snprintf(out, n, "%s is set per drive: give the drive (internal, usb0, ...).", key);
		return -1;
	}
	char why[800];
	DestOpts *dests = NULL;
	if (d->kind == SK_DEST && !(dests = dest_opts())) { snprintf(out, n, "Out of memory."); return -1; }   // before the lock
	int rc;
	{
		CfgLock lock;   // the answer shows this change, not one made right after it
		if (setting_apply(*d, card, value, dests, why, sizeof(why))) { snprintf(out, n, "%s", why); utf8_fix(out); rc = -1; }
		else {
			Js j = js_begin(out, n);
			setting_json(j, *d, card, dests);
			rc = js_end(j, out, n);
		}
	}
	free(dests);
	return rc;
}

// read_log: lines of psxrip.log from line `from` (-1: the last max), with the next line's number.
// A secret (the webhook address, a header) is never shown, even if a line has one.
static int log_line(FILE *f, char *buf, int n)   // the next line without its newline (a long one cut), 0 at the end
{
	if (!fgets(buf, n, f)) return 0;
	size_t l = strlen(buf);
	if (l && buf[l - 1] == '\n') buf[--l] = 0;
	else { int c; while ((c = fgetc(f)) != EOF && c != '\n') {} }   // longer than buf: the rest is skipped
	if (l && buf[l - 1] == '\r') buf[--l] = 0;
	return 1;
}
#define LOG_SECRETS (2 + 2 * WH_HEADERS)
#define LOG_SHOWN 1023                  // a line shows at most this many bytes of the log
#define LOG_READ (LOG_SHOWN + 512 + 1)  // read: a secret (< 512 bytes) that starts within them is read whole
#define LOG_CLEAN (LOG_SHOWN * 8 + 16)  // a shown line with each secret byte as "[secret]" at the worst
static int mcp_read_log(int from, int max, char *out, int n)
{
	if (max < 1) max = 1;
	if (max > MCP_MAX_LOG_LINES) max = MCP_MAX_LOG_LINES;
	char *sec = (char *)malloc(LOG_SECRETS * 512), *line = (char *)malloc(LOG_READ), *clean = (char *)malloc(LOG_CLEAN);
	if (!sec || !line || !clean) { free(sec); free(line); free(clean); snprintf(out, n, "Out of memory."); return -1; }
	int ns = 0;
	{   // the secrets as they are now: the address, its path (the secret id), each header and its value,
		// however short (a path "/" alone is none)
		CfgLock lock;
		auto add = [&](const char *s, size_t len) { if (len >= 1 && len < 512) { memcpy(sec + ns * 512, s, len); sec[ns * 512 + len] = 0; ns++; } };
		add(g_webhook, strlen(g_webhook));
		const char *h = strstr(g_webhook, "://"), *path = h ? strchr(h + 3, '/') : NULL;
		if (path && strlen(path) >= 2) add(path, strlen(path));
		for (int i = 0; i < WH_HEADERS; i++) {
			const char *hd = g_webhook_hdr[i], *c = strchr(hd, ':');
			add(hd, strlen(hd));
			if (c) { c++; while (*c == ' ') c++; add(c, strlen(c)); }
		}
	}
	for (int a = 0; a < ns; a++)   // longest first: the whole address goes before its path
		for (int b = a + 1; b < ns; b++)
			if (strlen(sec + b * 512) > strlen(sec + a * 512)) { char t[512]; memcpy(t, sec + a * 512, 512); memcpy(sec + a * 512, sec + b * 512, 512); memcpy(sec + b * 512, t, 512); }
	Js j = js_begin(out, n);
	int total = 0, got = 0, cut = 0;
	FILE *f = fopen(LOG_PATH, "r");
	if (f && from < 0) {   // the last max lines: count them first
		while (log_line(f, line, LOG_READ)) total++;
		from = total > max ? total - max : 0;
		rewind(f);
		total = 0;
	}
	if (from < 0) from = 0;
	js_put(j, "{\"lines\":[");
	int end_room = j.n > 200 ? 100 : 0;   // room for the end (a tiny buffer: js_end says it is too small)
	j.n -= end_room;
	while (f && log_line(f, line, LOG_READ)) {
		int at = total++;
		if (at < from || got >= max || cut) continue;
		size_t o = 0;   // the line's first LOG_SHOWN bytes with each secret replaced (a secret that starts in them whole)
		for (const char *p = line; *p && p - line < LOG_SHOWN && o + 9 < LOG_CLEAN; ) {
			int hit = 0;
			for (int s = 0; s < ns && !hit; s++) {
				size_t sl = strlen(sec + s * 512);
				if (!strncmp(p, sec + s * 512, sl)) { o += snprintf(clean + o, LOG_CLEAN - o, "[secret]"); p += sl; hit = 1; }
			}
			if (!hit) clean[o++] = *p++;
		}
		clean[o] = 0;
		int keep = j.o;
		js_put(j, got ? "," : "");
		js_str(j, clean);
		if (j.over) { j.over = false; j.o = keep; j.p[keep] = 0; cut = 1; continue; }   // no room: the answer ends before it
		got++;
	}
	if (f) fclose(f);
	j.n += end_room;
	int start = from < total ? from : total;
	js_put(j, "],\"from\":%d,\"next\":%d,\"total\":%d%s}", start, start + got, total, cut ? ",\"truncated\":true" : "");
	free(sec); free(line); free(clean);
	return js_end(j, out, n);
}

static const McpHooks g_mcp_hooks = {mcp_state, remote_jpeg, mcp_drives, mcp_list_settings, mcp_set_setting, mcp_na_ui, mcp_read_log};

static void event_json(char *js, size_t n, const char *event)
{
	uint32_t pos = g_readpos > g_done ? g_readpos : g_done;
	snprintf(js, n, "{\"event\":\"%s\",\"app\":\"PSX RiPPER\",\"percent\":%u,", event,
	         g_total ? (unsigned)((uint64_t)pos * 100 / g_total) : 0);
	json_fields(js, n);
	size_t o = strlen(js);
	char dn[96], did[8];
	int ec = cur_card();   // the card the event is about (a rip's thread), not the one on the screen
	source_name(ec ? ec - 1 : -1, dn, sizeof(dn));
	if (ec && usbodd_port(ec - 1) >= 0) snprintf(did, sizeof(did), "usb%d", usbodd_port(ec - 1));
	else snprintf(did, sizeof(did), ec ? "usb" : "ps3");
	snprintf(js + o, n - o, ",\"drive_id\":\"%s\",\"drive\":", did);
	json_str(js, n, dn);
	if (!strcmp(event, "app_started")) {   // every drive and the disc in it, when there is one
		o = strlen(js);
		snprintf(js + o, n - o, ",\"discs\":[");
		int list[NCARDS], nc = card_list(list), first = 1;
		for (int i = 0; i < nc; i++) {
			const Card &k = g_cards[list[i]];
			if (k.state != DISC_READY) continue;
			source_name(list[i] ? list[i] - 1 : -1, dn, sizeof(dn));
			o = strlen(js); snprintf(js + o, n - o, "%s{\"drive\":", first ? "" : ","); json_str(js, n, dn);
			o = strlen(js); snprintf(js + o, n - o, ",\"title\":"); json_str(js, n, k.title);
			o = strlen(js); snprintf(js + o, n - o, ",\"serial\":"); json_str(js, n, k.serial);
			o = strlen(js); snprintf(js + o, n - o, "}");
			first = 0;
		}
		o = strlen(js); snprintf(js + o, n - o, "]");
	}
	o = strlen(js);
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
	char js[2048];
	event_json(js, sizeof(js), event);
	logf("webhook event %s (card %d)", event, cur_card());
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
		if (g_headless) tell(msg);
		Card &k = g_cards[cur_card()];
		snprintf(k.res, sizeof(k.res), "%s", msg);
		k.res_col = 0xFF8A80; k.res_rd = -2;
		k.res_failed = 1;
		return 0;
	}
	const char *name = g_name;
	uint32_t mb = (uint32_t)((uint64_t)g_leadout * RAW_SIZE / (1024 * 1024));
	snprintf(msg, sizeof(msg), T(STR_FOUND),
	         name, g_serial[0] ? g_serial : T(STR_UNKNOWN), g_ntracks, mb, OUT_DIR);

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
	sysThreadCreate(&tid, rip_thread, (void *)(long)cur_card(), 1000, 0x10000, THREAD_JOINABLE, (char *)"psxrip");

	// the card on the home screen shows the progress; this thread only reports it
	uint32_t sent_pct = 0;   // last percentage sent as rip_progress
	while (!g_finished) {
		if (exit_requested) g_abort = 1;
		usleep(100 * 1000);
		uint32_t pos = g_readpos > g_done ? g_readpos : g_done;
		uint32_t pct = (uint32_t)((uint64_t)pos * 100 / (g_total ? g_total : 1));
		if (pct >= sent_pct + (uint32_t)g_webhook_step && pct < 100 && !g_phase) {   // every g_webhook_step percent
			sent_pct = pct - pct % g_webhook_step;
			fire_event("rip_progress");
		}
	}
	u64 rv;
	sysThreadJoin(tid, &rv);
	if (!g_result) g_phase = 3;   // LibCrypt check and Redump: the card says "finishing"
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
		int lost = g_result == 1 && g_disc_lost[cur_card()];
		fire_event(lost ? "disc_lost" : g_result == 1 ? "rip_cancelled" : "rip_failed");
		Card &k = g_cards[cur_card()];
		snprintf(k.res, sizeof(k.res), "%s", lost ? T(STR_CARD_DISC_LOST) : g_result == 1 ? T(STR_CARD_CANCELLED) : T(STR_CREATE_FAILED));
		k.res_col = g_result == 1 ? 0xF0C060 : 0xFF8A80;
		k.res_failed = g_result == 1 ? 2 : 1;   // 2 = cancelled
		return 0;
	}

	{
		char part[300], bin[300];
		snprintf(part, sizeof(part), "%s.bin.part", g_base);
		snprintf(bin, sizeof(bin), "%s.bin", g_base);
		vf_unlink(bin);
		if (vf_rename(part, bin)) logf("rename %s failed", part);
		Card &k = g_cards[cur_card()];
		if (k.exist[0] && strcmp(k.exist, g_base)) {   // the same disc under another name: this rip replaces it
			static const char *ext[] = {".bin", ".cue", ".md5", ".log", ".sbi"};
			for (int i = 0; i < 5; i++) { char o[300]; snprintf(o, sizeof(o), "%s%s", k.exist, ext[i]); vf_unlink(o); }
			logf("replaced the earlier rip %s", k.exist);
		}
		idx_stale();
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
	if (g_redump != RD_OFF) { redump_verify(rdinfo, sizeof(rdinfo)); audit_card_redump(cur_card()); }

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
		char d[24];
		fmt_dur(d, sizeof(d), (long)(time(NULL) - g_rip_t0));
		res_add(0xA8B0C8, T(STR_TOTAL_TIME), d);
	}
	write_riplog();
	logf("done: %s errs=%u", g_base, g_readerrs);
	snprintf(g_done_msg, sizeof(g_done_msg), "%s", msg);
	{   // the card: the Redump line says how the image compared, the result line how the rip went
		Card &k = g_cards[cur_card()];
		k.rd_done[0] = 0;
		k.rd_state = -1;
		if (g_rd_result == 1) {
			snprintf(k.rd_done, sizeof(k.rd_done), T(STR_RD_CARD_OK), g_rd_result_name); k.rd_done_col = 0x7CE0A0; k.rd_state = 1;
			snprintf(k.rd_name, sizeof(k.rd_name), "%s", g_rd_result_name);
		}
		else if (g_rd_result == 0) { snprintf(k.rd_done, sizeof(k.rd_done), "%s", T(STR_RD_NOMATCH)); k.rd_done_col = 0xFF8A80; k.rd_state = 0; }
		char d[24];
		fmt_dur(d, sizeof(d), (long)(time(NULL) - g_rip_t0));
		snprintf(k.res, sizeof(k.res), T(STR_CARD_DONE), d);
		if (g_readerrs) snprintf(k.res + strlen(k.res), sizeof(k.res) - strlen(k.res), T(STR_CARD_UNREADABLE), g_readerrs);
		if (g_recovered) snprintf(k.res + strlen(k.res), sizeof(k.res) - strlen(k.res), T(STR_CARD_FIXED), g_recovered);
		k.res_col = g_readerrs || g_badsect ? 0xF0C060 : 0x7CE0A0;
		k.res_failed = 0;
		k.res_rd = g_rd_result == 1 ? 1 : g_rd_result == 0 ? 0 : g_redump == RD_OFF ? -2 : -1;
		k.res_bad = g_readerrs || g_badsect;
	}
	g_state = ST_HOME;
	fire_event("rip_finished");
	return 1;
}

static int rip_disc(int fresh, int autorip)
{
	if (g_use == SRC_BD) while (g_ident_busy && g_ident_card == 0) usleep(10000);   // it patches/unpatches lv1 itself
	int ok = rip_disc_inner(fresh, autorip);
	if (g_use == SRC_BD) lv1_storage_unpatch();
	return ok;
}


static void job_reset(int c);

// Checks a rip from its file, without the disc: MD5 per track (track bounds from its .cue), its
// .md5 written, the track MD5s and the Redump line of its log rewritten (a small log made when it
// has none). As the card's job (job = 1) it shows its progress, can be cancelled and compares with
// Redump as set. Otherwise (the audit's undo, on the UI thread) it touches no card's state and
// compares with the local copy only (none: unchecked). Returns 0 checked, 1 cancelled, -1 no image.
static void (*g_check_tick)(uint32_t sectors);   // set: called now and then while check_image reads (not as a job)
static int check_image(const char *base, int job = 0)
{
	char path[320];
	// track starts (sectors) from the cue: INDEX 00 when there is one (the pregap belongs to the track)
	uint32_t st[MAX_TRACKS + 1];
	int nt = 0;
	snprintf(path, sizeof(path), "%s.cue", base);
	VFile *cf = vf_open(path, "rb");
	if (cf) {
		char cb[8192];
		int n = (int)vf_read(cf, cb, sizeof(cb) - 1);
		vf_close(cf);
		cb[n > 0 ? n : 0] = 0;
		int i0 = 0;
		char *save = NULL;
		for (char *l = strtok_r(cb, "\r\n", &save); l; l = strtok_r(NULL, "\r\n", &save)) {
			unsigned idx, m, sec, fr;
			if (strstr(l, "TRACK ")) { i0 = 0; if (nt < MAX_TRACKS) nt++; continue; }
			if (nt && sscanf(l, " INDEX %u %u:%u:%u", &idx, &m, &sec, &fr) == 4) {
				uint32_t lba = (m * 60 + sec) * 75 + fr;
				if (idx == 0) { st[nt - 1] = lba; i0 = 1; }
				else if (idx == 1 && !i0) st[nt - 1] = lba;
			}
		}
	}
	if (!nt) { nt = 1; st[0] = 0; }   // no cue: the whole image as one track
	st[0] = 0;
	snprintf(path, sizeof(path), "%s.bin", base);
	VFile *f = vf_open(path, "rb");
	if (!f) { if (job) g_total = 1; logf("check %s: no image", base); return -1; }
	if (job) {   // size: read to the end once is slow; seek-free estimate from the identification
		uint64_t size = (uint64_t)g_cards[cur_card()].mb * 1024 * 1024;
		g_total = size ? (uint32_t)(size / RAW_SIZE) : 1;
	}
	Md5 md[MAX_TRACKS + 1];
	for (int t = 0; t < nt; t++) md5_init(&md[t]);
	uint8_t *buf = (uint8_t *)malloc(512 * RAW_SIZE);
	uint32_t lba = 0;
	int t = 0;
	while (buf && !(job && g_abort)) {
		size_t got = vf_read(f, buf, 512 * RAW_SIZE);
		uint32_t secs = (uint32_t)(got / RAW_SIZE);
		for (uint32_t i = 0; i < secs; i++, lba++) {
			while (t + 1 < nt && lba >= st[t + 1]) t++;
			md5_update(&md[t], buf + (size_t)i * RAW_SIZE, RAW_SIZE);
		}
		if (job) { g_done = lba; if (lba > g_total) g_total = lba; }
		else if (g_check_tick && lba % (16 * 512) == 0) g_check_tick(lba);   // about every 19 MB
		if (got < 512 * RAW_SIZE) break;
	}
	vf_close(f);
	free(buf);
	if (job && g_abort) { logf("check %s: cancelled", base); return 1; }
	uint8_t bin[MAX_TRACKS + 1][16];
	char hex[MAX_TRACKS + 1][33];
	for (int i = 0; i < nt; i++) { md5_final(&md[i], bin[i]); md5_hex(bin[i], hex[i]); }
	int rd;
	char rdname[RD_NAME] = "";
	if (job) {
		g_total = lba;
		g_ntracks = nt;
		memcpy(g_track_md5bin, bin, sizeof(bin[0]) * nt);
		memcpy(g_track_md5, hex, sizeof(hex[0]) * nt);
		g_phase = 3;
		char info[RD_NAME + 80];
		redump_verify(info, sizeof(info));
		rd = g_rd_result;
		snprintf(rdname, sizeof(rdname), "%s", g_rd_result_name);
	} else {
		rd_lock();
		rd = rd_local_verify(bin, nt, rdname, sizeof(rdname));
		rd_unlock();
		if (rd < -2 || rd > 1) rd = -1;
		if (rd != 1) rdname[0] = 0;
	}
	logf("check %s: %d track(s), redump %s %s", base, nt, rd_code(rd), rdname);
	snprintf(path, sizeof(path), "%s.md5", base);
	VFile *mf = vf_open(path, "wb");
	if (mf) { for (int i = 0; i < nt; i++) vf_printf(mf, "%s  track %02d\n", hex[i], i + 1); vf_close(mf); }
	// its log: the tracks' MD5s and the Redump line replaced, or a small log
	snprintf(path, sizeof(path), "%s.log", base);
	char *lb = (char *)malloc(65536);
	int n = 0;
	VFile *lf = lb ? vf_open(path, "rb") : NULL;
	if (lf) { n = (int)vf_read(lf, lb, 65535); vf_close(lf); }
	if (lb) lb[n > 0 ? n : 0] = 0;
	char rdl[RD_NAME + 32];
	snprintf(rdl, sizeof(rdl), "redump=%s%s%s\n", rd_code(rd), rdname[0] ? " " : "", rdname);
	VFile *of = lb ? vf_open(path, "wb") : NULL;
	if (of) {
		if (n > 0) {
			int put = 0;
			for (char *l = lb; *l; ) {
				char *e = strchr(l, '\n');
				size_t len = e ? (size_t)(e - l + 1) : strlen(l);
				unsigned tn;
				const char *mp = NULL;
				if (!strncmp(l, "track=", 6) && sscanf(l, "track=%u", &tn) == 1 && tn >= 1 && (int)tn <= nt) {
					mp = strstr(l, " md5=");
					if (mp && mp >= l + len) mp = NULL;   // on a later line
				}
				if (!strncmp(l, "redump=", 7)) { if (!put) { vf_printf(of, "%s", rdl); put = 1; } }
				else if (mp) {   // the track's MD5 as the image has it now
					vf_write(of, l, (size_t)(mp + 5 - l));
					vf_printf(of, "%s\n", hex[tn - 1]);
				}
				else vf_write(of, l, len);
				l += len;
			}
			if (!put) vf_printf(of, "%s", rdl);
		} else {
			const Card *k = job ? &g_cards[cur_card()] : NULL;
			const char *slash = strrchr(base, '/');
			vf_printf(of, "PSX Ripper rip log\ntitle=%s\nserial=%s\nchecked=from the image (no rip log before)\n",
			          k ? k->title : slash ? slash + 1 : base, k ? k->serial : "");
			vf_printf(of, "sectors=%u\n", lba);
			for (int i = 0; i < nt; i++) vf_printf(of, "track=%02d start=%u md5=%s\n", i + 1, st[i], hex[i]);
			vf_printf(of, "%s", rdl);
		}
		vf_close(of);
	}
	free(lb);
	return 0;
}

// Sectors an undo put back into a rip: each goes into its log again as unreadable (its restored
// bytes are all zero, as the rip fills a sector it could not read) or bad, unless the log lists it
// already, and the counts follow; so a later repair reads them again.
static void riplog_put_back(const char *base, const uint32_t *lbas, const uint8_t *zero, int n)
{
	char path[320];
	snprintf(path, sizeof(path), "%s.log", base);
	char *lb = (char *)malloc(65536);
	uint8_t *add = (uint8_t *)malloc(n);
	if (!lb || !add) { free(lb); free(add); return; }
	int len = 0;
	VFile *f = vf_open(path, "rb");
	if (f) { len = (int)vf_read(f, lb, 65535); vf_close(f); }
	lb[len > 0 ? len : 0] = 0;
	int nu = 0, nb = 0;
	for (int i = 0; i < n; i++) {
		add[i] = 1;
		for (int j = 0; j < i; j++) if (add[j] && lbas[j] == lbas[i]) add[i] = 0;   // twice in one undo
	}
	for (char *l = lb; *l; ) {   // listed already: unreadable, bad, or uncertain and not confirmed
		size_t ll = strcspn(l, "\n");
		char t[96];
		snprintf(t, sizeof(t), "%.*s", (int)(ll < sizeof(t) - 1 ? ll : sizeof(t) - 1), l);
		unsigned v;
		if (sscanf(t, "unreadable=%u", &v) == 1 || sscanf(t, "bad=%u", &v) == 1 || (sscanf(t, "uncertain=%u", &v) == 1 && strstr(t, "unconfirmed")))
			for (int i = 0; i < n; i++) if (lbas[i] == v) add[i] = 0;
		l += ll + (l[ll] == '\n');
	}
	for (int i = 0; i < n; i++) if (add[i]) { if (zero[i]) nu++; else nb++; }
	if (nu + nb) {
		VFile *of = vf_open(path, "wb");
		if (of) {
			int seen_u = 0, seen_b = 0;
			for (char *l = lb; *l; ) {
				size_t ll = strcspn(l, "\n");
				unsigned v;
				if (sscanf(l, "unreadable_count=%u", &v) == 1) { vf_printf(of, "unreadable_count=%u\n", v + nu); seen_u = 1; }
				else if (sscanf(l, "bad_count=%u", &v) == 1) { vf_printf(of, "bad_count=%u\n", v + nb); seen_b = 1; }
				else if (sscanf(l, "fixed_count=%u", &v) == 1) vf_printf(of, "fixed_count=%u\n", v > (unsigned)(nu + nb) ? v - nu - nb : 0);
				else { vf_write(of, l, ll); vf_write(of, "\n", 1); }
				l += ll + (l[ll] == '\n');
			}
			if (!seen_u) vf_printf(of, "unreadable_count=%d\n", nu);
			if (!seen_b) vf_printf(of, "bad_count=%d\n", nb);
			for (int i = 0; i < n; i++) if (add[i]) vf_printf(of, "%s=%u\n", zero[i] ? "unreadable" : "bad", lbas[i]);
			vf_close(of);
			logf("undo: %d unreadable and %d bad sector(s) back in %s", nu, nb, path);
		}
	}
	free(add);
	free(lb);
}

// Sectors a redo wrote the repaired bytes into again (the inverse of riplog_put_back): their
// unreadable= / bad= lines leave the log and the counts follow.
static void riplog_take_out(const char *base, const uint32_t *lbas, int n)
{
	char path[320];
	snprintf(path, sizeof(path), "%s.log", base);
	char *lb = (char *)malloc(65536);
	if (!lb) return;
	int len = 0;
	VFile *f = vf_open(path, "rb");
	if (f) { len = (int)vf_read(f, lb, 65535); vf_close(f); }
	lb[len > 0 ? len : 0] = 0;
	auto listed = [&](const char *l, int *bad) {   // an unreadable= / bad= line of one of the sectors
		unsigned v;
		int u = sscanf(l, "unreadable=%u", &v) == 1, b = !u && sscanf(l, "bad=%u", &v) == 1;
		if (!u && !b) return false;
		for (int i = 0; i < n; i++) if (lbas[i] == v) { *bad = b; return true; }
		return false;
	};
	int nu = 0, nb = 0;
	for (char *l = lb; *l; ) {
		size_t ll = strcspn(l, "\n");
		int b;
		if (listed(l, &b)) { if (b) nb++; else nu++; }
		l += ll + (l[ll] == '\n');
	}
	if (nu + nb) {
		VFile *of = vf_open(path, "wb");
		if (of) {
			for (char *l = lb; *l; ) {
				size_t ll = strcspn(l, "\n");
				unsigned v;
				int b;
				if (listed(l, &b)) {}
				else if (sscanf(l, "unreadable_count=%u", &v) == 1) vf_printf(of, "unreadable_count=%u\n", v > (unsigned)nu ? v - nu : 0);
				else if (sscanf(l, "bad_count=%u", &v) == 1) vf_printf(of, "bad_count=%u\n", v > (unsigned)nb ? v - nb : 0);
				else if (sscanf(l, "fixed_count=%u", &v) == 1) vf_printf(of, "fixed_count=%u\n", v + nu + nb);
				else { vf_write(of, l, ll); vf_write(of, "\n", 1); }
				l += ll + (l[ll] == '\n');
			}
			vf_close(of);
			logf("redo: %d unreadable and %d bad sector(s) out of %s", nu, nb, path);
		}
	}
	free(lb);
}

// The audit's recheck after an undo wrote sectors back into `file` (the image, .bin): the sectors
// go back into its log (a redo: out of it again), then the image is checked again.
static void recheck_hook(const char *file, const uint32_t *lbas, const uint8_t *zero, int n, int redo)
{
	char base[320];
	snprintf(base, sizeof(base), "%s", file);
	size_t len = strlen(base);
	if (len > 4 && !strcmp(base + len - 4, ".bin")) base[len - 4] = 0;
	if (n > 0 && lbas && zero) {
		if (redo) riplog_take_out(base, lbas, n);
		else riplog_put_back(base, lbas, zero, n);
	}
	if (g_check_ui) {   // the audit screen's undo: reading the whole image takes a while, the screen says so
		const char *slash = strrchr(base, '/');
		snprintf(g_wait_name, sizeof(g_wait_name), "%s", slash ? slash + 1 : base);
		audit_wait_tick(0);
		g_check_tick = audit_wait_tick;
	}
	check_image(base);
	g_check_tick = NULL;
	// a drive's card showing this rip: its Redump state and known errors follow the log at once
	for (int c = 0; c < NCARDS; c++) {
		Card &k = g_cards[c];
		if (strcmp(k.exist, base) && strcmp(k.base, base)) continue;
		if (k.state == DISC_READY && !card_busy(c)) { OnCard o(c); prev_rip_check(); }
		else k.recheck = 1;   // busy: the home screen looks again when it is idle
	}
	g_redraw = 2;   // the tabs' Redump icons follow the log
}

// Checks an earlier rip against Redump from its file, without the disc (check_image). Runs as the
// card's job; the tab and the page show its progress.
static void check_job_thread(void *arg)
{
	t_card = (int)(long)arg;
	Card &k = g_cards[t_card];
	char base[256];
	snprintf(base, sizeof(base), "%s", k.exist);
	g_phase = 4; g_done = 0; g_total = 0; g_readpos = 0; g_pass2_total = 0;
	g_rd_result = -1; g_rd_result_name[0] = 0;
	int r = check_image(base, 1);
	if (r == 0) {
		audit_card_redump(t_card);
		fire_event("check_finished");
	}
	audit_card_end(t_card, r == 0 ? "finished" : r == 1 ? "cancelled" : "failed");
	prev_rip_check();   // the icon and the status follow the log
	g_state = ST_HOME;
	g_phase = 0;
	__sync_synchronize();
	k.job = 0;
	g_redraw = 2;
	sysThreadExit(0);
}

static void check_job_start(int c, AuditSrc src)
{
	Card &k = g_cards[c];
	if (k.job || !k.exist[0]) return;
	job_reset(c);
	k.job = 1;
	k.res[0] = 0; k.res_rd = -2;
	k.eta_t0 = 0;
	OnCard o(c);
	g_state = ST_RIPPING;   // busy (status, deploy); the drive is not used
	g_abort = 0; g_finished = 0; g_skip = 0;
	audit_card_begin(c, src, "check");
	sys_ppu_thread_t t;
	if (sysThreadCreate(&t, check_job_thread, (void *)(long)c, 1000, 0x10000, 0, (char *)"psxcheck")) { audit_card_end(c, "failed"); k.job = 0; g_state = ST_HOME; }
}

// A rip in the background, for one card: the home screen keeps running and shows it in the card.
static void rip_job_thread(void *arg)
{
	t_card = (int)((long)arg >> 8);
	int fresh = (int)((long)arg & 0xFF);
	Card &k = g_cards[t_card];
	k.res[0] = 0; k.res_rd = -2;
	snprintf(k.res_disc, sizeof(k.res_disc), "%s (%s)", k.rd_name[0] ? k.rd_name : k.title, k.serial);
	snprintf(k.res_key, sizeof(k.res_key), "%s", k.serial[0] ? k.serial : k.title);
	k.rd_done[0] = 0;
	k.eta_t0 = 0;
	if (fresh) usleep(2000 * 1000);   // give the drive a moment to spin up
	g_done = 0; g_total = 0; g_readpos = 0; g_phase = 0; g_pass2_total = 0;
	logf("start rip (%s)", fresh ? "disc inserted" : "disc already in drive");
	int ok = rip_disc(fresh, 1);
	char image[300];
	snprintf(image, sizeof(image), "%s.bin", g_base);   // the name its sector rows use
	audit_card_end(t_card, rip_result(ok), image);
	g_state = ST_HOME;     // also after an early failure
	k.state = DISC_NONE;   // ejected or ripped: identify again
	k.tried = 0;
	__sync_synchronize();
	k.job = 0;
	g_redraw = 2;
	sysThreadExit(0);
}

// A new job on card c starts from a clean page: no events, counts or progress from the last one.
static void job_reset(int c)
{
	OnCard o(c);
	g_nev = 0; g_ev_seq++;
	g_done = 0; g_total = 0; g_readpos = 0; g_phase = 0;
	g_pass2_total = 0; g_pass2_done = 0; g_rr_max = 0; g_rr_try = 0;
	g_readerrs = 0; g_recovered = 0; g_badsect = 0;
	g_cards[c].pg_scroll = 0;
	g_cards[c].eta_t0 = 0;
	memset(g_md5_known[c], 0, sizeof(g_md5_known[0])); memset(g_md5_dirty[c], 0, sizeof(g_md5_dirty[0]));
	g_disc_lost[c] = 0;
	g_md5_total[c] = 0; g_md5_done[c] = 0;
}

static void rip_job_start(int c, AuditSrc src, int fresh)
{
	Card &k = g_cards[c];
	if (k.job) return;
	if (!g_parallel && any_ripping()) { own_dialog(T(STR_REPAIR_WAIT), 0); return; }   // one at a time then
	job_reset(c);
	k.job = 1;
	OnCard o(c);
	g_state = ST_RIPPING;   // busy from now on (status, the poll thread, deploy)
	g_abort = 0; g_finished = 0; g_skip = 0;
	audit_card_begin(c, src, "rip");
	sys_ppu_thread_t t;
	if (sysThreadCreate(&t, rip_job_thread, (void *)(long)(fresh | c << 8), 1000, 0x10000, 0, (char *)"psxjob")) {
		audit_card_end(c, "failed");
		k.job = 0; g_state = ST_HOME;
	}
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
	t_card = (int)(long)arg;
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
	int r = repair_disc_inner();
	if (g_use == SRC_BD) lv1_storage_unpatch();   // whatever happened
	return r;
}

// A repair in the background, for one card, like a rip.
static void repair_job_thread(void *arg)
{
	t_card = (int)(long)arg;
	Card &k = g_cards[t_card];
	logf("start repair");
	int ok = repair_disc();
	audit_card_end(t_card, repair_result(ok));
	g_state = ST_HOME;
	k.state = DISC_NONE;   // identify again: the log changed
	k.tried = 0;
	__sync_synchronize();
	k.job = 0;
	k.repairing = 0;
	g_redraw = 2;
	sysThreadExit(0);
}

static void repair_job_start(int c, AuditSrc src)
{
	Card &k = g_cards[c];
	if (k.job) return;
	job_reset(c);
	k.job = 1;
	k.repairing = 1;
	k.res[0] = 0; k.res_rd = -2;
	snprintf(k.res_disc, sizeof(k.res_disc), "%s (%s)", k.rd_name[0] ? k.rd_name : k.title, k.serial);
	snprintf(k.res_key, sizeof(k.res_key), "%s", k.serial[0] ? k.serial : k.title);
	k.eta_t0 = 0;
	OnCard o(c);
	g_state = ST_RIPPING;   // busy (status, the poll thread, deploy)
	g_abort = 0; g_finished = 0; g_skip = 0;
	audit_card_begin(c, src, "repair");
	sys_ppu_thread_t t;
	if (sysThreadCreate(&t, repair_job_thread, (void *)(long)c, 1000, 0x10000, 0, (char *)"psxrepairjob")) { audit_card_end(c, "failed"); k.job = 0; k.repairing = 0; g_state = ST_HOME; }
}

static int repair_disc_inner()
{
	while (g_ident_busy) usleep(10000);
	int err = 0, code = 0;
	if (prepare_disc(&err, &code)) { if (g_use == SRC_BD) lv1_storage_unpatch(); return 0; }
	if (g_cards[cur_card()].exist[0]) snprintf(g_base, sizeof(g_base), "%s", g_cards[cur_card()].exist);   // maybe another name
	char path[320];
	snprintf(path, sizeof(path), "%s.log", g_base);
	VFile *lf = vf_open(path, "rb");
	// its own buffer: riplog_open_count() below reads the log into CX.log_buf and would overwrite
	// this one while it is being parsed (that once wrote a log without its sector list)
	static char rbuf[NCARDS][65536];
	char (&buf)[65536] = rbuf[cur_card()];
	int n = lf ? (int)vf_read(lf, buf, sizeof(buf) - 1) : 0;
	if (lf) vf_close(lf);
	buf[n > 0 ? n : 0] = 0;
	snprintf(path, sizeof(path), "%s.bin", g_base);
	g_out = vf_open(path, "r+b");
	if (!g_out || !n) {
		if (g_out) vf_close(g_out);
		sys_storage_close(g_fd);
		if (g_use == SRC_BD) lv1_storage_unpatch();
		Card &k = g_cards[cur_card()];
		snprintf(k.res, sizeof(k.res), "%s", T(STR_CREATE_FAILED)); k.res_col = 0xFF8A80; k.res_rd = -2; k.res_failed = 1;
		return 0;
	}
	if (!riplog_open_count()) {
		// nothing listed: the previous rip stands as it is
		vf_close(g_out);
		sys_storage_close(g_fd);
		if (g_use == SRC_BD) lv1_storage_unpatch();
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
	g_nsnap = 0; g_patch_min = 0xFFFFFFFFu;   // no midstates: the MD5s are recomputed from the start
	edc_init(); ecc_init();
	g_nhard = 0; g_nsuspect = 0; g_confirmed = 0; g_changed = 0; g_readerrs = 0; g_badsect = 0; g_recovered = 0;
	g_pass2_total = 0; g_pass2_done = 0; g_abort = 0; g_finished = 0; g_rd_result = -3; g_rd_result_name[0] = 0;
	tstart_init();
	uint32_t junk = pregap_junk_in(g_out, buf);   // before strtok cuts the log up
	memset(g_md5_known[cur_card()], 0, sizeof(g_md5_known[0]));
	memset(g_md5_dirty[cur_card()], 0, sizeof(g_md5_dirty[0]));
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
			char hex[40] = "";
			const char *mp = strstr(l, " md5=");
			if (mp) sscanf(mp + 5, "%32s", hex);
			for (int t = 0; t < g_ntracks; t++) if (g_tracks[t].num == (int)tn) {
				g_tstart[t] = st;
				// the earlier rip's MD5 of this track: kept unless the repair writes into the track
				if (strlen(hex) == 32) {
					for (int i = 0; i < 16; i++) { unsigned v; sscanf(hex + i * 2, "%2x", &v); g_track_md5bin[t][i] = (uint8_t)v; }
					g_md5_known[cur_card()][t] = 1;
				}
			}
		}
	}
	if (junk) {   // an older rip's junk pregap sector: read it again, it is stored as silence now
		note_hard(junk, 0); g_readerrs++;
		logf("repair: sector %u is junk from the drive (first pregap sector of track 2), reading it again", junk);
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
	sysThreadCreate(&tid, repair_thread, (void *)(long)cur_card(), 1000, 0x10000, THREAD_JOINABLE, (char *)"psxrepair");
	g_nev = 0;
	while (!g_finished) {   // the drive's tab shows the progress; this thread just waits
		if (exit_requested) g_abort = 1;
		usleep(100 * 1000);
	}
	u64 rv;
	sysThreadJoin(tid, &rv);
	g_check = check;
	if (g_eject) { uint8_t cdb[12] = {0x1B, 0, 0, 0, 0x02, 0, 0, 0, 0, 0, 0, 0}; atapi(cdb, NULL, 0, ATAPI_NON_DATA_PROTO); }
	sys_storage_close(g_fd);
	if (g_use == SRC_BD) lv1_storage_unpatch();
	for (int t = 0; t < g_ntracks; t++) md5_hex(g_track_md5bin[t], g_track_md5[t]);   // fresh when patched
	char rdinfo[RD_NAME + 80] = "";
	if (g_redump != RD_OFF) { redump_verify(rdinfo, sizeof(rdinfo)); audit_card_redump(cur_card()); }
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
	{   // the tab: how the repair went
		Card &k = g_cards[cur_card()];
		snprintf(k.res, sizeof(k.res), T(STR_REPAIRED), g_recovered + g_changed, g_pass2_total);
		k.res_col = g_readerrs || g_badsect ? 0xF0C060 : 0x7CE0A0;
		k.res_failed = 0;
		k.res_rd = g_rd_result == 1 ? 1 : g_rd_result == 0 ? 0 : g_redump == RD_OFF ? -2 : -1;
		k.res_bad = g_readerrs || g_badsect;
	}
	if (g_disc_lost[cur_card()]) {   // the disc left the drive: what was written stays, the rest was not tried
		Card &k = g_cards[cur_card()];
		snprintf(k.res, sizeof(k.res), "%s", T(STR_CARD_DISC_LOST));
		k.res_col = 0xFF8A80;
	}
	g_state = ST_HOME;
	fire_event(g_disc_lost[cur_card()] ? "disc_lost" : "repair_finished");
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
static void own_dialog_draw(const char *msg, int yesno)
{
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
}
static int own_dialog(const char *msg, int yesno)
{
	{
		UiLock lock;
		snprintf(g_ui.dlg_text, sizeof(g_ui.dlg_text), "%s", msg);
		g_ui.dlg_q = yesno;
		g_ui.dlg_open = 1;
	}
	pad_pressed();
	g_redraw = 2;
	int a = 2;
	while (!exit_requested) {
		if (g_redraw > 0) {
			g_redraw = 0;
			own_dialog_draw(msg, yesno);
			g_drawn = 2;
			g_backdrop_shown = 0;
		}
		flip_frame();
		int p = pad_pressed();
		if (p & B_CROSS) { a = 1; break; }
		if (yesno && (p & B_CIRCLE)) break;
	}
	{ UiLock lock; g_ui.dlg_open = 0; }
	return a;
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
		ui_at(SCR_RESULT);
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

// The app's locks (the UI tests create them too).
static void locks_init()
{
	sys_mutex_attr_t ma;
	sysMutexAttrInitialize(ma);
	sysMutexCreate(&g_rd_mx, &ma);
	sysMutexCreate(&g_idx_mx, &ma);
	sysMutexCreate(&g_ui_mx, &ma);
	ma.attr_recursive = SYS_MUTEX_ATTR_RECURSIVE;
	sysMutexCreate(&g_cfg_mx, &ma);
}

int main(int argc, char *argv[])
{
	void *host_addr = memalign(1024 * 1024, HOST_SIZE);
	init_screen(host_addr, HOST_SIZE);
	ui_init();
	ioPadInit(7);
	atexit(program_exit_callback);
	sysUtilRegisterCallback(SYSUTIL_EVENT_SLOT0, sysutil_callback, NULL);
	for (int c = 0; c < NCARDS; c++) ctx_init(c);
	{   // each thread must see its own t_card, or the cards' rips would share one context
		static volatile int seen = -2;
		struct P { static void run(void *) { seen = t_card; t_card = 2; sysThreadExit(0); } };
		t_card = 1;
		sys_ppu_thread_t th;
		u64 rv;
		if (sysThreadCreate(&th, P::run, NULL, 1000, 0x4000, THREAD_JOINABLE, (char *)"psxtls") == 0) sysThreadJoin(th, &rv);
		g_parallel = seen == -1 && t_card == 1;
		t_card = -1;
		logf("thread-local storage %s (thread saw %d)", g_parallel ? "ok: parallel rips" : "NOT working: one rip at a time", seen);
	}
	locks_init();
	lang_init();
	netInitialize();
	// start-up checks: config, lv1 state after an earlier crash, USB file systems
	char warn[400] = "";
	audit_init("/dev_hdd0/tmp/psxrip_audit", &vf_io);
	audit_set_hooks(set_setting_hook, recheck_hook);
	audit_set_progress(audit_undo_tick);
	int had_cfg = cfg_load();
	audit_apply_cap();   // also without a config file (the default size)
	cfg_prime();   // so the first change after the boot is logged
	card_defaults(g_cards[0]);
	if (g_bdcfg.set) card_from(g_cards[0], g_bdcfg);
	if (!had_cfg) { mkdir("/dev_hdd0/tmp", 0777); cfg_save(); logf("startup: no config, defaults written"); }   // editable over FTP
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
		static const RemoteHooks hooks = {status_json, debug_cmd, remote_press};
		remote_init(&hooks);
		mcp_init(&g_mcp_hooks, APP_VERSION);
		remote_apply();
	}
	// card 0 = the PS3's drive: its destination is the "outdir" of the config (the hard disk while
	// a chosen USB drive is out); the USB drives get theirs when they connect
	snprintf(g_cards[0].outdir, sizeof(g_cards[0].outdir), "%s", g_outdir);
	snprintf(g_cards[0].dest, sizeof(g_cards[0].dest), "%s", dest_available(g_outdir) ? g_outdir : HDD_DIR);
	g_cards[0].present = 1;   // assume a disc until the drive says otherwise
	select_card(0);
	{ struct stat gs; ui_guides = stat("/dev_hdd0/tmp/psxrip.guides", &gs) == 0; }
#ifdef PRIVATE_SBI
	install_missing_sbis();
#endif

	logf("---- PSX Ripper start ----");
	logf("version %s", APP_VERSION);
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
	{ struct stat ds; g_debug_on = stat("/dev_hdd0/tmp/psxrip.debug", &ds) == 0; if (g_debug_on) logf("debug commands on (/dbg)"); }
	usbodd_init();   // USB SuperDrives
	{ sys_ppu_thread_t t; sysThreadCreate(&t, usb_presence_thread, NULL, 1500, 0x4000, 0, (char *)"psxusbpoll"); }
	{ struct stat ps; if (stat("/dev_hdd0/tmp/psxrip.usbprobe", &ps) == 0) { logf("usbprobe: start"); usb_optical_probe(); logf("usbprobe: done"); } }
	sysDiscRegisterDiscChangeCallback(disc_eject_cb, disc_insert_cb);
	while (!exit_requested) {
		int a = home_screen();   // rips run on their cards, from the home screen
		if (exit_requested || a == 0) break;
		if (a == 3) {
			repair_disc();
			result_screen();
			g_disc_state = DISC_NONE;
			g_cards[g_card].tried = 0;   // identify it again (the log changed)
		}
	}
	// cancel what still rips and wait for it: the files are cleaned up and lv1 unpatched
	for (int c = 0; c < NCARDS; c++) if (g_cards[c].job) { OnCard o(c); g_abort = 1; }
	for (int c = 0; c < NCARDS; c++) while (g_cards[c].job) usleep(50 * 1000);
	sysDiscUnregisterDiscChangeCallback();
	while (g_ident_busy) usleep(10000);
	if (g_ripped && !exit_requested && ask_yesno(T(STR_ASK_REFRESH))) {
		if (webman_get("/refresh.ps3")) tell(T(STR_REFRESH_MANUAL));
	}
	lv1_storage_unpatch();
	logf("---- PSX Ripper exit ----");
	return 0;
}
