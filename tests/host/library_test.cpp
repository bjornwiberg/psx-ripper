// Host tests of the game library module (source/library.cpp) on real folders. Run: make -C tests/host
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <utility>
#include "../../include/library.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; bad = 1; } } while (0)

// ---------------------------------------------------------------- the file interface, POSIX
static int io_list(const char *dir, void (*cb)(const char *, void *), void *ctx)
{
	DIR *d = opendir(dir);
	if (!d) return -1;
	struct dirent *e;
	while ((e = readdir(d))) cb(e->d_name, ctx);
	closedir(d);
	return 0;
}
static int io_stat_size(const char *p, uint64_t *size)
{
	struct stat st;
	if (stat(p, &st)) return -1;
	*size = (uint64_t)st.st_size;
	return 0;
}
static int io_read(const char *p, char *buf, int max)
{
	FILE *f = fopen(p, "rb");
	if (!f) return -1;
	int n = (int)fread(buf, 1, (size_t)max, f);
	fclose(f);
	return n;
}
static int io_exists(const char *p) { struct stat st; return !stat(p, &st); }
static int lc_list(const char *id) { return !strcmp(id, "SLES_012.26") || !strcmp(id, "SCES_014.31"); }
static const LibIo IO = {io_list, io_stat_size, io_read, io_exists, lc_list, "./out/library/sbi"};

static const char *DIR_ = "./out/library/PSXISO";
static const char *leaf_name(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }

static void fresh()
{
	DIR_ = "./out/library/PSXISO";
	if (system("rm -rf ./out/library && mkdir -p ./out/library/PSXISO ./out/library/USB/PSXISO ./out/library/sbi")) {}
}
static void put(const char *name, const char *text)
{
	char p[512];
	snprintf(p, sizeof p, "%s/%s", DIR_, name);
	FILE *f = fopen(p, "wb");
	if (f) { fputs(text, f); fclose(f); }
}
static void put_size(const char *name, uint64_t size)
{
	char p[512];
	snprintf(p, sizeof p, "%s/%s", DIR_, name);
	FILE *f = fopen(p, "wb");
	if (f) { if (ftruncate(fileno(f), (off_t)size)) {} fclose(f); }
}
// A rip of the app: <name>.bin (sectors of 2352), .cue with one data track, optionally a log.
static void rip(const char *name, unsigned sectors, const char *log)
{
	char f[300], c[600];
	snprintf(f, sizeof f, "%s.bin", name);
	put_size(f, (uint64_t)sectors * 2352);
	snprintf(c, sizeof c, "FILE \"%s.bin\" BINARY\r\n  TRACK 01 MODE2/2352\r\n    INDEX 01 00:00:00\r\n", name);
	snprintf(f, sizeof f, "%s.cue", name);
	put(f, c);
	if (log) { snprintf(f, sizeof f, "%s.log", name); put(f, log); }
}

static GameDisc g_d[600];
static Game g_g[600];

static int scan() { return lib_scan(DIR_, &IO, g_d, 600); }
static int find(int n, const char *leaf)
{
	for (int i = 0; i < n; i++) {
		const char *s = strrchr(g_d[i].base, '/');
		if (s && !strcmp(s + 1, leaf)) return i;
	}
	return -1;
}
static int has_file(const GameDisc &d, const char *name)
{
	char p[512], want[512];
	snprintf(want, sizeof want, "%s/%s", DIR_, name);
	for (int i = 0; lib_file_path(&d, i, p, sizeof p); i++) if (!strcmp(p, want)) return 1;
	return 0;
}

// ---------------------------------------------------------------- tests
static void sets_by_base_name(int &bad)
{
	fresh();
	rip("Alundra (Europe) [SLES_010.12]", 10, NULL);
	put("Alundra (Europe) [SLES_010.12].md5", "x");
	put("Alundra (Europe) [SLES_010.12].jpg", "x");
	rip("Croc [SLES_005.93]", 5, NULL);
	put("readme.txt", "not a game");
	put(".hidden.bin", "x");
	if (system("mkdir -p ./out/library/PSXISO/.trash")) {}
	int n = scan();
	CHECK(n == 2);
	int a = find(n, "Alundra (Europe) [SLES_010.12]");
	CHECK(a >= 0);
	if (a < 0) return;
	CHECK(g_d[a].nfiles == 4);
	CHECK(has_file(g_d[a], "Alundra (Europe) [SLES_010.12].bin"));
	CHECK(has_file(g_d[a], "Alundra (Europe) [SLES_010.12].cue"));
	CHECK(has_file(g_d[a], "Alundra (Europe) [SLES_010.12].md5"));
	CHECK(has_file(g_d[a], "Alundra (Europe) [SLES_010.12].jpg"));
	CHECK(!g_d[a].files_cut);
	CHECK(g_d[a].has_cover == 1 && g_d[a].has_log == 0);
	CHECK(!strcmp(g_d[a].id, "SLES_010.12"));
	CHECK(!strcmp(g_d[a].title, "Alundra (Europe)"));
	CHECK(g_d[a].size == 10 * 2352ull);
	CHECK(g_d[a].dup == -1);
	char img[300];
	snprintf(img, sizeof img, "%s/Alundra (Europe) [SLES_010.12].bin", DIR_);
	CHECK(!strcmp(g_d[a].image, img));
	CHECK(lib_scan("./out/library/nope", &IO, g_d, 600) == -1);
}

static void format_from_size_and_cue(int &bad)
{
	fresh();
	rip("Raw", 16, NULL);                         // 2352, cue MODE2/2352
	put_size("Cooked.iso", 2048ull * 7);          // 2048: not a multiple of 2352
	put_size("RawIso.img", 2352ull * 3);          // 2352 without cue
	put_size("Both.bin", 2352ull * 128);           // a multiple of both: the cue says 2048
	put("Both.cue", "FILE \"Both.bin\" BINARY\n  TRACK 01 MODE1/2048\n    INDEX 01 00:00:00\n");
	put_size("Multi (Track 1).bin", 2352ull * 4);
	put_size("Multi (Track 2).bin", 2352ull * 2);
	put("Multi.cue", "FILE \"Multi (Track 1).bin\" BINARY\n  TRACK 01 MODE2/2352\n    INDEX 01 00:00:00\n"
	                 "FILE \"Multi (Track 2).bin\" BINARY\n  TRACK 02 AUDIO\n    INDEX 01 00:00:00\n");
	put_size("Odd.iso", 1000);
	int n = scan();
	CHECK(n == 6);
	int i;
	CHECK((i = find(n, "Raw")) >= 0 && g_d[i].fmt == GF_2352 && g_d[i].tracks == 1);
	CHECK((i = find(n, "Cooked")) >= 0 && g_d[i].fmt == GF_2048 && g_d[i].tracks == 1 && g_d[i].st == GS_RERIP);
	CHECK((i = find(n, "RawIso")) >= 0 && g_d[i].fmt == GF_2352 && g_d[i].st == GS_UNCHECKED);
	CHECK((i = find(n, "Both")) >= 0 && g_d[i].fmt == GF_2048);
	CHECK((i = find(n, "Multi")) >= 0 && g_d[i].fmt == GF_2352 && g_d[i].tracks == 2 && g_d[i].size == 2352ull * 6
	      && g_d[i].nfiles == 3 && has_file(g_d[i], "Multi (Track 2).bin") && g_d[i].st == GS_UNCHECKED);
	CHECK((i = find(n, "Odd")) >= 0 && g_d[i].fmt == GF_UNKNOWN);
}

static const char *LOG_OK =
	"PSX Ripper rip log\ntitle=Alundra (Europe)  \xC2\xB7  SLES_010.12\nserial=SLES_010.12\n"
	"date=2026-10-01 12:30 UTC\ndrive=internal\nsectors=10\ncheck=on\nretries=3\n"
	"track=01 start=0 md5=0123456789abcdef0123456789abcdef\nredump=verified Alundra (Europe)\n"
	"unreadable_count=0\nbad_count=0\nfixed_count=0\n";

static void log_parsed(int &bad)
{
	fresh();
	rip("A [SLES_010.12]", 10, LOG_OK);
	rip("B", 10, "PSX Ripper rip log\r\ntitle=B game  \xC2\xB7  SLUS_000.01\r\nserial=SLUS_000.01\r\n"
	             "redump=no_match\r\nunreadable_count=2\r\nbad_count=1\r\nunreadable=100\r\nunreadable=101\r\n"
	             "bad=200\r\nuncertain=300 confirmed\r\nuncertain=301 unconfirmed\r\n");
	rip("C", 10, "PSX Ripper rip log\ntitle=C\nserial=\nredump=not_found\n");
	rip("D", 10, "PSX Ripper rip log\ntitle=D\nserial=\nredump=unchecked\n");
	rip("E", 10, "PSX Ripper rip log\ntitle=E\n");
	int n = scan(), i;
	CHECK(n == 5);
	CHECK((i = find(n, "A [SLES_010.12]")) >= 0);
	if (i >= 0) {
		CHECK(!strcmp(g_d[i].title, "Alundra (Europe)"));
		CHECK(!strcmp(g_d[i].rdname, "Alundra (Europe)"));
		CHECK(!strcmp(g_d[i].date, "2026-10-01 12:30 UTC"));
		CHECK(!strcmp(g_d[i].drive, "internal"));
		CHECK(g_d[i].st == GS_VERIFIED && g_d[i].rd == 1 && g_d[i].has_log && g_d[i].errors == 0);
	}
	CHECK((i = find(n, "B")) >= 0);
	if (i >= 0) {
		CHECK(!strcmp(g_d[i].id, "SLUS_000.01") && !strcmp(g_d[i].title, "B game"));
		CHECK(g_d[i].errors == 4 && g_d[i].bad == 1);   // 2 unreadable, 1 bad, 1 unconfirmed
		CHECK(g_d[i].st == GS_ERRORS && g_d[i].rd == 0);
		CHECK(!g_d[i].rdname[0] && !g_d[i].drive[0]);
	}
	CHECK((i = find(n, "C")) >= 0 && g_d[i].st == GS_NOMATCH && g_d[i].rd == 0 && !g_d[i].id[0]);
	CHECK((i = find(n, "D")) >= 0 && g_d[i].st == GS_UNCHECKED && g_d[i].rd == -1);
	CHECK((i = find(n, "E")) >= 0 && g_d[i].st == GS_UNCHECKED && g_d[i].rd == -1);
}

static void status_precedence(int &bad)
{
	fresh();
	// broken beats everything: a .part next to a verified rip with errors
	rip("P", 10, LOG_OK);
	put_size("P.bin.part", 2352);
	// 2048 with errors and verified in its log: re-rip
	put_size("R.iso", 2048ull * 5);
	put("R.log", "title=R\nredump=verified R\nunreadable=5\n");
	// errors beat verified
	rip("E", 10, "title=E\nredump=verified E\nbad=7\n");
	// a lone .part (a cancelled rip): broken
	put_size("Cancelled [SCES_000.01].bin.part", 2352 * 3);
	// a cue naming a missing file
	put("Missing.cue", "FILE \"Gone.bin\" BINARY\n  TRACK 01 MODE2/2352\n");
	// a .bin without a cue
	put_size("Loose.bin", 2352 * 2);
	// a log without an image
	put("Orphan.log", "title=Orphan\n");
	// a lone cover is not a game
	put("Cover only.jpg", "x");
	int n = scan(), i;
	CHECK(n == 7);
	CHECK((i = find(n, "P")) >= 0 && g_d[i].st == GS_BROKEN && has_file(g_d[i], "P.bin.part") && g_d[i].nfiles == 4);
	CHECK((i = find(n, "R")) >= 0 && g_d[i].st == GS_RERIP && g_d[i].errors == 1);
	CHECK((i = find(n, "E")) >= 0 && g_d[i].st == GS_ERRORS && g_d[i].bad == 1);
	CHECK((i = find(n, "Cancelled [SCES_000.01]")) >= 0 && g_d[i].st == GS_BROKEN && !strcmp(g_d[i].id, "SCES_000.01"));
	CHECK((i = find(n, "Missing")) >= 0 && g_d[i].st == GS_BROKEN);
	CHECK((i = find(n, "Loose")) >= 0 && g_d[i].st == GS_BROKEN);
	CHECK((i = find(n, "Orphan")) >= 0 && g_d[i].st == GS_BROKEN);
}

static void iso_and_bin_with_one_name(int &bad)
{
	fresh();
	rip("Castlevania Chronicles", 10, NULL);
	put_size("Castlevania Chronicles.iso", 2048ull * 9);
	int n = scan();
	CHECK(n == 2);
	int bins = 0, isos = 0;
	for (int i = 0; i < n; i++) {
		if (g_d[i].fmt == GF_2352) { bins++; CHECK(g_d[i].nfiles == 2); }
		if (g_d[i].fmt == GF_2048) { isos++; CHECK(g_d[i].nfiles == 1 && has_file(g_d[i], "Castlevania Chronicles.iso")); }
	}
	CHECK(bins == 1 && isos == 1);
}

static void multi_disc_grouping(int &bad)
{
	fresh();
	rip("Final Fantasy IX (Disc 2) (Europe) [SLES_102.12]", 10, "title=Final Fantasy IX (Europe) (Disc 2)  \xC2\xB7  SLES_102.12\nredump=verified X\n");
	rip("Final Fantasy IX (Disc 1) (Europe) [SLES_002.12]", 10, "title=Final Fantasy IX (Europe) (Disc 1)  \xC2\xB7  SLES_002.12\nredump=verified Y\n");
	rip("Final Fantasy IX (Disc 3) (Europe) [SLES_202.12]", 10, "title=Final Fantasy IX (Europe) (Disc 3)\nunreadable=4\n");
	rip("Final Fantasy IX (Disc 4) (Europe) [SLES_302.12]", 10, NULL);   // no log: its name
	rip("Final Fantasy VIII (Disc 1) [SLES_000.01]", 10, NULL);
	rip("Final Fantasy VII (Europe) [SCES_008.67]", 10, "title=Final Fantasy VII\nredump=verified Z\n");
	rip("Final Fantasy VII (USA) [SCUS_941.63]", 10, "title=Final Fantasy VII (USA)\nredump=verified W\n");
	int n = scan();
	CHECK(n == 7);
	int rows = lib_group(g_d, n, g_g, 600);
	CHECK(rows == 4);
	int ff9 = -1;
	for (int r = 0; r < rows; r++) if (g_g[r].ndiscs == 4) ff9 = r;
	CHECK(ff9 >= 0);
	if (ff9 < 0) return;
	CHECK(!strcmp(g_g[ff9].name, "Final Fantasy IX (Europe)"));
	CHECK(g_g[ff9].st == GS_ERRORS);   // the worst of its discs
	for (int k = 0; k < 4; k++) {
		char want[16];
		snprintf(want, sizeof want, "(Disc %d)", k + 1);
		CHECK(strstr(g_d[g_g[ff9].first + k].base, want) != NULL);
	}
	// the rows sorted by name
	for (int r = 1; r < rows; r++) CHECK(strcasecmp(g_g[r - 1].name, g_g[r].name) <= 0);
	// each row's discs are its own
	int total = 0;
	for (int r = 0; r < rows; r++) total += g_g[r].ndiscs;
	CHECK(total == n);
}

static void duplicates(int &bad)
{
	fresh();
	rip("Croc (Europe) [SLES_005.93]", 10, NULL);
	rip("Croc copy [SLES_005.93]", 10, NULL);          // same ID and size
	rip("Croc other size [SLES_005.93]", 11, NULL);    // same ID, other size
	rip("Abe", 10, "redump=verified Oddworld - Abe's Oddysee (Europe)\n");
	put_size("Abe.iso", 2048ull * 3);
	put("Abe.log", "redump=verified Oddworld - Abe's Oddysee (Europe)\n");   // (overwrites: one log)
	rip("Abe again", 12, "redump=verified Oddworld - Abe's Oddysee (Europe)\n");
	rip("Lone", 10, "redump=verified Lone (Europe)\n");
	int n = scan();
	lib_mark_dups(g_d, n);
	int a = find(n, "Croc (Europe) [SLES_005.93]"), b = find(n, "Croc copy [SLES_005.93]"), c = find(n, "Croc other size [SLES_005.93]");
	CHECK(a >= 0 && b >= 0 && c >= 0);
	if (a < 0 || b < 0 || c < 0) return;
	CHECK(g_d[a].dup == b && g_d[b].dup == a);
	CHECK(g_d[c].dup == -1);
	int e = find(n, "Abe again"), l = find(n, "Lone");
	CHECK(e >= 0 && g_d[e].dup >= 0 && strstr(g_d[g_d[e].dup].base, "Abe"));
	CHECK(l >= 0 && g_d[l].dup == -1);
	// grouping moves discs: dup follows
	lib_group(g_d, n, g_g, 600);
	a = find(n, "Croc (Europe) [SLES_005.93]"); b = find(n, "Croc copy [SLES_005.93]");
	CHECK(a >= 0 && b >= 0 && g_d[a].dup == b && g_d[b].dup == a);
}

static void libcrypt_marks(int &bad)
{
	fresh();
	rip("MediEvil (Europe) [SCES_003.11]", 10, NULL);                // not on the list
	rip("Spyro 3 (Europe) [SCES_014.31]", 10, NULL);                 // on the list, sbi installed
	put_size("../sbi/SCES_014.31.sbi", 100);
	rip("Ape Escape (Europe) [SLES_012.26]", 10, NULL);              // on the list, nothing
	rip("Ape Escape copy [SLES_012.26]", 11, NULL);                  // on the list, its own .sbi
	put_size("Ape Escape copy [SLES_012.26].sbi", 100);
	int n = scan(), i;
	CHECK((i = find(n, "MediEvil (Europe) [SCES_003.11]")) >= 0 && g_d[i].libcrypt == 0);
	CHECK((i = find(n, "Spyro 3 (Europe) [SCES_014.31]")) >= 0 && g_d[i].libcrypt == 1);
	CHECK((i = find(n, "Ape Escape (Europe) [SLES_012.26]")) >= 0 && g_d[i].libcrypt == -1);
	CHECK((i = find(n, "Ape Escape copy [SLES_012.26]")) >= 0 && g_d[i].libcrypt == 1
	      && has_file(g_d[i], "Ape Escape copy [SLES_012.26].sbi"));
	LibIo nolc = IO;
	nolc.libcrypt = NULL;
	n = lib_scan(DIR_, &nolc, g_d, 600);
	for (int k = 0; k < n; k++) CHECK(g_d[k].libcrypt == 0);
}

static void sorting(int &bad)
{
	fresh();
	rip("\xC3\x89lan", 1, NULL);                  // Élan
	rip("zork", 1, NULL);
	rip("Zap", 1, NULL);
	rip("apple", 1, NULL);
	rip("Banana", 1, NULL);
	rip("\xE3\x83\x95\xE3\x82\xA1 [SLPS_000.01]", 1, NULL);   // Japanese
	rip("[SLUS_000.02] 2 Xtreme", 1, NULL);
	int n = scan();
	int rows = lib_group(g_d, n, g_g, 600);
	CHECK(rows == 7);
	const char *want[] = {"2 Xtreme", "apple", "Banana", "Zap", "zork", "\xC3\x89lan", "\xE3\x83\x95\xE3\x82\xA1"};
	for (int r = 0; r < rows && r < 7; r++) {
		if (strcmp(g_g[r].name, want[r])) { printf("  row %d: %s\n", r, g_g[r].name); }
		CHECK(!strcmp(g_g[r].name, want[r]));
	}
	// names with brackets and dots stay intact in the files
	CHECK(find(n, "[SLUS_000.02] 2 Xtreme") >= 0);
}

static void jump_by_letter(int &bad)
{
	fresh();
	const char *names[] = {"Ace", "alpha", "Bravo", "Bust", "beta", "Crash", "Zed", "\xC3\x89t\xC3\xA9", "\xC3\x96l"};
	for (const char *s : names) rip(s, 1, NULL);
	int n = scan();
	int rows = lib_group(g_d, n, g_g, 600);
	int shown[600];
	int ns = lib_filter(g_g, rows, g_d, NULL, shown);
	CHECK(ns == 9);
	// A A B B B C Z rest rest
	CHECK(lib_first_letter_next(g_g, shown, ns, 0, 1) == 2);
	CHECK(lib_first_letter_next(g_g, shown, ns, 1, 1) == 2);
	CHECK(lib_first_letter_next(g_g, shown, ns, 2, 1) == 5);
	CHECK(lib_first_letter_next(g_g, shown, ns, 5, 1) == 6);
	CHECK(lib_first_letter_next(g_g, shown, ns, 6, 1) == 7);
	CHECK(lib_first_letter_next(g_g, shown, ns, 7, 1) == 7);   // the last block: nowhere to go
	CHECK(lib_first_letter_next(g_g, shown, ns, 8, -1) == 7);  // inside a block: its first row
	CHECK(lib_first_letter_next(g_g, shown, ns, 7, -1) == 6);
	CHECK(lib_first_letter_next(g_g, shown, ns, 4, -1) == 2);
	CHECK(lib_first_letter_next(g_g, shown, ns, 2, -1) == 0);
	CHECK(lib_first_letter_next(g_g, shown, ns, 0, -1) == 0);
	CHECK(lib_first_letter_next(g_g, shown, 0, 0, 1) == 0);
}

// the filter: criteria combined
static void filters(int &bad)
{
	fresh();
	put_size("Old2048.iso", 2048ull * 5);                                     // 2048, not checked
	put_size("Old2048b.iso", 2048ull * 6);
	put("Old2048b.log", "redump=no_match\n");                                // 2048, no match
	rip("Good", 10, "redump=verified Good (Europe)\n");                     // 2352 verified
	rip("Good copy", 10, "redump=verified Good (Europe)\n");                // duplicate of Good
	rip("Errs [SLES_005.93]", 10, "redump=verified Errs\nunreadable=4\n");  // errors, duplicate of ErrsB
	rip("ErrsB [SLES_005.93]", 10, "unreadable=5\n");
	rip("Plain", 10, NULL);                                                  // 2352, not checked
	put_size("Broken.bin", 2352);                                             // broken
	rip("Ape [SLES_012.26]", 10, NULL);                                      // LibCrypt .sbi missing
	rip("Mix (Disc 1)", 10, "redump=verified M1\n");                         // group: one 2352, one 2048
	put_size("Mix (Disc 2).iso", 2048ull * 4);
	int n = scan();
	lib_mark_dups(g_d, n);
	int rows = lib_group(g_d, n, g_g, 600);
	CHECK(rows == 10);
	int out[600];
	auto names = [&](const GameFilter &f, const char *want) {
		int k = lib_filter(g_g, rows, g_d, &f, out);
		char got[1024] = "";
		for (int i = 0; i < k; i++) snprintf(got + strlen(got), sizeof got - strlen(got), "%s%s", i ? "|" : "", g_g[out[i]].name);
		if (strcmp(got, want)) printf("  filter %d/%d/%d/%d: got %s want %s\n", f.fmt, f.redump, f.errors, f.other, got, want);
		return !strcmp(got, want);
	};
	CHECK(lib_filter(g_g, rows, g_d, NULL, out) == rows);
	GameFilter any = {0, 0, 0, 0};
	CHECK(lib_filter(g_g, rows, g_d, &any, out) == rows);
	CHECK(names({2, 2, 0, 0}, "Mix|Old2048"));                 // 2048 and no Redump result
	CHECK(names({2, 0, 0, 0}, "Mix|Old2048|Old2048b"));
	CHECK(names({2, 4, 0, 0}, "Mix|Old2048|Old2048b"));        // not verified = not checked or no match
	CHECK(names({2, 3, 0, 0}, "Old2048b"));
	CHECK(names({1, 1, 0, 0}, "Errs|Good|Good copy|Mix"));     // verified 2352
	CHECK(names({0, 0, 1, 2}, "Errs|ErrsB"));                  // with errors and duplicates
	CHECK(names({0, 0, 1, 0}, "Errs|ErrsB"));
	CHECK(names({0, 0, 2, 2}, "Good|Good copy"));
	CHECK(names({0, 0, 0, 1}, "Broken"));
	CHECK(names({0, 0, 0, 3}, "Ape"));
	CHECK(names({1, 2, 2, 0}, "Ape|Broken|Plain"));   // Mix: no disc of it is both 2352 and not checked
	CHECK(names({2, 1, 0, 0}, ""));                   // Mix: its 2048 disc is not the verified one
	CHECK(names({2, 2, 2, 0}, "Mix|Old2048"));         // Mix: disc 2 alone matches all three
}

static void summary_counts(int &bad)
{
	fresh();
	rip("V1", 10, "redump=verified V1\n");
	rip("V2", 10, "redump=verified V2\n");
	rip("E", 10, "bad=4\n");
	put_size("R.iso", 2048);
	put_size("B.bin", 2352);
	rip("U", 10, NULL);
	int n = scan();
	int total = -1, ver = -1, err = -1, rr = -1, br = -1;
	lib_summary(g_d, n, &total, &ver, &err, &rr, &br);
	CHECK(total == 6 && ver == 2 && err == 1 && rr == 1 && br == 1);
	lib_summary(g_d, n, NULL, NULL, &err, NULL, NULL);
	CHECK(err == 1);
}

static void many_sets_bounded(int &bad)
{
	fresh();
	for (int i = 0; i < 500; i++) {
		char name[64];
		snprintf(name, sizeof name, "Game %03d [SLES_%03d.%02d]", i, i, i % 100);
		rip(name, 4, (i % 3) ? LOG_OK : NULL);
	}
	struct timespec a, b;
	clock_gettime(CLOCK_MONOTONIC, &a);
	int n = scan();
	lib_mark_dups(g_d, n);
	int rows = lib_group(g_d, n, g_g, 600);
	clock_gettime(CLOCK_MONOTONIC, &b);
	double secs = (b.tv_sec - a.tv_sec) + (b.tv_nsec - a.tv_nsec) / 1e9;
	printf("  500 sets: %.3f s\n", secs);
	CHECK(n == 500 && rows == 500);
	CHECK(secs < 2.0);
	// more sets than room: the first max, no overrun
	static GameDisc few[11];
	memset(&few[10], 0x5A, sizeof(GameDisc));
	CHECK(lib_scan(DIR_, &IO, few, 10) == 10);
	CHECK(((unsigned char *)&few[10])[0] == 0x5A);
}

static void long_names_cut(int &bad)
{
	fresh();
	// a cue with many track files whose names do not all fit: files_cut, nothing overrun
	char cue[16384] = "";
	for (int t = 1; t <= 60; t++) {
		char nm[128];
		snprintf(nm, sizeof nm, "A very long game name for the track list test (Europe) (Track %02d).bin", t);
		put_size(nm, 2352);
		snprintf(cue + strlen(cue), sizeof cue - strlen(cue), "FILE \"%s\" BINARY\n  TRACK %02d %s\n", nm, t, t == 1 ? "MODE2/2352" : "AUDIO");
	}
	put("Long.cue", cue);
	int n = scan();
	CHECK(n == 1);
	CHECK(g_d[0].files_cut == 1 && g_d[0].tracks == 60 && g_d[0].size == 2352ull * 60 && g_d[0].st != GS_BROKEN);
}

// two releases of a multi-disc game, and one disc number twice: separate rows
static void regions_stay_apart(int &bad)
{
	fresh();
	rip("Final Fantasy VII (Disc 1) (Europe) [SCES_008.67]", 10, NULL);
	rip("Final Fantasy VII (Disc 2) (Europe) [SCES_108.67]", 10, NULL);
	rip("Final Fantasy VII (Disc 1) (USA) [SCUS_941.63]", 10, NULL);
	rip("Final Fantasy VII (Disc 2) (USA) [SCUS_941.64]", 10, NULL);
	// the same with logs whose titles carry the region in another place
	rip("Parasite Eve (Disc 1) [SLUS_006.62]", 10, "title=Parasite Eve (USA) (Disc 1)\n");
	rip("Parasite Eve (Disc 2) [SLUS_006.68]", 10, NULL);   // region from its ID
	rip("Parasite Eve (Disc 1) (Japan) [SLPS_012.30]", 10, NULL);
	// disc 1 twice in one folder: the second copy is its own row
	rip("Riven (Disc 1) (Europe) [SLES_000.01]", 10, NULL);
	rip("Riven (Disc 1) (Europe) [SLES_000.01] copy", 10, NULL);
	rip("Riven (Disc 2) (Europe) [SLES_000.02]", 10, NULL);
	int n = scan();
	int rows = lib_group(g_d, n, g_g, 600);
	char got[2048] = "";
	for (int r = 0; r < rows; r++) snprintf(got + strlen(got), sizeof got - strlen(got), "%s%s=%d", r ? "|" : "", g_g[r].name, g_g[r].ndiscs);
	const char *want = "Final Fantasy VII (Europe)=2|Final Fantasy VII (USA)=2|Parasite Eve=2|"
	                   "Parasite Eve (Disc 1) (Japan)=1|Riven (Disc 1) (Europe) copy=1|Riven (Europe)=2";
	if (strcmp(got, want)) printf("  rows: %s\n", got);
	CHECK(!strcmp(got, want));
}

// the same set on two sources (hard disk and USB, "All"): two rows, each the other's duplicate
static void same_set_two_folders(int &bad)
{
	fresh();
	rip("Final Fantasy IX (Disc 1) (Europe) [SLES_002.12]", 10, NULL);
	rip("Final Fantasy IX (Disc 2) (Europe) [SLES_102.12]", 10, NULL);
	int n = scan();
	DIR_ = "./out/library/USB/PSXISO";
	rip("Final Fantasy IX (Disc 1) (Europe) [SLES_002.12]", 10, NULL);
	rip("Final Fantasy IX (Disc 2) (Europe) [SLES_102.12]", 10, NULL);
	n += lib_scan(DIR_, &IO, g_d + n, 600 - n);
	CHECK(n == 4);
	lib_mark_dups(g_d, n);
	int rows = lib_group(g_d, n, g_g, 600);
	CHECK(rows == 2 && g_g[0].ndiscs == 2 && g_g[1].ndiscs == 2);
	for (int i = 0; i < n; i++) {
		CHECK(g_d[i].dup >= 0);
		if (g_d[i].dup < 0) continue;
		const GameDisc &o = g_d[g_d[i].dup];
		CHECK(!strcmp(leaf_name(o.base), leaf_name(g_d[i].base)) && strcmp(o.base, g_d[i].base));
	}
}

static void empty_image_broken(int &bad)
{
	fresh();
	put_size("Empty.iso", 0);
	rip("Zero", 0, NULL);
	int n = scan(), i;
	CHECK(n == 2);
	CHECK((i = find(n, "Empty")) >= 0 && g_d[i].st == GS_BROKEN);
	CHECK((i = find(n, "Zero")) >= 0 && g_d[i].st == GS_BROKEN);
}

// the folder goes away between listing and reading (a USB stick pulled): no crash, nothing claimed
static int gone_stat(const char *, uint64_t *) { return -1; }
static int gone_read(const char *, char *, int) { return -1; }
static int gone_exists(const char *) { return 0; }
static void folder_gone_midway(int &bad)
{
	fresh();
	rip("A [SLES_012.26]", 10, LOG_OK);
	put_size("B.iso", 2048 * 4);
	LibIo gone = {io_list, gone_stat, gone_read, gone_exists, lc_list, "./out/library/sbi"};
	int n = lib_scan(DIR_, &gone, g_d, 600), i;
	CHECK(n == 2);
	CHECK((i = find(n, "A [SLES_012.26]")) >= 0 && g_d[i].st == GS_BROKEN && g_d[i].size == 0 && g_d[i].title[0]);
	CHECK((i = find(n, "B")) >= 0 && g_d[i].size == 0 && g_d[i].fmt == GF_UNKNOWN && g_d[i].st == GS_BROKEN);
}

// The app's own companions carry their image's type (<base>.bin.log, <base>.iso.md5): two sets of
// one base name each keep their own; an old <base>.log goes to the .bin/.cue set as before.
static const char *APP_LOG_ISO =
	"PSX Ripper rip log\ntitle=Breath of Fire IV\nserial=SLUS_013.24\nchecked=from the image (no rip log before)\n"
	"track=01 start=0 md5=0123456789abcdef0123456789abcdef\nredump=verified Breath of Fire IV (USA)\n";
static void typed_companions(int &bad)
{
	fresh();
	const char *B = "Breath of Fire IV [SLUS_013.24]";
	char f[300];
	rip(B, 10, NULL);
	snprintf(f, sizeof f, "%s.bin.log", B); put(f, LOG_OK);
	snprintf(f, sizeof f, "%s.bin.md5", B); put(f, "x  track 01\n");
	snprintf(f, sizeof f, "%s.iso", B); put_size(f, 2352ull * 10);
	snprintf(f, sizeof f, "%s.iso.log", B); put(f, APP_LOG_ISO);
	snprintf(f, sizeof f, "%s.iso.md5", B); put(f, "y  track 01\n");
	snprintf(f, sizeof f, "%s_.jpg", B); put(f, "jpg");
	int n = scan();
	CHECK(n == 2);
	int bin = -1, iso = -1;
	for (int i = 0; i < n; i++) {
		size_t l = strlen(g_d[i].image);
		if (l > 4 && !strcmp(g_d[i].image + l - 4, ".bin")) bin = i;
		if (l > 4 && !strcmp(g_d[i].image + l - 4, ".iso")) iso = i;
	}
	CHECK(bin >= 0 && iso >= 0);
	if (bin < 0 || iso < 0) return;
	snprintf(f, sizeof f, "%s.bin.log", B); CHECK(has_file(g_d[bin], f));
	snprintf(f, sizeof f, "%s.bin.md5", B); CHECK(has_file(g_d[bin], f));
	snprintf(f, sizeof f, "%s.iso.log", B); CHECK(!has_file(g_d[bin], f) && has_file(g_d[iso], f));
	snprintf(f, sizeof f, "%s.iso.md5", B); CHECK(has_file(g_d[iso], f));
	CHECK(g_d[bin].nfiles == 4 && g_d[iso].nfiles == 3);
	CHECK(g_d[bin].has_log && !strcmp(g_d[bin].log_sfx, ".bin.log") && !strcmp(g_d[bin].md5_sfx, ".bin.md5") && g_d[bin].rd == 1 && !g_d[bin].old_log);
	CHECK(g_d[iso].has_log && !strcmp(g_d[iso].log_sfx, ".iso.log") && !strcmp(g_d[iso].md5_sfx, ".iso.md5") && g_d[iso].rd == 1 && g_d[iso].st == GS_VERIFIED);
	CHECK(!strcmp(g_d[iso].rdname, "Breath of Fire IV (USA)") && !strcmp(g_d[bin].rdname, "Alundra (Europe)"));
	// an old <base>.log (and .md5) of the app goes to the .bin/.cue set and is marked for the migration
	fresh();
	rip(B, 10, LOG_OK);
	snprintf(f, sizeof f, "%s.md5", B); put(f, "x  track 01\n");
	snprintf(f, sizeof f, "%s.iso", B); put_size(f, 2352ull * 10);
	n = scan();
	CHECK(n == 2);
	for (int i = 0; i < n; i++) {
		size_t l = strlen(g_d[i].image);
		int is_iso = l > 4 && !strcmp(g_d[i].image + l - 4, ".iso");
		if (is_iso) CHECK(!g_d[i].has_log && !g_d[i].log_sfx[0] && !g_d[i].old_log && g_d[i].nfiles == 1 && g_d[i].rd == -1);
		else CHECK(g_d[i].has_log && !strcmp(g_d[i].log_sfx, ".log") && !strcmp(g_d[i].md5_sfx, ".md5") && g_d[i].old_log && g_d[i].rd == 1);
	}
	// a log another tool wrote under the old name: read as before, not the app's (no migration)
	fresh();
	rip("Other", 10, "some tool's log\nredump=verified Other\n");
	n = scan();
	CHECK(n == 1 && g_d[0].has_log && !g_d[0].old_log && g_d[0].rd == 1);
	// the typed log wins over an old one next to it
	fresh();
	rip("Both", 10, "PSX Ripper rip log\nredump=no_match\n");
	put("Both.bin.log", LOG_OK);
	n = scan();
	CHECK(n == 1 && !strcmp(g_d[0].log_sfx, ".bin.log") && g_d[0].rd == 1 && !g_d[0].old_log && g_d[0].nfiles == 4);
	// what is left of a lone .iso: its typed log alone is a broken set of that base
	fresh();
	put("Gone.iso.log", APP_LOG_ISO);
	n = scan();
	CHECK(n == 1 && g_d[0].st == GS_BROKEN && !strcmp(g_d[0].log_sfx, ".iso.log") && !strcmp(leaf_name(g_d[0].base), "Gone"));
	// the same name and title: the two rows keep one order (by image) whatever the folder order
	fresh();
	rip(B, 10, NULL);
	snprintf(f, sizeof f, "%s.iso", B); put_size(f, 2352ull * 10);
	n = scan();
	int ng = lib_group(g_d, n, g_g, 600);
	CHECK(ng == 2 && strstr(g_d[g_g[0].first].image, ".bin") && strstr(g_d[g_g[1].first].image, ".iso"));
	std::swap(g_d[0], g_d[1]);
	ng = lib_group(g_d, n, g_g, 600);
	CHECK(ng == 2 && strstr(g_d[g_g[0].first].image, ".bin") && strstr(g_d[g_g[1].first].image, ".iso"));
}

// The names: typed for each image, the reader's fallback to the old name only for the image it
// belongs to, and the migration's moves (only an app-written log, never onto a taken name).
static void own_names(int &bad)
{
	fresh();
	char b[300], out[400], from[2][320], to[2][320];
	snprintf(b, sizeof b, "%s/X", DIR_);
	CHECK(!strcmp(lib_image_type("/a/X.iso"), ".iso") && !strcmp(lib_image_type("X.IMG"), ".img") && !strcmp(lib_image_type("X.bin"), ".bin") &&
	      !strcmp(lib_image_type(""), ".bin") && !strcmp(lib_image_type(NULL), ".bin") && !strcmp(lib_image_type(".iso"), ".iso"));
	lib_own_name(b, "X.iso", "log", out, sizeof out);
	CHECK(strstr(out, "/X.iso.log") && strlen(out) == strlen(b) + 8);
	lib_own_name(b, ".bin", "md5", out, sizeof out);
	CHECK(strstr(out, "/X.bin.md5"));
	// nothing there: the typed name, 0
	CHECK(!lib_own_find(b, ".bin", "log", IO.exists, out, sizeof out) && strstr(out, "/X.bin.log"));
	// a lone .iso with an old log: it is the .iso's
	put("X.iso", "i");
	put("X.log", "PSX Ripper rip log\n");
	put("X.md5", "m");
	CHECK(lib_own_find(b, ".iso", "log", IO.exists, out, sizeof out) && strstr(out, "/X.log"));
	CHECK(lib_own_migration(b, ".iso", &IO, from, to) == 2 && strstr(from[0], "/X.log") && strstr(to[0], "/X.iso.log") &&
	      strstr(from[1], "/X.md5") && strstr(to[1], "/X.iso.md5"));
	// a .cue of that base: the old log is the .bin set's, not the .iso's
	put("X.cue", "FILE \"X.bin\" BINARY\n");
	CHECK(!lib_own_find(b, ".iso", "log", IO.exists, out, sizeof out) && strstr(out, "/X.iso.log"));
	CHECK(lib_own_find(b, ".bin", "log", IO.exists, out, sizeof out) && strstr(out, "/X.log"));
	CHECK(lib_own_migration(b, ".iso", &IO, from, to) == 0);
	CHECK(lib_own_migration(b, ".bin", &IO, from, to) == 2 && strstr(to[0], "/X.bin.log") && strstr(to[1], "/X.bin.md5"));
	// the typed one there: found first; a taken target is never moved onto
	put("X.bin.md5", "n");
	CHECK(lib_own_find(b, ".bin", "md5", IO.exists, out, sizeof out) && strstr(out, "/X.bin.md5"));
	CHECK(lib_own_migration(b, ".bin", &IO, from, to) == 1 && strstr(from[0], "/X.log") && strstr(to[0], "/X.bin.log"));
	put("X.bin.log", "new");
	CHECK(lib_own_migration(b, ".bin", &IO, from, to) == 0);
	// .img before .iso (the scan's order): a lone .img and .iso, the old log is the .img's
	fresh();
	put("Y.iso", "i"); put("Y.img", "i"); put("Y.log", "PSX Ripper rip log\r\ntitle=Y\n");
	snprintf(b, sizeof b, "%s/Y", DIR_);
	CHECK(lib_own_migration(b, ".iso", &IO, from, to) == 0 && lib_own_migration(b, ".img", &IO, from, to) == 1 && strstr(to[0], "/Y.img.log"));
	// a log the app did not write stays (and so does its .md5)
	fresh();
	put("Z.bin", "b"); put("Z.cue", "c"); put("Z.log", "EAC extraction logfile\n"); put("Z.md5", "m");
	snprintf(b, sizeof b, "%s/Z", DIR_);
	CHECK(lib_own_migration(b, ".bin", &IO, from, to) == 0);
	CHECK(lib_own_find(b, ".bin", "log", IO.exists, out, sizeof out) && strstr(out, "/Z.log"));   // still read, as before
}

// Discs named "CD1".."CD4" (another tool's .iso files) are one game, and the row's name has no "CD1".
static void cd_numbers_group(int &bad)
{
	fresh();
	for (int i = 1; i <= 4; i++) {
		char nm[80];
		snprintf(nm, sizeof nm, "The Legend Of Dragoon CD%d [SCES_%d30.43].iso", i, i - 1);
		put_size(nm, 2048ull * 4);
	}
	put_size("Chrono Cross CD 1 [SLES_033.98].iso", 2048ull * 4);
	put_size("Chrono Cross CD 2 [SLES_133.98].iso", 2048ull * 4);
	put_size("Ridge Racer Type 4 [SCES_016.46].iso", 2048ull * 4);   // one disc, no number: unchanged
	put_size("ABCD1 [SLUS_000.01].iso", 2048ull * 4);                  // "CD1" inside a word is not a disc number
	int n = scan();
	CHECK(n == 8);
	int rows = lib_group(g_d, n, g_g, 600);
	CHECK(rows == 4);
	int lod = -1, cc = -1;
	for (int r = 0; r < rows; r++) {
		if (!strcmp(g_g[r].name, "The Legend Of Dragoon")) lod = r;
		if (!strcmp(g_g[r].name, "Chrono Cross")) cc = r;
	}
	CHECK(lod >= 0 && g_g[lod].ndiscs == 4);
	CHECK(cc >= 0 && g_g[cc].ndiscs == 2);
	if (lod >= 0) CHECK(strstr(g_d[g_g[lod].first + 3].base, "CD4") != NULL);   // in disc order
	int one = 0;
	for (int r = 0; r < rows; r++) if (!strcmp(g_g[r].name, "ABCD1") && g_g[r].ndiscs == 1) one = 1;
	CHECK(one);
	for (int r = 0; r < rows; r++) if (strstr(g_g[r].name, "CD1") && strcmp(g_g[r].name, "ABCD1")) { CHECK(0); printf("  row %s\n", g_g[r].name); }
}

int main()
{
	struct { const char *name; void (*fn)(int &); } tests[] = {
		{"sets_by_base_name", sets_by_base_name}, {"format_from_size_and_cue", format_from_size_and_cue},
		{"log_parsed", log_parsed}, {"status_precedence", status_precedence},
		{"iso_and_bin_with_one_name", iso_and_bin_with_one_name}, {"multi_disc_grouping", multi_disc_grouping},
		{"duplicates", duplicates}, {"libcrypt_marks", libcrypt_marks}, {"sorting", sorting},
		{"jump_by_letter", jump_by_letter}, {"filters", filters}, {"summary_counts", summary_counts},
		{"many_sets_bounded", many_sets_bounded}, {"long_names_cut", long_names_cut},
		{"folder_gone_midway", folder_gone_midway}, {"regions_stay_apart", regions_stay_apart},
		{"same_set_two_folders", same_set_two_folders}, {"empty_image_broken", empty_image_broken},
		{"typed_companions", typed_companions}, {"own_names", own_names}, {"cd_numbers_group", cd_numbers_group},
	};
	if (system("mkdir -p out")) {}
	for (auto &t : tests) {
		int bad = 0;
		t.fn(bad);
		printf("%s %s\n", bad ? "FAIL" : "ok  ", t.name);
	}
	printf("library_test: %d problem(s)\n", g_fail);
	return g_fail ? 1 : 0;
}
