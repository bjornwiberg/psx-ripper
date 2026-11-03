// Audit log, see include/audit.h. Rows are single lines of tab-separated key=value pairs, values
// escaped (\t, \n, \r, \\), appended whole and flushed per row so a crash leaves at most one
// half line, which the next load drops.
#include "audit.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/mutex.h>
#include <vector>
#include <algorithm>

#define LINE_MAX_LEN 4096
#define BLOB_ROW 4704
#define SECTOR 2352

static const char *const SRC_NAME[] = {"pad", "remote", "app", "mcp", "undo"};
static const char *const DOM_NAME[] = {"rips", "settings", "files", "mcp"};

static sys_mutex_t g_mx;
static int g_mx_made;
static char g_dir[200];
static AuditIo g_io;
static uint32_t g_next_id = 1, g_next_job = 1;

// what the running jobs need on every row: where they came from and how many sectors they have
struct JobSlot { uint32_t job; AuditSrc src; char disc[96]; uint32_t blobs; };
static JobSlot g_slots[8];
static int g_slot_rr;

static void lock() { sysMutexLock(g_mx, 0); }
static void unlock() { sysMutexUnlock(g_mx); }

static void path_log(char *out, size_t n) { snprintf(out, n, "%s/audit.log", g_dir); }
static void path_blob(char *out, size_t n, uint32_t job) { snprintf(out, n, "%s/job%u.bin", g_dir, (unsigned)job); }

static void mkdirs(const char *dir)
{
	char p[200];
	snprintf(p, sizeof p, "%s", dir);
	for (char *c = p + 1; *c; c++)
		if (*c == '/') { *c = 0; mkdir(p, 0777); *c = '/'; }
	mkdir(p, 0777);
}

// Appends `s` (at most `max` bytes of it) escaped; returns the new length, never past cap-1.
static size_t put_esc(char *out, size_t len, size_t cap, const char *s, size_t max)
{
	for (size_t i = 0; s && s[i] && i < max; i++) {
		const char *e = NULL;
		char one[2] = {s[i], 0};
		if (s[i] == '\t') e = "\\t";
		else if (s[i] == '\n') e = "\\n";
		else if (s[i] == '\r') e = "\\r";
		else if (s[i] == '\\') e = "\\\\";
		else e = one;
		size_t k = strlen(e);
		if (len + k >= cap) break;
		memcpy(out + len, e, k);
		len += k;
	}
	out[len] = 0;
	return len;
}

static void unescape(char *s)
{
	char *w = s;
	for (char *r = s; *r; r++) {
		if (*r == '\\' && r[1]) {
			r++;
			*w++ = *r == 't' ? '\t' : *r == 'n' ? '\n' : *r == 'r' ? '\r' : *r;
		} else *w++ = *r;
	}
	*w = 0;
}

static void copyn(char *dst, size_t n, const char *s)
{
	size_t k = s ? strlen(s) : 0;
	if (k >= n) k = n - 1;
	if (k) memcpy(dst, s, k);
	dst[k] = 0;
}

// One line (without the newline) -> a row. False when it is not a row (no id or no kind).
static bool parse_line(char *line, AuditRow *r)
{
	memset(r, 0, sizeof *r);
	r->src = AU_APP;
	bool has_id = false, has_kind = false, has_end = false;
	char *save = line;
	while (save) {
		char *tab = strchr(save, '\t');
		if (tab) *tab = 0;
		char *eq = strchr(save, '=');
		if (eq) {
			*eq = 0;
			const char *k = save;
			char *v = eq + 1;
			unescape(v);
			if (!strcmp(k, "id")) { r->id = (uint32_t)strtoul(v, NULL, 10); has_id = true; }
			else if (!strcmp(k, "t")) r->t = (uint32_t)strtoul(v, NULL, 10);
			else if (!strcmp(k, "job")) r->job = (uint32_t)strtoul(v, NULL, 10);
			else if (!strcmp(k, "lba")) r->lba = (uint32_t)strtoul(v, NULL, 10);
			else if (!strcmp(k, "blob")) r->blob = (uint32_t)strtoul(v, NULL, 10);
			else if (!strcmp(k, "src")) { for (int i = 0; i < 5; i++) if (!strcmp(v, SRC_NAME[i])) r->src = (AuditSrc)i; }
			else if (!strcmp(k, "dom")) { for (int i = 0; i < 4; i++) if (!strcmp(v, DOM_NAME[i])) r->dom = (AuditDom)i; }
			else if (!strcmp(k, "disc")) copyn(r->disc, sizeof r->disc, v);
			else if (!strcmp(k, "kind")) { copyn(r->kind, sizeof r->kind, v); has_kind = true; }
			else if (!strcmp(k, "what")) copyn(r->what, sizeof r->what, v);
			else if (!strcmp(k, "before")) copyn(r->before, sizeof r->before, v);
			else if (!strcmp(k, "after")) copyn(r->after, sizeof r->after, v);
			else if (!strcmp(k, "end")) has_end = true;
			else if (!strcmp(k, "file")) copyn(r->file, sizeof r->file, v);
		}
		save = tab ? tab + 1 : NULL;
	}
	return has_id && has_kind && has_end && r->id;   // `end` is the last key: a cut line lacks it
}

// Calls fn for each whole row of the log in order. Returns the offset just past the last whole
// line (a final line without a newline is not whole).
template <class F> static long scan_log(F fn)
{
	char p[256];
	path_log(p, sizeof p);
	FILE *f = fopen(p, "rb");
	if (!f) return 0;
	static char line[LINE_MAX_LEN];   // under the lock, or in init
	long good = 0, pos = 0;
	for (;;) {
		if (!fgets(line, sizeof line, f)) break;
		size_t n = strlen(line);
		bool whole = n && line[n - 1] == '\n';
		if (!whole && n == sizeof line - 1) {   // too long for a row: skip the rest of it
			int c;
			while ((c = fgetc(f)) != EOF && c != '\n') n++;
			whole = c == '\n';
			pos += (long)n + (c == '\n');
			if (whole) good = pos;
			continue;
		}
		pos += (long)n;
		if (!whole) break;   // half-written last line
		good = pos;
		line[n - 1] = 0;
		AuditRow r;
		if (parse_line(line, &r)) fn(r);
	}
	fclose(f);
	return good;
}

// Copies the first `keep` bytes of the log over the log. The original is replaced only when every
// byte was copied and written; on any failure it stays as it is and false is returned.
static bool cut_log(long keep)
{
	char p[256], t[260];
	path_log(p, sizeof p);
	snprintf(t, sizeof t, "%s.tmp", p);
	FILE *in = fopen(p, "rb"), *out = fopen(t, "wb");
	long done = 0;
	if (in && out) {
		static char buf[4096];
		while (done < keep) {
			long want = keep - done < (long)sizeof buf ? keep - done : (long)sizeof buf;
			size_t n = fread(buf, 1, (size_t)want, in);
			if (!n || fwrite(buf, 1, n, out) != n) break;
			done += (long)n;
		}
	}
	if (in) fclose(in);
	bool ok = in && out && done == keep;
	if (out && fclose(out) != 0) ok = false;
	if (ok && (remove(p) != 0 || rename(t, p) != 0)) ok = false;
	if (!ok) remove(t);
	return ok;
}

void audit_init(const char *dir, const AuditIo *io)
{
	if (!g_mx_made) {
		sys_mutex_attr_t ma;
		sysMutexAttrInitialize(ma);
		sysMutexCreate(&g_mx, &ma);
		g_mx_made = 1;
	}
	lock();
	copyn(g_dir, sizeof g_dir, dir);
	if (io) g_io = *io;
	mkdirs(g_dir);
	g_next_id = g_next_job = 1;
	memset(g_slots, 0, sizeof g_slots);
	char p[256];
	path_log(p, sizeof p);
	long size = 0;
	if (FILE *f = fopen(p, "rb")) { fseek(f, 0, SEEK_END); size = ftell(f); fclose(f); }
	long good = scan_log([](const AuditRow &r) {
		if (r.id >= g_next_id) g_next_id = r.id + 1;
		if (r.job >= g_next_job) g_next_job = r.job + 1;
	});
	// Drop a half line so the next row does not glue onto it. If that fails, keep the original and
	// end the half line with a newline; it has no end marker, so it is never read as a row.
	if (good < size && !cut_log(good)) {
		if (FILE *f = fopen(p, "ab")) { fputc('\n', f); fclose(f); }
	}
	unlock();
}

// The slot of a job; filled from the log when the job is not in memory (after a reload).
static JobSlot *slot_of(uint32_t job)
{
	for (JobSlot &s : g_slots)
		if (s.job == job) return &s;
	JobSlot *s = &g_slots[g_slot_rr++ % 8];
	memset(s, 0, sizeof *s);
	s->job = job;
	s->src = AU_APP;
	JobSlot *sp = s;
	scan_log([sp, job](const AuditRow &r) {
		if (r.job != job) return;
		if (!strcmp(r.kind, "job")) { sp->src = r.src; copyn(sp->disc, sizeof sp->disc, r.disc); }
		else if (!strcmp(r.kind, "sector")) sp->blobs = r.blob + 1;
	});
	return s;
}

// Appends one row; the caller holds the lock. Returns the id.
static uint32_t write_row(uint32_t job, AuditSrc src, AuditDom dom, const char *disc, const char *kind,
                          const char *what, const char *before, const char *after,
                          const char *file, bool sector, uint32_t lba, uint32_t blob)
{
	static char line[LINE_MAX_LEN];
	uint32_t id = g_next_id++;
	size_t n = (size_t)snprintf(line, sizeof line, "id=%u\tt=%u\tsrc=%s\tdom=%s\tjob=%u",
	                            (unsigned)id, (unsigned)time(NULL), SRC_NAME[src], DOM_NAME[dom], (unsigned)job);
	struct { const char *k; const char *v; size_t max; } f[] = {
		{"disc", disc, 95}, {"kind", kind, 11}, {"what", what, 159},
		{"before", before, 191}, {"after", after, 191},
	};
	for (auto &x : f) {
		n += (size_t)snprintf(line + n, sizeof line - n, "\t%s=", x.k);
		n = put_esc(line, n, sizeof line - 24, x.v, x.max);
	}
	if (sector) {
		n += (size_t)snprintf(line + n, sizeof line - n, "\tfile=");
		n = put_esc(line, n, sizeof line - 24, file, 255);
		n += (size_t)snprintf(line + n, sizeof line - n, "\tlba=%u\tblob=%u", (unsigned)lba, (unsigned)blob);
	}
	n += (size_t)snprintf(line + n, sizeof line - n, "\tend=1\n");
	char p[256];
	path_log(p, sizeof p);
	if (FILE *fp = fopen(p, "ab")) {
		fwrite(line, 1, n, fp);   // one write of the whole line
		fflush(fp);
		fclose(fp);
	}
	return id;
}

uint32_t audit_job_begin(AuditSrc src, const char *disc, const char *name, const char *what)
{
	lock();
	uint32_t job = g_next_job++;
	JobSlot *s = &g_slots[g_slot_rr++ % 8];
	memset(s, 0, sizeof *s);
	s->job = job;
	s->src = src;
	copyn(s->disc, sizeof s->disc, disc);
	write_row(job, src, AU_RIPS, disc, "job", what, name, "", NULL, false, 0, 0);
	unlock();
	return job;
}

void audit_job_end(uint32_t job, const char *result)
{
	lock();
	JobSlot *s = slot_of(job);
	write_row(job, s->src, AU_RIPS, s->disc, "result", "", "", result, NULL, false, 0, 0);
	unlock();
}

void audit_note(uint32_t job, AuditDom dom, const char *disc, const char *kind,
                const char *what, const char *before, const char *after)
{
	lock();
	AuditSrc src = AU_APP;
	const char *d = disc;
	if (job) {
		JobSlot *s = slot_of(job);
		src = s->src;
		if (!d || !*d) d = s->disc;
	}
	write_row(job, src, dom, d, kind, what, before, after, NULL, false, 0, 0);
	unlock();
}

void audit_sector(uint32_t job, const char *file, uint32_t lba, const uint8_t *old2352, const uint8_t *new2352)
{
	lock();
	JobSlot *s = slot_of(job);
	uint32_t blob = s->blobs;
	char p[256];
	path_blob(p, sizeof p, job);
	FILE *f = fopen(p, "r+b");
	if (!f) f = fopen(p, "w+b");
	if (f) {
		static const uint8_t zero[SECTOR] = {0};
		fseek(f, (long)blob * BLOB_ROW, SEEK_SET);
		fwrite(old2352 ? old2352 : zero, 1, SECTOR, f);
		fwrite(new2352 ? new2352 : zero, 1, SECTOR, f);
		fflush(f);
		fclose(f);
	}
	s->blobs++;
	char what[24];
	snprintf(what, sizeof what, "lba %u", (unsigned)lba);
	write_row(job, s->src, AU_RIPS, s->disc, "sector", what, "", "", file, true, lba, blob);
	unlock();
}

void audit_setting(AuditSrc src, const char *key, const char *before, const char *after)
{
	lock();
	write_row(0, src, AU_SETTINGS, "", "setting", key, before, after, NULL, false, 0, 0);
	unlock();
}

// ---- queries ----

struct DiscAcc { char key[96], name[96]; int events; uint32_t last_t, last_id; };

// an event is a job row or a row outside any job (setting, undo, ...)
static bool is_event(const AuditRow &r) { return !strcmp(r.kind, "job") || r.job == 0; }

int audit_discs(char (*keys)[96], char (*names)[96], int *events, uint32_t *last_t, int max)
{
	std::vector<DiscAcc> v;
	lock();
	scan_log([&v](const AuditRow &r) {
		if (!r.disc[0]) return;
		DiscAcc *a = NULL;
		for (DiscAcc &x : v) if (!strcmp(x.key, r.disc)) { a = &x; break; }
		if (!a) {
			v.push_back(DiscAcc());
			a = &v.back();
			memset(a, 0, sizeof *a);
			copyn(a->key, sizeof a->key, r.disc);
		}
		if (is_event(r)) a->events++;
		if (!strcmp(r.kind, "job") && r.before[0]) copyn(a->name, sizeof a->name, r.before);   // newest wins
		a->last_t = r.t;
		a->last_id = r.id;
	});
	unlock();
	std::stable_sort(v.begin(), v.end(), [](const DiscAcc &a, const DiscAcc &b) {
		return a.last_t != b.last_t ? a.last_t > b.last_t : a.last_id > b.last_id;
	});
	int n = 0;
	for (; n < max && n < (int)v.size(); n++) {
		if (keys) copyn(keys[n], 96, v[n].key);
		if (names) copyn(names[n], 96, v[n].name[0] ? v[n].name : v[n].key);
		if (events) events[n] = v[n].events;
		if (last_t) last_t[n] = v[n].last_t;
	}
	return n;
}

int audit_events(AuditDom dom, const char *disc, AuditRow *out, int max)
{
	if (max <= 0) return 0;
	// keep the newest `max` matches in a ring, then hand them out newest first
	std::vector<AuditRow> ring(max);
	int total = 0;
	lock();
	scan_log([&](const AuditRow &r) {
		if (r.dom != dom || !is_event(r)) return;
		if (disc && *disc && strcmp(disc, r.disc)) return;
		ring[total % max] = r;
		total++;
	});
	unlock();
	int n = total < max ? total : max;
	for (int i = 0; i < n; i++) out[i] = ring[(total - 1 - i) % max];
	return n;
}

int audit_rows(uint32_t job, AuditRow *out, int first, int max)
{
	int n = 0, seen = 0;
	lock();
	scan_log([&](const AuditRow &r) {
		if (r.job != job || n >= max) return;
		if (seen++ < first) return;
		out[n++] = r;
	});
	unlock();
	return n;
}
