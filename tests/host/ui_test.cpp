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
	{   // the settings: the first row, and the About row at the end
		DriveCfg sd = g_std; sd.autostart = 1;
		const time_t when = 1793530800;   // 2026-11-01, when the local copy was downloaded
		g_boxes.clear(); settings_draw(0, sd, RD_LOCAL, 1, 1, 1, "2026-06-15 11-55-46", when); save("settings"); size_t b0 = g_fail; check("settings");
		printf("%s settings\n", g_fail == (int)b0 ? "ok  " : "FAIL");
		g_boxes.clear(); settings_draw(10, sd, RD_LOCAL, 1, 1, 1, "2026-06-15 11-55-46", when); save("settings-about-row"); b0 = g_fail; check("settings-about-row");
		printf("%s settings-about-row\n", g_fail == (int)b0 ? "ok  " : "FAIL");
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
			for (int c = 0; c < NCARDS; c++) g_cards[c].recheck = 0;
			set_setting_hook("name_pattern", "%D");
			bool all = true;
			for (int c = 0; c < NCARDS; c++) if (!g_cards[c].recheck) all = false;
			if (!all || strcmp(g_name_pat, "%D")) { printf("FAIL recheck after name_pattern undo\n"); bad++; }
			g_remote = 0;
			if (set_setting_hook("remote", "1") != 0 || g_remote != 1) { printf("FAIL remote undo\n"); bad++; }
			set_setting_hook("remote", "0");
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
			recheck_hook("out/t4/two.bin");
			if (!get_file("out/t4/two.md5", s) || s != exp) { printf("FAIL recheck hook\n"); bad++; }
			if (check_image("out/t4/none") == 0) { printf("FAIL check_image without an image\n"); bad++; }
		}
		audit_set_hooks(set_setting_hook, NULL);
		system("rm -rf out/audit");
		audit_init("out/audit", &vf_io);
		reset_all();
		if (bad) g_fail += bad; else printf("ok   jobs and sectors in the audit log (sector rows, results, redump, check_image)\n");
	}
	{ g_boxes.clear(); about_draw(); save("about-english"); size_t b0 = g_fail; check("about-english"); printf("%s about-english\n", g_fail == (int)b0 ? "ok  " : "FAIL"); }
	printf("%s: %d problem(s)\n", g_fail ? "FAILED" : "passed", g_fail);
	return g_fail ? 1 : 0;
}
