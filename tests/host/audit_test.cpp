// Host tests of the audit log module (source/audit.cpp). Run: make -C tests/host
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <set>
#include <string>
#include "sys/mutex.h"
#include "../../include/audit.h"

// a real mutex for the threads test (ui_test's stub does nothing)
static pthread_mutex_t g_mtx[4];   // sys_mutex_t is 32 bits: a handle is an index
static int g_mtx_n;
int sysMutexCreate(sys_mutex_t *m, const sys_mutex_attr_t *) { pthread_mutex_init(&g_mtx[g_mtx_n], NULL); *m = g_mtx_n++; return 0; }
int sysMutexLock(sys_mutex_t m, u64) { return pthread_mutex_lock(&g_mtx[m]); }
int sysMutexUnlock(sys_mutex_t m) { return pthread_mutex_unlock(&g_mtx[m]); }

static void *io_open(const char *p, const char *m) { return fopen(p, m); }
static size_t io_read(void *f, void *p, size_t n) { return fread(p, 1, n, (FILE *)f); }
static size_t io_write(void *f, const void *p, size_t n) { return fwrite(p, 1, n, (FILE *)f); }
static int io_seek(void *f, uint64_t pos) { return fseek((FILE *)f, (long)pos, SEEK_SET); }
static int io_close(void *f) { return fclose((FILE *)f); }
static const AuditIo IO = {io_open, io_read, io_write, io_seek, io_close};

static const char *DIR = "./out/audit";
static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("  FAIL %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; bad = 1; } } while (0)

static void fresh()
{
	char cmd[128];
	snprintf(cmd, sizeof cmd, "rm -rf %s", DIR);
	if (system(cmd)) {}
	audit_init(DIR, &IO);
}

static void sector_fill(uint8_t *b, uint8_t v) { memset(b, v, 2352); }

static void rows_survive_reload(int &bad)
{
	fresh();
	uint32_t job = audit_job_begin(AU_PAD, "SCES_002.55:319250", "Game", "repair");
	uint8_t a[2352], b[2352];
	for (int i = 0; i < 3; i++) {
		sector_fill(a, 1 + i); sector_fill(b, 10 + i);
		audit_sector(job, "/dev_hdd0/Game.bin", 100 + i, a, b);
	}
	audit_setting(AU_PAD, "audit_mb", "256", "64");
	AuditRow before[8];
	int nb = audit_rows(job, before, 0, 8);
	audit_init(DIR, &IO);
	uint32_t job2 = audit_job_begin(AU_APP, "X:1", "X", "rip");
	CHECK(job2 == job + 1);
	AuditRow r[8];
	int n = audit_rows(job, r, 0, 8);
	CHECK(nb == 4 && n == 4);
	CHECK(!strcmp(r[0].kind, "job") && !strcmp(r[0].before, "Game") && !strcmp(r[0].what, "repair"));
	for (int i = 0; i < 3; i++) {
		CHECK(!strcmp(r[1 + i].kind, "sector"));
		CHECK(r[1 + i].lba == (uint32_t)(100 + i));
		CHECK(r[1 + i].blob == (uint32_t)i);
		CHECK(!strcmp(r[1 + i].file, "/dev_hdd0/Game.bin"));
		CHECK(r[1 + i].src == AU_PAD && !strcmp(r[1 + i].disc, "SCES_002.55:319250"));
	}
	// ids continue: job row 1, sectors 2-4, setting 5, then the new job row is 6
	AuditRow j2[2];
	CHECK(audit_rows(job2, j2, 0, 2) == 1 && j2[0].id == 6);
	// a third sector after reload continues the blob index
	sector_fill(a, 7); sector_fill(b, 8);
	audit_sector(job, "/dev_hdd0/Game.bin", 200, a, b);
	AuditRow r2[8];
	n = audit_rows(job, r2, 0, 8);
	CHECK(n == 5 && r2[4].blob == 3);
}

static void half_line_ignored(int &bad)
{
	fresh();
	audit_note(0, AU_FILES, "", "file", "x", "", "");
	audit_note(0, AU_FILES, "", "file", "y", "", "");
	char p[256];
	snprintf(p, sizeof p, "%s/audit.log", DIR);
	FILE *f = fopen(p, "ab");
	fputs("id=99\tt=", f);
	fclose(f);
	audit_init(DIR, &IO);
	audit_note(0, AU_FILES, "", "file", "z", "", "");
	AuditRow r[8];
	int n = audit_events(AU_FILES, NULL, r, 8);
	CHECK(n == 3);
	for (int i = 0; i < n; i++) CHECK(r[i].id != 99);
	CHECK(n == 3 && r[0].id == 3 && !strcmp(r[0].what, "z"));   // next id after 2 is 3, not 100
}

// the copy that drops a half line cannot run (tmp file not creatable): the rows stay readable and
// the next row is not glued onto the half line
static void cut_fails(int &bad)
{
	fresh();
	audit_note(0, AU_FILES, "", "file", "x", "", "");
	audit_note(0, AU_FILES, "", "file", "y", "", "");
	char p[256], cmd[300];
	snprintf(p, sizeof p, "%s/audit.log", DIR);
	FILE *f = fopen(p, "ab");
	fputs("id=99\tt=1\tsrc=pad\tdom=files\tjob=0\tdisc=\tkind=file", f);   // cut after kind=
	fclose(f);
	snprintf(cmd, sizeof cmd, "mkdir %s.tmp", p);   // a directory in the way: fopen(tmp) fails
	if (system(cmd)) {}
	audit_init(DIR, &IO);
	audit_note(0, AU_FILES, "", "file", "z", "", "");
	AuditRow r[8];
	int n = audit_events(AU_FILES, NULL, r, 8);
	CHECK(n == 3);
	for (int i = 0; i < n; i++) CHECK(r[i].id != 99);
	CHECK(n == 3 && r[0].id == 3 && !strcmp(r[0].what, "z") && !strcmp(r[2].what, "x"));
	audit_init(DIR, &IO);   // and it reloads the same
	CHECK(audit_events(AU_FILES, NULL, r, 8) == 3);
}

static void escaping(int &bad)
{
	fresh();
	const char *w = "a\tb\nc\\d\\n\re";
	audit_note(0, AU_FILES, "d\tisc", "file", w, "b\\", "\n");
	AuditRow r[2];
	CHECK(audit_events(AU_FILES, NULL, r, 2) == 1);
	CHECK(!strcmp(r[0].what, w));
	CHECK(!strcmp(r[0].disc, "d\tisc") && !strcmp(r[0].before, "b\\") && !strcmp(r[0].after, "\n"));
	// the row is still one line
	char p[256];
	snprintf(p, sizeof p, "%s/audit.log", DIR);
	FILE *f = fopen(p, "rb");
	int lines = 0, c;
	while ((c = fgetc(f)) != EOF) if (c == '\n') lines++;
	fclose(f);
	CHECK(lines == 1);
}

static void blob_layout(int &bad)
{
	fresh();
	uint32_t job = audit_job_begin(AU_PAD, "A:1", "A", "repair");
	uint8_t o0[2352], n0[2352], o1[2352], n1[2352];
	sector_fill(o0, 1); sector_fill(n0, 2); sector_fill(o1, 3); sector_fill(n1, 4);
	audit_sector(job, "f", 5, o0, n0);
	audit_sector(job, "f", 6, o1, n1);
	char p[256];
	snprintf(p, sizeof p, "%s/job%u.bin", DIR, (unsigned)job);
	FILE *f = fopen(p, "rb");
	static uint8_t all[9500];
	size_t n = f ? fread(all, 1, sizeof all, f) : 0;
	if (f) fclose(f);
	CHECK(n == 9408);
	CHECK(n == 9408 && !memcmp(all, o0, 2352) && !memcmp(all + 2352, n0, 2352));
	CHECK(n == 9408 && !memcmp(all + 4704, o1, 2352) && !memcmp(all + 7056, n1, 2352));
}

static void *writer(void *)
{
	for (int i = 0; i < 1000; i++) audit_note(0, AU_FILES, "", "file", "thread", "a", "b");
	return NULL;
}

static void threads(int &bad)
{
	fresh();
	pthread_t t1, t2;
	pthread_create(&t1, NULL, writer, NULL);
	pthread_create(&t2, NULL, writer, NULL);
	pthread_join(t1, NULL);
	pthread_join(t2, NULL);
	char p[256];
	snprintf(p, sizeof p, "%s/audit.log", DIR);
	FILE *f = fopen(p, "rb");
	char line[1024];
	std::set<unsigned> ids;
	int lines = 0, whole = 0;
	while (fgets(line, sizeof line, f)) {
		lines++;
		unsigned id = 0;
		char what[16] = "";
		size_t n = strlen(line);
		if (n && line[n - 1] == '\n' && sscanf(line, "id=%u", &id) == 1 && strstr(line, "\tkind=file\twhat=thread\tbefore=a\tafter=b\tend=1\n")) { whole++; ids.insert(id); }
	}
	fclose(f);
	CHECK(lines == 2000 && whole == 2000 && ids.size() == 2000);
}

static void discs_grouping(int &bad)
{
	fresh();
	uint32_t a = audit_job_begin(AU_PAD, "AAA:100", "Alpha", "rip");
	audit_job_end(a, "finished");
	uint32_t b = audit_job_begin(AU_APP, "BBB:200", "Beta", "repair");
	uint8_t x[2352] = {0};
	audit_sector(b, "f", 1, x, x);
	audit_job_end(b, "finished");
	uint32_t c = audit_job_begin(AU_PAD, "BBB:200", "Beta", "check");
	audit_job_end(c, "finished");
	char keys[4][96], names[4][96];
	int ev[4];
	uint32_t lt[4];
	int n = audit_discs(keys, names, ev, lt, 4);
	CHECK(n == 2);
	// BBB has the newest rows; two jobs = two events (sector/result rows belong to them)
	CHECK(n == 2 && !strcmp(keys[0], "BBB:200") && !strcmp(names[0], "Beta") && ev[0] == 2);
	CHECK(n == 2 && !strcmp(keys[1], "AAA:100") && !strcmp(names[1], "Alpha") && ev[1] == 1);
	CHECK(n == 2 && lt[0] >= lt[1]);
	AuditRow r[4];
	CHECK(audit_events(AU_RIPS, "BBB:200", r, 4) == 2 && r[0].job == c && r[1].job == b);
	CHECK(audit_events(AU_RIPS, "AAA:100", r, 4) == 1);
	CHECK(audit_events(AU_RIPS, NULL, r, 4) == 3);
}

// ---- undo ----

static const char *IMG = "./out/audit_img.bin";

static void img_make(int sectors)
{
	FILE *f = fopen(IMG, "wb");
	static uint8_t z[2352];
	for (int i = 0; i < sectors; i++) fwrite(z, 1, sizeof z, f);
	fclose(f);
}

static void img_put(uint32_t lba, const uint8_t *b)
{
	FILE *f = fopen(IMG, "r+b");
	fseek(f, (long)lba * 2352, SEEK_SET);
	fwrite(b, 1, 2352, f);
	fclose(f);
}

// true when the image's sector `lba` is all `v`
static bool img_is(uint32_t lba, uint8_t v)
{
	uint8_t b[2352], w[2352];
	FILE *f = fopen(IMG, "rb");
	fseek(f, (long)lba * 2352, SEEK_SET);
	size_t n = fread(b, 1, sizeof b, f);
	fclose(f);
	sector_fill(w, v);
	return n == 2352 && !memcmp(b, w, 2352);
}

// a repair writing old -> new at lba: the image gets the new bytes, the audit both
static void repair_write(uint32_t job, uint32_t lba, uint8_t old_v, uint8_t new_v, const char *file = NULL)
{
	uint8_t o[2352], n[2352];
	sector_fill(o, old_v); sector_fill(n, new_v);
	if (!file) img_put(lba, n);
	audit_sector(job, file ? file : IMG, lba, o, n);
}

// the row with `id` among the rows of `job`
static bool row_of(uint32_t job, uint32_t id, AuditRow *out)
{
	static AuditRow r[64];
	int n = audit_rows(job, r, 0, 64);
	for (int i = 0; i < n; i++) if (r[i].id == id) { *out = r[i]; return true; }
	return false;
}

// the id of the k-th sector row of a job
static uint32_t sector_id(uint32_t job, int k)
{
	static AuditRow r[64];
	int n = audit_rows(job, r, 0, 64);
	for (int i = 0; i < n; i++) if (!strcmp(r[i].kind, "sector") && k-- == 0) return r[i].id;
	return 0;
}

static int undo_rows_naming(uint32_t job, uint32_t id)
{
	static AuditRow r[64];
	char w[16];
	snprintf(w, sizeof w, "%u", (unsigned)id);
	int n = audit_rows(job, r, 0, 64), k = 0;
	for (int i = 0; i < n; i++) if (!strcmp(r[i].kind, "undo") && !strcmp(r[i].what, w) && r[i].src == AU_UNDO) k++;
	return k;
}

static void undo_sector(int &bad)
{
	fresh();
	img_make(4);
	uint8_t a[2352];
	sector_fill(a, 0x11);
	img_put(2, a);
	uint32_t job = audit_job_begin(AU_PAD, "U:4", "U", "repair");
	repair_write(job, 2, 0x11, 0x22);
	audit_job_end(job, "finished");
	uint32_t id = sector_id(job, 0);
	AuditRow r;
	CHECK(row_of(job, id, &r) && r.reversible == 1 && r.undone == 0);
	char why[128] = "";
	CHECK(audit_undo_row(id, why, sizeof why) == 0);
	CHECK(img_is(2, 0x11));
	CHECK(undo_rows_naming(job, id) == 1);
	CHECK(row_of(job, id, &r) && r.undone == 1 && r.reversible == 0);
	// a second undo of the same row is refused and changes nothing
	CHECK(audit_undo_row(id, why, sizeof why) == -1 && why[0]);
	CHECK(img_is(2, 0x11) && undo_rows_naming(job, id) == 1);
}

static void undo_refused_when_changed(int &bad)
{
	fresh();
	img_make(4);
	uint32_t job = audit_job_begin(AU_PAD, "U:4", "U", "repair");
	repair_write(job, 1, 0x11, 0x22);
	audit_job_end(job, "finished");
	uint8_t c[2352];
	sector_fill(c, 0x33);
	img_put(1, c);   // re-ripped or repaired again since
	uint32_t id = sector_id(job, 0);
	AuditRow r;
	CHECK(row_of(job, id, &r) && r.reversible == 0 && r.undone == 0);
	char why[128] = "", want[24];
	CHECK(audit_undo_row(id, why, sizeof why) == -1);
	CHECK(img_is(1, 0x33));
	snprintf(want, sizeof want, "row %u", (unsigned)id);
	CHECK(strstr(why, want) != NULL);
	CHECK(undo_rows_naming(job, id) == 0);
}

static void undo_job_newest_first(int &bad)
{
	fresh();
	img_make(4);
	uint32_t job = audit_job_begin(AU_PAD, "U:4", "U", "repair");
	repair_write(job, 1, 0x0A, 0x0B);
	repair_write(job, 1, 0x0B, 0x0C);
	audit_job_end(job, "finished");
	char why[128] = "";
	CHECK(audit_undo_job(job, why, sizeof why) == 0);
	CHECK(img_is(1, 0x0A));
	AuditRow r;
	CHECK(row_of(job, sector_id(job, 0), &r) && r.undone == 1);
	CHECK(row_of(job, sector_id(job, 1), &r) && r.undone == 1);
}

// the job stops at the first row it cannot undo (newest first) and says which
static void undo_job_stops_at_refusal(int &bad)
{
	fresh();
	img_make(4);
	uint32_t job = audit_job_begin(AU_PAD, "U:4", "U", "repair");
	repair_write(job, 1, 0x0A, 0x0B);
	repair_write(job, 2, 0x0A, 0x0B);
	repair_write(job, 3, 0x0A, 0x0B);
	audit_job_end(job, "finished");
	uint8_t c[2352];
	sector_fill(c, 0x33);
	img_put(2, c);
	char why[128] = "", want[24];
	CHECK(audit_undo_job(job, why, sizeof why) == -1);
	snprintf(want, sizeof want, "row %u", (unsigned)sector_id(job, 1));
	CHECK(strstr(why, want) != NULL);
	CHECK(img_is(3, 0x0A) && img_is(2, 0x33) && img_is(1, 0x0B));   // lba 1 is older: left alone
}

static void undo_missing_blob(int &bad)
{
	fresh();
	img_make(4);
	uint32_t job = audit_job_begin(AU_PAD, "U:4", "U", "repair");
	repair_write(job, 1, 0x11, 0x22);
	audit_job_end(job, "finished");
	char p[256];
	snprintf(p, sizeof p, "%s/job%u.bin", DIR, (unsigned)job);
	remove(p);
	uint32_t id = sector_id(job, 0);
	AuditRow r;
	CHECK(row_of(job, id, &r) && r.reversible == 0);
	char why[128] = "";
	CHECK(audit_undo_row(id, why, sizeof why) == -1 && why[0]);
	CHECK(img_is(1, 0x22));
}

static char g_set_key[32], g_set_val[32];
static int g_set_calls;
static int set_hook(const char *k, const char *v)
{
	snprintf(g_set_key, sizeof g_set_key, "%s", k);
	snprintf(g_set_val, sizeof g_set_val, "%s", v);
	g_set_calls++;
	return 0;
}
static char g_recheck_file[256];
static int g_recheck_calls;
static void recheck_hook(const char *f) { snprintf(g_recheck_file, sizeof g_recheck_file, "%s", f); g_recheck_calls++; }

static void hooks_reset()
{
	g_set_calls = g_recheck_calls = 0;
	g_set_key[0] = g_set_val[0] = g_recheck_file[0] = 0;
	audit_set_hooks(set_hook, recheck_hook);
}

static uint32_t newest_setting_id()
{
	AuditRow r[4];
	return audit_events(AU_SETTINGS, NULL, r, 4) > 0 ? r[0].id : 0;
}

static void undo_setting(int &bad)
{
	fresh();
	hooks_reset();
	audit_setting(AU_PAD, "speed", "0", "3");
	uint32_t id = newest_setting_id();
	AuditRow r[4];
	CHECK(audit_events(AU_SETTINGS, NULL, r, 4) == 1 && r[0].reversible == 1);
	char why[128] = "";
	CHECK(audit_undo_row(id, why, sizeof why) == 0);
	CHECK(g_set_calls == 1 && !strcmp(g_set_key, "speed") && !strcmp(g_set_val, "0"));
	// the setting row reads undone; its undo row is an event of the settings domain
	int n = audit_events(AU_SETTINGS, NULL, r, 4);
	CHECK(n == 2 && !strcmp(r[0].kind, "undo") && r[1].id == id && r[1].undone == 1 && r[1].reversible == 0);
	CHECK(audit_undo_row(id, why, sizeof why) == -1 && g_set_calls == 1);
	// a setting changed again since is not undone
	audit_setting(AU_PAD, "speed", "0", "3");
	uint32_t id2 = newest_setting_id();
	audit_setting(AU_PAD, "speed", "3", "5");
	CHECK(audit_events(AU_SETTINGS, NULL, r, 4) >= 2 && r[1].id == id2 && r[1].reversible == 0 && r[0].reversible == 1);
	char want[24];
	snprintf(want, sizeof want, "row %u", (unsigned)id2);
	CHECK(audit_undo_row(id2, why, sizeof why) == -1 && strstr(why, want) && g_set_calls == 1);
	audit_set_hooks(NULL, NULL);
}

static void recheck_called_once_per_file(int &bad)
{
	fresh();
	hooks_reset();
	img_make(4);
	uint32_t job = audit_job_begin(AU_PAD, "U:4", "U", "repair");
	repair_write(job, 0, 1, 2);
	repair_write(job, 1, 1, 2);
	repair_write(job, 2, 1, 2);
	audit_job_end(job, "finished");
	char why[128] = "";
	CHECK(audit_undo_job(job, why, sizeof why) == 0);
	CHECK(g_recheck_calls == 1 && !strcmp(g_recheck_file, IMG));
	audit_set_hooks(NULL, NULL);
}

static void undo_disc_since(int &bad)
{
	fresh();
	img_make(4);
	uint32_t j1 = audit_job_begin(AU_PAD, "X:4", "X", "repair");
	repair_write(j1, 0, 0x0A, 0x0B);
	audit_job_end(j1, "finished");
	uint32_t j2 = audit_job_begin(AU_PAD, "Y:4", "Y", "repair");
	repair_write(j2, 1, 0x0A, 0x0B);
	audit_job_end(j2, "finished");
	uint32_t j3 = audit_job_begin(AU_PAD, "X:4", "X", "repair");
	repair_write(j3, 0, 0x0B, 0x0C);
	audit_job_end(j3, "finished");
	char why[128] = "";
	CHECK(audit_undo_disc_since("X:4", (uint32_t)time(NULL) + 60, why, sizeof why) == 0);   // nothing that new
	CHECK(img_is(0, 0x0C));
	CHECK(audit_undo_disc_since("X:4", 0, why, sizeof why) == 0);
	CHECK(img_is(0, 0x0A) && img_is(1, 0x0B));   // the other disc is left alone
}

static bool blob_exists(uint32_t job)
{
	char p[256];
	snprintf(p, sizeof p, "%s/job%u.bin", DIR, (unsigned)job);
	FILE *f = fopen(p, "rb");
	if (f) fclose(f);
	return f != NULL;
}

static void cap_keeps_running_job(int &bad)
{
	fresh();
	img_make(8);
	audit_set_cap(3 * 4704);
	uint32_t j1 = audit_job_begin(AU_PAD, "U:8", "U", "repair");
	repair_write(j1, 0, 1, 2);
	repair_write(j1, 1, 1, 2);
	audit_job_end(j1, "finished");
	uint32_t j2 = audit_job_begin(AU_PAD, "U:8", "U", "repair");
	repair_write(j2, 2, 1, 2);
	CHECK(blob_exists(j1) && blob_exists(j2));   // 3 rows: at the cap, not over it
	repair_write(j2, 3, 1, 2);
	CHECK(!blob_exists(j1) && blob_exists(j2));
	for (uint32_t l = 4; l < 8; l++) repair_write(j2, l, 1, 2);   // the running job alone is over the cap
	CHECK(blob_exists(j2));
	audit_set_cap(4704);   // lowering it does not touch the running job either
	CHECK(blob_exists(j2));
	AuditRow r;
	CHECK(row_of(j1, sector_id(j1, 0), &r) && r.reversible == 0);
	audit_job_end(j2, "finished");
	CHECK(row_of(j2, sector_id(j2, 5), &r) && r.reversible == 1);
	audit_set_cap(4704);   // finished now: it goes
	CHECK(!blob_exists(j2));
	audit_set_cap(256ull << 20);
}

static void cap_lowered_prunes_now(int &bad)
{
	fresh();
	img_make(4);
	uint32_t j[3];
	for (int i = 0; i < 3; i++) {
		j[i] = audit_job_begin(AU_PAD, "U:4", "U", "repair");
		repair_write(j[i], (uint32_t)i, 1, 2);
		audit_job_end(j[i], "finished");
	}
	AuditRow r;
	for (int i = 0; i < 3; i++) CHECK(row_of(j[i], sector_id(j[i], 0), &r) && r.reversible == 1);
	audit_set_cap(2 * 4704 + 100);
	CHECK(!blob_exists(j[0]) && blob_exists(j[1]) && blob_exists(j[2]));
	CHECK(row_of(j[0], sector_id(j[0], 0), &r) && r.reversible == 0);
	CHECK(row_of(j[1], sector_id(j[1], 0), &r) && r.reversible == 1);
	audit_set_cap(4704);
	CHECK(!blob_exists(j[1]) && blob_exists(j[2]));
	CHECK(row_of(j[1], sector_id(j[1], 0), &r) && r.reversible == 0);
	audit_set_cap(256ull << 20);
}

int main()
{
	struct { const char *name; void (*fn)(int &); } tests[] = {
		{"rows_survive_reload", rows_survive_reload}, {"half_line_ignored", half_line_ignored},
		{"cut_fails", cut_fails}, {"escaping", escaping}, {"blob_layout", blob_layout}, {"threads", threads},
		{"discs_grouping", discs_grouping},
		{"undo_sector", undo_sector}, {"undo_refused_when_changed", undo_refused_when_changed},
		{"undo_job_newest_first", undo_job_newest_first}, {"undo_job_stops_at_refusal", undo_job_stops_at_refusal},
		{"undo_missing_blob", undo_missing_blob}, {"undo_setting", undo_setting},
		{"recheck_called_once_per_file", recheck_called_once_per_file}, {"undo_disc_since", undo_disc_since},
		{"cap_keeps_running_job", cap_keeps_running_job}, {"cap_lowered_prunes_now", cap_lowered_prunes_now},
	};
	if (system("mkdir -p out")) {}
	for (auto &t : tests) {
		int bad = 0;
		t.fn(bad);
		printf("%s %s\n", bad ? "FAIL" : "ok  ", t.name);
	}
	printf("audit_test: %d problem(s)\n", g_fail);
	return g_fail ? 1 : 0;
}
