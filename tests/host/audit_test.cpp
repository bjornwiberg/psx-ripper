// Host tests of the audit log module (source/audit.cpp). Run: make -C tests/host
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
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

int main()
{
	struct { const char *name; void (*fn)(int &); } tests[] = {
		{"rows_survive_reload", rows_survive_reload}, {"half_line_ignored", half_line_ignored},
		{"cut_fails", cut_fails}, {"escaping", escaping}, {"blob_layout", blob_layout}, {"threads", threads},
		{"discs_grouping", discs_grouping},
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
