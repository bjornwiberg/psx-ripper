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
#include <dirent.h>
#include <vector>
#include <string>
#include <map>
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
// the last append failed: it may have left a fragment without a newline, which the next row ends
static bool g_append_failed;

// what the running jobs need on every row: where they came from and how many sectors they have
struct JobSlot { uint32_t job; AuditSrc src; char disc[96]; uint32_t blobs; char file[256]; };
static JobSlot g_slots[8];
static int g_slot_rr;

// jobs begun and not ended in this run: their job<N>.bin is never pruned and their rows not undone
static uint32_t g_running[16];
// how each ended job ended (its result row), and which jobs are rips: a rip's sectors can only be
// undone when it finished
static std::map<uint32_t, std::string> g_results;
static std::map<uint32_t, bool> g_rips;
// image file -> the newest finished rip that made it: older jobs' rows on it name a replaced image
static std::map<std::string, uint32_t> g_made;
static uint64_t g_cap = 256ull << 20, g_blob_total;   // the audit_mb setting; all job<N>.bin together
static int (*g_set_setting)(const char *key, const char *value);
static void (*g_recheck)(const char *file, const uint32_t *lbas, const uint8_t *zero, int n, int redo);
static int (*g_tick)(uint32_t done, uint32_t total);

static void lock() { sysMutexLock(g_mx, 0); }
static void unlock() { sysMutexUnlock(g_mx); }

static bool is_running(uint32_t job)
{
	for (uint32_t j : g_running) if (j && j == job) return true;
	return false;
}

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
			else if (!strcmp(k, "rev")) r->norev = !strcmp(v, "0");
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
	if (!ok) { remove(t); return false; }
	if (rename(t, p) == 0) return true;   // replaces the log where the file system allows it
	if (remove(p) != 0) { remove(t); return false; }
	// The log is gone now and the tmp copy holds every row: never delete it. audit_init takes it
	// back when it finds no log next to it.
	return rename(t, p) == 0;
}

// Sums the job<N>.bin files and, while they exceed the cap, deletes the oldest whose job is not
// running. Caller holds the lock.
static void prune()
{
	struct Blob { uint32_t job; uint64_t size; };
	std::vector<Blob> v;
	uint64_t total = 0;
	if (DIR *d = opendir(g_dir)) {
		while (struct dirent *de = readdir(d)) {
			const char *nm = de->d_name;
			char *end;
			if (strncmp(nm, "job", 3)) continue;
			unsigned long job = strtoul(nm + 3, &end, 10);
			if (end == nm + 3 || strcmp(end, ".bin")) continue;
			char p[256];
			struct stat st;
			path_blob(p, sizeof p, (uint32_t)job);
			if (stat(p, &st) != 0) continue;
			Blob b = {(uint32_t)job, (uint64_t)st.st_size};
			v.push_back(b);
			total += b.size;
		}
		closedir(d);
	}
	std::sort(v.begin(), v.end(), [](const Blob &a, const Blob &b) { return a.job < b.job; });
	for (const Blob &b : v) {
		if (total <= g_cap) break;
		if (is_running(b.job)) continue;
		char p[256];
		path_blob(p, sizeof p, b.job);
		if (remove(p) == 0) total -= b.size;
	}
	g_blob_total = total;
}

// A rip ended: when it finished, it made `image` (renamed over any earlier one). Caller holds the lock.
static void note_made(uint32_t job, const char *result, const char *image)
{
	if (!image || !*image || !g_rips.count(job) || strcmp(result, "finished")) return;
	uint32_t &k = g_made[image];
	if (job > k) k = job;
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
	g_append_failed = false;   // a half line at the end is dropped or ended below
	memset(g_slots, 0, sizeof g_slots);
	memset(g_running, 0, sizeof g_running);
	g_results.clear();
	g_rips.clear();
	g_made.clear();
	char p[256], t[260];
	path_log(p, sizeof p);
	snprintf(t, sizeof t, "%s.tmp", p);
	struct stat st;
	if (stat(p, &st) != 0 && stat(t, &st) == 0) rename(t, p);   // a cut that lost its rename
	long size = 0;
	if (FILE *f = fopen(p, "rb")) { fseek(f, 0, SEEK_END); size = ftell(f); fclose(f); }
	long good = scan_log([](const AuditRow &r) {
		if (r.id >= g_next_id) g_next_id = r.id + 1;
		if (r.job >= g_next_job) g_next_job = r.job + 1;
		if (!strcmp(r.kind, "job") && !strcmp(r.what, "rip")) g_rips[r.job] = true;
		if (!strcmp(r.kind, "result")) { g_results[r.job] = r.after; note_made(r.job, r.after, r.file); }
	});
	// Drop a half line so the next row does not glue onto it. If that fails, keep the original and
	// end the half line with a newline; it has no end marker, so it is never read as a row.
	// "r+b" does not create the log: after a failed rename the rows are in the tmp copy only.
	if (good < size && !cut_log(good)) {
		if (FILE *f = fopen(p, "r+b")) { fseek(f, 0, SEEK_END); fputc('\n', f); fclose(f); }
	}
	prune();
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
		else if (!strcmp(r.kind, "sector")) { sp->blobs = r.blob + 1; copyn(sp->file, sizeof sp->file, r.file); }
	});
	return s;
}

// Appends one row; the caller holds the lock. Returns the id, 0 when the row could not be written.
static uint32_t write_row(uint32_t job, AuditSrc src, AuditDom dom, const char *disc, const char *kind,
                          const char *what, const char *before, const char *after,
                          const char *file, bool sector, uint32_t lba, uint32_t blob, bool norev = false)
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
	if (sector || file) {   // a sector row's image, or the image a rip made (its result row)
		n += (size_t)snprintf(line + n, sizeof line - n, "\tfile=");
		n = put_esc(line, n, sizeof line - 24, file, 255);
	}
	if (sector) n += (size_t)snprintf(line + n, sizeof line - n, "\tlba=%u\tblob=%u", (unsigned)lba, (unsigned)blob);
	if (norev) n += (size_t)snprintf(line + n, sizeof line - n, "\trev=0");
	n += (size_t)snprintf(line + n, sizeof line - n, "\tend=1\n");
	char p[256];
	path_log(p, sizeof p);
	bool ok = false;
	if (FILE *fp = fopen(p, "ab")) {
		// a fragment of the last row (disk full) would glue onto this one and lend it its id; an
		// empty line is no row
		ok = !g_append_failed || fputc('\n', fp) == '\n';
		if (fwrite(line, 1, n, fp) != n) ok = false;   // one write of the whole line
		if (fflush(fp) != 0) ok = false;
		if (fclose(fp) != 0) ok = false;
	}
	g_append_failed = !ok;
	return ok ? id : 0;
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
	for (uint32_t &r : g_running) if (!r) { r = job; break; }
	if (what && !strcmp(what, "rip")) g_rips[job] = true;
	write_row(job, src, AU_RIPS, disc, "job", what, name, "", NULL, false, 0, 0);
	unlock();
	return job;
}

void audit_job_end(uint32_t job, const char *result, const char *image)
{
	lock();
	JobSlot *s = slot_of(job);
	for (uint32_t &r : g_running) if (r == job) r = 0;
	g_results[job] = result ? result : "";
	const char *img = image && *image && g_rips.count(job) ? image : NULL;
	write_row(job, s->src, AU_RIPS, s->disc, "result", "", "", result, img, false, 0, 0);
	note_made(job, g_results[job].c_str(), img);
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
		g_blob_total += BLOB_ROW;
	}
	s->blobs++;
	copyn(s->file, sizeof s->file, file);
	char what[24];
	snprintf(what, sizeof what, "lba %u", (unsigned)lba);
	write_row(job, s->src, AU_RIPS, s->disc, "sector", what, "", "", file, true, lba, blob);
	if (g_blob_total > g_cap) prune();
	unlock();
}

void audit_setting(AuditSrc src, const char *key, const char *before, const char *after)
{
	lock();
	write_row(0, src, AU_SETTINGS, "", "setting", key, before, after, NULL, false, 0, 0);
	unlock();
}

void audit_setting_private(AuditSrc src, const char *key, const char *before, const char *after)
{
	lock();
	write_row(0, src, AU_SETTINGS, "", "setting", key, before, after, NULL, false, 0, 0, true);
	unlock();
}

// ---- undo state ----

// What the log says about undo: which rows were undone, and the value each setting has now.
// Undo rows form chains: an undo row names the row it undid, which may be an undo row itself (a
// redo). Every chain starts at its root (a sector or setting row); each undo in it flips the root,
// so the root is undone after an odd number of them, and each undo row but the newest is undone.
struct UndoState {
	struct Chain { uint32_t tip; int k; };    // its newest undo row, how many undos so far
	struct Link { uint32_t root; int k; };     // an undo row: its chain's root, its place (1 = first)
	struct Setting { std::string key, before, after; bool norev; };
	std::vector<uint32_t> undone;   // sorted
	std::map<std::string, std::string> cur;
	std::map<uint32_t, Chain> chains;     // by root
	std::map<uint32_t, Link> links;       // by undo row
	std::map<uint32_t, Setting> sets;     // every setting row
	std::map<uint32_t, AuditRow> roots;   // sector roots fetched for the rows at hand (fetch_roots)
	bool is_undone(uint32_t id) const { return std::binary_search(undone.begin(), undone.end(), id); }
	// the chain an undo row is in, or NULL
	const Link *link(uint32_t id) const { auto it = links.find(id); return it == links.end() ? NULL : &it->second; }
	// the newest undo row of a root's chain, 0 when none
	uint32_t tip(uint32_t root) const { auto it = chains.find(root); return it == chains.end() ? 0 : it->second.tip; }
};

// Caller holds the lock.
static void build_state(UndoState &s)
{
	std::vector<uint32_t> superseded;
	scan_log([&](const AuditRow &r) {
		if (!strcmp(r.kind, "setting")) {
			s.cur[r.what] = r.after;
			UndoState::Setting &x = s.sets[r.id];
			x.key = r.what; x.before = r.before; x.after = r.after; x.norev = r.norev != 0;
		} else if (!strcmp(r.kind, "undo")) {
			uint32_t id = (uint32_t)strtoul(r.what, NULL, 10);
			if (!id) return;
			auto l = s.links.find(id);
			uint32_t root = l == s.links.end() ? id : l->second.root;
			UndoState::Chain &c = s.chains[root];
			if (c.tip) superseded.push_back(c.tip);   // the undo before is reversed by this one
			if (id != root) superseded.push_back(id);
			c.tip = r.id;
			c.k++;
			UndoState::Link &k = s.links[r.id];
			k.root = root; k.k = c.k;
			auto it = s.sets.find(root);
			if (it != s.sets.end()) s.cur[it->second.key] = c.k % 2 ? it->second.before : it->second.after;
		}
	});
	for (auto &c : s.chains) if (c.second.k % 2) s.undone.push_back(c.first);
	s.undone.insert(s.undone.end(), superseded.begin(), superseded.end());
	std::sort(s.undone.begin(), s.undone.end());
	s.undone.erase(std::unique(s.undone.begin(), s.undone.end()), s.undone.end());
}

// Reads the sector rows the undo rows among `rows` go back to into s.roots (one pass over the log,
// only when one is missing). Caller holds the lock.
static void fetch_roots(UndoState &s, const AuditRow *rows, int n)
{
	std::vector<uint32_t> want;
	for (int i = 0; i < n; i++) {
		if (strcmp(rows[i].kind, "undo")) continue;
		const UndoState::Link *l = s.link(rows[i].id);
		if (l && !s.sets.count(l->root) && !s.roots.count(l->root)) want.push_back(l->root);
	}
	if (want.empty()) return;
	std::sort(want.begin(), want.end());
	scan_log([&](const AuditRow &r) {
		if (std::binary_search(want.begin(), want.end(), r.id)) s.roots[r.id] = r;
	});
}

static const char *base_name(const char *path)
{
	const char *b = strrchr(path, '/');
	return b ? b + 1 : path;
}

static uint8_t g_blob_buf[BLOB_ROW], g_sec_buf[SECTOR];   // under the lock

// Fills rs (may be NULL) with code c about row r; job, lba, file and what as the row has them.
static void reason(AuditReason *rs, AuditWhy c, const AuditRow &r, uint32_t job = 0)
{
	if (!rs) return;
	memset(rs, 0, sizeof *rs);
	rs->code = c;
	rs->row = r.id;
	rs->job = job ? job : r.job;
	rs->lba = r.lba;
	copyn(rs->file, sizeof rs->file, base_name(r.file));
	copyn(rs->what, sizeof rs->what, r.what);
}

// Why a sector row cannot be undone now (redo: applied again), or NULL; then g_blob_buf holds its
// old and new bytes and *restored says the image already has the bytes the undo would write (an undo
// whose log row was lost: only the row is missing). r.id is the row named in the reason. Caller
// holds the lock.
static const char *sector_block(const AuditRow &r, char *msg, size_t n, bool *restored, AuditReason *rs = NULL, bool redo = false)
{
	*restored = false;
	if (is_running(r.job)) { reason(rs, AW_RUNNING, r); snprintf(msg, n, "job %u is still running", (unsigned)r.job); return msg; }
	// another job writing the same rip reads and writes sectors outside the lock: wait for it
	for (uint32_t j : g_running) {
		if (!j) continue;
		JobSlot *s = slot_of(j);
		if ((r.disc[0] && !strcmp(s->disc, r.disc)) || (r.file[0] && !strcmp(s->file, r.file))) {
			reason(rs, AW_JOB_ON_DISC, r, j);
			snprintf(msg, n, "a job on this disc is running (job %u)", (unsigned)j);
			return msg;
		}
	}
	// A rip that did not finish wrote into a .part that is gone; its rows name the image an earlier
	// rip left under that name, which must not get this job's first reads. No result row and not
	// running: the app stopped in the middle, the same. A repair or check writes into the image
	// itself: its rows stay reversible whatever its result.
	auto res = g_results.find(r.job);
	if (g_rips.count(r.job) && (res == g_results.end() || res->second != "finished")) {
		reason(rs, AW_UNFINISHED, r);
		if (rs) copyn(rs->what, sizeof rs->what, res == g_results.end() ? "" : res->second.c_str());
		snprintf(msg, n, "the rip was not finished (%s)", res == g_results.end() ? "no result" : res->second.c_str());
		return msg;
	}
	// A newer rip replaced the file: the sector may hold the row's new bytes again, but the old ones
	// belong to the image that is gone.
	auto made = g_made.find(r.file);
	if (made != g_made.end() && made->second > r.job) {
		reason(rs, AW_REPLACED, r, made->second);
		snprintf(msg, n, "%s was ripped again since (job %u)", base_name(r.file), (unsigned)made->second);
		return msg;
	}
	char p[256];
	path_blob(p, sizeof p, r.job);
	FILE *f = fopen(p, "rb");
	bool kept = f && fseek(f, (long)r.blob * BLOB_ROW, SEEK_SET) == 0 && fread(g_blob_buf, 1, BLOB_ROW, f) == BLOB_ROW;
	if (f) fclose(f);
	if (!kept) { reason(rs, AW_DROPPED, r); snprintf(msg, n, "the sector's bytes are no longer kept (size limit)"); return msg; }
	void *img = g_io.open ? g_io.open(r.file, "rb") : NULL;
	if (!img) { reason(rs, AW_CANNOT_OPEN, r); snprintf(msg, n, "cannot open %s", base_name(r.file)); return msg; }
	bool got = g_io.seek(img, (uint64_t)r.lba * SECTOR) == 0 && g_io.read(img, g_sec_buf, SECTOR) == SECTOR;
	const uint8_t *now = redo ? g_blob_buf : g_blob_buf + SECTOR, *then = redo ? g_blob_buf + SECTOR : g_blob_buf;
	bool same = got && !memcmp(g_sec_buf, now, SECTOR);
	if (got && !same && !memcmp(g_sec_buf, then, SECTOR)) *restored = true, same = true;
	g_io.close(img);
	if (!same) { reason(rs, AW_SECTOR_CHANGED, r); snprintf(msg, n, "sector %u of %s changed since", (unsigned)r.lba, base_name(r.file)); return msg; }
	reason(rs, AW_NONE, r);
	return NULL;
}

// The setting row an undo row's chain goes back to, or NULL.
static const UndoState::Setting *chain_setting(const UndoState &s, const UndoState::Link *l)
{
	if (!l) return NULL;
	auto it = s.sets.find(l->root);
	return it == s.sets.end() ? NULL : &it->second;
}

// Why the setting `key` cannot be set from `now` (the value it must still have) now, or NULL.
static const char *setting_block(const AuditRow &r, const UndoState &s, const char *key, const std::string &now,
                                 char *msg, size_t n, AuditReason *rs)
{
	auto it = s.cur.find(key);
	if (it == s.cur.end() || it->second != now) {
		reason(rs, AW_SETTING_CHANGED, r);
		if (rs) copyn(rs->what, sizeof rs->what, key);
		snprintf(msg, n, "setting %.100s changed since", key);
		return msg;
	}
	reason(rs, AW_NONE, r);
	return NULL;
}

// The sector row an undo row's chain goes back to, as the row to check (its id the undo row's),
// or false. s.roots must hold it (fetch_roots).
static bool chain_sector(const AuditRow &u, const UndoState &s, AuditRow &out)
{
	const UndoState::Link *l = s.link(u.id);
	if (!l) return false;
	auto it = s.roots.find(l->root);
	if (it == s.roots.end() || strcmp(it->second.kind, "sector")) return false;
	out = it->second;
	out.id = u.id;
	return true;
}

// Why a row cannot be undone now, or NULL. Caller holds the lock.
static const char *row_block(const AuditRow &r, const UndoState &s, char *msg, size_t n, AuditReason *rs = NULL)
{
	if (s.is_undone(r.id)) { reason(rs, AW_ALREADY, r); snprintf(msg, n, "already undone"); return msg; }
	if (!strcmp(r.kind, "sector")) { bool restored; return sector_block(r, msg, n, &restored, rs); }
	if (!strcmp(r.kind, "setting")) {
		if (r.norev) { reason(rs, AW_SECRET, r); snprintf(msg, n, "the value is not kept in the log"); return msg; }
		return setting_block(r, s, r.what, r.after, msg, n, rs);
	}
	if (!strcmp(r.kind, "undo")) {   // the newest undo of its chain: undoing it flips the root again
		const UndoState::Link *l = s.link(r.id);
		bool redo = l && l->k % 2;
		if (const UndoState::Setting *x = chain_setting(s, l)) {
			if (x->norev) { reason(rs, AW_SECRET, r); snprintf(msg, n, "the value is not kept in the log"); return msg; }
			return setting_block(r, s, x->key.c_str(), redo ? x->before : x->after, msg, n, rs);
		}
		AuditRow sec;
		if (chain_sector(r, s, sec)) { bool restored; return sector_block(sec, msg, n, &restored, rs, redo); }
	}
	reason(rs, AW_RECORD, r);
	snprintf(msg, n, "a %s row cannot be undone", r.kind);
	return msg;
}

// Fills the computed fields of a row for the queries; s.roots must hold the sector rows of undo
// rows (fetch_roots). Caller holds the lock.
static void mark(AuditRow &r, const UndoState &s)
{
	char msg[96];
	AuditReason rs;
	r.undone = s.is_undone(r.id);
	r.reversible = !row_block(r, s, msg, sizeof msg, &rs);
	r.why = rs.code;
	r.why_job = rs.code == AW_RUNNING || rs.code == AW_JOB_ON_DISC || rs.code == AW_UNFINISHED || rs.code == AW_DROPPED ? rs.job : 0;
	r.redo = 0;
	if (strcmp(r.kind, "undo")) return;
	const UndoState::Link *l = s.link(r.id);
	r.redo = l && !r.undone && l->k % 2;
	AuditRow sec;
	if (const UndoState::Setting *x = chain_setting(s, l)) copyn(r.key, sizeof r.key, x->key.c_str());
	else if (chain_sector(r, s, sec)) { r.lba = sec.lba; copyn(r.file, sizeof r.file, sec.file); }
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

int audit_setting_values(char (*keys)[48], char (*vals)[192], int max)
{
	UndoState st;
	lock();
	build_state(st);
	unlock();
	int n = 0;
	for (auto it = st.cur.begin(); it != st.cur.end() && n < max; ++it, n++) {
		copyn(keys[n], 48, it->first.c_str());
		copyn(vals[n], 192, it->second.c_str());
	}
	return n;
}

int audit_events(AuditDom dom, const char *disc, AuditRow *out, int max)
{
	if (max <= 0) return 0;
	// keep the newest `max` matches in a ring, then hand them out newest first
	std::vector<AuditRow> ring(max);
	int total = 0;
	UndoState st;
	lock();
	build_state(st);
	scan_log([&](const AuditRow &r) {
		if (r.dom != dom || !is_event(r)) return;
		if (disc && *disc && strcmp(disc, r.disc)) return;
		ring[total % max] = r;
		total++;
	});
	int n = total < max ? total : max;
	for (int i = 0; i < n; i++) out[i] = ring[(total - 1 - i) % max];
	fetch_roots(st, out, n);
	for (int i = 0; i < n; i++) mark(out[i], st);
	unlock();
	return n;
}

int audit_rows(uint32_t job, AuditRow *out, int first, int max)
{
	int n = 0, seen = 0;
	UndoState st;
	lock();
	build_state(st);
	scan_log([&](const AuditRow &r) {
		if (r.job != job || n >= max) return;
		if (seen++ < first) return;
		out[n++] = r;
	});
	fetch_roots(st, out, n);
	for (int i = 0; i < n; i++) mark(out[i], st);   // reads the image for sector rows: one page only
	unlock();
	return n;
}

// ---- undo ----

static void say(char *why, size_t n, const char *fmt, uint32_t id, const char *msg)
{
	if (why && n) snprintf(why, n, fmt, (unsigned)id, msg);
}

// A sector row to undo, kept small (a job can have thousands): file and disc index into lists.
// id is the row the undo row names: the sector row, or for a redo the newest undo row of its chain.
struct Target { uint32_t id, job, lba, blob; int file, disc; };

static int index_of(std::vector<std::string> &v, const char *s)
{
	for (size_t i = 0; i < v.size(); i++) if (v[i] == s) return (int)i;
	v.push_back(s);
	return (int)v.size() - 1;
}

// The sector rows `want` picks that are not undone yet, newest first. When there are none but some
// are undone, those instead, oldest first, each named by its chain's newest undo row: the undo is
// then a redo (*redo = true).
template <class F> static void collect(F want, std::vector<Target> &ts, std::vector<std::string> &files,
                                       std::vector<std::string> &discs, bool *redo)
{
	std::vector<Target> back;
	UndoState st;
	lock();
	build_state(st);
	scan_log([&](const AuditRow &r) {
		if (strcmp(r.kind, "sector") || !want(r)) return;
		bool u = st.is_undone(r.id);
		Target t = {u ? st.tip(r.id) : r.id, r.job, r.lba, r.blob, index_of(files, r.file), index_of(discs, r.disc)};
		(u ? back : ts).push_back(t);
	});
	unlock();
	*redo = ts.empty() && !back.empty();
	if (*redo) ts.swap(back);
	else std::reverse(ts.begin(), ts.end());
}

// The row a target stands for (enough for sector_block and the undo row).
static void target_row(const Target &t, const std::vector<std::string> &files, const std::vector<std::string> &discs, AuditRow &r)
{
	memset(&r, 0, sizeof r);
	r.id = t.id; r.job = t.job; r.lba = t.lba; r.blob = t.blob; r.dom = AU_RIPS;
	copyn(r.kind, sizeof r.kind, "sector");
	copyn(r.file, sizeof r.file, files[t.file].c_str());
	copyn(r.disc, sizeof r.disc, discs[t.disc].c_str());
}

// Writes each sector's old bytes back (redo: its new bytes again) in the targets' order and stops
// at the first refusal. Then every file that got a sector is checked again (outside the lock: the
// check writes rows).
static int undo_sectors(const std::vector<Target> &ts, const std::vector<std::string> &files,
                        const std::vector<std::string> &discs, bool redo, char *why, size_t n, AuditReason *rs)
{
	std::vector<std::vector<uint32_t> > lbas(files.size());   // per file: the sectors written
	std::vector<std::vector<uint8_t> > zero(files.size());   // and whether their old bytes are all zero
	int rc = 0;
	char msg[160];
	int (*tick)(uint32_t, uint32_t) = ts.size() > AUDIT_TICK ? g_tick : NULL;   // a few rows need no wait
	for (size_t i = 0; i < ts.size(); i++) {
		const Target &t = ts[i];
		AuditRow r;
		target_row(t, files, discs, r);
		if (tick && i % AUDIT_TICK == 0 && tick((uint32_t)i, (uint32_t)ts.size())) {
			reason(rs, AW_STOPPED, r);
			say(why, n, "row %u: %s", t.id, "the undo was stopped");
			rc = -1;
			break;
		}
		lock();
		bool restored;
		const char *e = sector_block(r, msg, sizeof msg, &restored, rs, redo);
		bool written = false;
		bool zeros = true;   // the old bytes, still in g_blob_buf
		if (!e) for (int i = 0; i < SECTOR && zeros; i++) zeros = !g_blob_buf[i];
		if (!e && !restored) {
			// A failed close may still have written the sector; the next undo then finds the bytes
			// it would write and only adds the row.
			void *img = g_io.open(r.file, "r+b");
			bool ok = img && g_io.seek(img, (uint64_t)r.lba * SECTOR) == 0 && g_io.write(img, redo ? g_blob_buf + SECTOR : g_blob_buf, SECTOR) == SECTOR;
			if (img && g_io.close(img) != 0) ok = false;
			if (!ok) { reason(rs, AW_CANNOT_WRITE, r); snprintf(msg, sizeof msg, "cannot write %s", base_name(r.file)); e = msg; }
			written = ok;
		}
		if (!e) {
			char id[16];
			snprintf(id, sizeof id, "%u", (unsigned)r.id);
			if (!write_row(r.job, AU_UNDO, r.dom, r.disc, "undo", id, "", "", NULL, false, 0, 0)) {
				reason(rs, AW_LOG_WRITE, r);
				snprintf(msg, sizeof msg, "undone, but the log row could not be written");
				e = msg;
			}
		}
		unlock();
		if (written || restored) { lbas[t.file].push_back(r.lba); zero[t.file].push_back(zeros ? 1 : 0); }
		if (e) { say(why, n, "row %u: %s", t.id, e); rc = -1; break; }
	}
	if (!rc && rs) memset(rs, 0, sizeof *rs);
	void (*recheck)(const char *, const uint32_t *, const uint8_t *, int, int) = g_recheck;
	if (recheck)
		for (size_t i = 0; i < files.size(); i++)
			if (!lbas[i].empty()) recheck(files[i].c_str(), lbas[i].data(), zero[i].data(), (int)lbas[i].size(), redo ? 1 : 0);
	return rc;
}

// Sets a setting back to the row's `before` through the app's hook (outside the lock: the app
// records the change itself). An undo row of a setting: the value its undo replaced (a redo) or
// set again, the same way.
static int undo_setting_row(const AuditRow &r, char *why, size_t n, AuditReason *rs)
{
	int (*set)(const char *, const char *) = g_set_setting;
	if (!set) { reason(rs, AW_NO_HOOK, r); say(why, n, "row %u: %s", r.id, "settings cannot be changed here"); return -1; }
	char msg[160];
	UndoState st;
	lock();
	build_state(st);
	const char *e = row_block(r, st, msg, sizeof msg, rs);
	std::string key = r.what, from = r.after, to = r.before;
	if (!strcmp(r.kind, "undo")) {
		const UndoState::Link *l = st.link(r.id);
		const UndoState::Setting *x = chain_setting(st, l);
		if (x) {
			bool redo = l->k % 2;
			key = x->key;
			from = redo ? x->before : x->after;
			to = redo ? x->after : x->before;
		}
	}
	unlock();
	if (e) { say(why, n, "row %u: %s", r.id, e); return -1; }
	if (set(key.c_str(), to.c_str()) != 0) {
		reason(rs, AW_NOT_APPLIED, r);
		if (rs) copyn(rs->what, sizeof rs->what, key.c_str());
		say(why, n, "row %u: setting %s could not be applied", r.id, key.c_str());
		return -1;
	}
	char id[16];
	snprintf(id, sizeof id, "%u", (unsigned)r.id);
	lock();
	uint32_t ok = write_row(0, AU_UNDO, AU_SETTINGS, "", "undo", id, from.c_str(), to.c_str(), NULL, false, 0, 0);
	unlock();
	if (!ok) { reason(rs, AW_LOG_WRITE, r); say(why, n, "row %u: %s", r.id, "undone, but the log row could not be written"); return -1; }
	if (rs) memset(rs, 0, sizeof *rs);
	return 0;
}

int audit_undo_row(uint32_t id, char *why, size_t n, AuditReason *rs)
{
	AuditRow row;
	memset(&row, 0, sizeof row);
	bool found = false, undone = false, setting = false;
	if (why && n) why[0] = 0;
	if (rs) memset(rs, 0, sizeof *rs);
	UndoState st;
	AuditRow sec;   // an undo row of a sector: that sector row
	bool has_sec = false, redo = false;
	lock();
	build_state(st);
	scan_log([&](const AuditRow &r) { if (r.id == id) { row = r; found = true; } });
	undone = found && st.is_undone(id);
	if (found && !undone && !strcmp(row.kind, "undo")) {
		const UndoState::Link *l = st.link(id);
		setting = chain_setting(st, l) != NULL;
		redo = l && l->k % 2;
		if (!setting) {
			fetch_roots(st, &row, 1);
			has_sec = chain_sector(row, st, sec);
		}
	}
	unlock();
	row.id = id;
	if (!found) { reason(rs, AW_NO_ROW, row); say(why, n, "row %u: %s", id, "no such row"); return -1; }
	if (undone) { reason(rs, AW_ALREADY, row); say(why, n, "row %u: %s", id, "already undone"); return -1; }
	if (!strcmp(row.kind, "setting") || setting) return undo_setting_row(row, why, n, rs);
	const AuditRow &r = has_sec ? sec : row;
	if (strcmp(r.kind, "sector")) { reason(rs, AW_RECORD, row); say(why, n, "row %u: a %s row cannot be undone", id, row.kind); return -1; }
	std::vector<Target> ts;
	std::vector<std::string> files, discs;
	Target t = {id, r.job, r.lba, r.blob, index_of(files, r.file), index_of(discs, r.disc)};
	ts.push_back(t);
	return undo_sectors(ts, files, discs, redo, why, n, rs);
}

int audit_undo_job(uint32_t job, char *why, size_t n, AuditReason *rs)
{
	if (why && n) why[0] = 0;
	if (rs) memset(rs, 0, sizeof *rs);
	std::vector<Target> ts;
	std::vector<std::string> files, discs;
	bool redo;
	collect([job](const AuditRow &r) { return r.job == job; }, ts, files, discs, &redo);
	return undo_sectors(ts, files, discs, redo, why, n, rs);
}

int audit_undo_disc_since(const char *disc, uint32_t since_t, char *why, size_t n, AuditReason *rs)
{
	if (why && n) why[0] = 0;
	if (rs) { memset(rs, 0, sizeof *rs); }
	if (!disc || !*disc) { if (rs) rs->code = AW_NO_DISC; if (why && n) snprintf(why, n, "no disc given"); return -1; }
	std::vector<Target> ts;
	std::vector<std::string> files, discs;
	bool redo;
	collect([disc, since_t](const AuditRow &r) { return r.t >= since_t && !strcmp(r.disc, disc); }, ts, files, discs, &redo);
	return undo_sectors(ts, files, discs, redo, why, n, rs);
}

// Would an undo of these targets undo (or redo) at least one row? It goes in the targets' order and
// stops at the first refusal, so that is the first one's verdict.
static int targets_reversible(const std::vector<Target> &ts, const std::vector<std::string> &files,
                              const std::vector<std::string> &discs, bool redo, AuditReason *rs, int *redo_out)
{
	if (rs) { memset(rs, 0, sizeof *rs); rs->code = AW_NOTHING; }
	if (redo_out) *redo_out = redo ? 1 : 0;
	if (ts.empty()) return 0;
	AuditRow r;
	target_row(ts[0], files, discs, r);
	char msg[160];
	bool restored;
	lock();
	const char *e = sector_block(r, msg, sizeof msg, &restored, rs, redo);
	unlock();
	return e ? 0 : 1;
}

int audit_job_reversible(uint32_t job, AuditReason *rs, int *redo)
{
	std::vector<Target> ts;
	std::vector<std::string> files, discs;
	bool rd;
	collect([job](const AuditRow &r) { return r.job == job; }, ts, files, discs, &rd);
	return targets_reversible(ts, files, discs, rd, rs, redo);
}

int audit_disc_reversible(const char *disc, AuditReason *rs, int *redo)
{
	if (redo) *redo = 0;
	if (!disc || !*disc) { if (rs) { memset(rs, 0, sizeof *rs); rs->code = AW_NO_DISC; } return 0; }
	std::vector<Target> ts;
	std::vector<std::string> files, discs;
	bool rd;
	collect([disc](const AuditRow &r) { return !strcmp(r.disc, disc); }, ts, files, discs, &rd);
	return targets_reversible(ts, files, discs, rd, rs, redo);
}

void audit_set_hooks(int (*set_setting)(const char *key, const char *value),
                     void (*recheck)(const char *file, const uint32_t *lbas, const uint8_t *zero, int n, int redo))
{
	g_set_setting = set_setting;
	g_recheck = recheck;
}

void audit_set_progress(int (*tick)(uint32_t done, uint32_t total))
{
	g_tick = tick;
}

void audit_set_cap(uint64_t bytes)
{
	if (!g_mx_made) { g_cap = bytes; return; }   // before audit_init: pruned there
	lock();
	g_cap = bytes;
	prune();
	unlock();
}
