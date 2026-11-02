// UI tests on the host: the app's own main.cpp, built against stubs (tests/host/include), draws
// scenarios into the canvas. Each frame is written as a PNG (tests/host/out) and checked:
//  - texts that overlap other texts,
//  - text outside the screen or, above the general row, outside the content area,
//  - characters the font does not have (they come out as "?").
// Run: make -C tests/host   (exit status 1 when a check fails)
#include "host.h"
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
		snprintf(k.res, sizeof(k.res), "%s", c == 1 ? "klar på 1:12:40 · 239 oläsbara · 39 lagade" : "klar på 14:02");
		k.res_col = c == 1 ? 0xF0C060 : 0x7CE0A0;
		k.res_rd = c == 0 ? 1 : c == 1 ? 0 : -1;
		k.res_bad = c == 1;
	}
	frame("ejected-verified", 0);
	frame("ejected-nomatch", 1);
	frame("ejected-unchecked", 2);

	// English
	host_lang = PS3_LANG_ENGLISH_US; lang_init();
	reset_all(); usb(0, 0); usb(1, 1);
	disc(0, "Motorhead", "", "SLUS_007.17", 647); ripped(0, 0, 277);
	disc(1, "Final Fantasy IX (Disc 1)", "Final Fantasy IX (Europe) (Disc 1)", "SLES_029.65", 706); ripping(1, 100000, 314937, 0, 6, 0);
	frame("english-idle", 0);
	frame("english-ripping", 1);

	printf("%s: %d problem(s)\n", g_fail ? "FAILED" : "passed", g_fail);
	return g_fail ? 1 : 0;
}
