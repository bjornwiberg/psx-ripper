// Host tests of the audit log module (source/audit.cpp). Run: make -C tests/host
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <set>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
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
// set by a test: the next close of an image file makes audit.log read-only (the disk filling up
// between the sector write and its undo row)
static int g_log_fails_after_close;
static int io_close(void *f)
{
	int rc = fclose((FILE *)f);
	if (g_log_fails_after_close) { g_log_fails_after_close = 0; if (system("chmod 444 ./out/audit/audit.log")) {} }
	return rc;
}
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

// the newest undo row naming `id` among the rows of `job` (0 = none)
static uint32_t undo_row_of(uint32_t job, uint32_t id)
{
	static AuditRow r[64];
	char w[16];
	snprintf(w, sizeof w, "%u", (unsigned)id);
	int n = audit_rows(job, r, 0, 64);
	uint32_t u = 0;
	for (int i = 0; i < n; i++) if (!strcmp(r[i].kind, "undo") && !strcmp(r[i].what, w)) u = r[i].id;
	return u;
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
	CHECK(row_of(job, id, &r) && r.undone == 1 && r.reversible == 1 && r.redo == 1);
	// a second undo of the same row is a redo: the new bytes again, through its undo row
	uint32_t u = undo_row_of(job, id);
	CHECK(audit_undo_row(id, why, sizeof why) == 0 && img_is(2, 0x22));
	CHECK(undo_rows_naming(job, id) == 1 && undo_rows_naming(job, u) == 1);
	CHECK(row_of(job, id, &r) && r.undone == 0 && r.reversible == 1 && r.redo == 0);
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
static uint32_t g_recheck_lbas[16];
static uint8_t g_recheck_zero[16];
static int g_recheck_n, g_recheck_redo;
static void recheck_hook(const char *f, const uint32_t *lbas, const uint8_t *zero, int n, int redo)
{
	snprintf(g_recheck_file, sizeof g_recheck_file, "%s", f);
	g_recheck_calls++;
	g_recheck_n = n;
	g_recheck_redo = redo;
	for (int i = 0; i < n && i < 16; i++) { g_recheck_lbas[i] = lbas[i]; g_recheck_zero[i] = zero[i]; }
}

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
	CHECK(n == 2 && !strcmp(r[0].kind, "undo") && r[1].id == id && r[1].undone == 1 && r[1].reversible == 1 && r[1].redo == 1);
	// undone again: a redo, the row's value again
	CHECK(audit_undo_row(id, why, sizeof why) == 0 && g_set_calls == 2 && !strcmp(g_set_val, "3"));
	CHECK(audit_events(AU_SETTINGS, NULL, r, 4) == 3 && r[2].id == id && r[2].undone == 0 && r[2].reversible == 1 && r[2].redo == 0);
	// a setting changed again since is not undone
	audit_setting(AU_PAD, "speed", "0", "3");
	uint32_t id2 = newest_setting_id();
	audit_setting(AU_PAD, "speed", "3", "5");
	CHECK(audit_events(AU_SETTINGS, NULL, r, 4) >= 2 && r[1].id == id2 && r[1].reversible == 0 && r[0].reversible == 1);
	char want[24];
	snprintf(want, sizeof want, "row %u", (unsigned)id2);
	CHECK(audit_undo_row(id2, why, sizeof why) == -1 && strstr(why, want) && g_set_calls == 2);
	audit_set_hooks(NULL, NULL);
}

// a secret setting's row (rev=0) is never reversible, also after a reload, and says why
static void private_setting_not_reversible(int &bad)
{
	fresh();
	hooks_reset();
	audit_setting_private(AU_PAD, "webhook_header1", "cleared", "set");
	AuditRow r[4];
	CHECK(audit_events(AU_SETTINGS, NULL, r, 4) == 1 && r[0].reversible == 0 && r[0].norev == 1);
	uint32_t id = r[0].id;
	char why[128] = "";
	CHECK(audit_undo_row(id, why, sizeof why) == -1 && strstr(why, "the value is not kept in the log") && g_set_calls == 0);
	audit_init(DIR, &IO);   // reload from the log
	hooks_reset();
	CHECK(audit_events(AU_SETTINGS, NULL, r, 4) == 1 && r[0].reversible == 0);
	why[0] = 0;
	CHECK(audit_undo_row(id, why, sizeof why) == -1 && strstr(why, "not kept") && g_set_calls == 0);
	audit_setting(AU_PAD, "speed", "0", "1");
	CHECK(audit_events(AU_SETTINGS, NULL, r, 4) == 2 && r[0].reversible == 1 && r[1].reversible == 0);
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

// a repair running on the same disc (second drive) blocks undo of an older job's rows
static void undo_refused_while_disc_busy(int &bad)
{
	fresh();
	img_make(4);
	uint32_t a = audit_job_begin(AU_PAD, "U:4", "U", "repair");
	repair_write(a, 1, 0x11, 0x22);
	audit_job_end(a, "finished");
	uint32_t b = audit_job_begin(AU_PAD, "U:4", "U", "repair");
	uint32_t id = sector_id(a, 0);
	AuditRow r;
	CHECK(row_of(a, id, &r) && r.reversible == 0);
	char why[128] = "", want[24];
	CHECK(audit_undo_row(id, why, sizeof why) == -1);
	snprintf(want, sizeof want, "row %u", (unsigned)id);
	CHECK(strstr(why, want) && strstr(why, "running"));
	CHECK(img_is(1, 0x22));
	CHECK(audit_undo_job(a, why, sizeof why) == -1 && img_is(1, 0x22));
	CHECK(audit_undo_disc_since("U:4", 0, why, sizeof why) == -1 && img_is(1, 0x22));
	audit_job_end(b, "finished");
	CHECK(audit_undo_row(id, why, sizeof why) == 0 && img_is(1, 0x11));
}

// the recheck gets every undone sector of the file and whether its restored bytes are all zero
// (an unreadable sector the rip filled with zeros)
static void recheck_gets_sectors(int &bad)
{
	fresh();
	hooks_reset();
	img_make(4);
	uint32_t job = audit_job_begin(AU_PAD, "U:4", "U", "repair");
	repair_write(job, 1, 0x00, 0x22);
	repair_write(job, 3, 0x11, 0x22);
	audit_job_end(job, "finished");
	char why[128] = "";
	CHECK(audit_undo_job(job, why, sizeof why) == 0);
	CHECK(g_recheck_calls == 1 && g_recheck_n == 2);
	bool z1 = false, b3 = false;
	for (int i = 0; i < g_recheck_n; i++) {
		if (g_recheck_lbas[i] == 1 && g_recheck_zero[i]) z1 = true;
		if (g_recheck_lbas[i] == 3 && !g_recheck_zero[i]) b3 = true;
	}
	CHECK(z1 && b3);
	audit_set_hooks(NULL, NULL);
}

// R8: a rip that was cancelled (or never ended) wrote into a .part that is gone; its rows name the
// image an earlier rip left there, so their undo is refused; a finished rip's undo works. A repair
// writes into the image itself: its rows stay reversible whatever its result (R8b).
static void undo_refused_unfinished(int &bad)
{
	fresh();
	img_make(4);
	uint32_t job = audit_job_begin(AU_PAD, "U:4", "U", "rip");
	repair_write(job, 1, 0x11, 0x22);
	audit_job_end(job, "cancelled");
	char why[128] = "";
	AuditRow r;
	CHECK(row_of(job, sector_id(job, 0), &r) && r.reversible == 0);
	CHECK(audit_undo_job(job, why, sizeof why) == -1 && strstr(why, "rip was not finished"));
	CHECK(img_is(1, 0x22));
	// no result row and not running (the app stopped in the middle): the same
	uint32_t j2 = audit_job_begin(AU_PAD, "V:4", "V", "rip");
	repair_write(j2, 2, 0x11, 0x22);
	audit_init(DIR, &IO);   // a new run: nothing is running
	why[0] = 0;
	CHECK(audit_undo_job(j2, why, sizeof why) == -1 && strstr(why, "rip was not finished"));
	CHECK(img_is(2, 0x22));
	// the result survives a reload: a finished rip stays reversible
	uint32_t j3 = audit_job_begin(AU_PAD, "W:4", "W", "rip");
	repair_write(j3, 3, 0x11, 0x22);
	audit_job_end(j3, "finished");
	audit_init(DIR, &IO);
	CHECK(row_of(j3, sector_id(j3, 0), &r) && r.reversible == 1);
	CHECK(audit_undo_job(j3, why, sizeof why) == 0 && img_is(3, 0x11));
}

static void undo_unfinished_repair(int &bad)
{
	fresh();
	img_make(4);
	char why[128] = "";
	AuditRow r;
	// a cancelled repair: what it wrote can be undone
	uint32_t job = audit_job_begin(AU_PAD, "U:4", "U", "repair");
	repair_write(job, 1, 0x11, 0x22);
	audit_job_end(job, "cancelled");
	CHECK(row_of(job, sector_id(job, 0), &r) && r.reversible == 1);
	CHECK(audit_undo_job(job, why, sizeof why) == 0 && img_is(1, 0x11));
	// a repair without a result row (the app stopped in the middle), after a reload: nothing runs
	uint32_t j2 = audit_job_begin(AU_PAD, "V:4", "V", "repair");
	repair_write(j2, 2, 0x11, 0x22);
	audit_init(DIR, &IO);
	CHECK(row_of(j2, sector_id(j2, 0), &r) && r.reversible == 1);
	why[0] = 0;
	CHECK(audit_undo_job(j2, why, sizeof why) == 0 && img_is(2, 0x11));
}

// the image got its old bytes back but the undo row could not be appended: say so, and the next
// undo only adds the missing row instead of being refused forever
static void undo_row_lost(int &bad)
{
	fresh();
	hooks_reset();
	img_make(4);
	uint32_t job = audit_job_begin(AU_PAD, "U:4", "U", "repair");
	repair_write(job, 1, 0x11, 0x22);
	repair_write(job, 2, 0x11, 0x22);
	audit_job_end(job, "finished");
	uint32_t id1 = sector_id(job, 0), id2 = sector_id(job, 1);
	char why[128] = "", want[24];
	g_log_fails_after_close = 1;
	CHECK(audit_undo_job(job, why, sizeof why) == -1);
	snprintf(want, sizeof want, "row %u", (unsigned)id2);
	CHECK(strstr(why, want) && strstr(why, "could not be written"));
	CHECK(img_is(2, 0x11) && img_is(1, 0x22));   // newest restored, then stopped
	CHECK(g_recheck_calls == 1);                  // the file did change
	if (system("chmod 644 ./out/audit/audit.log")) {}
	AuditRow r;
	CHECK(row_of(job, id2, &r) && r.undone == 0 && r.reversible == 1);
	CHECK(audit_undo_job(job, why, sizeof why) == 0);
	CHECK(img_is(1, 0x11) && img_is(2, 0x11));
	CHECK(row_of(job, id2, &r) && r.undone == 1 && undo_rows_naming(job, id2) == 1);
	CHECK(row_of(job, id1, &r) && r.undone == 1 && undo_rows_naming(job, id1) == 1);
	audit_set_hooks(NULL, NULL);
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

// Every verdict that a row cannot be undone carries a reason code (the UI says it in its language)
static void reason_codes(int &bad)
{
	fresh();
	hooks_reset();
	img_make(8);
	AuditRow r;
	AuditReason rs;
	char why[160];
	// an unfinished rip
	uint32_t rip = audit_job_begin(AU_PAD, "U:8", "U", "rip");
	repair_write(rip, 1, 0x11, 0x22);
	audit_job_end(rip, "cancelled");
	CHECK(row_of(rip, sector_id(rip, 0), &r) && r.why == AW_UNFINISHED && r.why_job == rip);
	CHECK(audit_undo_job(rip, why, sizeof why, &rs) == -1 && rs.code == AW_UNFINISHED && rs.job == rip && strstr(why, "rip was not finished"));
	// the job row itself is a record, not a change
	uint32_t jid = 0;
	{ static AuditRow a[8]; int n = audit_rows(rip, a, 0, 8); for (int i = 0; i < n; i++) if (!strcmp(a[i].kind, "job")) { jid = a[i].id; CHECK(a[i].why == AW_RECORD); } }
	CHECK(jid && audit_undo_row(jid, why, sizeof why, &rs) == -1 && rs.code == AW_RECORD && rs.row == jid);
	// a sector changed since
	uint32_t rep = audit_job_begin(AU_PAD, "V:8", "V", "repair");
	repair_write(rep, 2, 0x11, 0x22);
	audit_job_end(rep, "finished");
	uint8_t x[2352];
	sector_fill(x, 0x33);
	img_put(2, x);
	uint32_t sid = sector_id(rep, 0);
	CHECK(row_of(rep, sid, &r) && r.reversible == 0 && r.why == AW_SECTOR_CHANGED && r.lba == 2);
	CHECK(audit_undo_row(sid, why, sizeof why, &rs) == -1 && rs.code == AW_SECTOR_CHANGED && rs.lba == 2 && rs.row == sid);
	// still running
	uint32_t run = audit_job_begin(AU_PAD, "W:8", "W", "repair");
	repair_write(run, 3, 0x11, 0x22);
	CHECK(row_of(run, sector_id(run, 0), &r) && r.why == AW_RUNNING && r.why_job == run);
	audit_job_end(run, "finished");
	// already undone
	uint32_t s3 = sector_id(run, 0);
	CHECK(audit_undo_row(s3, why, sizeof why, &rs) == 0 && rs.code == AW_NONE);
	CHECK(row_of(run, s3, &r) && r.why == AW_NONE && r.undone == 1 && r.redo == 1);   // undone: its undo is a redo
	uint32_t su = undo_row_of(run, s3);
	CHECK(audit_undo_row(s3, why, sizeof why, &rs) == 0 && rs.code == AW_NONE);
	// the undo row the redo reversed is history: already undone
	CHECK(row_of(run, su, &r) && r.why == AW_ALREADY);
	CHECK(audit_undo_row(su, why, sizeof why, &rs) == -1 && rs.code == AW_ALREADY);
	// kept bytes dropped by the size cap
	uint32_t old = audit_job_begin(AU_PAD, "X:8", "X", "repair");
	repair_write(old, 4, 0x11, 0x22);
	audit_job_end(old, "finished");
	audit_set_cap(0);
	CHECK(row_of(old, sector_id(old, 0), &r) && r.why == AW_DROPPED);
	audit_set_cap(256ull << 20);
	// a secret setting, a setting changed since, an unknown row
	audit_setting_private(AU_PAD, "webhook", "cleared", "set");
	uint32_t sec = newest_setting_id();
	CHECK(audit_undo_row(sec, why, sizeof why, &rs) == -1 && rs.code == AW_SECRET);
	audit_setting(AU_PAD, "speed", "0", "1");
	uint32_t s1 = newest_setting_id();
	audit_setting(AU_PAD, "speed", "1", "2");
	AuditRow e[8];
	int n = audit_events(AU_SETTINGS, NULL, e, 8);
	for (int i = 0; i < n; i++) {
		if (e[i].id == sec) CHECK(e[i].why == AW_SECRET);
		if (e[i].id == s1) CHECK(e[i].why == AW_SETTING_CHANGED && !strcmp(e[i].what, "speed"));
	}
	CHECK(audit_undo_row(s1, why, sizeof why, &rs) == -1 && rs.code == AW_SETTING_CHANGED && !strcmp(rs.what, "speed"));
	CHECK(audit_undo_row(999999, why, sizeof why, &rs) == -1 && rs.code == AW_NO_ROW);
	audit_set_hooks(NULL, NULL);
}

// audit_job_reversible / audit_disc_reversible: 1 when an undo would undo at least one row
static void reversible_queries(int &bad)
{
	fresh();
	hooks_reset();
	img_make(8);
	AuditReason rs;
	char why[160];
	// a disc whose only rows are an unfinished rip
	uint32_t rip = audit_job_begin(AU_PAD, "U:8", "U", "rip");
	repair_write(rip, 1, 0x11, 0x22);
	audit_job_end(rip, "cancelled");
	CHECK(audit_job_reversible(rip, &rs) == 0 && rs.code == AW_UNFINISHED);
	CHECK(audit_disc_reversible("U:8", &rs) == 0 && rs.code == AW_UNFINISHED);
	// a check wrote nothing: nothing to undo
	uint32_t chk = audit_job_begin(AU_PAD, "C:8", "C", "check");
	audit_job_end(chk, "finished");
	CHECK(audit_job_reversible(chk, &rs) == 0 && rs.code == AW_NOTHING);
	CHECK(audit_disc_reversible("C:8", &rs) == 0 && rs.code == AW_NOTHING);
	// a finished repair: yes, until it is undone
	uint32_t rep = audit_job_begin(AU_PAD, "V:8", "V", "repair");
	repair_write(rep, 2, 0x11, 0x22);
	repair_write(rep, 3, 0x11, 0x22);
	audit_job_end(rep, "finished");
	CHECK(audit_job_reversible(rep, &rs) == 1 && rs.code == AW_NONE);
	CHECK(audit_disc_reversible("V:8", NULL) == 1);
	// its newest sector changed since: the undo would stop at once, so no
	uint8_t x[2352];
	sector_fill(x, 0x44);
	img_put(3, x);
	CHECK(audit_job_reversible(rep, &rs) == 0 && rs.code == AW_SECTOR_CHANGED && rs.lba == 3);
	CHECK(audit_disc_reversible("V:8", &rs) == 0 && rs.code == AW_SECTOR_CHANGED);
	CHECK(audit_undo_job(rep, why, sizeof why) == -1);
	sector_fill(x, 0x22);
	img_put(3, x);
	CHECK(audit_undo_job(rep, why, sizeof why) == 0);
	// all undone: an undo of the job or disc now is a redo
	int redo = 0;
	CHECK(audit_job_reversible(rep, &rs, &redo) == 1 && rs.code == AW_NONE && redo == 1);
	redo = 0;
	CHECK(audit_disc_reversible("V:8", &rs, &redo) == 1 && redo == 1);
	// its oldest sector changed since the undo: the redo would stop at once, so no
	sector_fill(x, 0x55);
	img_put(2, x);
	CHECK(audit_job_reversible(rep, &rs, &redo) == 0 && rs.code == AW_SECTOR_CHANGED && rs.lba == 2 && redo == 1);
	CHECK(audit_disc_reversible("V:8", &rs) == 0 && rs.code == AW_SECTOR_CHANGED);
	sector_fill(x, 0x11);
	img_put(2, x);
	CHECK(audit_undo_job(rep, why, sizeof why) == 0 && img_is(2, 0x22) && img_is(3, 0x22));
	CHECK(audit_job_reversible(rep, &rs, &redo) == 1 && redo == 0);
	audit_set_hooks(NULL, NULL);
}

// I1: the disc ripped again to the same file replaces the image by a rename (no logged change); an
// older job's rows on that file hold bytes of the image it replaced, even when the new image's
// sector happens to equal the row's new bytes. Their undo is refused; the new rip's own rows are not.
static void undo_refused_after_rerip(int &bad)
{
	fresh();
	hooks_reset();
	img_make(4);
	AuditReason rs;
	AuditRow r;
	char why[160];
	uint32_t rep = audit_job_begin(AU_PAD, "U:4", "U", "repair");
	repair_write(rep, 1, 0x11, 0x22);
	audit_job_end(rep, "finished");
	uint32_t id = sector_id(rep, 0);
	// a cancelled rip of the same file made no image: the repair stays reversible
	uint32_t can = audit_job_begin(AU_PAD, "U:4", "U", "rip");
	audit_job_end(can, "cancelled", IMG);
	CHECK(row_of(rep, id, &r) && r.reversible == 1);
	// a finished rip of the same file; its sector 1 holds 0x22 as well (a good rip)
	uint32_t rip = audit_job_begin(AU_PAD, "U:4", "U", "rip");
	audit_job_end(rip, "finished", IMG);
	CHECK(row_of(rep, id, &r) && r.reversible == 0 && r.why == AW_REPLACED);
	CHECK(audit_undo_row(id, why, sizeof why, &rs) == -1 && rs.code == AW_REPLACED && strstr(why, "ripped again"));
	CHECK(audit_undo_job(rep, why, sizeof why, &rs) == -1 && rs.code == AW_REPLACED);
	CHECK(audit_job_reversible(rep, &rs) == 0 && rs.code == AW_REPLACED);
	CHECK(audit_disc_reversible("U:4", &rs) == 0 && rs.code == AW_REPLACED);
	CHECK(audit_undo_disc_since("U:4", 0, why, sizeof why, &rs) == -1 && rs.code == AW_REPLACED);
	CHECK(img_is(1, 0x22) && undo_rows_naming(rep, id) == 0 && g_recheck_calls == 0);
	audit_init(DIR, &IO);   // the image the rip made is in the log
	CHECK(audit_undo_row(id, why, sizeof why, &rs) == -1 && rs.code == AW_REPLACED && img_is(1, 0x22));
	// the newer rip's own corrections stay reversible; a rip of another file changes nothing
	uint32_t rip2 = audit_job_begin(AU_PAD, "U:4", "U", "rip");
	repair_write(rip2, 2, 0x11, 0x33);
	audit_job_end(rip2, "finished", IMG);
	uint32_t other = audit_job_begin(AU_PAD, "W:4", "W", "rip");
	audit_job_end(other, "finished", "./out/other.bin");
	CHECK(row_of(rip2, sector_id(rip2, 0), &r) && r.reversible == 1);
	CHECK(audit_undo_job(rip2, why, sizeof why) == 0 && img_is(2, 0x11));
	audit_set_hooks(NULL, NULL);
}

// M4: an append cut short (disk full) leaves a fragment without a newline; the next row must not be
// glued onto it (it would be read with the fragment's id)
static void short_write_not_glued(int &bad)
{
	fresh();
	audit_note(0, AU_FILES, "", "file", "x", "", "");
	char p[256];
	snprintf(p, sizeof p, "%s/audit.log", DIR);
	struct stat st;
	CHECK(stat(p, &st) == 0);
	signal(SIGXFSZ, SIG_IGN);   // a write past the limit fails with EFBIG instead
	struct rlimit old, lim;
	getrlimit(RLIMIT_FSIZE, &old);
	lim = old;
	lim.rlim_cur = (rlim_t)st.st_size + 30;
	setrlimit(RLIMIT_FSIZE, &lim);
	audit_note(0, AU_FILES, "", "file", "y", "", "");   // only its first 30 bytes reach the file
	setrlimit(RLIMIT_FSIZE, &old);
	CHECK(stat(p, &st) == 0 && st.st_size > 0);
	audit_note(0, AU_FILES, "", "file", "z", "", "");
	AuditRow r[8];
	int n = audit_events(AU_FILES, NULL, r, 8);
	CHECK(n == 2 && r[0].id == 3 && !strcmp(r[0].what, "z") && !strcmp(r[1].what, "x"));
	audit_init(DIR, &IO);   // and after a reload
	n = audit_events(AU_FILES, NULL, r, 8);
	CHECK(n == 2 && r[0].id == 3 && !strcmp(r[0].what, "z"));
	audit_note(0, AU_FILES, "", "file", "w", "", "");
	CHECK(audit_events(AU_FILES, NULL, r, 8) == 3 && r[0].id == 4);
}

// I2: what the log says each setting is now (newest row, undos counted), for the boot's comparison
static void setting_values(int &bad)
{
	fresh();
	hooks_reset();
	audit_setting(AU_PAD, "speed", "1", "2");
	audit_setting(AU_PAD, "retries", "4", "8");
	audit_setting(AU_REMOTE, "speed", "2", "3");
	audit_setting_private(AU_PAD, "webhook", "cleared", "set");
	char why[160];
	CHECK(audit_undo_row(newest_setting_id() - 1, why, sizeof why) == 0);   // speed 3 -> 2 (the hook)
	char k[8][48], v[8][192];
	int n = audit_setting_values(k, v, 8);
	std::string got;
	for (int i = 0; i < n; i++) got += std::string(k[i]) + "=" + v[i] + ";";
	CHECK(n == 3 && got.find("speed=2;") != std::string::npos && got.find("retries=8;") != std::string::npos &&
	      got.find("webhook=set;") != std::string::npos);
	CHECK(audit_setting_values(k, v, 1) == 1);
	audit_set_hooks(NULL, NULL);
}

// M3: a big undo calls the app back every AUDIT_TICK sectors (frames, the system's exit request),
// and stops when it says so; what was undone stays undone and is checked again
static uint32_t g_tick_done[16], g_tick_total;
static int g_ticks, g_tick_stop_at = -1;
static int tick(uint32_t done, uint32_t total)
{
	if (g_ticks < 16) g_tick_done[g_ticks] = done;
	g_ticks++;
	g_tick_total = total;
	return g_tick_stop_at >= 0 && done >= (uint32_t)g_tick_stop_at;
}
static void undo_progress(int &bad)
{
	fresh();
	hooks_reset();
	img_make(200);
	uint32_t job = audit_job_begin(AU_PAD, "P:200", "P", "repair");
	for (uint32_t l = 0; l < 150; l++) repair_write(job, l, 0x11, 0x22);
	audit_job_end(job, "finished");
	audit_set_progress(tick);
	g_ticks = 0; g_tick_stop_at = -1;
	char why[160];
	AuditReason rs;
	CHECK(audit_undo_row(sector_id(job, 0), why, sizeof why) == 0 && g_ticks == 0);   // one row: no wait
	CHECK(audit_undo_job(job, why, sizeof why) == 0);
	CHECK(g_ticks == 3 && g_tick_total == 149 && g_tick_done[0] == 0 && g_tick_done[1] == 64 && g_tick_done[2] == 128);
	// stopped (the app is closing): the rows before stay undone, the file is checked again
	uint32_t j2 = audit_job_begin(AU_PAD, "Q:200", "Q", "repair");
	for (uint32_t l = 0; l < 150; l++) repair_write(j2, l, 0x11, 0x33);
	audit_job_end(j2, "finished");
	g_ticks = 0; g_tick_stop_at = 64; g_recheck_calls = 0;
	CHECK(audit_undo_job(j2, why, sizeof why, &rs) == -1 && rs.code == AW_STOPPED && strstr(why, "stopped"));
	CHECK(img_is(149, 0x11) && img_is(86, 0x11) && img_is(85, 0x33) && img_is(0, 0x33));   // newest first: 64 rows
	CHECK(g_recheck_calls == 1 && g_recheck_n == 64);
	audit_set_progress(NULL);
	CHECK(audit_undo_job(j2, why, sizeof why) == 0 && img_is(0, 0x11));
	audit_set_hooks(NULL, NULL);
}

// an undo is itself logged and can be undone (a redo), and that again: X -> U1 -> U2 -> U3
static void redo_sector_chain(int &bad)
{
	fresh();
	hooks_reset();
	img_make(4);
	uint32_t job = audit_job_begin(AU_PAD, "U:4", "U", "repair");
	repair_write(job, 2, 0x11, 0x22);
	audit_job_end(job, "finished");
	uint32_t x = sector_id(job, 0);
	char why[160] = "";
	AuditReason rs;
	AuditRow r;
	CHECK(audit_undo_row(x, why, sizeof why) == 0 && img_is(2, 0x11));
	CHECK(g_recheck_calls == 1 && g_recheck_redo == 0 && g_recheck_n == 1 && g_recheck_lbas[0] == 2);
	uint32_t u1 = undo_row_of(job, x);
	CHECK(u1 && row_of(job, u1, &r) && r.undone == 0 && r.reversible == 1 && r.redo == 1 && r.why == AW_NONE);
	// the redo: the new bytes again, the original row no longer undone, the undo row undone
	CHECK(audit_undo_row(u1, why, sizeof why, &rs) == 0 && rs.code == AW_NONE && img_is(2, 0x22));
	CHECK(g_recheck_calls == 2 && g_recheck_redo == 1 && g_recheck_n == 1 && g_recheck_lbas[0] == 2 && g_recheck_zero[0] == 0);
	uint32_t u2 = undo_row_of(job, u1);
	CHECK(u2 > u1 && undo_rows_naming(job, u1) == 1);
	CHECK(row_of(job, x, &r) && r.undone == 0 && r.reversible == 1 && r.redo == 0);
	CHECK(row_of(job, u1, &r) && r.undone == 1 && r.reversible == 0 && r.why == AW_ALREADY);
	CHECK(row_of(job, u2, &r) && r.undone == 0 && r.reversible == 1 && r.redo == 0 && r.src == AU_UNDO);
	CHECK(audit_undo_row(u1, why, sizeof why, &rs) == -1 && rs.code == AW_ALREADY && img_is(2, 0x22));
	// undoing the redo undoes the original again
	CHECK(audit_undo_row(u2, why, sizeof why) == 0 && img_is(2, 0x11) && g_recheck_redo == 0);
	uint32_t u3 = undo_row_of(job, u2);
	CHECK(u3 > u2);
	CHECK(row_of(job, x, &r) && r.undone == 1 && r.reversible == 1 && r.redo == 1);
	CHECK(row_of(job, u2, &r) && r.undone == 1 && r.reversible == 0);
	CHECK(row_of(job, u3, &r) && r.undone == 0 && r.reversible == 1 && r.redo == 1);
	// the state survives a reload
	audit_init(DIR, &IO);
	CHECK(row_of(job, x, &r) && r.undone == 1);
	CHECK(row_of(job, u3, &r) && r.reversible == 1 && r.redo == 1);
	CHECK(audit_undo_row(u3, why, sizeof why) == 0 && img_is(2, 0x22));
	CHECK(row_of(job, x, &r) && r.undone == 0 && r.reversible == 1);
	// the original undone directly after a redo: the redo's row is then undone too
	CHECK(audit_undo_row(x, why, sizeof why) == 0 && img_is(2, 0x11));
	uint32_t u4 = undo_row_of(job, u3);
	CHECK(u4 && row_of(job, u4, &r) && r.undone == 1 && r.reversible == 0);
	CHECK(row_of(job, x, &r) && r.undone == 1);
	audit_set_hooks(NULL, NULL);
}

// a redo is refused when the sector changed since the undo, and under the undo's guards
static void redo_refused(int &bad)
{
	fresh();
	hooks_reset();
	img_make(4);
	uint32_t job = audit_job_begin(AU_PAD, "U:4", "U", "repair");
	repair_write(job, 1, 0x11, 0x22);
	audit_job_end(job, "finished");
	uint32_t x = sector_id(job, 0);
	char why[160] = "", want[24];
	AuditReason rs;
	AuditRow r;
	CHECK(audit_undo_row(x, why, sizeof why) == 0 && img_is(1, 0x11));
	uint32_t u1 = undo_row_of(job, x);
	uint8_t c[2352];
	sector_fill(c, 0x33);
	img_put(1, c);
	CHECK(row_of(job, u1, &r) && r.reversible == 0 && r.why == AW_SECTOR_CHANGED && r.lba == 1);
	CHECK(audit_undo_row(u1, why, sizeof why, &rs) == -1 && rs.code == AW_SECTOR_CHANGED && rs.row == u1 && rs.lba == 1);
	snprintf(want, sizeof want, "row %u", (unsigned)u1);
	CHECK(strstr(why, want) && img_is(1, 0x33) && undo_rows_naming(job, u1) == 0);
	// back to the undone bytes, but a job runs on the disc
	sector_fill(c, 0x11);
	img_put(1, c);
	uint32_t busy = audit_job_begin(AU_PAD, "U:4", "U", "repair");
	CHECK(audit_undo_row(u1, why, sizeof why, &rs) == -1 && rs.code == AW_JOB_ON_DISC && img_is(1, 0x11));
	CHECK(audit_job_reversible(job, &rs) == 0 && rs.code == AW_JOB_ON_DISC);
	audit_job_end(busy, "finished");
	// the file ripped again since
	uint32_t rip = audit_job_begin(AU_PAD, "U:4", "U", "rip");
	audit_job_end(rip, "finished", IMG);
	CHECK(audit_undo_row(u1, why, sizeof why, &rs) == -1 && rs.code == AW_REPLACED && img_is(1, 0x11));
	CHECK(audit_undo_job(job, why, sizeof why, &rs) == -1 && rs.code == AW_REPLACED);
	CHECK(g_recheck_calls == 1);   // no refused redo wrote a sector
	audit_set_hooks(NULL, NULL);
}

// a setting's undo is undone through the same hook, and that again
static void redo_setting_chain(int &bad)
{
	fresh();
	hooks_reset();
	audit_setting(AU_PAD, "speed", "0", "3");
	uint32_t s = newest_setting_id();
	char why[160] = "";
	AuditReason rs;
	AuditRow e[8];
	CHECK(audit_undo_row(s, why, sizeof why) == 0 && g_set_calls == 1 && !strcmp(g_set_val, "0"));
	uint32_t u1 = newest_setting_id();
	CHECK(audit_events(AU_SETTINGS, NULL, e, 8) == 2 && e[0].id == u1 && !strcmp(e[0].kind, "undo") && e[0].reversible == 1 && e[0].redo == 1);
	CHECK(audit_undo_row(u1, why, sizeof why, &rs) == 0 && rs.code == AW_NONE);
	CHECK(g_set_calls == 2 && !strcmp(g_set_key, "speed") && !strcmp(g_set_val, "3"));
	int n = audit_events(AU_SETTINGS, NULL, e, 8);
	uint32_t u2 = e[0].id;
	char w[16];
	snprintf(w, sizeof w, "%u", (unsigned)u1);
	CHECK(n == 3 && !strcmp(e[0].kind, "undo") && !strcmp(e[0].what, w) && e[0].src == AU_UNDO && !strcmp(e[0].before, "0") && !strcmp(e[0].after, "3"));
	CHECK(e[0].reversible == 1 && e[0].redo == 0);
	CHECK(e[1].id == u1 && e[1].undone == 1 && e[1].reversible == 0);
	CHECK(e[2].id == s && e[2].undone == 0 && e[2].reversible == 1);
	{   // the value the log knows follows the chain
		char keys[4][48], vals[4][192];
		CHECK(audit_setting_values(keys, vals, 4) == 1 && !strcmp(vals[0], "3"));
	}
	CHECK(audit_undo_row(u2, why, sizeof why) == 0 && g_set_calls == 3 && !strcmp(g_set_val, "0"));
	n = audit_events(AU_SETTINGS, NULL, e, 8);
	CHECK(n == 4 && e[0].reversible == 1 && e[0].redo == 1 && e[3].id == s && e[3].undone == 1);
	// changed since the undo: the redo is refused, the hook not called
	audit_setting(AU_PAD, "speed", "0", "5");
	uint32_t u3 = 0;
	n = audit_events(AU_SETTINGS, NULL, e, 8);
	for (int i = 0; i < n; i++) if (!strcmp(e[i].kind, "undo") && !e[i].undone) { u3 = e[i].id; CHECK(e[i].reversible == 0 && e[i].why == AW_SETTING_CHANGED); }
	CHECK(u3 && audit_undo_row(u3, why, sizeof why, &rs) == -1 && rs.code == AW_SETTING_CHANGED && !strcmp(rs.what, "speed") && g_set_calls == 3);
	audit_set_hooks(NULL, NULL);
}

// a job whose rows are all undone: its undo is a redo, oldest first, stopping at the first refusal
static void redo_job(int &bad)
{
	fresh();
	hooks_reset();
	img_make(4);
	uint32_t job = audit_job_begin(AU_PAD, "U:4", "U", "repair");
	repair_write(job, 1, 0x0A, 0x0B);
	repair_write(job, 1, 0x0B, 0x0C);
	repair_write(job, 2, 0x0A, 0x0B);
	audit_job_end(job, "finished");
	char why[160] = "";
	AuditReason rs;
	AuditRow r;
	CHECK(audit_undo_job(job, why, sizeof why) == 0 && img_is(1, 0x0A) && img_is(2, 0x0A));
	CHECK(audit_undo_job(job, why, sizeof why) == 0 && img_is(1, 0x0C) && img_is(2, 0x0B));
	CHECK(g_recheck_redo == 1 && g_recheck_n == 3);
	for (int k = 0; k < 3; k++) CHECK(row_of(job, sector_id(job, k), &r) && r.undone == 0 && r.reversible == (k == 0 ? 0 : 1));
	CHECK(audit_undo_job(job, why, sizeof why) == 0 && img_is(1, 0x0A) && img_is(2, 0x0A) && g_recheck_redo == 0);
	for (int k = 0; k < 3; k++) CHECK(row_of(job, sector_id(job, k), &r) && r.undone == 1);
	// the redo stops at the first row it cannot redo (oldest first): nothing written before it
	uint8_t c[2352];
	sector_fill(c, 0x33);
	img_put(1, c);
	CHECK(audit_undo_job(job, why, sizeof why, &rs) == -1 && rs.code == AW_SECTOR_CHANGED && rs.lba == 1);
	CHECK(img_is(1, 0x33) && img_is(2, 0x0A));
	// the newest refused: the older ones are redone
	sector_fill(c, 0x0A);
	img_put(1, c);
	sector_fill(c, 0x44);
	img_put(2, c);
	CHECK(audit_undo_job(job, why, sizeof why, &rs) == -1 && rs.code == AW_SECTOR_CHANGED && rs.lba == 2);
	CHECK(img_is(1, 0x0C) && img_is(2, 0x44));
	CHECK(row_of(job, sector_id(job, 0), &r) && r.undone == 0);
	CHECK(row_of(job, sector_id(job, 2), &r) && r.undone == 1);
	// mixed: an undo again (the redone rows first)
	CHECK(audit_undo_job(job, why, sizeof why) == 0 && img_is(1, 0x0A));
	audit_set_hooks(NULL, NULL);
}

static void redo_disc(int &bad)
{
	fresh();
	hooks_reset();
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
	char why[160] = "";
	int redo = -1;
	CHECK(audit_disc_reversible("X:4", NULL, &redo) == 1 && redo == 0);
	CHECK(audit_undo_disc_since("X:4", 0, why, sizeof why) == 0 && img_is(0, 0x0A) && img_is(1, 0x0B));
	CHECK(audit_disc_reversible("X:4", NULL, &redo) == 1 && redo == 1);
	CHECK(audit_job_reversible(j1, NULL, &redo) == 1 && redo == 1);
	CHECK(audit_undo_disc_since("X:4", 0, why, sizeof why) == 0 && img_is(0, 0x0C) && img_is(1, 0x0B));
	CHECK(audit_disc_reversible("X:4", NULL, &redo) == 1 && redo == 0);
	CHECK(audit_undo_disc_since("X:4", 0, why, sizeof why) == 0 && img_is(0, 0x0A));
	audit_set_hooks(NULL, NULL);
}

// the user never has to find the undo row: the undone row itself is redone, any number of times
static void redo_original_row(int &bad)
{
	fresh();
	hooks_reset();
	img_make(4);
	uint32_t job = audit_job_begin(AU_PAD, "U:4", "U", "repair");
	repair_write(job, 1, 0x11, 0x22);
	audit_job_end(job, "finished");
	uint32_t x = sector_id(job, 0);
	char why[160] = "";
	AuditReason rs;
	AuditRow r;
	for (int i = 0; i < 6; i++) {   // undo, redo, undo, redo, ...: a chain of 6 on the same row id
		bool undo = i % 2 == 0;
		CHECK(audit_undo_row(x, why, sizeof why, &rs) == 0 && rs.code == AW_NONE);
		CHECK(img_is(1, undo ? 0x11 : 0x22));
		CHECK(g_recheck_redo == (undo ? 0 : 1) && g_recheck_calls == i + 1);
		CHECK(row_of(job, x, &r) && r.undone == (undo ? 1 : 0) && r.reversible == 1 && r.redo == (undo ? 1 : 0));
	}
	CHECK(undo_rows_naming(job, x) == 3);   // each undo names the row, each redo the undo it reverses
	// an undone row whose sector changed since: the redo is refused and says why
	CHECK(audit_undo_row(x, why, sizeof why) == 0 && img_is(1, 0x11));
	uint8_t c[2352];
	sector_fill(c, 0x33);
	img_put(1, c);
	CHECK(row_of(job, x, &r) && r.undone == 1 && r.reversible == 0 && r.why == AW_SECTOR_CHANGED && r.redo == 1);
	CHECK(audit_undo_row(x, why, sizeof why, &rs) == -1 && rs.code == AW_SECTOR_CHANGED && rs.lba == 1 && img_is(1, 0x33));
	// a setting the same way
	audit_setting(AU_PAD, "speed", "0", "3");
	uint32_t s = newest_setting_id();
	g_set_calls = 0;
	for (int i = 0; i < 3; i++) {
		bool undo = i % 2 == 0;
		CHECK(audit_undo_row(s, why, sizeof why) == 0 && g_set_calls == i + 1 && !strcmp(g_set_val, undo ? "0" : "3"));
		AuditRow e[8];
		int n = audit_events(AU_SETTINGS, NULL, e, 8);
		CHECK(n == 2 + i && e[n - 1].id == s && e[n - 1].undone == (undo ? 1 : 0) && e[n - 1].reversible == 1);
	}
	audit_setting(AU_PAD, "speed", "0", "5");   // changed since the undo: no redo
	CHECK(audit_undo_row(s, why, sizeof why, &rs) == -1 && rs.code == AW_SETTING_CHANGED && !strcmp(rs.what, "speed") && g_set_calls == 3);
	audit_set_hooks(NULL, NULL);
}

int main()
{
	struct { const char *name; void (*fn)(int &); } tests[] = {
		{"rows_survive_reload", rows_survive_reload}, {"half_line_ignored", half_line_ignored},
		{"cut_fails", cut_fails}, {"escaping", escaping}, {"blob_layout", blob_layout}, {"threads", threads},
		{"discs_grouping", discs_grouping},
		{"undo_sector", undo_sector}, {"undo_refused_when_changed", undo_refused_when_changed},
		{"undo_job_newest_first", undo_job_newest_first}, {"undo_job_stops_at_refusal", undo_job_stops_at_refusal},
		{"undo_missing_blob", undo_missing_blob}, {"undo_setting", undo_setting}, {"private_setting_not_reversible", private_setting_not_reversible},
		{"recheck_called_once_per_file", recheck_called_once_per_file}, {"undo_disc_since", undo_disc_since},
		{"recheck_gets_sectors", recheck_gets_sectors}, {"undo_refused_unfinished", undo_refused_unfinished},
		{"undo_unfinished_repair", undo_unfinished_repair},
		{"undo_refused_while_disc_busy", undo_refused_while_disc_busy}, {"undo_row_lost", undo_row_lost},
		{"cap_keeps_running_job", cap_keeps_running_job}, {"cap_lowered_prunes_now", cap_lowered_prunes_now},
		{"reason_codes", reason_codes}, {"reversible_queries", reversible_queries},
		{"undo_refused_after_rerip", undo_refused_after_rerip}, {"short_write_not_glued", short_write_not_glued},
		{"setting_values", setting_values}, {"undo_progress", undo_progress},
		{"redo_sector_chain", redo_sector_chain}, {"redo_refused", redo_refused}, {"redo_setting_chain", redo_setting_chain},
		{"redo_job", redo_job}, {"redo_disc", redo_disc}, {"redo_original_row", redo_original_row},
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
