// UI tests on the host: the app's own main.cpp, built against stubs (tests/host/include), draws
// scenarios into the canvas. Each frame is written as a PNG (tests/host/out) and checked:
//  - texts that overlap other texts,
//  - text outside the screen or, above the general row, outside the content area,
//  - characters the font does not have (they come out as "?").
// Run: make -C tests/host   (exit status 1 when a check fails)
#include "host.h"
#define CFG_PATH "out/psxrip.cfg"   // not the console's path
#define main psx_main
#include "../../source/main.cpp"
#undef main
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#include <vector>
#include <string>

struct Box { int kind, x, y, w, h, extra; std::string text; };
static std::vector<Box> g_boxes;
static void trace(int kind, int x, int y, int w, int h, const char *text, int extra)
{
	g_boxes.push_back({kind, x, y, w, h, extra, text ? text : ""});
}

static int g_fail = 0;
static void problem(const char *scen, const char *fmt, ...)
{
	if (!strcmp(scen, "self-test")) { g_fail++; return; }   // counted, not printed
	va_list ap;
	va_start(ap, fmt);
	printf("  FAIL %s: ", scen);
	vprintf(fmt, ap);
	printf("\n");
	va_end(ap);
	g_fail++;
}

// the checks over everything one frame drew
static void check(const char *scen)
{
	int W = ui_w(), H = ui_h(), cl = W / 8 - W / 40, cr = W * 7 / 8 + W / 40;
	int general = hint_y() - ui_line(UI_BODY) * 3 / 2;   // the general row; below it: the hint bars
	std::vector<Box> t;
	for (auto &b : g_boxes) if (b.kind == UI_TRACE_TEXT && b.w > 0 && !b.text.empty() && b.text != " ") t.push_back(b);
	for (auto &b : t) {
		if (b.extra) problem(scen, "%d character(s) not in the font: \"%s\"", b.extra, b.text.c_str());
		if (b.x < 0 || b.y < 0 || b.x + b.w > W || b.y + b.h > H) problem(scen, "off the screen: \"%s\" (%d,%d %dx%d)", b.text.c_str(), b.x, b.y, b.w, b.h);
		else if (b.y + b.h <= general && (b.x < cl || b.x + b.w > cr)) problem(scen, "outside the content area: \"%s\" (x %d..%d)", b.text.c_str(), b.x, b.x + b.w);
	}
	// glyph boxes are the font's line height; the visible ink is smaller, so a few pixels of
	// vertical overlap between lines are fine
	for (size_t i = 0; i < t.size(); i++)
		for (size_t j = i + 1; j < t.size(); j++) {
			const Box &a = t[i], &b = t[j];
			int ox = (a.x + a.w < b.x + b.w ? a.x + a.w : b.x + b.w) - (a.x > b.x ? a.x : b.x);
			int oy = (a.y + a.h < b.y + b.h ? a.y + a.h : b.y + b.h) - (a.y > b.y ? a.y : b.y);
			int lim = (a.h < b.h ? a.h : b.h) / 3;
			if (ox > 2 && oy > lim) problem(scen, "overlap: \"%s\" and \"%s\"", a.text.c_str(), b.text.c_str());
		}
}

static void save(const char *name)
{
	const uint32_t *c = ui_canvas();
	int W = ui_w(), H = ui_h();
	std::vector<unsigned char> rgb((size_t)W * H * 3);
	for (int i = 0; i < W * H; i++) { rgb[i * 3] = c[i] >> 16; rgb[i * 3 + 1] = c[i] >> 8; rgb[i * 3 + 2] = c[i]; }
	char path[300];
	snprintf(path, sizeof(path), "out/%s.png", name);
	stbi_write_png(path, W, H, 3, rgb.data(), W * 3);
}

// ---------------------------------------------------------------- scenario building blocks
static void reset_all()
{
	for (int c = 0; c < NCARDS; c++) {
		Card &k = g_cards[c];
		memset((void *)&k, 0, sizeof(k));
		card_defaults(k);
		k.rd_state = -1;
		snprintf(k.dest, sizeof(k.dest), "/dev_hdd0/PSXISO");
		snprintf(k.outdir, sizeof(k.outdir), "/dev_hdd0/PSXISO");
		OnCard o(c);
		g_nev = 0; g_done = g_total = g_readpos = 0; g_phase = 0; g_pass2_total = g_pass2_done = 0;
		g_rr_max = 0; g_readerrs = g_recovered = g_badsect = 0; g_abort = 0; g_skip = 0;
	}
	for (int i = 0; i < USBODD_DRIVES; i++) host_usb[i] = HostUsb{0, -1, ""};
	g_webhook_on = 1; snprintf(g_webhook, sizeof(g_webhook), "http://192.168.1.10:8123/x"); g_webhook_mask = (1 << WH_COUNT) - 1;
}
static void usb(int i, int port) { host_usb[i].attached = 1; host_usb[i].port = port; snprintf(host_usb[i].name, sizeof(host_usb[i].name), "Apple SuperDrive"); }
static void disc(int c, const char *title, const char *rd, const char *serial, unsigned mb)
{
	Card &k = g_cards[c];
	k.present = 1; k.state = DISC_READY;
	snprintf(k.title, sizeof(k.title), "%s", title);
	snprintf(k.rd_name, sizeof(k.rd_name), "%s", rd);
	snprintf(k.serial, sizeof(k.serial), "%s", serial);
	k.mb = mb;
	name_from(g_name_pat, k.rd_name, k.title, k.serial, k.fname, sizeof(k.fname));
}
static void ripped(int c, int rd_state, int errors)
{
	Card &k = g_cards[c];
	k.prev_rip = 1; k.rd_state = rd_state; k.repair_n = errors;
	snprintf(k.exist, sizeof(k.exist), "/dev_hdd0/PSXISO/%s", k.fname);
}
static void ripping(int c, uint32_t done, uint32_t total, int phase, int events, int rereads)
{
	Card &k = g_cards[c];
	k.job = 1;
	OnCard o(c);
	g_done = done; g_total = total; g_phase = phase;
	if (phase == 1 || phase == 2) { g_pass2_total = 279; g_pass2_done = 3; }
	if (rereads) { g_rr_lba = 237637; g_rr_try = 1; g_rr_max = 15; }
	g_readerrs = events; g_recovered = 2;
	if (phase == 1) rip_event(0xA8B0C8, STR_EV_VERIFY_START, 41237);
	for (int i = 0; i < events; i++) rip_event(i % 3 ? 0xF0C060 : 0x7CE0A0, i % 3 ? STR_EV_UNREADABLE : STR_EV_RECOVERED, 235847 + i, "");
}
static void frame(const char *name, int sel)
{
	int list[NCARDS], nc = card_list(list);
	g_boxes.clear();
	home_draw(list, nc, sel);
	save(name);
	size_t before = g_fail;
	check(name);
	printf("%s %s\n", g_fail == (int)before ? "ok  " : "FAIL", name);
}

// ---------------------------------------------------------------- the audit log screen
static int g_sfx_sv;   // the Swedish frames get "-sv" after their name
static void aframe(const char *name, const AuditView &v)
{
	char nm[96];
	snprintf(nm, sizeof nm, "%s%s", name, g_sfx_sv ? "-sv" : "");
	g_boxes.clear();
	audit_draw(v);
	save(nm);
	size_t before = g_fail;
	check(nm);
	printf("%s %s\n", g_fail == (int)before ? "ok  " : "FAIL", nm);
}
static void put_bin(const char *p, const void *d, size_t n) { FILE *f = fopen(p, "wb"); if (f) { fwrite(d, 1, n, f); fclose(f); } }
static bool has_text(const char *s)
{
	for (auto &b : g_boxes) if (b.kind == UI_TRACE_TEXT && strstr(b.text.c_str(), s)) return true;
	return false;
}

// ---------------------------------------------------------------- game list: sources, labels, the scan
// The console's drives under out/gl (the stubs map /dev_... and exfatN:/... there): the hard disk,
// a FAT32 stick on port 1 and an exFAT drive on port 3, each with a set in its PSXISO.
static void gl_text(const char *p, const char *s) { put_bin(p, s, strlen(s)); }
static void gl_set(const char *dir, const char *name)
{
	char p[300], cue[400];
	snprintf(p, sizeof p, "%s/%s.bin", dir, name);
	static uint8_t sec[2 * RAW_SIZE];
	put_bin(p, sec, sizeof sec);
	snprintf(p, sizeof p, "%s/%s.cue", dir, name);
	snprintf(cue, sizeof cue, "FILE \"%s.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n", name);
	gl_text(p, cue);
}
static void gl_drives()
{
	system("rm -rf out/gl && mkdir -p out/gl/dev_hdd0/PSXISO out/gl/dev_usb001/PSXISO out/gl/exfat3/PSXISO");
	snprintf(host_root, 256, "out/gl");
	memset(host_exfat, 0, sizeof(int) * 8);
	host_exfat[3] = 1;
	for (int i = 0; i < 8; i++) host_label[i][0] = 0;
	snprintf(host_label[1], 24, "RIPS");
	snprintf(host_label[3], 24, "GAMES");
	gl_set("out/gl/dev_hdd0/PSXISO", "Alundra (Europe) [SLES_011.35]");
	gl_set("out/gl/dev_usb001/PSXISO", "Croc 2 (Europe) [SLES_020.88]");
	gl_set("out/gl/exfat3/PSXISO", "Motorhead (USA) [SLUS_007.17]");
}
static void gl_old(const char *dir) { struct timeval tv[2] = {{time(NULL) - 3600, 0}, {time(NULL) - 3600, 0}}; utimes(dir, tv); }
static int gl_take(const char *key) { gl_open(key); gl_poll(); return g_gl.n; }   // the host has no threads: the scan runs at once
static int gl_has(const char *title)
{
	for (int i = 0; i < g_gl.n; i++) if (strstr(g_gl.d[i].base, title)) return i;
	return -1;
}
// A file interface that removes the folder it scans after the first file it reads (a stick pulled
// in the middle of a scan).
static const char *g_gl_pull;
static int gl_pull_read(const char *p, char *buf, int max)
{
	if (g_gl_pull) { char c[300]; snprintf(c, sizeof c, "rm -rf %s", g_gl_pull); system(c); g_gl_pull = NULL; }
	VFile *f = vf_open(p, "rb");
	if (!f) return -1;
	int n = (int)vf_read(f, buf, max);
	vf_close(f);
	return n;
}

// A listing after which the drive is pulled (exFAT on port 3 gone, its folder with it).
static int gl_pull_list(const char *dir, void (*cb)(const char *, void *), void *ctx)
{
	int n = vf_listdir(dir, cb, ctx);
	if (g_gl_pull && strstr(dir, "exfat3")) {
		char c[300];
		snprintf(c, sizeof c, "rm -rf %s", g_gl_pull);
		system(c);
		g_gl_pull = NULL;
		host_exfat[3] = 0;
	}
	return n;
}

static void game_source_tests()
{
	int bad = 0;
	host_lang = PS3_LANG_ENGLISH_US; lang_init();
	gl_drives();
	{   // the sources: the same candidates as next_target, each with its label, then "All"
		GlSource s[GL_SRCS];
		int n = gl_sources(s, GL_SRCS);
		const char *keys[] = {"hdd", "usb1", "usb3", "all"};
		const char *dirs[] = {"/dev_hdd0/PSXISO", "/dev_usb001/PSXISO", "exfat3:/PSXISO", ""};
		const char *names[] = {"Hard disk", "USB 1 (RIPS)", "USB 3 (GAMES)", "All"};
		if (n != 4) { printf("FAIL %d game sources, want 4\n", n); bad++; }
		else for (int i = 0; i < 4; i++)
			if (strcmp(s[i].key, keys[i]) || strcmp(s[i].dir, dirs[i]) || strcmp(s[i].name, names[i]))
				{ printf("FAIL source %d: %s %s \"%s\", want %s %s \"%s\"\n", i, s[i].key, s[i].dir, s[i].name, keys[i], dirs[i], names[i]); bad++; }
		char od[64] = "/dev_hdd0/PSXISO";
		for (int i = 1; i < 4 && n == 4; i++) {
			next_target(od, 1);
			if (strcmp(od, i < 3 ? dirs[i] : dirs[0])) { printf("FAIL next_target step %d: %s\n", i, od); bad++; }
		}
		host_label[1][0] = 0;   // a stick without a label: just its port
		n = gl_sources(s, GL_SRCS);
		if (n != 4 || strcmp(s[1].name, "USB 1")) { printf("FAIL a source without a label: \"%s\"\n", n > 1 ? s[1].name : ""); bad++; }
		snprintf(host_label[1], 24, "RIPS");
		host_lang = PS3_LANG_SWEDISH; lang_init();
		n = gl_sources(s, GL_SRCS);
		if (n != 4 || strcmp(s[0].name, "Hårddisk") || strcmp(s[3].name, "Alla")) { printf("FAIL Swedish source names: \"%s\" \"%s\"\n", s[0].name, n > 3 ? s[3].name : ""); bad++; }
		host_lang = PS3_LANG_ENGLISH_US; lang_init();
	}
	{   // All: every source's sets, each with its place; one source: only its own
		int n = gl_take("all");
		int a = gl_has("Alundra"), c = gl_has("Croc 2"), m = gl_has("Motorhead");
		if (n != 3 || a < 0 || c < 0 || m < 0 || g_gl.gone || g_gl.more || strcmp(g_gl.key, "all") || g_gl.ng != 3)
			{ printf("FAIL All: %d discs, %d rows, gone %d\n", n, g_gl.ng, g_gl.gone); bad++; }
		else {
			const GlSource *pa = gl_place(&g_gl, &g_gl.d[a]), *pc = gl_place(&g_gl, &g_gl.d[c]), *pm = gl_place(&g_gl, &g_gl.d[m]);
			if (!pa || !pc || !pm || strcmp(pa->key, "hdd") || strcmp(pc->key, "usb1") || strcmp(pm->key, "usb3"))
				{ printf("FAIL the place of each disc in All: %s %s %s\n", pa ? pa->key : "-", pc ? pc->key : "-", pm ? pm->key : "-"); bad++; }
		}
		n = gl_take("usb3");
		if (n != 1 || gl_has("Motorhead") < 0 || g_gl.gone || strcmp(g_gl.key, "usb3")) { printf("FAIL one source: %d discs\n", n); bad++; }
	}
	{   // the cache: rescanned on a source switch, after a change, and when the hard disk's folder
		// changed; not when nothing did
		gl_old("out/gl/dev_hdd0/PSXISO");
		gl_take("hdd");
		uint32_t gen = g_gl.gen;
		gl_old("out/gl/dev_hdd0/PSXISO");
		if (gl_open("hdd")) { printf("FAIL the hard disk rescanned with nothing changed\n"); bad++; }
		gl_changed();
		if (!gl_open("hdd")) { printf("FAIL no rescan after a change\n"); bad++; }
		gl_poll();
		if (g_gl.gen != gen + 1) { printf("FAIL change counter %u, want %u\n", (unsigned)g_gl.gen, (unsigned)gen + 1); bad++; }
		gl_old("out/gl/dev_hdd0/PSXISO");
		if (!gl_open("usb1")) { printf("FAIL no rescan on a source switch\n"); bad++; }
		gl_poll();
		if (!gl_open("usb1")) { printf("FAIL a USB source is not rescanned on open (its folder dates are not kept)\n"); bad++; }
		gl_poll();
		gl_take("hdd");
		gl_old("out/gl/dev_hdd0/PSXISO");
		gl_set("out/gl/dev_hdd0/PSXISO", "Croc (Europe) [SLES_008.95]");   // copied in over FTP: the folder's date moves
		if (!gl_open("hdd")) { printf("FAIL no rescan after the folder changed\n"); bad++; }
		gl_poll();
		if (g_gl.n != 2) { printf("FAIL after the folder changed: %d discs\n", g_gl.n); bad++; }
	}
	{   // a source that disappears: gone, no crash; All goes on without it
		system("rm -rf out/gl/dev_usb001");
		int n = gl_take("usb1");
		if (n != 0 || !g_gl.gone) { printf("FAIL a pulled stick: %d discs, gone %d\n", n, g_gl.gone); bad++; }
		n = gl_take("all");
		if (n != 3 || g_gl.gone || gl_has("Croc 2") >= 0) { printf("FAIL All without the stick: %d discs, gone %d\n", n, g_gl.gone); bad++; }
		// the folder goes away in the middle of the scan (the drive itself is still there)
		gl_set("out/gl/exfat3/PSXISO", "Croc 2 (Europe) [SLES_020.88]");
		g_gl_pull = "out/gl/exfat3/PSXISO";
		LibIo io = gl_libio();
		io.read = gl_pull_read;
		GlResult *r = gl_build("usb3", &io);
		if (!r || r->n > 2 || r->gone) { printf("FAIL a folder removed mid-scan: %d discs, gone %d\n", r ? r->n : -1, r ? r->gone : -1); bad++; }
		gl_free(r);
		// the drive is pulled after its folder was listed: gone, no phantom broken sets; in All
		// the other sources stay and that one has none
		system("mkdir -p out/gl/dev_usb001/PSXISO");
		gl_set("out/gl/dev_usb001/PSXISO", "Croc 2 (Europe) [SLES_020.88]");
		gl_set("out/gl/exfat3/PSXISO", "Croc 2 (Europe) [SLES_020.88]");
		io.read = gl_io_read;
		io.list = gl_pull_list;
		g_gl_pull = "out/gl/exfat3/PSXISO";
		r = gl_build("usb3", &io);
		if (!r || r->n || !r->gone || r->oom) { printf("FAIL a drive pulled after its listing: %d discs, gone %d\n", r ? r->n : -1, r ? r->gone : -1); bad++; }
		gl_free(r);
		system("mkdir -p out/gl/exfat3/PSXISO");
		gl_set("out/gl/exfat3/PSXISO", "Croc 2 (Europe) [SLES_020.88]");
		host_exfat[3] = 1;
		g_gl_pull = "out/gl/exfat3/PSXISO";
		r = gl_build("all", &io);
		int u3 = -1;
		for (int i = 0; r && i < r->nsrc; i++) if (!strcmp(r->src[i].key, "usb3")) u3 = r->src[i].n;
		if (!r || r->n != 3 || r->gone || u3 != -1) { printf("FAIL All with a drive pulled after its listing: %d discs, gone %d, usb3 n %d\n", r ? r->n : -1, r ? r->gone : -1, u3); bad++; }
		for (int i = 0; r && i < r->n; i++) if (!strncmp(r->d[i].base, "exfat3:", 7)) { printf("FAIL a disc of the pulled drive in All: %s\n", r->d[i].base); bad++; }
		gl_free(r);
		// out of memory: its own state, not gone
		g_gl_alloc_max = 1024;
		host_exfat[3] = 1;
		system("mkdir -p out/gl/exfat3/PSXISO");
		io = gl_libio();
		r = gl_build("hdd", &io);
		if (!r || !r->oom || r->gone || r->n) { printf("FAIL out of memory: oom %d, gone %d, %d discs\n", r ? r->oom : -1, r ? r->gone : -1, r ? r->n : -1); bad++; }
		gl_free(r);
		g_gl_alloc_max = (size_t)-1;
		system("rm -rf out/gl/dev_usb001/PSXISO/*");
		system("rm -rf out/gl/exfat3");
		host_exfat[3] = 0;   // and then the drive itself
		n = gl_take("usb3");
		if (n != 0 || !g_gl.gone) { printf("FAIL a pulled exFAT drive: %d discs, gone %d\n", n, g_gl.gone); bad++; }
	}
	{   // more than 1000 sets in a source: the first 1000, and it says so
		system("mkdir -p out/gl/dev_usb001/PSXISO");
		for (int i = 0; i < GL_MAX + 5; i++) {
			char p[96];
			snprintf(p, sizeof p, "out/gl/dev_usb001/PSXISO/Game %04d.iso", i);
			put_bin(p, "", 0);
		}
		int n = gl_take("usb1");
		if (n != GL_MAX || !g_gl.more) { printf("FAIL more than %d sets: %d discs, more %d\n", GL_MAX, n, g_gl.more); bad++; }
	}
	{   // the config keys round-trip, and wrong values change nothing
		snprintf(g_games_source, sizeof g_games_source, "all");
		g_games_filter = GameFilter{0, 0, 0, 0};
		int ok = cfg_apply_line("games_source=usb3") && cfg_apply_line("games_filter=1,2,0,3");
		int rej = !cfg_apply_line("games_source=usb9") && !cfg_apply_line("games_source=dvd") && !cfg_apply_line("games_filter=3,0,0,0") &&
		          !cfg_apply_line("games_filter=0,5,0,0") && !cfg_apply_line("games_filter=0,0,3,0") && !cfg_apply_line("games_filter=0,0,0,4") &&
		          !cfg_apply_line("games_filter=1,2");
		char txt[4096];
		cfg_text(txt, sizeof txt);
		if (!ok || !rej || strcmp(g_games_source, "usb3") || g_games_filter.fmt != 1 || g_games_filter.redump != 2 || g_games_filter.other != 3 ||
		    !strstr(txt, "\ngames_source=usb3\n") || !strstr(txt, "\ngames_filter=1,2,0,3\n"))
			{ printf("FAIL games_source/games_filter: ok %d, rejected %d, %s\n", ok, rej, txt); bad++; }
		snprintf(g_games_source, sizeof g_games_source, "hdd");
		g_games_filter = GameFilter{0, 0, 0, 0};
		int back = cfg_apply_line("games_source=usb3") && cfg_apply_line("games_source=all") && cfg_apply_line("games_source=hdd") && cfg_apply_line("games_filter=0,0,0,0");
		if (!back || strcmp(g_games_source, "hdd")) { printf("FAIL games_source back to hdd\n"); bad++; }
	}
	host_root[0] = 0;
	memset(host_exfat, 0, sizeof(int) * 8);
	gl_take("hdd");   // nothing (the console's paths do not exist here)
	host_lang = PS3_LANG_SWEDISH; lang_init();
	if (bad) g_fail += bad; else printf("ok   game list sources, labels, All, the cache, a source that goes, games_* config keys\n");
}

// ---------------------------------------------------------------- game list: the screen
// A hard disk with one of every kind: a verified rip with its cover, unchecked rips, a game of four
// discs (one with errors, one a LibCrypt disc with its .sbi), a rip with errors, a 2048 .iso, a
// MediEvil that does not match (LibCrypt, no .sbi), a .bin without its cue, two copies of one disc,
// and a name that is not ASCII. Croc 2 also on a stick (USB 1, "RIPS").
static const char *GS_MD5 = "4f1c7d0b9e2a3c5d6e7f8091a2b3c4d5";
static void gs_set(const char *dir, const char *name, int sectors, const char *log)
{
	char p[300], cue[400];
	std::vector<uint8_t> img((size_t)sectors * RAW_SIZE);
	snprintf(p, sizeof p, "%s/%s.bin", dir, name);
	put_bin(p, img.data(), img.size());
	snprintf(p, sizeof p, "%s/%s.cue", dir, name);
	snprintf(cue, sizeof cue, "FILE \"%s.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n", name);
	gl_text(p, cue);
	if (!log) return;
	snprintf(p, sizeof p, "%s/%s.log", dir, name);
	gl_text(p, log);
}
static std::string gs_log(const char *title, const char *serial, const char *redump, const char *errors = "")
{
	char b[1200];
	snprintf(b, sizeof b, "PSX Ripper rip log\ntitle=%s\nserial=%s\ndate=2026-10-02 18:41 UTC\nsectors=3\ncheck=bitperfect\nretries=4\n"
	         "track=01 start=0 md5=%s\n%s%s%sunreadable_count=0\nbad_count=0\nfixed_count=0\n%s",
	         title, serial, GS_MD5, redump ? "redump=" : "", redump ? redump : "", redump ? "\n" : "", errors);
	return b;
}
static void gs_library()
{
	system("rm -rf out/gl && mkdir -p out/gl/dev_hdd0/PSXISO out/gl/dev_usb001/PSXISO out/gl/dev_hdd0/tmp/sbi");
	snprintf(host_root, 256, "out/gl");
	memset(host_exfat, 0, sizeof(int) * 8);
	for (int i = 0; i < 8; i++) host_label[i][0] = 0;
	snprintf(host_label[1], 24, "RIPS");
	const char *H = "out/gl/dev_hdd0/PSXISO";
	gs_set(H, "Alundra (Europe) [SLES_011.35]", 3, gs_log("Alundra", "SLES_011.35", "verified Alundra (Europe)").c_str());
	put_bin("out/gl/dev_hdd0/PSXISO/Alundra (Europe) [SLES_011.35].jpg", "jpg", 3);
	gs_set(H, "Ape Escape (Europe) [SCES_011.15]", 3, NULL);
	put_bin("out/gl/dev_hdd0/PSXISO/Castlevania Chronicles (USA) [SLUS_013.48].iso", std::vector<uint8_t>(3 * 2048).data(), 3 * 2048);
	gs_set(H, "Croc 2 (Europe) [SLES_020.88]", 3, NULL);
	gs_set("out/gl/dev_usb001/PSXISO", "Croc 2 (Europe) [SLES_020.88]", 3, NULL);
	const char *ff[] = {"SLES_029.65", "SLES_129.65", "SLES_229.65", "SLES_329.65"};
	for (int i = 0; i < 4; i++) {
		char n[96], t[64], r[96];
		snprintf(n, sizeof n, "Final Fantasy IX (Europe) (Disc %d) [%s]", i + 1, ff[i]);
		snprintf(t, sizeof t, "Final Fantasy IX (Disc %d)", i + 1);
		snprintf(r, sizeof r, "verified Final Fantasy IX (Europe) (Disc %d)", i + 1);
		gs_set(H, n, 3, gs_log(t, ff[i], i == 1 ? NULL : r, i == 1 ? "unreadable=120344\nunreadable=120345\nbad=130001\n" : "").c_str());
	}
	for (const char *id : ff) {   // every disc of it is a LibCrypt disc: their .sbi files are installed
		char p[96];
		snprintf(p, sizeof p, "out/gl/dev_hdd0/tmp/sbi/%s.sbi", id);
		put_bin(p, "sbi", 3);
	}
	gs_set(H, "MediEvil (Europe) [SCES_003.11]", 3, gs_log("MediEvil", "SCES_003.11", "no_match").c_str());
	gs_set(H, "Motorhead (USA) [SLUS_007.17]", 3, gs_log("Motorhead", "SLUS_007.17", NULL, "unreadable=1000\nunreadable=1001\nbad=2000\nuncertain=2100 unconfirmed\nuncertain=2101 confirmed\n").c_str());
	put_bin("out/gl/dev_hdd0/PSXISO/Tekken 3 (Europe) [SCES_012.37].bin", std::vector<uint8_t>(3 * RAW_SIZE).data(), 3 * RAW_SIZE);
	gs_set(H, "WipEout 3 (Europe) [SCES_028.45]", 3, NULL);
	gs_set(H, "WipEout 3 (Europe) (Copy) [SCES_028.45]", 3, NULL);
	gs_set(H, "\xC3\x89" "chapp\xC3\xA9" "e (France) [SLES_000.01]", 3, NULL);
	for (const char *n : {"Bushido Blade", "Driver", "Gran Turismo", "Hogs of War", "Klonoa", "Legend of Dragoon", "Oddworld", "Parasite Eve", "Rayman", "Spyro"})
		gs_set(H, n, 3, NULL);
}
static void gframe(const char *name, const GamesView &v)
{
	char nm[96];
	snprintf(nm, sizeof nm, "%s%s", name, g_sfx_sv ? "-sv" : "");
	g_boxes.clear();
	games_draw(v);
	save(nm);
	size_t before = g_fail;
	check(nm);
	printf("%s %s\n", g_fail == (int)before ? "ok  " : "FAIL", nm);
}
static const char *gs_focus(const GamesView &v) { return v.nshown ? g_gl.g[v.shown[v.row]].name : ""; }
static bool gs_at(const GamesView &v, const char *name) { return !strncmp(gs_focus(v), name, strlen(name)); }
static int gs_find(GamesView &v, const char *name)
{
	for (int i = 0; i < v.nshown; i++) if (!strncmp(g_gl.g[v.shown[i]].name, name, strlen(name))) { v.row = i; gv_fix(v); return 1; }
	return 0;
}
static void games_scenarios()
{
	int bad = 0, sv = g_sfx_sv;
	auto want = [&](bool ok, const char *what) { if (!ok) { printf("FAIL games (%s): %s\n", sv ? "sv" : "en", what); bad++; } };
	gs_library();
	system("rm -f out/psxrip.cfg");
	g_cfg_primed = false;
	snprintf(g_games_source, sizeof g_games_source, "hdd");
	g_games_filter = GameFilter{0, 0, 0, 0};
	g_gl_stale = 1;
	GamesView v;
	gv_open(v);
	want(!gv_ready(v), "ready before the scan was taken");
	gframe("games-reading", v);
	want(has_text(sv ? "Läser spelen" : "Reading the games"), "the reading text");
	gv_poll(v);
	want(gv_ready(v) && v.nshown == 21, "21 rows on the hard disk");
	gframe("games-list", v);
	want(has_text(sv ? "21 spel" : "21 games") && has_text(sv ? "1 verifierat" : "1 verified") && has_text(sv ? "2 med fel" : "2 with errors") &&
	     has_text(sv ? "1 rippa om" : "1 re-rip") && has_text(sv ? "1 trasigt" : "1 broken"),
	     "the summary");
	want(has_text(sv ? "Alla spel" : "All games"), "no criteria: all games");
	want(has_text(sv ? "4 skivor" : "4 discs"), "the multi-disc row");
	want(gs_at(v, "Alundra"), "first row Alundra");
	// statuses further down: errors, no match, broken, the duplicates
	gs_find(v, "MediEvil");
	gframe("games-list-statuses", v);
	want(has_text(sv ? "Matchar inte" : "Does not match") && has_text(sv ? "3 fel" : "3 errors") && has_text(sv ? ".sbi saknas" : ".sbi missing"),
	     "no match, errors, LibCrypt");
	gs_find(v, "WipEout 3");
	gframe("games-list-end", v);
	want(has_text(sv ? "Trasig" : "Broken") && has_text(sv ? "Dubblett" : "Duplicate") && has_text(sv ? "4 fel" : "4 errors") && has_text("(Copy) [SCES_028.45]"), "broken, duplicate (the other's file), errors");
	// jump by letter: R2 to the next letter, L2 back
	gs_find(v, "Alundra");
	gv_input(v, B_R2);
	want(gs_at(v, "Bushido Blade"), "R2 jumps from A to B");
	gv_input(v, B_R2); gv_input(v, B_R2);
	want(gs_at(v, "Driver"), "R2 twice more: D");
	gv_input(v, B_L2);
	want(gs_at(v, "Castlevania Chronicles"), "L2: the first of C");
	// each criterion
	struct { GameFilter f; int n; const char *line_en, *line_sv; } fc[] = {
		{{2, 0, 0, 0}, 1, "2048", "2048"},
		{{0, 1, 0, 0}, 2, "Verified", "Verifierad"},
		{{0, 3, 0, 0}, 1, "Does not match", "Matchar inte"},
		{{0, 0, 1, 0}, 2, "With errors", "Med fel"},
		{{0, 0, 0, 1}, 1, "Broken sets", "Trasiga"},
		{{0, 0, 0, 2}, 2, "Duplicates", "Dubbletter"},
		{{0, 0, 0, 3}, 1, "LibCrypt .sbi missing", "LibCrypt .sbi saknas"},
		{{1, 2, 0, 0}, 0, "2352 · Not checked", "2352 · Ej kontrollerad"},
	};
	for (auto &c : fc) {
		g_games_filter = c.f;
		gv_refilter(v);
		int n = lib_filter(g_gl.g, g_gl.ng, g_gl.d, &c.f, std::vector<int>(g_gl.ng).data());
		g_boxes.clear(); games_draw(v);
		char w[160];
		snprintf(w, sizeof w, "filter %d,%d,%d,%d: %d rows (lib %d), line \"%s\"", c.f.fmt, c.f.redump, c.f.errors, c.f.other, v.nshown, n, sv ? c.line_sv : c.line_en);
		want(v.nshown == n && (c.n == 0 || n == c.n) && has_text(sv ? c.line_sv : c.line_en), w);
	}
	g_games_filter = GameFilter{2, 0, 0, 0};
	gv_refilter(v);
	gframe("games-criteria", v);
	g_games_filter = GameFilter{0, 0, 1, 3};
	gv_refilter(v);
	gframe("games-no-match", v);
	want(v.nshown == 0 && has_text(sv ? "Inga spel matchar filtret" : "No games match the filter"), "nothing matches");
	gv_input(v, B_L1);   // L1: every criterion back to any, saved
	{
		char txt[4096];
		cfg_text(txt, sizeof txt);
		want(v.nshown == 21 && !g_games_filter.fmt && !g_games_filter.other && strstr(txt, "\ngames_filter=0,0,0,0\n"), "L1 resets and saves");
	}
	// the filter panel: R1 opens, right increases, O applies and saves
	gv_input(v, B_R1);
	want(v.mode == GV_FILTER, "R1 opens the filter panel");
	gv_input(v, B_RIGHT); gv_input(v, B_RIGHT);   // Format: 2048
	gv_input(v, B_DOWN); gv_input(v, B_RIGHT); gv_input(v, B_RIGHT);   // Redump: not checked
	gframe("games-filter", v);
	want(has_text("Format") && has_text("Redump") && has_text(sv ? "Övrigt" : "Other") && has_text(sv ? "Visa" : "Show"), "the panel's rows");
	want(g_games_filter.fmt == 0, "the panel applies only when closed");
	gv_input(v, B_CIRCLE);
	{
		char txt[4096];
		cfg_text(txt, sizeof txt);
		want(v.mode == GV_LIST && g_games_filter.fmt == 2 && g_games_filter.redump == 2 && strstr(txt, "\ngames_filter=2,2,0,0\n"), "O applies and saves");
	}
	gframe("games-criteria-two", v);
	want(has_text(sv ? "2048 · Ej kontrollerad" : "2048 · Not checked"), "the criteria line");
	gv_input(v, B_L1);
	// the detail view of an app rip: tracks and their MD5, Redump's name, the cover
	gs_find(v, "Alundra");
	gv_input(v, B_CROSS);
	want(v.mode == GV_DETAIL, "X opens the detail view");
	gframe("games-detail-rip", v);
	want(has_text(GS_MD5) && has_text("Alundra (Europe)") && has_text("SLES_011.35") && has_text(sv ? "Omslagsbild" : "Cover picture"), "the rip's details");
	gv_input(v, B_CIRCLE);
	want(v.mode == GV_LIST && gs_at(v, "Alundra"), "O back to the same row");
	gs_find(v, "Castlevania Chronicles");
	gv_input(v, B_CROSS);
	gframe("games-detail-iso", v);
	want(has_text(sv ? "Rippa om för att verifiera" : "Re-rip to verify") && has_text("2048"), "the .iso's details");
	gv_input(v, B_CIRCLE);
	gs_find(v, "Final Fantasy IX");
	gv_input(v, B_CROSS);
	gframe("games-detail-discs", v);
	want(has_text(sv ? "Skiva 1" : "Disc 1") && has_text(sv ? ".sbi finns" : ".sbi present"), "the group's first disc");
	want(!has_text("120344"), "disc 2's sectors not on the first page");
	for (int i = 0; i < 60 && !has_text("120344"); i++) { gv_input(v, B_DOWN); g_boxes.clear(); games_draw(v); }
	gframe("games-detail-discs-down", v);
	want(has_text("120344") && has_text(sv ? "Skiva 2" : "Disc 2"), "disc 2's sectors further down");
	gv_input(v, B_CIRCLE);
	gs_find(v, "Motorhead");
	gv_input(v, B_CROSS);
	gframe("games-detail-errors", v);
	gv_input(v, B_CIRCLE);
	gs_find(v, "Tekken 3");
	gv_input(v, B_CROSS);
	gframe("games-detail-broken", v);
	gv_input(v, B_CIRCLE);
	// SELECT: the next source (USB 1 (RIPS)), then All: each row with its place
	gv_input(v, B_SELECT);
	want(!strcmp(v.key, "usb1") && !strcmp(g_games_source, "usb1"), "SELECT: USB 1, remembered");
	gv_poll(v);
	gframe("games-usb", v);
	want(v.nshown == 1 && has_text("USB 1 (RIPS)"), "the stick's list and name");
	gv_input(v, B_SELECT);
	gv_poll(v);
	want(!strcmp(v.key, "all") && v.nshown == 22, "SELECT: All (Croc 2 twice: two places)");
	gs_find(v, "Croc 2");
	gframe("games-all", v);
	want(has_text(sv ? "Hårddisk" : "Hard disk") && has_text("USB 1 (RIPS)"), "All: the focused row's place and its duplicate's");
	// the trash view: "Show: Trash" in the panel; empty, then with a set
	gv_input(v, B_SELECT);
	gv_poll(v);
	want(!strcmp(v.key, "hdd"), "SELECT round to the hard disk");
	gv_input(v, B_R1);
	for (int i = 0; i < 4; i++) gv_input(v, B_DOWN);
	gv_input(v, B_RIGHT);
	gframe("games-filter-trash", v);
	gv_input(v, B_CIRCLE);
	want(v.trash == 1, "Show: Trash");
	gv_poll(v);
	gframe("games-trash-empty", v);
	want(gv_ready(v) && has_text(sv ? "Papperskorgen" : "trash"), "the empty trash");
	system("mkdir -p out/gl/dev_hdd0/PSXISO/.trash");
	gs_set("out/gl/dev_hdd0/PSXISO/.trash", "Croc (Europe) [SLES_008.95]", 3, NULL);
	gv_switch(v);   // read again (the actions of a later step do this after a move)
	gv_poll(v);
	gframe("games-trash", v);
	want(v.nshown == 1 && gs_at(v, "Croc"), "the trash's set");
	gv_input(v, B_R1);
	for (int i = 0; i < 4; i++) gv_input(v, B_DOWN);
	gv_input(v, B_LEFT);
	gv_input(v, B_CIRCLE);
	gv_poll(v);
	want(!v.trash && v.nshown == 21, "back to the games");
	// another requester's scan handed over to the screen (as /games.json will): it asks for its own
	// source again and shows it; a new result closes the detail view; no redraws without news
	gs_find(v, "Alundra");
	gv_input(v, B_CROSS);
	gl_request("usb1");
	gv_poll(v);
	want(!gv_ready(v) && !strcmp(g_gl.key, "usb1"), "another source's result is not shown");
	want(gv_poll(v) && gv_ready(v) && !strcmp(g_gl.key, "hdd") && v.nshown == 21, "the screen asks for its source again");
	want(v.mode == GV_LIST, "a new result closes the detail view");
	want(!gv_poll(v) && !gv_poll(v), "no redraw without a new result");
	// the two settings in the audit log: by their name, values in words
	{
		char av[160];
		want(!strcmp(av_key_label("games_source"), sv ? "Spellistan: källa" : "Game list: source") &&
		     !strcmp(av_key_label("games_filter"), sv ? "Spellistan: filter" : "Game list: filter"), "the audit's names of the settings");
		av_setting_val("games_filter", "2,2,0,0", av, sizeof av);
		want(!strcmp(av, sv ? "2048 · Ej kontrollerad" : "2048 · Not checked"), "the audit's filter value");
		av_setting_val("games_filter", "0,0,0,0", av, sizeof av);
		want(!strcmp(av, sv ? "Alla spel" : "All games"), "the audit's empty filter");
		av_setting_val("games_source", "usb5", av, sizeof av);
		want(!strcmp(av, "USB 5"), "the audit's source");
		av_setting_val("games_source", "hdd", av, sizeof av);
		want(!strcmp(av, sv ? "Hårddisk" : "Hard disk"), "the audit's hard disk");
	}
	// an empty source, a gone one, not enough memory
	system("rm -rf out/gl/dev_usb001/PSXISO/*");
	snprintf(v.key, sizeof v.key, "usb1");
	gv_switch(v);
	gv_poll(v);
	gframe("games-empty", v);
	want(has_text(sv ? "Inga spel på USB 1 (RIPS)" : "No games on USB 1 (RIPS)"), "the empty source");
	snprintf(v.key, sizeof v.key, "usb5");
	gv_switch(v);
	gv_poll(v);
	gframe("games-gone", v);
	want(g_gl.gone && has_text(sv ? "USB 5 är inte ansluten" : "USB 5 is not connected"), "the gone source by its key");
	g_gl_alloc_max = 1024;
	snprintf(v.key, sizeof v.key, "hdd");
	gv_switch(v);
	gv_poll(v);
	g_gl_alloc_max = (size_t)-1;
	gframe("games-oom", v);
	want(g_gl.oom && v.nshown == 0 && has_text(sv ? "Inte tillräckligt med minne" : "Not enough memory"), "out of memory");
	gv_input(v, B_CROSS);
	want(v.mode == GV_LIST, "no detail view without a list");
	// closing frees the list
	gv_close(v);
	want(!g_gl.d && !g_gl.g && !v.shown && !g_gl.key[0], "closing frees the list");
	host_root[0] = 0;
	g_games_filter = GameFilter{0, 0, 0, 0};
	snprintf(g_games_source, sizeof g_games_source, "all");
	if (bad) g_fail += bad; else printf("ok   game list screen (%s): reading, list, statuses, letters, filters, panel, detail, sources, trash, gone, memory\n", sv ? "sv" : "en");
}

// ---------------------------------------------------------------- game list: checks against Redump from the files
// A 2352 image without a cue (one MODE2/2352 track) with a tiny ISO 9660 file system: its PVD at
// sector 16, the root directory at 18 with SYSTEM.CNF at 20 naming `serial`. Without a serial: no
// file system at all.
static std::vector<uint8_t> gc_iso(const char *serial, int sectors)
{
	std::vector<uint8_t> img((size_t)sectors * RAW_SIZE);
	for (int s = 0; s < sectors; s++) {
		uint8_t *b = &img[(size_t)s * RAW_SIZE];
		b[15] = 2;   // MODE2: the user data after the 8-byte subheader, at 24
		for (int i = 2072; i < RAW_SIZE; i++) b[i] = (uint8_t)(s * 31 + i);   // something to hash
	}
	if (!serial) return img;
	uint8_t *pvd = &img[16 * RAW_SIZE + 24];
	pvd[0] = 1; memcpy(pvd + 1, "CD001", 5);
	memcpy(pvd + 40, "TESTDISC                        ", 32);
	pvd[158] = 18;   // the root's extent (little-endian copy)
	uint8_t *dir = &img[18 * RAW_SIZE + 24];
	dir[0] = 33 + 12; dir[9] = 20; dir[17] = 64; dir[32] = 12; memcpy(dir + 33, "SYSTEM.CNF;1", 12);
	char cnf[64];
	snprintf(cnf, sizeof cnf, "BOOT = cdrom:\\%s;1\r\nTCB = 4\r\n", serial);
	memcpy(&img[20 * RAW_SIZE + 24], cnf, strlen(cnf));
	return img;
}
static std::string gc_file(const char *p) { std::string s; FILE *f = fopen(p, "rb"); if (!f) return s; char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n); fclose(f); return s; }
static bool has_exact(const char *s)
{
	for (auto &b : g_boxes) if (b.kind == UI_TRACE_TEXT && b.text == s) return true;
	return false;
}
static int g_gc_cancel_after = -1;   // the check-all job is cancelled after this many games
static void gc_cancel_hook(int i) { if (i + 1 == g_gc_cancel_after) g_gc.abort = 1; }
static void games_check_scenarios()
{
	int bad = 0, sv = g_sfx_sv;
	auto want = [&](bool ok, const char *what) { if (!ok) { printf("FAIL games check (%s): %s\n", sv ? "sv" : "en", what); bad++; } };
	system("rm -rf out/gl out/audit && mkdir -p out/gl/dev_hdd0/PSXISO");
	audit_init("out/audit", &vf_io);
	snprintf(host_root, 256, "out/gl");
	memset(host_exfat, 0, sizeof(int) * 8);
	const char *H = "out/gl/dev_hdd0/PSXISO";
	reset_all();
	// a disc ID the title list knows: the new log's title comes from it
	const char *ID = "SLUS_005.53";
	const char *idtitle = lookup_title(ID);
	want(idtitle != NULL, "the title list knows the test's disc ID");
	if (!idtitle) idtitle = "?";
	std::vector<uint8_t> iso = gc_iso(ID, 24), unk = gc_iso(NULL, 4);
	put_bin("out/gl/dev_hdd0/PSXISO/mystery.iso", iso.data(), iso.size());
	put_bin("out/gl/dev_hdd0/PSXISO/Unknown Game (Europe).img", unk.data(), unk.size());
	put_bin("out/gl/dev_hdd0/PSXISO/Castlevania Chronicles (USA) [SLUS_013.48].iso", std::vector<uint8_t>(3 * 2048).data(), 3 * 2048);
	gs_set(H, "Ape Escape (Europe) [SCES_011.15]", 3, gs_log("Ape Escape", "SCES_011.15", NULL).c_str());
	gs_set(H, "MediEvil (Europe) [SCES_003.11]", 3, gs_log("MediEvil", "SCES_003.11", "verified MediEvil (Europe)").c_str());
	gs_set(H, "Motorhead (USA) [SLUS_007.17]", 3, gs_log("Motorhead", "SLUS_007.17", NULL, "unreadable=1000\n").c_str());
	for (const char *n : {"Croc 2", "Driver", "Klonoa"}) gs_set(H, n, 3, NULL);
	uint8_t md5[16];
	{ Md5 m; md5_init(&m); md5_update(&m, iso.data(), iso.size()); md5_final(&m, md5); }
	char hex[33];
	md5_hex(md5, hex);
	memcpy(host_rd_md5, md5, 16);
	snprintf(host_rd_name, sizeof host_rd_name, "Test Disc (USA)");
	host_rd_local = 1;
	g_cfg_primed = false;
	system("rm -f out/psxrip.cfg");
	snprintf(g_games_source, sizeof g_games_source, "hdd");
	g_games_filter = GameFilter{0, 0, 0, 0};
	g_gl_stale = 1;
	g_input_src = AU_PAD;
	GamesView v;
	// Redump off: neither check is offered, the screen says how to turn it on
	g_redump = RD_OFF;
	gv_open(v);
	gv_poll(v);
	want(gv_listed(v) && v.nshown == 9, "nine games");
	gs_find(v, "mystery");
	want(!gv_can_check(v) && !gv_can_check_all(v), "Redump off: no check offered");
	gframe("games-check-off", v);
	want(has_text(sv ? "Redump är av" : "Redump is off") && !has_exact(T(STR_HINT_CHECK_ALL)), "Redump off: the note, no hint");
	gv_input(v, B_SQUARE);
	gv_input(v, B_START);
	gv_poll(v);
	want(!vf_exists("/dev_hdd0/PSXISO/mystery.log"), "Redump off: □ and START do nothing");
	gs_find(v, "mystery");
	gv_input(v, B_CROSS);
	gframe("games-check-off-detail", v);
	want(has_text(sv ? "Redump är av" : "Redump is off"), "Redump off: the detail view's note");
	gv_input(v, B_CIRCLE);
	// Redump on (a local copy): □ and START with their names
	g_redump = RD_LOCAL;
	want(gv_can_check(v) && gv_can_check_all(v), "Redump on: both offered");
	gframe("games-check-hints", v);
	want(has_exact(T(STR_HINT_CHECK_ONE)) && has_exact(T(STR_HINT_CHECK_ALL)) && !has_text(sv ? "Redump är av" : "Redump is off"), "the hints' names");
	want(!strcmp(T(STR_HINT_CHECK_ONE), sv ? "Kontrollera" : "Check") && !strcmp(T(STR_HINT_CHECK_ALL), sv ? "Kontrollera alla" : "Check all"), "the hints' words");
	// a 2352 .iso without a cue: one track, its log written, verified by the local copy
	gs_find(v, "mystery");
	gv_input(v, B_SQUARE);
	gv_poll(v);   // the job ran (no threads on the host); the list is read again
	gv_poll(v);
	std::string log = gc_file("out/gl/dev_hdd0/PSXISO/mystery.log");
	char t[200];
	snprintf(t, sizeof t, "track=01 start=0 md5=%s\n", hex);
	want(log.find(t) != std::string::npos, "the .iso's log: its track MD5");
	want(log.find("redump=verified Test Disc (USA)\n") != std::string::npos, "the .iso's log: the Redump line");
	snprintf(t, sizeof t, "title=%s\nserial=%s\n", idtitle, ID);
	want(log.find(t) != std::string::npos && log.find("PSX Ripper rip log\n") == 0, "the .iso's log: title from the disc ID, serial");
	want(gv_listed(v) && v.nshown && g_gl.d[g_gl.g[v.shown[v.row]].first].rd == 1 && strstr(g_gl.d[g_gl.g[v.shown[v.row]].first].base, "mystery"),
	     "focus on the same game (now named by its log) after the rescan");
	{   // the list read again with a game added before it: the focus follows the game, not its row
		int row = v.row;
		gs_set(H, "Lemmings (Europe) [SCES_000.20]", 3, gs_log("Lemmings", "SCES_000.20", "verified Lemmings (Europe)").c_str());
		gl_changed();
		gl_open("hdd");
		gv_poll(v);
		want(v.row == row + 1 && strstr(g_gl.d[g_gl.g[v.shown[v.row]].first].base, "mystery"), "focus follows the game when rows move");
	}
	{   // its audit job: a check, with the Redump line before and after
		AuditRow r[20];
		char key[40];
		snprintf(key, sizeof key, "%s:24", ID);
		int n = audit_events(AU_RIPS, key, r, 20), jobs = 0, rd = 0, fin = 0;
		for (int i = 0; i < n; i++) {
			if (strcmp(r[i].kind, "job")) continue;
			jobs++;
			want(r[i].src == AU_PAD && !strcmp(r[i].what, "check"), "the job row: a check from the pad");
			AuditRow rr[10];
			int m = audit_rows(r[i].job, rr, 0, 10);
			for (int j = 0; j < m; j++) {
				if (!strcmp(rr[j].kind, "redump") && !strcmp(rr[j].before, "unchecked") && !strcmp(rr[j].after, "verified Test Disc (USA)")) rd++;
				if (!strcmp(rr[j].kind, "result") && !strcmp(rr[j].after, "finished")) fin++;
			}
		}
		want(jobs == 1 && rd == 1 && fin == 1, "the audit rows of the check");
	}
	// an image without a file system and unknown to Redump: titled by its file name, no match
	gs_find(v, "Unknown Game");
	gv_input(v, B_SQUARE);
	gv_poll(v); gv_poll(v);
	log = gc_file("out/gl/dev_hdd0/PSXISO/Unknown Game (Europe).log");
	want(log.find("title=Unknown Game (Europe)\nserial=\n") != std::string::npos && log.find("redump=no_match\n") != std::string::npos, "the unknown image's log");
	want(gs_at(v, "Unknown Game"), "focus kept");
	// a 2048 image: refused with the reason, nothing written
	gs_find(v, "Castlevania");
	gv_input(v, B_SQUARE);
	gv_poll(v);
	gframe("games-check-2048", v);
	want(!vf_exists("/dev_hdd0/PSXISO/Castlevania Chronicles (USA) [SLUS_013.48].log") && has_text(T(STR_GC_NO_2048)), "a 2048 image is refused");
	// an app rip: through check_image, its log kept apart from the track MD5 and the Redump line
	gs_find(v, "Ape Escape");
	gv_input(v, B_CROSS);   // from the detail view: back in it after the check
	gv_input(v, B_SQUARE);
	gv_poll(v); gv_poll(v);
	log = gc_file("out/gl/dev_hdd0/PSXISO/Ape Escape (Europe) [SCES_011.15].log");
	want(log.find("date=2026-10-02 18:41 UTC\n") != std::string::npos && log.find("check=bitperfect\n") != std::string::npos && log.find("redump=no_match\n") != std::string::npos &&
	     log.find(GS_MD5) == std::string::npos, "the app rip's log rewritten by check_image");
	want(v.mode == GV_DETAIL && gs_at(v, "Ape Escape"), "back in the detail view of the same game");
	gv_input(v, B_CIRCLE);
	// a drive rips into a set: its check is refused
	{
		Card &k = g_cards[0];
		k.job = 1;
		snprintf(k.base, sizeof k.base, "/dev_hdd0/PSXISO/Croc 2");
		gs_find(v, "Croc 2");
		gv_input(v, B_SQUARE);
		gv_poll(v);
		gframe("games-check-busy", v);
		want(!vf_exists("/dev_hdd0/PSXISO/Croc 2.log") && has_text(T(STR_GC_BUSY)), "a set a drive rips into is refused");
		k.job = 0; k.base[0] = 0;
	}
	// Check all: only the unchecked 2352 games the filter shows (without errors: not Motorhead);
	// cancelled after the first, the rest stays unchecked
	g_games_filter = GameFilter{0, 0, 2, 0};
	gv_refilter(v);
	g_gc_after = gc_cancel_hook;
	g_gc_cancel_after = 1;
	gv_input(v, B_START);
	gv_poll(v); gv_poll(v);
	int logs = vf_exists("/dev_hdd0/PSXISO/Croc 2.log") + vf_exists("/dev_hdd0/PSXISO/Driver.log") + vf_exists("/dev_hdd0/PSXISO/Klonoa.log");
	want(logs == 1 && vf_exists("/dev_hdd0/PSXISO/Croc 2.log"), "cancelled after the first game");
	gframe("games-check-cancelled", v);
	want(has_text(sv ? "avbröts" : "cancelled"), "the cancelled note");
	g_gc_cancel_after = -1;
	std::string medievil = gc_file("out/gl/dev_hdd0/PSXISO/MediEvil (Europe) [SCES_003.11].log");
	gv_input(v, B_START);
	gv_poll(v); gv_poll(v);
	logs = vf_exists("/dev_hdd0/PSXISO/Croc 2.log") + vf_exists("/dev_hdd0/PSXISO/Driver.log") + vf_exists("/dev_hdd0/PSXISO/Klonoa.log");
	want(logs == 3, "the rest checked");
	want(gc_file("out/gl/dev_hdd0/PSXISO/Motorhead (USA) [SLUS_007.17].log").find("redump=") == std::string::npos, "a game the filter hides is not checked");
	want(gc_file("out/gl/dev_hdd0/PSXISO/MediEvil (Europe) [SCES_003.11].log") == medievil, "a verified game is not checked again");
	want(!vf_exists("/dev_hdd0/PSXISO/Castlevania Chronicles (USA) [SLUS_013.48].log"), "a 2048 image is never checked");
	gframe("games-check-done", v);
	want(!gv_can_check_all(v), "nothing unchecked shown: no Check all");
	g_gc_after = NULL;
	want(!vf_exists("/dev_hdd0/PSXISO/Croc 2.log.tmp") && !vf_exists("/dev_hdd0/PSXISO/Croc 2.md5.tmp"), "no .tmp left after a check");
	// Redump online without an answer (no network on the host): a verified game keeps its line; a
	// disc without an ID is left out before it is read; Check all stops at the first game
	g_redump = RD_ONLINE;
	g_games_filter = GameFilter{0, 0, 0, 0};
	for (const char *n : {"Spyro (Europe) [SCES_013.15]", "Tomba (Europe) [SCES_011.24]"}) gs_set(H, n, 3, NULL);
	put_bin("out/gl/dev_hdd0/PSXISO/Nameless.img", unk.data(), unk.size());
	gl_changed(); gl_open("hdd"); gv_poll(v);
	gs_find(v, "MediEvil");
	gv_input(v, B_SQUARE);
	gv_poll(v); gv_poll(v);
	want(gc_file("out/gl/dev_hdd0/PSXISO/MediEvil (Europe) [SCES_003.11].log").find("redump=verified MediEvil (Europe)\n") != std::string::npos,
	     "offline: a verified game keeps its Redump line");
	want(!strstr(v.note, sv ? "Kontrollerade" : "Checked against"), "offline: not counted as checked");
	gs_find(v, "Nameless");
	gv_input(v, B_SQUARE);
	gv_poll(v); gv_poll(v);
	want(!vf_exists("/dev_hdd0/PSXISO/Nameless.md5") && !vf_exists("/dev_hdd0/PSXISO/Nameless.log") && strstr(v.note, sv ? "utan skiv-ID" : "without disc ID"),
	     "online: a disc without an ID is not read");
	gv_input(v, B_START);
	gv_poll(v); gv_poll(v);
	gframe("games-check-offline", v);
	want(vf_exists("/dev_hdd0/PSXISO/Motorhead (USA) [SLUS_007.17].md5") && gc_file("out/gl/dev_hdd0/PSXISO/Motorhead (USA) [SLUS_007.17].log").find("redump=") == std::string::npos &&
	     !vf_exists("/dev_hdd0/PSXISO/Spyro (Europe) [SCES_013.15].md5") && !vf_exists("/dev_hdd0/PSXISO/Tomba (Europe) [SCES_011.24].md5") && has_text(sv ? "gick inte att fråga" : "could not be asked"),
	     "offline: Check all stops at the first game (Motorhead: MD5 written, no Redump line), and says why");
	g_redump = RD_LOCAL;
	// both directions with a drive: the job's file is busy for a rip or a card check on it; the job
	// writes nothing when a drive started on the set meanwhile
	{
		GcItem it[1] = {};
		snprintf(it[0].base, sizeof it[0].base, "/dev_hdd0/PSXISO/Klonoa");
		g_gc.items = it; g_gc.n = 1; g_gc.i = 0; g_gc.running = 1;
		want(g_state_busy(), "/status.json busy while the check job runs");
		Card &k = g_cards[1];
		k.job = 0; k.res[0] = 0;
		snprintf(k.exist, sizeof k.exist, "/dev_hdd0/PSXISO/Klonoa");
		snprintf(k.base, sizeof k.base, "/dev_hdd0/PSXISO/Klonoa");
		check_job_start(1, AU_PAD);
		want(!k.job && !strcmp(k.res, T(STR_GC_CARD_BUSY)), "a card's check of the job's file is refused");
		k.res[0] = 0;
		rip_job_start(1, AU_PAD, 0);
		want(!k.job && !strcmp(k.res, T(STR_GC_CARD_BUSY)), "a rip into the job's file is refused");
		k.res[0] = 0;
		snprintf(k.exist, sizeof k.exist, "/dev_hdd0/PSXISO/Driver");
		snprintf(k.base, sizeof k.base, "/dev_hdd0/PSXISO/Driver");
		check_job_start(1, AU_PAD);
		want(strcmp(k.res, T(STR_GC_CARD_BUSY)) != 0, "another file: not refused");
		k.job = 0; k.res[0] = 0; k.exist[0] = k.base[0] = 0;
		g_gc.running = 0; g_gc.items = NULL; g_gc.n = 0;
		want(!g_state_busy(), "not busy after the job");
		// a drive starts on the set while the job reads it: nothing is written
		std::string before = gc_file("out/gl/dev_hdd0/PSXISO/Driver.log");
		volatile int ab = 0; volatile uint32_t dn = 0, tt = 0;
		CheckFile cf = {};
		cf.busy = [](const char *) { return 1; };
		cf.image = "/dev_hdd0/PSXISO/Driver.bin"; cf.abort = &ab; cf.done = &dn; cf.total = &tt;
		remove("out/gl/dev_hdd0/PSXISO/Driver.md5");
		int r = check_image("/dev_hdd0/PSXISO/Driver", 0, &cf);
		want(r == -1 && !vf_exists("/dev_hdd0/PSXISO/Driver.md5") && gc_file("out/gl/dev_hdd0/PSXISO/Driver.log") == before && !vf_exists("/dev_hdd0/PSXISO/Driver.log.tmp"),
		     "a drive on the set: the check writes nothing");
	}
	{   // a full disk: a short write into the temp file keeps the original log and leaves no .tmp;
		// stdio's rename replaces at once (the old file is never removed first)
		std::string before = gc_file("out/gl/dev_hdd0/PSXISO/Driver.log");
		volatile int ab = 0; volatile uint32_t dn = 0, tt = 0;
		CheckFile cf = {};
		cf.image = "/dev_hdd0/PSXISO/Driver.bin"; cf.abort = &ab; cf.done = &dn; cf.total = &tt;
		g_tmp_cap = 20;
		check_image("/dev_hdd0/PSXISO/Driver", 0, &cf);
		g_tmp_cap = (size_t)-1;
		want(gc_file("out/gl/dev_hdd0/PSXISO/Driver.log") == before && !vf_exists("/dev_hdd0/PSXISO/Driver.log.tmp") && !vf_exists("/dev_hdd0/PSXISO/Driver.md5.tmp"),
		     "a short temp write keeps the original, no .tmp left");
		g_replace_unlinks = 0;
		check_image("/dev_hdd0/PSXISO/Driver", 0, &cf);
		want(g_replace_unlinks == 0 && vf_exists("/dev_hdd0/PSXISO/Driver.md5"), "stdio: rename replaces, nothing removed first");
		// a power cut between removing the log and renaming its .tmp: the .tmp is put back first
		std::string log2 = gc_file("out/gl/dev_hdd0/PSXISO/Driver.log") + "drive=Test Drive\n";
		put_bin("out/gl/dev_hdd0/PSXISO/Driver.log.tmp", log2.data(), log2.size());
		remove("out/gl/dev_hdd0/PSXISO/Driver.log");
		check_image("/dev_hdd0/PSXISO/Driver", 0, &cf);
		std::string log3 = gc_file("out/gl/dev_hdd0/PSXISO/Driver.log");
		want(!vf_exists("/dev_hdd0/PSXISO/Driver.log.tmp") && log3 == log2, "a lone .log.tmp is put back (not a new small log)");
		// a repair of the set the job works on is refused, as a rip or a card check
		GcItem it[1] = {};
		snprintf(it[0].base, sizeof it[0].base, "/dev_hdd0/PSXISO/Driver");
		g_gc.items = it; g_gc.n = 1; g_gc.i = 0; g_gc.running = 1;
		Card &k = g_cards[1];
		k.job = 0; k.res[0] = 0;
		snprintf(k.exist, sizeof k.exist, "/dev_hdd0/PSXISO/Driver");
		snprintf(k.base, sizeof k.base, "/dev_hdd0/PSXISO/Driver");
		repair_job_start(1, AU_PAD);
		want(!k.job && !strcmp(k.res, T(STR_GC_CARD_BUSY)), "a repair of the job's file is refused");
		k.job = 0; k.res[0] = 0; k.exist[0] = k.base[0] = 0;
		g_gc.running = 0; g_gc.items = NULL; g_gc.n = 0;
	}
	// the progress screen, and O cancels
	{
		GcItem it[3] = {};
		snprintf(it[0].base, sizeof it[0].base, "/dev_hdd0/PSXISO/Croc 2");
		snprintf(it[1].base, sizeof it[1].base, "/dev_hdd0/PSXISO/Driver");
		snprintf(it[2].base, sizeof it[2].base, "/dev_hdd0/PSXISO/Klonoa");
		g_gc.items = it; g_gc.n = 3; g_gc.i = 1; g_gc.done = 120000; g_gc.total = 300000; g_gc.running = 1; g_gc.abort = 0;
		v.checking = 1;
		gframe("games-check-progress", v);
		want(has_text(sv ? "Spel 2 av 3" : "Game 2 of 3") && has_text("Driver") && has_text("40 %"), "the progress screen");
		gv_input(v, B_CIRCLE);
		want(g_gc.abort == 1 && v.checking, "O cancels the job (the screen waits for it)");
		g_gc.running = 0; g_gc.items = NULL; g_gc.n = 0;
		gv_poll(v);
		want(!v.checking, "back to the list when the job ended");
	}
	gv_close(v);
	host_rd_local = 0;
	g_redump = RD_OFF;
	host_root[0] = 0;
	g_games_filter = GameFilter{0, 0, 0, 0};
	snprintf(g_games_source, sizeof g_games_source, "all");
	system("rm -rf out/audit");
	audit_init("out/audit", &vf_io);
	reset_all();
	if (bad) g_fail += bad; else printf("ok   game list checks (%s): .iso without cue, 2048 refused, app rip, busy, check all + cancel, audit, Redump off, focus\n", sv ? "sv" : "en");
}

// A prepared log: Alundra (a rip, and a repair that wrote 1015 sectors: 1017 rows), Final Fantasy IX
// (a rip, a check, and a rip that was cancelled after writing a sector), and settings from every source.
static void audit_scenarios()
{
	int bad = 0;
	system("rm -rf out/audit out/t5 out/psxrip.cfg && mkdir -p out/t5");
	audit_init("out/audit", &vf_io);
	audit_set_hooks(set_setting_hook, recheck_hook);
	g_cfg_primed = false;
	reset_all();
	const uint32_t NS = 1100;
	std::vector<uint8_t> img((size_t)NS * RAW_SIZE);
	for (size_t i = 0; i < img.size(); i++) img[i] = (uint8_t)(i * 13 + i / RAW_SIZE);
	put_bin("out/t5/alundra.bin", img.data(), img.size());
	put_bin("out/t5/ff9.bin", img.data(), 10 * RAW_SIZE);
	static const char cue[] = "FILE \"alundra.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n";
	put_bin("out/t5/alundra.cue", cue, sizeof cue - 1);
	static const char rlog[] = "PSX Ripper rip log\ntitle=Alundra\nredump=verified Alundra (Europe)\n";
	put_bin("out/t5/alundra.log", rlog, sizeof rlog - 1);
	uint8_t zero[RAW_SIZE];
	memset(zero, 0, sizeof zero);
	const char *FF9 = "SLES_029.65:314937", *ALU = "SLES_011.35:1100";
	uint32_t j = audit_job_begin(AU_REMOTE, FF9, "Final Fantasy IX (Disc 1)", "rip");
	audit_job_end(j, "finished");
	j = audit_job_begin(AU_APP, FF9, "Final Fantasy IX (Disc 1)", "check");
	audit_note(j, AU_RIPS, FF9, "redump", "", "unchecked", "verified Final Fantasy IX (Europe) (Disc 1)");
	audit_job_end(j, "finished");
	j = audit_job_begin(AU_PAD, FF9, "Final Fantasy IX (Disc 1)", "rip");
	audit_sector(j, "out/t5/ff9.bin", 3, zero, img.data() + 3 * RAW_SIZE);
	audit_job_end(j, "cancelled");
	g_std.speed = 1; g_redump = RD_OFF; g_webhook_hdr[0][0] = 0; g_audit_mb = 256;
	g_input_src = AU_PAD; cfg_save();   // primes
	g_std.speed = 2; cfg_save();
	g_input_src = AU_REMOTE; g_redump = RD_ONLINE; cfg_save();
	g_redump = RD_LOCAL; cfg_save();   // the row before is then "changed since"
	g_input_src = AU_PAD; snprintf(g_webhook_hdr[0], sizeof g_webhook_hdr[0], "X-Token: abc"); cfg_save();
	g_input_src = AU_MCP; g_audit_mb = 64; cfg_save();
	g_input_src = AU_PAD;
	j = audit_job_begin(AU_PAD, ALU, "Alundra", "rip");
	audit_job_end(j, "finished");
	uint32_t rep = audit_job_begin(AU_APP, ALU, "Alundra", "repair");
	for (uint32_t l = 20; l < 20 + 1015; l++) audit_sector(rep, "out/t5/alundra.bin", l, zero, img.data() + (size_t)l * RAW_SIZE);
	audit_job_end(rep, "finished");

	AuditView v;
	audit_view_init(v);
	if (v.level != 0 || v.empty) { printf("FAIL the log's first level: level %d, empty %d\n", v.level, v.empty); bad++; }
	aframe("audit-domains", v);
	audit_open(v);   // Rips
	if (v.level != 1 || v.n != 2 || strcmp(av_names[0], "Alundra") || strcmp(av_names[1], "Final Fantasy IX (Disc 1)"))
		{ printf("FAIL the discs: level %d, %d disc(s), first \"%s\"\n", v.level, v.n, v.n ? av_names[0] : ""); bad++; }
	aframe("audit-discs", v);
	audit_open(v);   // Alundra
	if (v.level != 2 || v.n != 2 || strcmp(av_rows[0].what, "repair") || !audit_can_undo(v))
		{ printf("FAIL Alundra's events: level %d, %d event(s), undo %d\n", v.level, v.n, audit_can_undo(v)); bad++; }
	aframe("audit-events", v);
	audit_open(v);   // the repair: 1017 rows, a page at a time
	if (v.level != 3 || !v.more || v.base != 0 || strcmp(av_rows[0].kind, "job") || audit_can_undo(v))
		{ printf("FAIL the repair's rows: level %d, more %d, first \"%s\"\n", v.level, v.more, av_rows[0].kind); bad++; }
	aframe("audit-rows", v);
	for (int i = 0; i < 55; i++) audit_nav(v, B_R2);   // far down: 55 pages
	audit_nav(v, B_DOWN);
	const AuditRow *r = audit_focused(v);
	if (v.row != 55 * AV_PAGE + 1 || !r || strcmp(r->kind, "sector") || r->lba != 20 + 55 * AV_PAGE || !audit_can_undo(v))
		{ printf("FAIL paging the rows: row %d, %s %u\n", v.row, r ? r->kind : "-", r ? (unsigned)r->lba : 0); bad++; }
	aframe("audit-rows-paged", v);
	if (!has_text(g_sfx_sv ? "Sektor 515: ändrad" : "Sector 515: changed")) { printf("FAIL the focused sector row is not shown\n"); bad++; }
	{   // the question before an undo
		char q[256], label[160];
		audit_label(v, v.row, label, sizeof label);
		snprintf(q, sizeof q, T(STR_AUDIT_UNDO_Q), label);
		char nm[64];
		snprintf(nm, sizeof nm, "audit-undo-dialog%s", g_sfx_sv ? "-sv" : "");
		g_boxes.clear(); own_dialog_draw(q, 1); save(nm); size_t b0 = g_fail; check(nm); printf("%s %s\n", g_fail == (int)b0 ? "ok  " : "FAIL", nm);
	}
	{   // the wait while the rip is checked again
		char nm[64];
		snprintf(nm, sizeof nm, "audit-wait%s", g_sfx_sv ? "-sv" : "");
		g_boxes.clear(); audit_wait_draw("Alundra (Europe) (En,Fr,De,Es,It,Nl,Sv) [SLES-01135]", 412); save(nm); size_t b0 = g_fail; check(nm); printf("%s %s\n", g_fail == (int)b0 ? "ok  " : "FAIL", nm);
	}
	{   // the wait while a big undo writes its sectors back; closing the app stops it
		char nm[64];
		snprintf(nm, sizeof nm, "audit-undoing%s", g_sfx_sv ? "-sv" : "");
		g_boxes.clear(); audit_wait_draw("Alundra (Europe) (En,Fr,De,Es,It,Nl,Sv) [SLES-01135]", 2, STR_AUDIT_UNDOING); save(nm); size_t b0 = g_fail; check(nm); printf("%s %s\n", g_fail == (int)b0 ? "ok  " : "FAIL", nm);
		snprintf(nm, sizeof nm, "audit-redoing%s", g_sfx_sv ? "-sv" : "");
		g_boxes.clear(); audit_wait_draw("Alundra (Europe) (En,Fr,De,Es,It,Nl,Sv) [SLES-01135]", 2, STR_AUDIT_REDOING); save(nm); b0 = g_fail; check(nm); printf("%s %s\n", g_fail == (int)b0 ? "ok  " : "FAIL", nm);
		if (!has_text(g_sfx_sv ? "Gör om" : "Redoing")) { printf("FAIL the redo's wait screen does not say so\n"); bad++; }
		if (audit_wait_what(1) != STR_AUDIT_REDOING || audit_wait_what(0) != STR_AUDIT_UNDOING) { printf("FAIL the wait text of an undo / redo\n"); bad++; }
		if (audit_undo_tick(0, 100) != 0) { printf("FAIL the undo tick stops without an exit request\n"); bad++; }
		exit_requested = 1;
		if (audit_undo_tick(64, 100) == 0) { printf("FAIL the undo tick does not stop on exit\n"); bad++; }
		exit_requested = 0;
	}
	{   // undo of that sector: the image gets its old bytes, the rip is checked again, the drive's card follows
		disc(0, "Alundra", "Alundra (Europe)", "SLES_011.35", 3);
		Card &k = g_cards[0];
		snprintf(k.dest, sizeof k.dest, "out/t5");
		snprintf(k.exist, sizeof k.exist, "out/t5/alundra");
		k.rd_state = 1;
		{ OnCard o(0); OUT_DIR = k.dest; g_serial[0] = 0; snprintf(g_base, sizeof g_base, "out/t5/alundra"); }
		uint32_t lba = r->lba;
		char why[200];
		if (audit_undo(v, why, sizeof why) != 0) { printf("FAIL undo of a sector from the screen: %s\n", why); bad++; }
		r = audit_focused(v);
		FILE *f = fopen("out/t5/alundra.bin", "rb");
		uint8_t got[RAW_SIZE] = {1};
		if (f) { fseek(f, (long)lba * RAW_SIZE, SEEK_SET); if (fread(got, 1, RAW_SIZE, f) != RAW_SIZE) got[0] = 1; fclose(f); }
		if (memcmp(got, zero, RAW_SIZE) || !r || !r->undone || !audit_can_undo(v)) { printf("FAIL after the undo: bytes back %d, row undone %d, redo offered %d\n", !memcmp(got, zero, RAW_SIZE), r ? r->undone : -1, audit_can_undo(v)); bad++; }
		if (!vf_exists("out/t5/alundra.md5")) { printf("FAIL the rip was not checked again after the undo\n"); bad++; }
		if (k.rd_state != -1 || strcmp(k.exist, "out/t5/alundra")) { printf("FAIL the card after the undo: redump state %d, exist \"%s\"\n", k.rd_state, k.exist); bad++; }
		aframe("audit-rows-undone", v);
		if (!has_text(T(STR_HINT_REDO)) || (g_sfx_sv && !has_text("Gör om"))) { printf("FAIL the undone sector row does not offer \"%s\"\n", T(STR_HINT_REDO)); bad++; }
		// △ on the undone row redoes it: the new bytes again, the row not undone, △ undoes again
		if (audit_undo(v, why, sizeof why) != 0) { printf("FAIL redo of the sector row from the screen: %s\n", why); bad++; }
		r = audit_focused(v);
		f = fopen("out/t5/alundra.bin", "rb");
		if (f) { fseek(f, (long)lba * RAW_SIZE, SEEK_SET); if (fread(got, 1, RAW_SIZE, f) != RAW_SIZE) got[0] ^= 1; fclose(f); }
		if (memcmp(got, img.data() + (size_t)lba * RAW_SIZE, RAW_SIZE) || !r || r->undone || !audit_can_undo(v)) { printf("FAIL after the redo of the row: row undone %d\n", r ? r->undone : -1); bad++; }
		aframe("audit-rows-redone", v);
		if (!has_text(T(STR_HINT_UNDO)) || has_text(T(STR_HINT_REDO))) { printf("FAIL the redone row's hint is not \"%s\"\n", T(STR_HINT_UNDO)); bad++; }
		if (audit_undo(v, why, sizeof why) != 0 || !(r = audit_focused(v)) || !r->undone) { printf("FAIL undo again of the sector row: %s\n", why); bad++; }
		reset_all();
	}
	{   // the whole repair undone: its undo is now a redo, and says so; then redone from the screen
		audit_back(v);   // Alundra's events, the repair focused
		char why[200];
		if (v.level != 2 || !audit_can_undo(v) || audit_undo(v, why, sizeof why) != 0) { printf("FAIL undo of the repair from the screen: %s\n", why); bad++; }
		const AuditRow *e = audit_focused(v);
		if (!e || strcmp(e->kind, "job") || !audit_can_undo(v) || !v.undo_redo) { printf("FAIL the undone repair does not offer a redo (%d)\n", v.undo_redo); bad++; }
		aframe("audit-events-redo", v);
		if (!has_text(T(STR_HINT_REDO)) || has_text(T(STR_HINT_UNDO))) { printf("FAIL the undone repair's hint is not \"%s\"\n", T(STR_HINT_REDO)); bad++; }
		if (g_sfx_sv && !has_text("Gör om")) { printf("FAIL the Swedish redo hint\n"); bad++; }
		{
			char q[256], label[160], nm[64];
			audit_label(v, v.row, label, sizeof label);
			snprintf(q, sizeof q, T(STR_AUDIT_REDO_Q), label);
			snprintf(nm, sizeof nm, "audit-redo-dialog%s", g_sfx_sv ? "-sv" : "");
			g_boxes.clear(); own_dialog_draw(q, 1); save(nm); size_t b0 = g_fail; check(nm); printf("%s %s\n", g_fail == (int)b0 ? "ok  " : "FAIL", nm);
		}
		if (audit_undo(v, why, sizeof why) != 0) { printf("FAIL redo of the repair from the screen: %s\n", why); bad++; }
		FILE *f = fopen("out/t5/alundra.bin", "rb");
		uint8_t got[RAW_SIZE] = {0};
		if (f) { fseek(f, 515L * RAW_SIZE, SEEK_SET); if (fread(got, 1, RAW_SIZE, f) != RAW_SIZE) got[0] ^= 1; fclose(f); }
		if (memcmp(got, img.data() + 515 * RAW_SIZE, RAW_SIZE) || v.undo_redo || !audit_can_undo(v)) { printf("FAIL after the redo: bytes again %d, redo %d\n", !memcmp(got, img.data() + 515 * RAW_SIZE, RAW_SIZE), v.undo_redo); bad++; }
	}
	audit_back(v);   // the discs
	audit_nav(v, B_DOWN);
	audit_open(v);   // Final Fantasy IX: the cancelled rip first; its sector cannot be undone
	if (v.level != 2 || v.n != 3 || strcmp(av_rows[0].what, "rip") || audit_can_undo(v))
		{ printf("FAIL FF9's events: %d, undo of the cancelled rip offered %d\n", v.n, audit_can_undo(v)); bad++; }
	aframe("audit-events-cancelled", v);
	if (!has_text(T(STR_AW_UNFINISHED))) { printf("FAIL the cancelled rip does not say why it cannot be undone\n"); bad++; }
	audit_back(v);   // the discs, FF9 focused: its only change is the cancelled rip's, so no undo
	if (v.level != 1 || v.row != 1 || audit_can_undo(v)) { printf("FAIL the FF9 disc offers undo (%d)\n", audit_can_undo(v)); bad++; }
	aframe("audit-discs-no-undo", v);
	if (!has_text(T(STR_AW_UNFINISHED)) || has_text(T(STR_HINT_UNDO))) { printf("FAIL the FF9 disc: no reason, or an undo hint\n"); bad++; }
	audit_nav(v, B_UP);   // Alundra: its repair can be undone; the question names the whole disc
	if (!audit_can_undo(v)) { printf("FAIL the Alundra disc does not offer undo\n"); bad++; }
	{
		char q[256], label[160], nm[64];
		audit_label(v, v.row, label, sizeof label);
		snprintf(q, sizeof q, T(STR_AUDIT_UNDO_DISC_Q), label);
		snprintf(nm, sizeof nm, "audit-undo-disc-dialog%s", g_sfx_sv ? "-sv" : "");
		g_boxes.clear(); own_dialog_draw(q, 1); save(nm); size_t b0 = g_fail; check(nm); printf("%s %s\n", g_fail == (int)b0 ? "ok  " : "FAIL", nm);
	}
	audit_back(v);   // the domains
	audit_nav(v, B_DOWN);
	audit_open(v);   // Settings
	if (v.level != 1 || v.dom != AU_SETTINGS || v.n != 5) { printf("FAIL the settings: level %d, %d event(s)\n", v.level, v.n); bad++; }
	aframe("audit-settings", v);
	if (!has_text("MCP") || !has_text(T(STR_AUDIT_SRC_REMOTE))) { printf("FAIL the settings rows do not show their sources\n"); bad++; }
	{
		int hdr = -1, spd = -1;
		for (int i = 0; i < v.n; i++) {
			if (!strcmp(av_rows[i].what, "webhook_header1")) hdr = i;
			if (!strcmp(av_rows[i].what, "speed")) spd = i;
		}
		v.row = hdr;
		if (hdr < 0 || audit_can_undo(v)) { printf("FAIL the header row offers undo\n"); bad++; }
		aframe("audit-settings-secret", v);
		if (!has_text(T(STR_AW_SECRET))) { printf("FAIL the secret row does not say why\n"); bad++; }
		int old = -1;
		for (int i = 0; i < v.n; i++) if (!strcmp(av_rows[i].what, "redump") && !strcmp(av_rows[i].after, "1")) old = i;
		v.row = old;
		char exp[160];
		snprintf(exp, sizeof exp, T(STR_AW_SETTING_CHANGED), T(STR_L_REDUMP));
		if (old < 0 || audit_can_undo(v)) { printf("FAIL the redump row changed since offers undo\n"); bad++; }
		aframe("audit-settings-changed", v);
		if (!has_text(exp)) { printf("FAIL the changed setting does not say \"%s\"\n", exp); bad++; }
		v.row = spd;
		char why[200];
		if (spd < 0 || !audit_can_undo(v) || audit_undo(v, why, sizeof why) != 0 || g_std.speed != 1) { printf("FAIL undo of the speed from the screen (speed %d)\n", g_std.speed); bad++; }
		if (v.n != 7) { printf("FAIL the settings after the undo: %d event(s), want 7\n", v.n); bad++; }   // + the undo's own rows
		aframe("audit-settings-undone", v);
		// the undo's own row: undoing it sets the speed again
		int u = -1;
		for (int i = 0; i < v.n; i++) if (!strcmp(av_rows[i].kind, "undo")) u = i;
		v.row = u;
		if (u < 0 || !audit_can_undo(v) || !av_rows[u].redo) { printf("FAIL the speed's undo row offers no redo\n"); bad++; }
		aframe("audit-settings-redo", v);
		if (!has_text(T(STR_HINT_REDO))) { printf("FAIL the undo row's hint is not \"%s\"\n", T(STR_HINT_REDO)); bad++; }
		if (u < 0 || audit_undo(v, why, sizeof why) != 0 || g_std.speed != 2) { printf("FAIL redo of the speed from the screen (speed %d): %s\n", g_std.speed, why); bad++; }
		if (v.n != 9) { printf("FAIL the settings after the redo: %d event(s), want 9\n", v.n); bad++; }
	}
	{   // sector reasons: one changed since, then all kept bytes dropped by the size cap
		FILE *f = fopen("out/t5/alundra.bin", "r+b");
		uint8_t junk[RAW_SIZE];
		memset(junk, 0x5A, sizeof junk);
		if (f) { fseek(f, 21L * RAW_SIZE, SEEK_SET); fwrite(junk, 1, sizeof junk, f); fclose(f); }
		AuditView w;
		audit_view_init(w);
		audit_open(w); audit_open(w); audit_open(w);   // Rips, Alundra, the repair
		audit_nav(w, B_DOWN); audit_nav(w, B_DOWN);   // sector 21
		char exp[160];
		snprintf(exp, sizeof exp, T(STR_AW_SECTOR_CHANGED), 21u);
		const AuditRow *x = audit_focused(w);
		if (!x || x->lba != 21 || audit_can_undo(w)) { printf("FAIL sector 21 changed since: row %u, undo %d\n", x ? (unsigned)x->lba : 0, audit_can_undo(w)); bad++; }
		aframe("audit-rows-changed", w);
		if (!has_text(exp)) { printf("FAIL the changed sector does not say why\n"); bad++; }
		audit_set_cap(0);
		audit_back(w); audit_open(w);   // read again
		audit_nav(w, B_DOWN);   // sector 20
		aframe("audit-rows-dropped", w);
		if (audit_can_undo(w) || !has_text(T(STR_AW_DROPPED))) { printf("FAIL a sector whose bytes were dropped: undo %d\n", audit_can_undo(w)); bad++; }
		audit_set_cap(256ull << 20);
	}
	{   // an empty log
		system("rm -rf out/audit");
		audit_init("out/audit", &vf_io);
		AuditView e;
		audit_view_init(e);
		if (!e.empty) { printf("FAIL an empty log is not shown as empty\n"); bad++; }
		aframe("audit-empty", e);
		if (!has_text(T(STR_AUDIT_EMPTY))) { printf("FAIL the empty log says nothing\n"); bad++; }
	}
	audit_set_hooks(set_setting_hook, NULL);
	g_webhook_hdr[0][0] = 0; g_audit_mb = 256; g_std.speed = 1; g_redump = RD_OFF;
	reset_all();
	if (bad) g_fail += bad; else printf("ok   the audit log screen (levels, paging, undo, sources)%s\n", g_sfx_sv ? " (sv)" : "");
}

int main()
{
	system("mkdir -p out");
	ui_init();
	lang_init();
	for (int c = 0; c < NCARDS; c++) ctx_init(c);
	ui_trace = trace;

	{   // the checks themselves: an overlap, a missing glyph and text off the screen must be found
		g_boxes.clear();
		draw_backdrop();
		ui_text(UI_SMALL, 300, 300, 0xFFFFFF, "SLES_029.65  ·  706 MB");
		ui_text(UI_SMALL, 320, 305, 0xFFFFFF, "339 GB ledigt");
		ui_text(UI_SMALL, 600, 500, 0xFFFFFF, "L1  ◂  ▸  R1");
		ui_text(UI_BODY, 1850, 20, 0xFFFFFF, "off screen");
		int before = g_fail;
		check("self-test");
		int found = g_fail - before;
		g_fail = before;
		if (found < 3) { printf("FAIL the checks found %d of 3 planted problems\n", found); g_fail++; }
		else printf("ok   self-test (the checks find planted problems)\n");
	}

	{   // the disc ID out of SYSTEM.CNF, also when the disc writes it in lower case
		struct { const char *cnf, *want; } c[] = {
			{"BOOT = cdrom:\\SLUS_005.53;1\r\nTCB = 4\r\n", "SLUS_005.53"},
			{"BOOT = cdrom:\\scus_943.51;1\r\nTCB = 4\r\n", "SCUS_943.51"},
			{"BOOT=cdrom:\\SLES_029.65;1", "SLES_029.65"},
			{"BOOT = cdrom:\\PSX.EXE;1\r\n", ""},
		};
		int bad = 0;
		for (auto &t : c) {
			char out[16];
			serial_from_cnf(t.cnf, out);
			if (strcmp(out, t.want)) { printf("FAIL serial from \"%s\": got \"%s\", want \"%s\"\n", t.cnf, out, t.want); bad++; }
		}
		if (bad) g_fail += bad; else printf("ok   disc ID from SYSTEM.CNF (upper and lower case)\n");
	}

	{   // the earliest discs have no SYSTEM.CNF: the ID comes from the volume name
		struct { const char *vol, *want; } c[] = {
			{"SLPS00002", "SLPS_000.02"}, {"SLPS_00002", "SLPS_000.02"}, {"slus-01234", "SLUS_012.34"},
			{"WIPEOUT2", ""}, {"SLPS000021", ""}, {"", ""}, {"MEGAMAN_LEGENDS2", ""},
		};
		int bad = 0;
		for (auto &t : c) {
			char out[16];
			serial_from_volume(t.vol, out);
			if (strcmp(out, t.want)) { printf("FAIL serial from volume \"%s\": got \"%s\", want \"%s\"\n", t.vol, out, t.want); bad++; }
		}
		if (bad) g_fail += bad; else printf("ok   disc ID from the volume name when there is no SYSTEM.CNF\n");
	}

	{   // a disc pressed with another game's ID: Castlevania Chronicles says SLUS_013.48 (Tennis)
		const char *a = title_override("SLUS_013.48", "KONAMI"), *b = title_override("SLUS_013.48", "AGETEC");
		const char *c = lookup_title("SLUS_013.48");
		if (!a || strcmp(a, "Castlevania Chronicles") || b || !c || strcmp(c, "Tennis")) { printf("FAIL title override: %s / %s / %s\n", a ? a : "-", b ? b : "-", c ? c : "-"); g_fail++; }
		else printf("ok   Castlevania Chronicles despite its SLUS_013.48, Tennis stays Tennis\n");
	}

	{   // every text in both languages: a StrId without an entry in a table would be null
		int missing = 0;
		for (int lang : {PS3_LANG_SWEDISH, PS3_LANG_ENGLISH_US}) {
			host_lang = lang; lang_init();
			for (int i = 0; i < STR_COUNT; i++) if (!T((StrId)i)) { printf("FAIL text %d missing (%s)\n", i, lang == PS3_LANG_SWEDISH ? "sv" : "en"); missing++; }
		}
		host_lang = PS3_LANG_SWEDISH; lang_init();
		if (missing) g_fail += missing; else printf("ok   every text in both languages\n");
	}

	// three drives, nothing ripping: the three kinds of earlier rip
	reset_all(); usb(0, 0); usb(1, 1);
	disc(0, "Motorhead", "", "SLUS_007.17", 647); ripped(0, 0, 277);
	disc(1, "Final Fantasy IX (Disc 1)", "Final Fantasy IX (Europe) (Disc 1)", "SLES_029.65", 706); ripped(1, 1, 0);
	disc(2, "Croc 2", "Croc 2 (Europe) (En,Fr,De,Es,It,Nl,Sv)", "SLES_020.88", 654); ripped(2, -1, 0);
	frame("idle-internal-errors", 0);
	frame("idle-usb0-verified", 1);
	frame("idle-usb1-unchecked", 2);

	// ripping: many events, the slow re-reads, every tab busy
	reset_all(); usb(0, 0); usb(1, 1);
	disc(0, "Motorhead", "", "SLUS_007.17", 647); ripping(0, 288669, 288669, 2, 18, 1);
	disc(1, "Final Fantasy IX (Disc 3)", "Final Fantasy IX (Europe) (Disc 3)", "SLES_229.65", 695); ripping(1, 32768, 310277, 0, 0, 0);
	disc(2, "Croc 2", "Croc 2 (Europe) (En,Fr,De,Es,It,Nl,Sv)", "SLES_020.88", 654); ripping(2, 120000, 291570, 0, 4, 0);
	frame("ripping-rereads-events", 0);
	g_cards[0].pg_scroll = 200; frame("ripping-scrolled-middle", 0);   // the page scrolled: arrows up and down
	g_cards[0].pg_scroll = 100000; frame("ripping-scrolled-bottom", 0);
	g_cards[0].pg_scroll = 0;
	frame("ripping-plain", 1);
	{   // a repair recomputing its MD5s: the page says so, with the percentage
		Card &k = g_cards[1]; k.repairing = 1;
		OnCard o(1); g_phase = 3; g_md5_total[1] = 11906; g_md5_done[1] = 5000;
		frame("repair-md5", 1);
		g_md5_total[1] = 0; g_phase = 0; k.repairing = 0;
	}
	{   // /status.json for the same drives: valid JSON (checked by python) with every drive
		static char js[8192];
		status_json(js, sizeof(js));
		FILE *f = fopen("out/status.json", "w"); if (f) { fputs(js, f); fclose(f); }
		if (system("python3 -c 'import json,sys; d=json.load(open(\"out/status.json\")); sys.exit(0 if len(d[\"drives\"])==3 else 1)'")) { printf("FAIL status.json\n"); g_fail++; }
		else printf("ok   status.json\n");
	}
	reset_all(); usb(0, 0);
	disc(0, "Mega Man X4", "Mega Man X4 (USA)", "SLUS_005.61", 564); ripping(0, 251793, 251793, 1, 0, 0);
	g_pass2_total = 41237; g_pass2_done = 12000;
	frame("ripping-verify-pass", 0);

	// a very long name, a drive with no disc, one drive only
	reset_all(); usb(0, 0);
	disc(0, "Final Fantasy IX (Disc 1)", "Final Fantasy IX (Europe) (Disc 1) (En,Fr,De,Es,It) (Rev 1) (Greatest Hits Collector Edition)", "SLES_029.65", 706);
	ripped(0, -1, 12);
	g_cards[1].present = 0;
	frame("long-name", 0);
	frame("empty-usb", 1);
	g_cards[1].usb_dead = 1; frame("usb-not-answering", 1); g_cards[1].usb_dead = 0;
	reset_all();
	disc(0, "Alundra", "Alundra (Europe)", "SLES_011.35", 577); ripped(0, 1, 0);
	frame("single-drive", 0);
	// ejected after a rip: the last rip's line and how it compared with Redump
	reset_all(); usb(0, 0); usb(1, 1);
	for (int c = 0; c < 3; c++) {
		Card &k = g_cards[c];
		k.present = 1; k.state = DISC_NONE;
		snprintf(k.res_disc, sizeof(k.res_disc), "%s", c == 0 ? "Alundra (Europe) (SLES_011.35)" : c == 1 ? "Motorhead (SLUS_007.17)" : "Croc 2 (Europe) (En,Fr,De,Es,It,Nl,Sv) (SLES_020.88)");
		snprintf(k.res_key, sizeof(k.res_key), "%s", c == 0 ? "SLES_011.35" : c == 1 ? "SLUS_007.17" : "SLES_020.88");
		snprintf(k.res, sizeof(k.res), "%s", c == 1 ? "klar på 1:12:40 · 239 oläsbara · 39 lagade" : "klar på 14:02");
		k.res_col = c == 1 ? 0xF0C060 : 0x7CE0A0;
		k.res_rd = c == 0 ? 1 : c == 1 ? 0 : -1;
		k.res_bad = c == 1;
	}
	frame("ejected-verified", 0);
	frame("ejected-nomatch", 1);
	frame("ejected-unchecked", 2);
	{   // after a rip another disc goes in: the last rip's line is about the old disc and is not shown;
		// the same disc in again: it is
		auto last_shown = []() { for (auto &b : g_boxes) if (b.text.find("Senaste rippningen") != std::string::npos) return true; return false; };
		Card &k = g_cards[0];
		snprintf(k.res_key, sizeof(k.res_key), "SLES_011.35");
		disc(0, "WipEout 3 - Special Edition", "", "SCES_028.45", 615);
		frame("other-disc-in", 0);
		if (last_shown()) { printf("FAIL another disc's page shows the last rip of the old one\n"); g_fail++; }
		disc(0, "Alundra", "Alundra (Europe)", "SLES_011.35", 577);
		frame("same-disc-in", 0);
		if (!last_shown()) { printf("FAIL the same disc in again: the last rip's line is missing\n"); g_fail++; }
	}

	{   // About, in Swedish here and in English below
		g_boxes.clear(); about_draw(); save("about"); size_t b0 = g_fail; check("about"); printf("%s about\n", g_fail == (int)b0 ? "ok  " : "FAIL");
	}
	// English
	// for the README: one verified disc, one ripping, one with errors (English, drawn below)
	host_lang = PS3_LANG_ENGLISH_US; lang_init();
	reset_all(); usb(0, 0); usb(1, 1);
	disc(0, "Motorhead", "", "SLUS_007.17", 647); ripped(0, 0, 277);
	disc(1, "Final Fantasy IX (Disc 1)", "Final Fantasy IX (Europe) (Disc 1)", "SLES_029.65", 706); ripping(1, 100000, 314937, 0, 6, 0);
	frame("english-idle", 0);
	frame("english-ripping", 1);
	reset_all(); usb(0, 0); usb(1, 1);
	disc(0, "Final Fantasy IX (Disc 1)", "Final Fantasy IX (Europe) (Disc 1)", "SLES_029.65", 706); ripped(0, 1, 0);
	disc(1, "Alundra", "Alundra (Europe)", "SLES_011.35", 577); ripping(1, 142000, 257659, 0, 0, 0);
	disc(2, "Motorhead", "", "SLUS_007.17", 647); ripped(2, 0, 277);
	frame("readme-tabs", 0);
	frame("readme-ripping", 1);
	{   // the settings: the first row (the game list), and the About row at the end
		DriveCfg sd = g_std; sd.autostart = 1;
		const time_t when = 1793530800;   // 2026-11-01, when the local copy was downloaded
		auto sframe = [&](const char *name, int row, const char *must) {
			g_boxes.clear(); settings_draw(row, sd, RD_LOCAL, 1, 1, 1, "2026-06-15 11-55-46", when); save(name); size_t b0 = g_fail; check(name);
			if (must && !has_text(must)) { printf("FAIL %s: no \"%s\"\n", name, must); g_fail++; }
			printf("%s %s\n", g_fail == (int)b0 ? "ok  " : "FAIL", name);
		};
		sframe("settings", SR_GAMES, "Games");
		if (!has_text("Open") || has_text("‹  ›")) { printf("FAIL settings: the Games row is opened with X\n"); g_fail++; }
		sframe("settings-speed-row", SR_SPEED, "Read speed");
		sframe("settings-about-row", SR_ABOUT, "About");
		// the audit log rows, just above About: the log (X opens it) and the size of its sector data
		g_audit_mb = 64;
		sframe("settings-audit-row", SR_AUDIT_MB, "64 MB");
		sframe("settings-audit-log-row", SR_AUDIT, "Open");
		g_audit_mb = 256;
		host_lang = PS3_LANG_SWEDISH; lang_init();
		sframe("settings-sv", SR_GAMES, "Spel");
		host_lang = PS3_LANG_ENGLISH_US; lang_init();
		if (SR_COUNT != 14 || SR_GAMES != 0 || SR_ABOUT != SR_COUNT - 1) { printf("FAIL settings: 14 rows, Games first, About last\n"); g_fail++; }
	}

	{   // settings in the audit log: what changed, where it came from, no secrets
		system("rm -rf out/audit out/psxrip.cfg");
		audit_init("out/audit", &vf_io);
		audit_set_hooks(set_setting_hook, NULL);
		g_cfg_primed = false; g_webhook_hdr[0][0] = 0; g_webhook[0] = 0;
		int bad = 0;
		AuditRow r[20];
		auto rows = [&]() { return audit_events(AU_SETTINGS, NULL, r, 20); };
		g_input_src = AU_PAD;
		cfg_save();   // the first save after start only primes the last text
		if (rows() != 0) { printf("FAIL the first cfg_save logged %d rows\n", rows()); bad++; }
		g_std.speed = 2;
		cfg_save();
		int n = rows();
		if (n != 1 || strcmp(r[0].kind, "setting") || strcmp(r[0].what, "speed") || strcmp(r[0].before, "0") || strcmp(r[0].after, "2") || r[0].src != AU_PAD)
			{ printf("FAIL speed change: %d rows, %s %s -> %s\n", n, n ? r[0].what : "-", n ? r[0].before : "-", n ? r[0].after : "-"); bad++; }
		cfg_save();
		if (rows() != 1) { printf("FAIL a save without a change logged a row\n"); bad++; }
		snprintf(g_webhook_hdr[0], sizeof(g_webhook_hdr[0]), "X-Token: topsecret");
		snprintf(g_webhook, sizeof(g_webhook), "http://h/secretid");
		cfg_save();
		n = rows();
		bool hdr = false, url = false;
		for (int i = 0; i < n; i++) {
			if (!strcmp(r[i].what, "webhook_header1") && !strcmp(r[i].before, "cleared") && !strcmp(r[i].after, "set")) hdr = true;
			if (!strcmp(r[i].what, "webhook") && !strcmp(r[i].before, "cleared") && !strcmp(r[i].after, "set")) url = true;
		}
		if (n != 3 || !hdr || !url) { printf("FAIL header/address rows: %d rows, header %d, address %d\n", n, hdr, url); bad++; }
		{   // the secrets are nowhere in the audit directory
			int leak = system("grep -rq -e topsecret -e secretid out/audit");
			if (leak == 0) { printf("FAIL a secret reached the audit log\n"); bad++; }
		}
		// the remote pad: a press from the web page is a remote source
		g_remote_btn = B_CROSS;
		int p = pad_pressed();
		if (!(p & B_CROSS) || g_input_src != AU_REMOTE) { printf("FAIL remote press source: %d\n", (int)g_input_src); bad++; }
		g_std.eject = 0;
		cfg_save();
		n = rows();
		if (n != 4 || r[0].src != AU_REMOTE || strcmp(r[0].what, "eject")) { printf("FAIL a change after a remote press is not from the remote\n"); bad++; }
		// undo through the hook: the setting goes back and the undo is its own source
		char why[200];
		g_std.speed = 2;
		uint32_t sid = 0;
		for (int i = 0; i < n; i++) if (!strcmp(r[i].what, "speed")) sid = r[i].id;
		if (audit_undo_row(sid, why, sizeof why) != 0 || g_std.speed != 0) { printf("FAIL undo of speed: %s (speed %d)\n", why, g_std.speed); bad++; }
		n = rows();
		bool us = false, um = false;   // the hook's save row, then the undo marker row
		for (int i = 0; i < n; i++) {
			if (r[i].src == AU_UNDO && !strcmp(r[i].kind, "setting") && !strcmp(r[i].what, "speed") && !strcmp(r[i].after, "0")) us = true;
			if (r[i].src == AU_UNDO && !strcmp(r[i].kind, "undo")) um = true;
		}
		if (!us || !um) { printf("FAIL the undo's rows: setting %d, marker %d\n", us, um); bad++; }
		// R4: a header (or the address) is never put back with "set" / "cleared"
		snprintf(g_webhook_hdr[0], sizeof(g_webhook_hdr[0]), "X-Token: topsecret");
		uint32_t hid = 0;
		n = rows();
		for (int i = 0; i < n; i++) if (!strcmp(r[i].what, "webhook_header1")) hid = r[i].id;
		if (set_setting_hook("webhook_header1", "cleared") == 0 || strcmp(g_webhook_hdr[0], "X-Token: topsecret")) { printf("FAIL the hook accepted a header\n"); bad++; }
		if (audit_undo_row(hid, why, sizeof why) == 0 || strcmp(g_webhook_hdr[0], "X-Token: topsecret") || !strstr(why, "not kept in the log")) { printf("FAIL undo of a header row: \"%s\"\n", why); bad++; }
		for (int i = 0; i < n; i++)
			if (!strcmp(r[i].what, "webhook_header1") || !strcmp(r[i].what, "webhook")) if (r[i].reversible) { printf("FAIL a secret row reads reversible\n"); bad++; }
		// a boot with a config: the first change after it is logged, and so is an undo as the first one
		{
			system("rm -rf out/audit");
			audit_init("out/audit", &vf_io);
			audit_set_hooks(set_setting_hook, NULL);
			g_std.retries = 4; g_std.speed = 0; g_remote = 0;
			cfg_save();   // the config of the "earlier run"
			g_std.retries = 9; g_cfg_primed = false;
			cfg_load(); cfg_prime();   // boot: reads the file (retries=4 again)
			if (g_std.retries != 4) { printf("FAIL boot read the config\n"); bad++; }
			g_std.speed = 3;
			cfg_save();
			n = rows();
			if (n != 1 || strcmp(r[0].what, "speed") || strcmp(r[0].after, "3")) { printf("FAIL first change after boot not logged (%d rows)\n", n); bad++; }
			// the same for an undo as the very first action after a boot
			g_cfg_primed = false; cfg_load(); cfg_prime();
			if (set_setting_hook("speed", "2") != 0) { printf("FAIL hook speed\n"); bad++; }
			n = rows();
			if (n != 2 || r[0].src != AU_UNDO || strcmp(r[0].what, "speed") || strcmp(r[0].before, "3") || strcmp(r[0].after, "2")) { printf("FAIL first undo after boot not logged (%d rows)\n", n); bad++; }
			// the cards follow an undo like they follow the screens
			g_bdcfg = {1, 5, 1, 1, 0, 1};
			cfg_save();
			g_bdcfg = {1, 6, 2, 0, 1, 0};
			cfg_save();
			card_from(g_cards[0], g_bdcfg);
			if (set_setting_hook("drive_bd", "5,1,1,0,1") != 0 || g_cards[0].retries != 5 || g_cards[0].speed != 1 || g_cards[0].eject != 1 || g_cards[0].autostart != 0 || g_cards[0].check != 1)
				{ printf("FAIL card copy after drive_bd undo (retries %d)\n", g_cards[0].retries); bad++; }
			g_cards[0].recheck = 0;
			snprintf(g_outdir, sizeof(g_outdir), "/dev_hdd0/PSXISO");
			if (set_setting_hook("outdir", "/dev_hdd0/OTHER") != 0 || strcmp(g_cards[0].outdir, "/dev_hdd0/OTHER") || strcmp(g_cards[0].dest, "/dev_hdd0/OTHER") || !g_cards[0].recheck)
				{ printf("FAIL card outdir after undo\n"); bad++; }
			// M1: the card takes the global destination itself (the copy is not made onto itself)
			snprintf(g_outdir, sizeof(g_outdir), "/dev_hdd0/SELF");
			card_apply_outdir(0, g_outdir);
			if (strcmp(g_outdir, "/dev_hdd0/SELF") || strcmp(g_cards[0].outdir, "/dev_hdd0/SELF") || strcmp(g_cards[0].dest, "/dev_hdd0/SELF"))
				{ printf("FAIL card_apply_outdir with the global itself: %s\n", g_outdir); bad++; }
			// M2: a card with a running job keeps its destination and drive settings until the job ends
			{
				Card &k0 = g_cards[0];
				snprintf(g_outdir, sizeof(g_outdir), "/dev_hdd0/OTHER");
				card_apply_outdir(0, g_outdir);
				card_from(k0, g_bdcfg);
				int sp = k0.speed;
				k0.job = 1; k0.recheck = 0;
				if (set_setting_hook("outdir", "/dev_hdd0/BUSY") != 0 || strcmp(g_outdir, "/dev_hdd0/BUSY") || strcmp(k0.outdir, "/dev_hdd0/OTHER") || strcmp(k0.dest, "/dev_hdd0/OTHER") || !k0.recheck)
					{ printf("FAIL outdir undo on a busy card: card %s / %s, global %s\n", k0.outdir, k0.dest, g_outdir); bad++; }
				if (set_setting_hook("drive_bd", "5,3,1,0,1") != 0 || g_bdcfg.speed != 3 || k0.speed != sp)
					{ printf("FAIL drive_bd undo on a busy card: card speed %d\n", k0.speed); bad++; }
				cards_take_pending();   // still busy: nothing
				if (strcmp(k0.outdir, "/dev_hdd0/OTHER") || k0.speed != sp) { printf("FAIL pending applied while busy\n"); bad++; }
				k0.job = 0;
				cards_take_pending();   // the job ended
				if (strcmp(k0.outdir, "/dev_hdd0/BUSY") || strcmp(k0.dest, "/dev_hdd0/BUSY") || k0.speed != 3 || !k0.recheck)
					{ printf("FAIL pending after the job: card %s, speed %d\n", k0.outdir, k0.speed); bad++; }
			}
			for (int c = 0; c < NCARDS; c++) g_cards[c].recheck = 0;
			set_setting_hook("name_pattern", "%D");
			bool all = true;
			for (int c = 0; c < NCARDS; c++) if (!g_cards[c].recheck) all = false;
			if (!all || strcmp(g_name_pat, "%D")) { printf("FAIL recheck after name_pattern undo\n"); bad++; }
			g_remote = 0;
			if (set_setting_hook("remote", "1") != 0 || g_remote != 1) { printf("FAIL remote undo\n"); bad++; }
			set_setting_hook("remote", "0");
		}
		// I2: the config file edited while the app was off (FTP): the boot logs the value it finds as
		// an app row, so the undo of an older row sees that the setting changed since
		{
			system("rm -rf out/audit");
			audit_init("out/audit", &vf_io);
			audit_set_hooks(set_setting_hook, NULL);
			g_webhook_hdr[0][0] = 0; g_webhook[0] = 0;
			g_cfg_primed = false; g_std.speed = 1; cfg_save();   // primes
			g_std.speed = 2; cfg_save();   // the log: speed 1 -> 2
			n = rows();
			uint32_t old = n == 1 ? r[0].id : 0;
			g_cfg_primed = false; g_std.speed = 3;   // the edit: written, not logged
			snprintf(g_webhook_hdr[0], sizeof(g_webhook_hdr[0]), "X-Token: topsecret");
			cfg_save();
			g_std.speed = 0; g_webhook_hdr[0][0] = 0;
			g_cfg_primed = false; cfg_load(); cfg_prime();   // boot
			n = rows();
			bool spd = false;
			for (int i = 0; i < n; i++)
				if (!strcmp(r[i].what, "speed") && r[i].src == AU_APP && !strcmp(r[i].before, "2") && !strcmp(r[i].after, "3")) spd = true;
			// the header was never in the log: nothing to compare with, no row
			if (g_std.speed != 3 || n != 2 || !spd) { printf("FAIL boot after an edit: %d rows, speed row %d\n", n, spd); bad++; }
			AuditReason rs;
			if (audit_undo_row(old, why, sizeof why, &rs) == 0 || rs.code != AW_SETTING_CHANGED || g_std.speed != 3)
				{ printf("FAIL undo over an edited setting: code %d, speed %d\n", (int)rs.code, g_std.speed); bad++; }
			g_cfg_primed = false; cfg_load(); cfg_prime();   // a second boot: nothing new
			if (rows() != 2) { printf("FAIL a boot without an edit logged %d rows\n", rows() - 2); bad++; }
			// a secret the log knows as set, cleared in the file: a set -> cleared row, never reversible
			g_webhook_hdr[0][0] = 0; cfg_save();   // the log: set -> cleared -> set
			snprintf(g_webhook_hdr[0], sizeof(g_webhook_hdr[0]), "X-Token: topsecret"); cfg_save();
			if (rows() != 4) { printf("FAIL header rows: %d\n", rows()); bad++; }
			g_cfg_primed = false; g_webhook_hdr[0][0] = 0; cfg_save();   // the edit
			g_cfg_primed = false; cfg_load(); cfg_prime();
			n = rows();
			if (n != 5 || strcmp(r[0].what, "webhook_header1") || strcmp(r[0].before, "set") || strcmp(r[0].after, "cleared") || r[0].src != AU_APP || r[0].reversible)
				{ printf("FAIL boot after a header edit: %d rows, %s %s -> %s\n", n, r[0].what, r[0].before, r[0].after); bad++; }
			g_std.speed = 0;
		}
		// audit_mb: the listed sizes, anything else is 256
		cfg_apply_line("audit_mb=16");
		if (g_audit_mb != 16) { printf("FAIL audit_mb=16 gave %d\n", g_audit_mb); bad++; }
		cfg_apply_line("audit_mb=3");
		if (g_audit_mb != 256) { printf("FAIL audit_mb=3 gave %d\n", g_audit_mb); bad++; }
		if (bad) g_fail += bad; else printf("ok   settings in the audit log (changes, source, secrets, undo, audit_mb)\n");
	}
	{   // jobs and their sector writes in the audit log, and the check of a rip from its file
		system("rm -rf out/audit out/t4 && mkdir -p out/t4");
		audit_init("out/audit", &vf_io);
		audit_set_hooks(set_setting_hook, recheck_hook);
		int bad = 0;
		auto put_file = [](const char *p, const void *d, size_t n) { FILE *f = fopen(p, "wb"); fwrite(d, 1, n, f); fclose(f); };
		auto get_file = [](const char *p, std::string &s) { s.clear(); FILE *f = fopen(p, "rb"); if (!f) return false; char b[4096]; size_t n; while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n); fclose(f); return true; };
		static uint8_t img[5 * RAW_SIZE];
		for (size_t i = 0; i < sizeof img; i++) img[i] = (uint8_t)(i * 7 + i / RAW_SIZE);
		put_file("out/t4/img.bin", img, sizeof img);
		reset_all();
		disc(0, "Alundra", "Alundra (Europe)", "SLES_011.35", 577);
		Card &k = g_cards[0];
		{
			OnCard o(0);
			g_leadout = 5;
			char key[96];
			disc_key(key, sizeof key);
			if (strcmp(key, "SLES_011.35:5")) { printf("FAIL disc key \"%s\"\n", key); bad++; }
			snprintf(k.exist, sizeof(k.exist), "out/t4/img");
			snprintf(g_base, sizeof(g_base), "out/t4/img");
			audit_card_begin(0, AU_PAD, "repair");
			if (!k.audit_job || strcmp(k.rd_before, "unchecked")) { printf("FAIL job begin: job %u, before \"%s\"\n", (unsigned)k.audit_job, k.rd_before); bad++; }
			// a sector written into an image opened r+b: the new bytes in the file, a sector row with the old ones
			g_out = vf_open("out/t4/img.bin", "r+b");
			uint8_t sec[RAW_SIZE];
			memset(sec, 0xAB, sizeof sec);
			if (!g_out || write_sector_audited(2, sec) != 0) { printf("FAIL write_sector_audited on r+b\n"); bad++; }
			// past the end of the image: the old bytes cannot be read, so the write fails and nothing is logged
			if (g_out && write_sector_audited(40, sec) == 0) { printf("FAIL a sector without readable old bytes was written\n"); bad++; }
			if (g_out) vf_close(g_out);
			g_out = NULL;
			std::string s;
			get_file("out/t4/img.bin", s);
			if (s.size() != sizeof img || memcmp(s.data() + 2 * RAW_SIZE, sec, RAW_SIZE) || memcmp(s.data(), img, 2 * RAW_SIZE) || memcmp(s.data() + 3 * RAW_SIZE, img + 3 * RAW_SIZE, 2 * RAW_SIZE))
				{ printf("FAIL the image after the write (size %zu)\n", s.size()); bad++; }
			AuditRow r[8];
			int n = audit_rows(k.audit_job, r, 0, 8), sectors = 0;
			for (int i = 0; i < n; i++) if (!strcmp(r[i].kind, "sector")) {
				sectors++;
				if (r[i].lba != 2 || strcmp(r[i].file, "out/t4/img.bin") || strcmp(r[i].disc, "SLES_011.35:5")) { printf("FAIL sector row: lba %u file %s disc %s\n", (unsigned)r[i].lba, r[i].file, r[i].disc); bad++; }
			}
			if (sectors != 1) { printf("FAIL %d sector row(s), want 1\n", sectors); bad++; }
			char bp[64];
			snprintf(bp, sizeof bp, "out/audit/job%u.bin", (unsigned)k.audit_job);
			if (!get_file(bp, s) || s.size() != 2 * RAW_SIZE || memcmp(s.data(), img + 2 * RAW_SIZE, RAW_SIZE) || memcmp(s.data() + RAW_SIZE, sec, RAW_SIZE))
				{ printf("FAIL the job's sector bytes (old, new): %zu bytes\n", s.size()); bad++; }
			// the Redump result as a row: before = the rip's earlier line
			g_rd_result = 1; snprintf(g_rd_result_name, sizeof(g_rd_result_name), "Alundra (Europe)");
			audit_card_redump(0);
			uint32_t job = k.audit_job;
			audit_card_end(0, "finished");
			n = audit_rows(job, r, 0, 8);
			bool rd = false, res = false;
			for (int i = 0; i < n; i++) {
				if (!strcmp(r[i].kind, "redump") && !strcmp(r[i].before, "unchecked") && !strcmp(r[i].after, "verified Alundra (Europe)")) rd = true;
				if (!strcmp(r[i].kind, "result") && !strcmp(r[i].after, "finished")) res = true;
			}
			if (!rd || !res || k.audit_job) { printf("FAIL redump row %d, result row %d, job cleared %d\n", rd, res, !k.audit_job); bad++; }
			// the rip's earlier line from its log
			put_file("out/t4/img.log", "PSX Ripper rip log\nredump=verified Alundra (Europe)\n", 51);
			audit_card_begin(0, AU_REMOTE, "check");
			if (strcmp(k.rd_before, "verified Alundra (Europe)") || k.job_src != AU_REMOTE) { printf("FAIL earlier redump line \"%s\"\n", k.rd_before); bad++; }
			audit_card_end(0, "cancelled");
			// a rip writes its file w+b: the old bytes can be read back before a sector is written again
			g_out = vf_open("out/t4/new.bin", "w+b");
			audit_card_begin(0, AU_APP, "rip");
			if (!g_out || vf_write(g_out, img, sizeof img) != sizeof img || write_sector_audited(1, sec) != 0) { printf("FAIL write_sector_audited on a w+b rip file\n"); bad++; }
			if (g_out) vf_close(g_out);
			g_out = NULL;
			audit_card_end(0, "finished");
		}
		// how each job ends: every way out names its result
		{
			OnCard o(0);
			g_disc_lost[0] = 0; g_abort = 0;
			const char *a = rip_result(1), *b = rip_result(0);
			g_abort = 1; const char *c = rip_result(0), *d = repair_result(1);
			g_disc_lost[0] = 1; const char *e = rip_result(0), *f = repair_result(1);
			g_disc_lost[0] = 0; g_abort = 0; const char *g = repair_result(0), *h = repair_result(1);
			if (strcmp(a, "finished") || strcmp(b, "failed") || strcmp(c, "cancelled") || strcmp(d, "cancelled") || strcmp(e, "disc_lost") || strcmp(f, "disc_lost") || strcmp(g, "failed") || strcmp(h, "finished"))
				{ printf("FAIL job results: %s %s %s %s %s %s %s %s\n", a, b, c, d, e, f, g, h); bad++; }
		}
		// a job whose thread cannot start (always on the host) is ended at once: it never stays "running"
		{
			snprintf(k.exist, sizeof(k.exist), "out/t4/img");
			k.job = 0;
			rip_job_start(0, AU_PAD, 0);
			repair_job_start(0, AU_APP);
			check_job_start(0, AU_REMOTE);
			AuditRow r[30];
			int n = audit_events(AU_RIPS, "SLES_011.35:5", r, 30), open = 0;
			for (int i = 0; i < n; i++) {
				if (strcmp(r[i].kind, "job")) continue;
				AuditRow rr[30];
				int m = audit_rows(r[i].job, rr, 0, 30), ended = 0;
				for (int j = 0; j < m; j++) if (!strcmp(rr[j].kind, "result")) ended = 1;
				if (!ended) open++;
			}
			if (n != 6 || open || k.audit_job || k.job) { printf("FAIL jobs that could not start: %d events, %d not ended\n", n, open); bad++; }
		}
		// check_image: MD5 per track from the cue, the .md5 and the log written
		{
			static uint8_t two[5 * RAW_SIZE];
			for (size_t i = 0; i < sizeof two; i++) two[i] = (uint8_t)(i * 13 + 5);
			put_file("out/t4/two.bin", two, sizeof two);
			const char *cue = "FILE \"two.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n  TRACK 02 AUDIO\n    INDEX 00 00:00:03\n    INDEX 01 00:00:04\n";
			put_file("out/t4/two.cue", cue, strlen(cue));
			const char *log = "PSX Ripper rip log\ntitle=Two\nsectors=5\ntrack=01 start=0 md5=00000000000000000000000000000000\ntrack=02 start=3 md5=11111111111111111111111111111111\nredump=verified Two (Europe)\nunreadable_count=0\n";
			put_file("out/t4/two.log", log, strlen(log));
			char want[2][33];
			for (int t = 0; t < 2; t++) {
				Md5 m; uint8_t d[16];
				md5_init(&m);
				md5_update(&m, two + (t ? 3 : 0) * RAW_SIZE, (t ? 2 : 3) * RAW_SIZE);
				md5_final(&m, d);
				md5_hex(d, want[t]);
			}
			int r = check_image("out/t4/two");
			std::string s;
			char exp[200];
			snprintf(exp, sizeof exp, "%s  track 01\n%s  track 02\n", want[0], want[1]);
			if (r != 0 || !get_file("out/t4/two.md5", s) || s != exp) { printf("FAIL check_image .md5 (%d): \"%s\"\n", r, s.c_str()); bad++; }
			// the log: the tracks' MD5 and the Redump line now (no local copy on the host: unchecked)
			get_file("out/t4/two.log", s);
			char t1[80], t2[80];
			snprintf(t1, sizeof t1, "track=01 start=0 md5=%s\n", want[0]);
			snprintf(t2, sizeof t2, "track=02 start=3 md5=%s\n", want[1]);
			if (s.find(t1) == std::string::npos || s.find(t2) == std::string::npos || s.find("redump=unchecked\n") == std::string::npos || s.find("verified") != std::string::npos || s.find("unreadable_count=0\n") == std::string::npos)
				{ printf("FAIL check_image log:\n%s", s.c_str()); bad++; }
			// the audit's recheck hook takes the image's path
			remove("out/t4/two.md5");
			recheck_hook("out/t4/two.bin", NULL, NULL, 0, 0);
			if (!get_file("out/t4/two.md5", s) || s != exp) { printf("FAIL recheck hook\n"); bad++; }
			if (check_image("out/t4/none") == 0) { printf("FAIL check_image without an image\n"); bad++; }
		}
		// R9: undoing a repair puts its sectors back into the rip's log, so a later repair reads them again
		{
			static uint8_t rep[4 * RAW_SIZE];
			memset(rep, 0x5A, sizeof rep);
			memset(rep + RAW_SIZE, 0, RAW_SIZE);   // sector 1: unreadable, the rip filled it with zeros
			memset(rep + 2 * RAW_SIZE, 0x66, RAW_SIZE);   // sector 2: bad bytes
			put_file("out/t4/rep.bin", rep, sizeof rep);
			const char *cue = "FILE \"rep.bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n";
			put_file("out/t4/rep.cue", cue, strlen(cue));
			// the log after the repair fixed both: no sector lines, counts 0
			const char *log = "PSX Ripper rip log\nsectors=4\ntrack=01 start=0 md5=00000000000000000000000000000000\nredump=no_match\nunreadable_count=0\nbad_count=0\nfixed_count=2\nuncertain=3 unconfirmed\n";
			put_file("out/t4/rep.log", log, strlen(log));
			OnCard o(0);
			snprintf(k.exist, sizeof(k.exist), "out/t4/rep");
			snprintf(g_base, sizeof(g_base), "out/t4/rep");
			g_leadout = 4;
			audit_card_begin(0, AU_PAD, "repair");
			uint32_t job = k.audit_job;
			g_out = vf_open("out/t4/rep.bin", "r+b");
			uint8_t good[RAW_SIZE];
			memset(good, 0x77, sizeof good);
			if (!g_out || write_sector_audited(1, good) || write_sector_audited(2, good)) { printf("FAIL repair writes\n"); bad++; }
			if (g_out) vf_close(g_out);
			g_out = NULL;
			audit_card_end(0, "finished");
			char why[200] = "";
			if (audit_undo_job(job, why, sizeof why) != 0) { printf("FAIL undo of the repair: %s\n", why); bad++; }
			std::string s;
			get_file("out/t4/rep.log", s);
			auto has = [&](const char *l) { return s.find(l) != std::string::npos; };
			if (!has("\nunreadable=1\n") || !has("\nbad=2\n") || !has("unreadable_count=1\n") || !has("bad_count=1\n") || !has("fixed_count=0\n") || !has("uncertain=3 unconfirmed\n"))
				{ printf("FAIL the log after the undo:\n%s", s.c_str()); bad++; }
			snprintf(g_base, sizeof(g_base), "out/t4/rep");
			int open = riplog_open_count();
			if (open != 3) { printf("FAIL riplog_open_count after the undo: %d, want 3\n", open); bad++; }
			// already listed: not added twice, counts unchanged
			uint32_t l2[2] = {1, 2};
			uint8_t z2[2] = {1, 0};
			recheck_hook("out/t4/rep.bin", l2, z2, 2, 0);
			get_file("out/t4/rep.log", s);
			size_t a = s.find("unreadable=1\n"), b = s.find("bad=2\n");
			if (a == std::string::npos || s.find("unreadable=1\n", a + 1) != std::string::npos || s.find("bad=2\n", b + 1) != std::string::npos || !has("unreadable_count=1\n") || !has("bad_count=1\n"))
				{ printf("FAIL the log after a second recheck:\n%s", s.c_str()); bad++; }
			// undoing the undo (a redo): the repaired bytes again, the sectors out of the log again
			if (audit_undo_job(job, why, sizeof why) != 0) { printf("FAIL redo of the repair: %s\n", why); bad++; }
			get_file("out/t4/rep.bin", s);
			if (s.size() != sizeof rep || memcmp(s.data() + RAW_SIZE, good, RAW_SIZE) || memcmp(s.data() + 2 * RAW_SIZE, good, RAW_SIZE)) { printf("FAIL the image after the redo\n"); bad++; }
			get_file("out/t4/rep.log", s);
			if (has("unreadable=1\n") || has("bad=2\n") || !has("unreadable_count=0\n") || !has("bad_count=0\n") || !has("fixed_count=2\n") || !has("uncertain=3 unconfirmed\n"))
				{ printf("FAIL the log after the redo:\n%s", s.c_str()); bad++; }
			snprintf(g_base, sizeof(g_base), "out/t4/rep");
			if ((open = riplog_open_count()) != 1) { printf("FAIL riplog_open_count after the redo: %d, want 1\n", open); bad++; }
			// and undone again: back in the log once more
			if (audit_undo_job(job, why, sizeof why) != 0) { printf("FAIL second undo of the repair: %s\n", why); bad++; }
			get_file("out/t4/rep.log", s);
			if (!has("\nunreadable=1\n") || !has("\nbad=2\n") || !has("unreadable_count=1\n") || !has("bad_count=1\n") || !has("fixed_count=0\n"))
				{ printf("FAIL the log after the second undo:\n%s", s.c_str()); bad++; }
		}
		audit_set_hooks(set_setting_hook, NULL);
		system("rm -rf out/audit");
		audit_init("out/audit", &vf_io);
		reset_all();
		if (bad) g_fail += bad; else printf("ok   jobs and sectors in the audit log (sector rows, results, redump, check_image)\n");
	}
	// the audit log screen, in both languages
	host_lang = PS3_LANG_SWEDISH; lang_init(); g_sfx_sv = 1;
	audit_scenarios();
	host_lang = PS3_LANG_ENGLISH_US; lang_init(); g_sfx_sv = 0;
	audit_scenarios();
	{   // undoing an audit_mb row applies the size cap at once (the kept sectors over it go)
		system("rm -rf out/audit out/psxrip.cfg");
		audit_init("out/audit", &vf_io);
		audit_set_hooks(set_setting_hook, NULL);
		int bad = 0;
		g_cfg_primed = false; g_input_src = AU_PAD;
		g_audit_mb = 1; audit_apply_cap(); cfg_save();   // primes
		g_audit_mb = 256; cfg_save(); audit_apply_cap();
		AuditRow e[4];
		int n = audit_events(AU_SETTINGS, NULL, e, 4);
		uint32_t job = audit_job_begin(AU_APP, "SLES_011.35:500", "Alundra", "repair");
		uint8_t a[RAW_SIZE], b[RAW_SIZE];
		memset(a, 1, sizeof a); memset(b, 2, sizeof b);
		for (uint32_t l = 0; l < 500; l++) audit_sector(job, "out/t5/none.bin", l, a, b);   // 2.3 MB kept
		audit_job_end(job, "finished");
		char bp[64], why[200];
		snprintf(bp, sizeof bp, "out/audit/job%u.bin", (unsigned)job);
		bool kept = vf_exists(bp);
		int rc = n == 1 && !strcmp(e[0].what, "audit_mb") ? audit_undo_row(e[0].id, why, sizeof why) : -2;
		if (!kept || rc != 0 || g_audit_mb != 1 || vf_exists(bp)) { printf("FAIL undo of audit_mb: rows %d, kept %d, rc %d, audit_mb %d, file left %d\n", n, kept, rc, g_audit_mb, vf_exists(bp)); bad++; }
		g_audit_mb = 256; audit_apply_cap();
		audit_set_hooks(set_setting_hook, NULL);
		if (bad) g_fail += bad; else printf("ok   undoing an audit_mb row applies the cap at once\n");
	}
	game_source_tests();
	// the game list screen, in both languages
	host_lang = PS3_LANG_SWEDISH; lang_init(); g_sfx_sv = 1;
	games_scenarios();
	games_check_scenarios();
	host_lang = PS3_LANG_ENGLISH_US; lang_init(); g_sfx_sv = 0;
	games_scenarios();
	games_check_scenarios();
	{ g_boxes.clear(); about_draw(); save("about-english"); size_t b0 = g_fail; check("about-english"); printf("%s about-english\n", g_fail == (int)b0 ? "ok  " : "FAIL"); }
	printf("%s: %d problem(s)\n", g_fail ? "FAILED" : "passed", g_fail);
	return g_fail ? 1 : 0;
}
