// Game library, see include/library.h. A folder's names are read once, sorted, and turned into
// sets: each .cue first (its FILE lines decide which images are its own), then the images without
// a cue, then what is left of a set whose image is gone (a log, an .md5, a .part). The log is read
// for the rest. The app's own log and .md5 are named after their image (<base>.bin.log, .iso.md5):
// they go to the set of that image; an old <base>.log / .md5 to the first set of the base, as
// before. No static state: the app runs the scan in a background thread.
#include "library.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <strings.h>
#include <vector>
#include <string>
#include <algorithm>

namespace {

enum Kind { K_NONE, K_BIN, K_CUE, K_LOG, K_MD5, K_JPG, K_ISO, K_IMG, K_SBI, K_PART, K_OWN_LOG, K_OWN_MD5 };

// own: for K_OWN_LOG / K_OWN_MD5 the image kind in its name (K_BIN, K_ISO, K_IMG)
struct Name { std::string s; Kind k; size_t blen; int owner; Kind own; };

struct Extra {   // what the scan knows about a set beyond GameDisc
	int cue = 0, cue_files = 0, cue_missing = 0, cue_unread = 0, has2048 = 0, has2352 = 0, part = 0, image_kind = 0, image_bad = 0;
	std::vector<std::string> images;   // all of them, also when the file names did not fit
};

const int LOG_MAX = 65536, CUE_MAX = 65536;

int lc(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

int ends_ci(const std::string &s, const char *ext)
{
	size_t n = strlen(ext);
	if (s.size() <= n) return 0;
	for (size_t i = 0; i < n; i++) if (lc((unsigned char)s[s.size() - n + i]) != ext[i]) return 0;
	return 1;
}

Kind kind_of(const std::string &s, size_t *blen, Kind *own)
{
	static const struct { const char *ext; Kind k, own; } ex[] = {
		{".bin.part", K_PART, K_NONE},
		{".bin.log", K_OWN_LOG, K_BIN}, {".bin.md5", K_OWN_MD5, K_BIN}, {".iso.log", K_OWN_LOG, K_ISO},
		{".iso.md5", K_OWN_MD5, K_ISO}, {".img.log", K_OWN_LOG, K_IMG}, {".img.md5", K_OWN_MD5, K_IMG},
		{".bin", K_BIN, K_NONE}, {".cue", K_CUE, K_NONE}, {".log", K_LOG, K_NONE}, {".md5", K_MD5, K_NONE},
		{".jpg", K_JPG, K_NONE}, {".iso", K_ISO, K_NONE}, {".img", K_IMG, K_NONE}, {".sbi", K_SBI, K_NONE},
	};
	for (const auto &e : ex)
		if (ends_ci(s, e.ext)) { *blen = s.size() - strlen(e.ext); *own = e.own; return e.k; }
	*own = K_NONE;
	return K_NONE;
}

int is_image(Kind k) { return k == K_BIN || k == K_ISO || k == K_IMG; }
int is_companion(Kind k) { return k == K_LOG || k == K_MD5 || k == K_JPG || k == K_SBI || k == K_PART; }   // of the base's first set
int is_own(Kind k) { return k == K_OWN_LOG || k == K_OWN_MD5; }   // of the set of its image kind

void on_name(const char *name, void *ctx)
{
	if (!name || name[0] == '.' || !name[0]) return;   // ., .., .trash, hidden files
	Name n;
	n.s = name;
	n.k = kind_of(n.s, &n.blen, &n.own);
	n.owner = -1;
	if (n.k != K_NONE && n.blen) ((std::vector<Name> *)ctx)->push_back(n);
}

int same_base(const Name &n, const char *base, size_t blen) { return n.blen == blen && !memcmp(n.s.data(), base, blen); }

void add_file(GameDisc &d, const std::string &name)
{
	size_t used = 0;
	for (int i = 0; i < d.nfiles; i++) used += strlen(d.files + used) + 1;
	if (used + name.size() + 1 > sizeof(d.files)) { d.files_cut = 1; return; }
	memcpy(d.files + used, name.c_str(), name.size() + 1);
	d.nfiles++;
}

const char *leaf_of(const char *path) { const char *s = strrchr(path, '/'); return s ? s + 1 : path; }

// Spaces: single, none at the ends.
void squeeze(char *s)
{
	char *w = s;
	for (char *r = s; *r; r++) if (!(*r == ' ' && (w == s || w[-1] == ' '))) *w++ = *r;
	while (w > s && w[-1] == ' ') w--;
	*w = 0;
}

// Text without the parts in [ ] (the disc ID), as webMAN lists a file.
void without_brackets(const char *in, char *out, size_t n)
{
	size_t o = 0;
	int depth = 0;
	for (const char *p = in; *p && o < n - 1; p++) {
		if (*p == '[') { depth++; continue; }
		if (*p == ']') { if (depth) depth--; continue; }
		if (!depth) out[o++] = *p;
	}
	out[o] = 0;
	squeeze(out);
}

// The disc number in "(Disc N)", 0 without one; *at/*len = where it is.
int disc_number(const char *s, const char **at = NULL, size_t *len = NULL)
{
	for (const char *p = strstr(s, "(Disc "); p; p = strstr(p + 1, "(Disc ")) {
		char *e;
		long v = strtol(p + 6, &e, 10);
		if (e > p + 6 && *e == ')' && v > 0) {
			if (at) *at = p;
			if (len) *len = (size_t)(e + 1 - p);
			return (int)v;
		}
	}
	return 0;
}

void cut_disc(char *s)
{
	const char *at;
	size_t len;
	if (disc_number(s, &at, &len)) memmove((char *)at, at + len, strlen(at + len) + 1);
	squeeze(s);
}

// "(Europe)", "(USA, Europe)", "(En,Fr,De)": regions and language codes.
int is_region_group(const char *a, const char *b)
{
	static const char *const regions[] = {"Europe", "USA", "Japan", "Asia", "Australia", "Brazil", "Canada",
		"China", "France", "Germany", "Italy", "Korea", "Netherlands", "Spain", "Sweden", "UK", "World",
		"Scandinavia", "Taiwan", "Russia", "Portugal", "Greece", "Denmark", "Finland", "Norway", "Poland"};
	if (a >= b) return 0;
	const char *p = a;
	while (p < b) {
		while (p < b && *p == ' ') p++;
		const char *e = p;
		while (e < b && *e != ',') e++;
		const char *t = e;
		while (t > p && t[-1] == ' ') t--;
		size_t n = (size_t)(t - p);
		int ok = n == 2 && p[0] >= 'A' && p[0] <= 'Z' && p[1] >= 'a' && p[1] <= 'z';
		for (const char *r : regions) if (!ok && strlen(r) == n && !memcmp(r, p, n)) ok = 1;
		if (!ok) return 0;
		p = e < b ? e + 1 : e;
	}
	return 1;
}

// The region and language groups of a text ("(Europe)(En,Fr)"), lower case; *rest = the text
// without them, without [..] and "(Disc N)", lower case.
void split_regions(const char *text, std::string *regions, std::string *rest)
{
	char t[256];
	without_brackets(text, t, sizeof t);
	cut_disc(t);
	for (const char *p = t; *p; p++) {
		if (*p == '(') {
			const char *e = strchr(p, ')');
			if (e && is_region_group(p + 1, e)) {
				for (const char *q = p; q <= e; q++) if (*q != ' ') *regions += (char)lc((unsigned char)*q);
				p = e;
				continue;
			}
		}
		*rest += (char)lc((unsigned char)*p);
	}
}

// What makes discs one game: the title without [..], "(Disc N)" and regions, plus its regions
// (from the title, else the file name, else the disc ID's prefix), so two releases stay apart.
std::string group_key(const char *title, const char *leaf, const char *id)
{
	std::string reg, rest, unused;
	split_regions(title, &reg, &rest);
	if (reg.empty()) split_regions(leaf, &reg, &unused);
	if (reg.empty() && strlen(id) >= 4)
		reg = id[2] == 'E' ? "(europe)" : id[2] == 'U' ? "(usa)" : id[2] == 'P' || (id[0] == 'S' && id[1] == 'I') ? "(japan)" : "";
	char r[256];
	snprintf(r, sizeof r, "%s", rest.c_str());
	squeeze(r);
	return std::string(r) + "|" + reg;
}

std::string folder_of(const char *base) { return std::string(base, (size_t)(leaf_of(base) - base)); }

// Alphabetical, case-insensitive (ASCII), bytes of other scripts after ASCII.
int name_cmp(const char *a, const char *b)
{
	for (;; a++, b++) {
		int x = lc((unsigned char)*a), y = lc((unsigned char)*b);
		if (x != y || !x) return x - y;
	}
}

// A disc ID in a file name: "[SCES_014.31]".
void id_from_name(const char *s, char *out, size_t n)
{
	for (const char *b = strchr(s, '['); b; b = strchr(b + 1, '['))
		if (strlen(b) >= 13 && b[12] == ']' && b[5] == '_' && b[9] == '.') { snprintf(out, n, "%.11s", b + 1); return; }
}

void parse_log(GameDisc &d, char *buf, int n)
{
	buf[n > 0 ? n : 0] = 0;
	char *save = NULL;
	for (char *l = strtok_r(buf, "\r\n", &save); l; l = strtok_r(NULL, "\r\n", &save)) {
		if (!strncmp(l, "title=", 6)) {
			char *dot = strstr(l + 6, " \xC2\xB7 ");   // "Name  ·  SERIAL"
			if (dot) *dot = 0;
			snprintf(d.title, sizeof d.title, "%s", l + 6);
			squeeze(d.title);
		} else if (!strncmp(l, "serial=", 7)) {
			if (l[7] && !strchr(l + 7, ' ') && strlen(l + 7) < sizeof d.id) snprintf(d.id, sizeof d.id, "%s", l + 7);
		} else if (!strncmp(l, "date=", 5)) snprintf(d.date, sizeof d.date, "%s", l + 5);
		else if (!strncmp(l, "drive=", 6)) snprintf(d.drive, sizeof d.drive, "%s", l + 6);
		else if (!strncmp(l, "redump=", 7)) {
			const char *v = l + 7, *sp = strchr(v, ' ');
			size_t w = sp ? (size_t)(sp - v) : strlen(v);
			if (w == 8 && !strncmp(v, "verified", 8)) d.rd = 1;
			else if ((w == 8 && !strncmp(v, "no_match", 8)) || (w == 9 && !strncmp(v, "not_found", 9))) d.rd = 0;
			else d.rd = -1;
			d.rdname[0] = 0;
			if (d.rd == 1 && sp) snprintf(d.rdname, sizeof d.rdname, "%s", sp + 1);
		} else if (!strncmp(l, "unreadable=", 11) || (!strncmp(l, "uncertain=", 10) && strstr(l, "unconfirmed"))) d.errors++;
		else if (!strncmp(l, "bad=", 4)) { d.errors++; d.bad++; }
	}
}

// A cue's FILE lines (their images) and TRACK lines.
void parse_cue(std::vector<Name> &names, int di, GameDisc &d, Extra &x, char *buf, int n)
{
	buf[n > 0 ? n : 0] = 0;
	char *save = NULL;
	for (char *l = strtok_r(buf, "\r\n", &save); l; l = strtok_r(NULL, "\r\n", &save)) {
		while (*l == ' ' || *l == '\t') l++;
		if (!strncmp(l, "FILE ", 5)) {
			char *f = l + 5, *e;
			while (*f == ' ') f++;
			if (*f == '"') { f++; e = strchr(f, '"'); }
			else e = strrchr(f, ' ');
			if (!e || e == f) { x.cue_missing = 1; continue; }
			*e = 0;
			const char *bs = strrchr(f, '\\'), *fs = strrchr(f, '/');
			const char *leaf = bs && (!fs || bs > fs) ? bs + 1 : fs ? fs + 1 : f;
			x.cue_files++;
			int found = -1;
			for (size_t i = 0; i < names.size() && found < 0; i++) if (names[i].s == leaf) found = (int)i;
			for (size_t i = 0; i < names.size() && found < 0; i++) if (!strcasecmp(names[i].s.c_str(), leaf)) found = (int)i;
			if (found < 0) { x.cue_missing = 1; continue; }
			Name &m = names[found];
			if (m.owner >= 0) continue;   // another cue's: counted there
			m.owner = di;
			add_file(d, m.s);
			x.images.push_back(m.s);
			if (!x.image_kind) x.image_kind = m.k;
		} else if (!strncmp(l, "TRACK ", 6)) {
			d.tracks++;
			if (strstr(l, "/2048")) x.has2048 = 1;
			if (strstr(l, "/2352") || strstr(l, "AUDIO")) x.has2352 = 1;
		}
	}
}

// The files of base that go with set di: the app's log and .md5 of its image kind (own), and with
// first (the base's first set) the old-named log and .md5, the cover, the .sbi and a .part.
void attach_companions(std::vector<Name> &names, int di, GameDisc &d, Extra &x, const char *base, size_t blen, Kind own, int first)
{
	if (own != K_ISO && own != K_IMG) own = K_BIN;
	for (auto &m : names)
		if (m.owner < 0 && same_base(m, base, blen) && ((is_own(m.k) && m.own == own) || (first && is_companion(m.k)))) {
			m.owner = di;
			add_file(d, m.s);
			if (m.k == K_PART) x.part = 1;
			if (m.k == K_LOG || m.k == K_OWN_LOG) d.has_log = 1;
			if (m.k == K_JPG) d.has_cover = 1;
			char *sfx = m.k == K_LOG || m.k == K_OWN_LOG ? d.log_sfx : m.k == K_MD5 || m.k == K_OWN_MD5 ? d.md5_sfx : NULL;
			if (sfx && (is_own(m.k) || !sfx[0])) snprintf(sfx, sizeof d.log_sfx, "%s", m.s.c_str() + blen);   // the typed one wins
		}
}

int exists_at(int (*exists)(const char *), const char *base, const char *ext)
{
	char p[400];
	snprintf(p, sizeof p, "%s%s", base, ext);
	return exists(p);
}
// The old <base>.<what> is this image type's: the scan gives it to the base's first set (a cue's,
// then the images without one in name order: .bin, .img, .iso).
int old_is_of(const char *base, const char *type, int (*exists)(const char *))
{
	if (!strcmp(type, ".bin")) return 1;
	if (exists_at(exists, base, ".cue") || exists_at(exists, base, ".bin")) return 0;
	return strcmp(type, ".iso") || !exists_at(exists, base, ".img");
}

}  // namespace

int lib_file_path(const GameDisc *d, int i, char *out, int n)
{
	if (!d || i < 0 || i >= d->nfiles || n <= 0) return 0;
	const char *p = d->files;
	for (int k = 0; k < i; k++) p += strlen(p) + 1;
	const char *leaf = leaf_of(d->base);
	snprintf(out, (size_t)n, "%.*s%s", (int)(leaf - d->base), d->base, p);
	return 1;
}

int lib_scan(const char *dir, const LibIo *io, GameDisc *discs, int max)
{
	std::vector<Name> names;
	if (io->list(dir, on_name, &names) < 0) return -1;
	std::sort(names.begin(), names.end(), [](const Name &a, const Name &b) { return a.s < b.s; });
	std::vector<Extra> ex;
	int n = 0;
	auto new_disc = [&](const Name &m) -> int {
		if (n >= max) return -1;
		GameDisc &d = discs[n];
		memset(&d, 0, sizeof d);
		snprintf(d.base, sizeof d.base, "%s/%.*s", dir, (int)m.blen, m.s.c_str());
		d.dup = -1;
		d.rd = -1;
		ex.push_back(Extra());
		return n++;
	};
	char *buf = (char *)malloc(LOG_MAX > CUE_MAX ? LOG_MAX : CUE_MAX);
	if (!buf) return -1;
	char path[512];
	// 1. each cue with its images and companions
	for (size_t i = 0; i < names.size(); i++) {
		if (names[i].k != K_CUE || names[i].owner >= 0) continue;
		int di = new_disc(names[i]);
		if (di < 0) break;
		GameDisc &d = discs[di];
		Extra &x = ex[di];
		names[i].owner = di;
		add_file(d, names[i].s);
		x.cue = 1;
		snprintf(path, sizeof path, "%s/%s", dir, names[i].s.c_str());
		int got = io->read(path, buf, CUE_MAX - 1);
		if (got >= 0) parse_cue(names, di, d, x, buf, got);
		else {   // unreadable: its images by name, so the set stays whole
			x.cue_unread = 1;
			for (auto &m : names)
				if (m.owner < 0 && is_image(m.k) && same_base(m, names[i].s.c_str(), names[i].blen)) {
					m.owner = di;
					add_file(d, m.s);
					x.images.push_back(m.s);
					if (!x.image_kind) x.image_kind = m.k;
				}
		}
		attach_companions(names, di, d, x, names[i].s.c_str(), names[i].blen, (Kind)x.image_kind, 1);
	}
	// 2. images without a cue; a base's first one takes its companions, each its own log and .md5
	for (size_t i = 0; i < names.size() && n < max; i++) {
		if (!is_image(names[i].k) || names[i].owner >= 0) continue;
		int taken = 0;
		for (int k = 0; k < n && !taken; k++) {
			const char *lf = leaf_of(discs[k].base);
			taken = strlen(lf) == names[i].blen && !memcmp(lf, names[i].s.data(), names[i].blen);
		}
		int di = new_disc(names[i]);
		GameDisc &d = discs[di];
		Extra &x = ex[di];
		names[i].owner = di;
		add_file(d, names[i].s);
		x.image_kind = names[i].k;
		x.images.push_back(names[i].s);
		attach_companions(names, di, d, x, names[i].s.c_str(), names[i].blen, names[i].k, !taken);
	}
	// 3. what is left of a set without its image (a lone cover or .sbi is not a game)
	for (size_t i = 0; i < names.size() && n < max; i++) {
		Kind k = names[i].k;
		if (names[i].owner >= 0 || !(k == K_LOG || k == K_MD5 || k == K_PART || is_own(k))) continue;
		int di = new_disc(names[i]);
		attach_companions(names, di, discs[di], ex[di], names[i].s.c_str(), names[i].blen, is_own(k) ? names[i].own : K_BIN, 1);
	}
	// each set: sizes, format, log, ID, title, status, LibCrypt
	for (int di = 0; di < n; di++) {
		GameDisc &d = discs[di];
		Extra &x = ex[di];
		for (const auto &img : x.images) {
			snprintf(path, sizeof path, "%s/%s", dir, img.c_str());
			if (!d.image[0]) snprintf(d.image, sizeof d.image, "%s", path);
			uint64_t sz = 0;
			if (!io->stat_size(path, &sz) && sz) d.size += sz;
			else x.image_bad = 1;   // gone or empty
		}
		if (!x.cue) d.tracks = x.images.empty() ? 0 : 1;
		if (d.size) {
			int m2352 = d.size % 2352 == 0, m2048 = d.size % 2048 == 0;
			if (x.has2048 && m2048) d.fmt = GF_2048;
			else if (x.cue && x.has2352) d.fmt = m2352 ? GF_2352 : GF_UNKNOWN;
			else d.fmt = m2352 ? GF_2352 : m2048 ? GF_2048 : GF_UNKNOWN;
		}
		if (d.has_log) {
			snprintf(path, sizeof path, "%s%s", d.base, d.log_sfx);
			int got = io->read(path, buf, LOG_MAX - 1);
			size_t ml = strlen(LIB_LOG_MARK);
			d.old_log = !strcmp(d.log_sfx, ".log") && got >= (int)ml && !memcmp(buf, LIB_LOG_MARK, ml) && (got == (int)ml || buf[ml] == '\n' || buf[ml] == '\r');
			if (got >= 0) parse_log(d, buf, got);
		}
		const char *leaf = leaf_of(d.base);
		if (!d.id[0]) id_from_name(leaf, d.id, sizeof d.id);
		if (!d.title[0]) without_brackets(leaf, d.title, sizeof d.title);
		int broken = x.part || x.image_bad || x.images.empty() || (x.cue && (x.cue_unread || x.cue_missing || !x.cue_files))
		             || (!x.cue && x.image_kind == K_BIN);
		d.st = broken ? GS_BROKEN : d.fmt == GF_2048 ? GS_RERIP : d.errors ? GS_ERRORS
		     : d.rd == 1 ? GS_VERIFIED : d.rd == 0 ? GS_NOMATCH : GS_UNCHECKED;
		if (io->libcrypt && d.id[0] && io->libcrypt(d.id)) {
			int have = 0;
			size_t bl = strlen(leaf);
			for (const auto &m : names) if (m.k == K_SBI && same_base(m, leaf, bl)) have = 1;
			if (!have && io->sbi_dir) {
				snprintf(path, sizeof path, "%s/%s.sbi", io->sbi_dir, d.id);
				have = io->exists(path);
			}
			d.libcrypt = have ? 1 : -1;
		}
	}
	free(buf);
	return n;
}

const char *lib_image_type(const char *image)
{
	size_t l = image ? strlen(image) : 0;
	if (l >= 4 && !strcasecmp(image + l - 4, ".iso")) return ".iso";
	if (l >= 4 && !strcasecmp(image + l - 4, ".img")) return ".img";
	return ".bin";
}

void lib_own_name(const char *base, const char *image, const char *what, char *out, int n)
{
	snprintf(out, (size_t)n, "%s%s.%s", base, lib_image_type(image), what);
}

int lib_own_find(const char *base, const char *image, const char *what, int (*exists)(const char *), char *out, int n)
{
	lib_own_name(base, image, what, out, n);
	if (exists(out)) return 1;
	char old[400];
	snprintf(old, sizeof old, "%s.%s", base, what);
	if (!exists(old) || !old_is_of(base, lib_image_type(image), exists)) return 0;
	snprintf(out, (size_t)n, "%s", old);
	return 1;
}

int lib_old_is_of(const char *base, const char *image, int (*exists)(const char *)) { return old_is_of(base, lib_image_type(image), exists); }

int lib_own_migration(const char *base, const char *image, const LibIo *io, char (*from)[320], char (*to)[320])
{
	const char *type = lib_image_type(image);
	char p[400], head[64];
	snprintf(p, sizeof p, "%s.log", base);
	size_t ml = strlen(LIB_LOG_MARK);
	int got = io->exists(p) ? io->read(p, head, (int)sizeof head - 1) : -1;
	if (got < (int)ml || memcmp(head, LIB_LOG_MARK, ml) || (got > (int)ml && head[ml] != '\n' && head[ml] != '\r')) return 0;
	if (!old_is_of(base, type, io->exists)) return 0;
	int n = 0;
	for (const char *what : {"log", "md5"}) {
		char a[400], b[400];
		snprintf(a, sizeof a, "%s.%s", base, what);
		snprintf(b, sizeof b, "%s%s.%s", base, type, what);
		if (!io->exists(a) || io->exists(b) || strlen(a) >= 320 || strlen(b) >= 320) continue;
		snprintf(from[n], 320, "%s", a);
		snprintf(to[n], 320, "%s", b);
		n++;
	}
	return n;
}

void lib_mark_dups(GameDisc *d, int n)
{
	for (int i = 0; i < n; i++) d[i].dup = -1;
	for (int i = 0; i < n; i++)
		for (int j = i + 1; j < n; j++) {
			int same = (d[i].id[0] && d[i].size && d[i].size == d[j].size && !strcmp(d[i].id, d[j].id))
			        || (d[i].rdname[0] && !strcmp(d[i].rdname, d[j].rdname));
			if (!same) continue;
			if (d[i].dup < 0) d[i].dup = j;
			if (d[j].dup < 0) d[j].dup = i;
		}
}

int lib_group(GameDisc *d, int n, Game *games, int max)
{
	if (n <= 0) return 0;
	struct Row { char name[128]; std::vector<int> discs; };
	std::vector<Row> rows;
	std::vector<std::string> keys((size_t)n);
	std::vector<int> dnum((size_t)n), row_of((size_t)n);
	for (int i = 0; i < n; i++) {
		const char *leaf = leaf_of(d[i].base);
		dnum[i] = disc_number(d[i].title);
		if (!dnum[i]) dnum[i] = disc_number(leaf);
		// one game: the same title and regions, in the same folder, and each disc number once
		keys[i] = folder_of(d[i].base) + "|" + group_key(d[i].title, leaf, d[i].id);
		int r = -1;
		if (dnum[i])
			for (size_t c = 0; c < rows.size() && r < 0; c++) {
				const Row &row = rows[c];
				if (!dnum[row.discs[0]] || keys[row.discs[0]] != keys[i]) continue;
				int taken = 0;
				for (int j : row.discs) if (dnum[j] == dnum[i]) taken = 1;
				if (!taken) r = (int)c;
			}
		if (r < 0) { r = (int)rows.size(); rows.push_back(Row()); }
		row_of[i] = r;
		rows[r].discs.push_back(i);
	}
	for (auto &r : rows) {
		std::sort(r.discs.begin(), r.discs.end(), [&](int a, int b) {
			return dnum[a] != dnum[b] ? dnum[a] < dnum[b] : strcmp(d[a].base, d[b].base) < 0;
		});
		without_brackets(leaf_of(d[r.discs[0]].base), r.name, sizeof r.name);
		if (r.discs.size() > 1) cut_disc(r.name);
	}
	std::vector<int> order(rows.size());
	for (size_t i = 0; i < order.size(); i++) order[i] = (int)i;
	std::sort(order.begin(), order.end(), [&](int a, int b) {
		int c = name_cmp(rows[a].name, rows[b].name);
		if (c) return c < 0;
		c = strcmp(rows[a].name, rows[b].name);
		if (c) return c < 0;
		c = strcmp(d[rows[a].discs[0]].base, d[rows[b].discs[0]].base);
		if (c) return c < 0;
		return strcmp(d[rows[a].discs[0]].image, d[rows[b].discs[0]].image) < 0;   // one base, two images: one order
	});
	// the discs in row order; dup indices follow
	std::vector<int> newpos((size_t)n);
	std::vector<GameDisc> copy(d, d + n);
	int pos = 0, out = 0;
	for (int r : order) {
		if (out < max) {
			Game &g = games[out++];
			snprintf(g.name, sizeof g.name, "%s", rows[r].name);
			g.first = pos;
			g.ndiscs = (int)rows[r].discs.size();
			g.st = GS_VERIFIED;
			for (int i : rows[r].discs) if (copy[i].st < g.st) g.st = copy[i].st;
		}
		for (int i : rows[r].discs) newpos[i] = pos++;
	}
	for (int i = 0; i < n; i++) {
		d[newpos[i]] = copy[i];
		if (copy[i].dup >= 0 && copy[i].dup < n) d[newpos[i]].dup = newpos[copy[i].dup];
	}
	return out;
}

int lib_filter(const Game *g, int n, const GameDisc *d, const GameFilter *f, int *out)
{
	int k = 0;
	for (int r = 0; r < n; r++) {
		int hit = 0;   // one of its discs matches every criterion
		for (int i = g[r].first; i < g[r].first + g[r].ndiscs && !hit; i++) {
			const GameDisc &x = d[i];
			int fmt = !f || !f->fmt || (f->fmt == 1 && x.fmt == GF_2352) || (f->fmt == 2 && x.fmt == GF_2048);
			int rd = !f || !f->redump || (f->redump == 1 && x.rd == 1) || (f->redump == 2 && x.rd == -1)
			         || (f->redump == 3 && x.rd == 0) || (f->redump == 4 && x.rd != 1);
			int err = !f || !f->errors || (f->errors == 1 && x.errors > 0) || (f->errors == 2 && !x.errors);
			int oth = !f || !f->other || (f->other == 1 && x.st == GS_BROKEN) || (f->other == 2 && x.dup >= 0)
			          || (f->other == 3 && x.libcrypt == -1);
			hit = fmt && rd && err && oth;
		}
		if (hit) out[k++] = r;
	}
	return k;
}

void lib_summary(const GameDisc *d, int n, int *total, int *verified, int *errors, int *rerip, int *broken)
{
	int c[GS_VERIFIED + 1] = {0};
	for (int i = 0; i < n; i++) if (d[i].st >= GS_BROKEN && d[i].st <= GS_VERIFIED) c[d[i].st]++;
	if (total) *total = n;
	if (verified) *verified = c[GS_VERIFIED];
	if (errors) *errors = c[GS_ERRORS];
	if (rerip) *rerip = c[GS_RERIP];
	if (broken) *broken = c[GS_BROKEN];
}

// The "letter" of a row: an ASCII letter (lower case); '#' for other ASCII; the rest one block.
static int letter_of(const Game &g)
{
	int c = (unsigned char)g.name[0];
	if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')) return lc(c);
	return c < 0x80 ? '#' : 0x80;
}

int lib_first_letter_next(const Game *g, const int *shown, int n, int from, int dir)
{
	if (n <= 0 || from < 0 || from >= n) return from;
	int cur = letter_of(g[shown[from]]);
	if (dir > 0) {
		for (int p = from + 1; p < n; p++) if (letter_of(g[shown[p]]) != cur) return p;
		return from;
	}
	int p = from;
	while (p > 0 && letter_of(g[shown[p - 1]]) == cur) p--;
	if (p < from) return p;
	if (!p) return from;
	int prev = letter_of(g[shown[p - 1]]);
	while (p > 0 && letter_of(g[shown[p - 1]]) == prev) p--;
	return p;
}
