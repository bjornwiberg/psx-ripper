// Verification against Redump (redump.org), see redump.h.
// Online: /discs/quicksearch/<serial>/ lists the disc pages, /disc/<id>/md5/ gives the track MD5s.
// Local copy: the PlayStation dat (a zip with one Logiqx XML file) is downloaded to the console,
// inflated with zlib and reduced to a compact index: per disc its name and the size and MD5 of
// every track. Plain HTTP, so no TLS library is needed.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <net/netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <zlib.h>
#include "redump.h"

#define HOST      "redump.org"
#ifndef RD_DIR   // overridable for host tests
#define RD_DIR    "/dev_hdd0/tmp/psxrip"
#endif
#define RD_ZIP    RD_DIR "/redump.zip"
#define RD_IDX    RD_DIR "/redump.idx"
#define RD_VER    RD_DIR "/redump.ver"
#ifndef LOG_PATH
#define LOG_PATH  "/dev_hdd0/tmp/psxrip.log"
#endif

static void rlog(const char *fmt, ...)
{
	FILE *f = fopen(LOG_PATH, "a");
	if (!f) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

// ---------------------------------------------------------------- HTTP/1.0 client
typedef int (*sink_fn)(const uint8_t *p, int n, void *ctx);   // non-zero return aborts

// GET http://redump.org<path>. Returns the HTTP status (body passed to sink), negative on error.
// *clen gets Content-Length (0 if absent), loc the Location header of a redirect.
static int http_get(const char *path, sink_fn sink, void *ctx, uint32_t *clen, char *loc, size_t locn)
{
	struct hostent *h = gethostbyname(HOST);
	if (!h || !h->h_addr_list || !h->h_addr_list[0]) { rlog("redump: DNS lookup failed"); return -1; }
	int sock = socket(AF_INET, SOCK_STREAM, 0);
	if (sock < 0) return -2;
	struct timeval tv = {15, 0};
	setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(80);
	memcpy(&sa.sin_addr, h->h_addr_list[0], 4);
	if (connect(sock, (struct sockaddr *)&sa, sizeof(sa))) { close(sock); rlog("redump: connect failed"); return -2; }
	char req[320];
	int n = snprintf(req, sizeof(req), "GET %s HTTP/1.0\r\nHost: " HOST "\r\nUser-Agent: PSXRipper\r\nConnection: close\r\n\r\n", path);
	if (send(sock, req, n, 0) != n) { close(sock); return -2; }

	static char buf[16384];
	int have = 0, status = -3, in_body = 0, aborted = 0;
	if (clen) *clen = 0;
	if (loc && locn) loc[0] = 0;
	for (;;) {
		int r = recv(sock, buf + have, sizeof(buf) - have, 0);
		if (r <= 0) break;
		have += r;
		if (!in_body) {
			char *end = NULL;
			for (int i = 0; i + 3 < have; i++)
				if (buf[i] == '\r' && buf[i + 1] == '\n' && buf[i + 2] == '\r' && buf[i + 3] == '\n') { end = buf + i; break; }
			if (!end) { if (have == (int)sizeof(buf)) break; continue; }
			*end = 0;
			sscanf(buf, "HTTP/%*s %d", &status);
			for (char *l = strstr(buf, "\r\n"); l; l = strstr(l + 2, "\r\n")) {
				if (!strncasecmp(l + 2, "Content-Length:", 15) && clen) *clen = strtoul(l + 17, NULL, 10);
				if (!strncasecmp(l + 2, "Location:", 9) && loc && locn) {
					const char *v = l + 11;
					while (*v == ' ') v++;
					size_t k = strcspn(v, "\r\n");
					if (k >= locn) k = locn - 1;
					memcpy(loc, v, k); loc[k] = 0;
				}
			}
			int body = (int)(end + 4 - buf);
			have -= body;
			memmove(buf, buf + body, have);
			in_body = 1;
		}
		if (have && sink && sink((const uint8_t *)buf, have, ctx)) { aborted = 1; break; }
		have = 0;
	}
	close(sock);
	if (aborted) return -4;
	return status;
}

struct MemSink { char *p; int n, cap; };
static int mem_sink(const uint8_t *p, int n, void *ctx)
{
	MemSink *m = (MemSink *)ctx;
	if (m->n + n + 1 > m->cap) return 1;   // larger than expected: stop
	memcpy(m->p + m->n, p, n);
	m->n += n;
	m->p[m->n] = 0;
	return 0;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int parse_md5(const char *s, uint8_t out[16])
{
	for (int i = 0; i < 16; i++) {
		int a = hexval(s[i * 2]), b = hexval(s[i * 2 + 1]);
		if (a < 0 || b < 0) return -1;
		out[i] = (uint8_t)(a << 4 | b);
	}
	return 0;
}

// "Alundra (Europe) (Track 1).bin" -> "Alundra (Europe)"
static void disc_name(const char *file, char *out, size_t n)
{
	const char *e = strstr(file, " (Track ");
	if (!e) e = strstr(file, ".bin");
	size_t k = e ? (size_t)(e - file) : strlen(file);
	if (k >= n) k = n - 1;
	memcpy(out, file, k);
	out[k] = 0;
}

// ---------------------------------------------------------------- online
int rd_online(const char *serial, RdDisc *out, int max)
{
	char rs[24], path[96], loc[160];
	size_t o = 0;
	for (const char *p = serial; *p && o < sizeof(rs) - 1; p++)
		if (*p != '.') rs[o++] = (*p == '_') ? '-' : *p;   // SLES_011.35 -> SLES-01135
	rs[o] = 0;
	snprintf(path, sizeof(path), "/discs/quicksearch/%s/", rs);

	MemSink m;
	m.cap = 256 * 1024; m.n = 0; m.p = (char *)malloc(m.cap);
	if (!m.p) return -1;
	m.p[0] = 0;
	int st = http_get(path, mem_sink, &m, NULL, loc, sizeof(loc));
	int ids[8], nid = 0;
	if (st == 301 || st == 302) {   // a single hit may redirect straight to the disc page
		const char *d = strstr(loc, "/disc/");
		if (d) ids[nid++] = atoi(d + 6);
	} else if (st == 200) {
		for (const char *p = m.p; (p = strstr(p, "href=\"/disc/")) && nid < 8; p += 12) {
			int id = atoi(p + 12);
			int dup = 0;
			for (int i = 0; i < nid; i++) dup |= ids[i] == id;
			if (id > 0 && !dup) ids[nid++] = id;
		}
	}
	rlog("redump online %s -> HTTP %d, %d disc(s)", rs, st, nid);
	if (st != 200 && st != 301 && st != 302) { free(m.p); return -1; }

	int nd = 0;
	for (int i = 0; i < nid && nd < max; i++) {
		snprintf(path, sizeof(path), "/disc/%d/md5/", ids[i]);
		m.n = 0; m.p[0] = 0;
		if (http_get(path, mem_sink, &m, NULL, NULL, 0) != 200) continue;
		RdDisc *d = &out[nd];
		d->n = 0; d->name[0] = 0;
		// lines: "<md5> *<file name>"
		for (char *l = m.p; *l; ) {
			char *e = strchr(l, '\n');
			if (e) *e = 0;
			size_t len = strlen(l);
			if (len && l[len - 1] == '\r') l[--len] = 0;
			if (len > 35 && l[32] == ' ' && l[33] == '*' && len > 4 && !strcmp(l + len - 4, ".bin") && d->n < RD_MAX_TRACKS &&
			    !parse_md5(l, d->md5[d->n])) {
				if (!d->name[0]) disc_name(l + 34, d->name, sizeof(d->name));
				d->size[d->n++] = 0;
			}
			if (!e) break;
			l = e + 1;
		}
		if (d->n) { rlog("redump online: %s (%d tracks)", d->name, d->n); nd++; }
	}
	free(m.p);
	return nd;
}

// ---------------------------------------------------------------- local copy: download + index
// Index record: u8 tracks, u8 name length, name, then per track u32 size (big-endian) + 16-byte MD5.
static int read_rec(FILE *f, RdDisc *d)
{
	int n = fgetc(f), nl = fgetc(f);
	if (n == EOF || nl == EOF) return 0;
	if (fread(d->name, 1, nl, f) != (size_t)nl) return 0;
	d->name[nl] = 0;
	d->n = n;
	for (int i = 0; i < n; i++) {
		uint8_t b[20];
		if (fread(b, 1, 20, f) != 20) return 0;
		d->size[i] = (uint32_t)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3];
		memcpy(d->md5[i], b + 4, 16);
	}
	return 1;
}

int rd_local_info(char *version, size_t n)
{
	FILE *f = fopen(RD_VER, "r");
	if (!f) return 0;
	char line[96] = "";
	int count = 0;
	if (fgets(line, sizeof(line), f)) line[strcspn(line, "\r\n")] = 0;
	if (fscanf(f, "%d", &count) != 1) count = 0;
	fclose(f);
	struct stat st;
	if (stat(RD_IDX, &st)) return 0;
	if (version && n) snprintf(version, n, "%s", line);
	return count;
}

struct DlCtx { FILE *f; volatile uint32_t *done; volatile int *abort; };
static int file_sink(const uint8_t *p, int n, void *ctx)
{
	DlCtx *c = (DlCtx *)ctx;
	if (*c->abort) return 1;
	if (fwrite(p, 1, n, c->f) != (size_t)n) return 1;
	*c->done += n;
	return 0;
}

// XML attribute value with the five XML entities decoded.
static int attr(const char *line, const char *key, char *out, size_t n)
{
	const char *p = strstr(line, key);
	if (!p) return -1;
	p += strlen(key);
	size_t o = 0;
	while (*p && *p != '"' && o < n - 1) {
		if (*p == '&') {
			static const char *ent[] = {"&amp;", "&apos;", "&quot;", "&lt;", "&gt;"};
			static const char rep[] = "&'\"<>";
			int k = 0;
			while (k < 5 && strncmp(p, ent[k], strlen(ent[k]))) k++;
			if (k < 5) { out[o++] = rep[k]; p += strlen(ent[k]); continue; }
		}
		out[o++] = *p++;
	}
	out[o] = 0;
	return 0;
}

struct Indexer {
	FILE *idx;
	char line[4096];
	int len, count;
	char version[64];
	RdDisc d;
};

static void index_line(Indexer *x)
{
	char *l = x->line, tmp[512];
	const char *v;
	if ((v = strstr(l, "<version>"))) {
		v += 9;
		size_t k = strcspn(v, "<");
		if (k >= sizeof(x->version)) k = sizeof(x->version) - 1;
		memcpy(x->version, v, k); x->version[k] = 0;
	} else if (strstr(l, "<game ") && !attr(l, "name=\"", x->d.name, sizeof(x->d.name))) {
		x->d.n = 0;
	} else if (strstr(l, "<rom ") && !attr(l, "name=\"", tmp, sizeof(tmp))) {
		size_t k = strlen(tmp);
		char sz[24], md[40];
		if (k > 4 && !strcmp(tmp + k - 4, ".bin") && x->d.n < RD_MAX_TRACKS &&
		    !attr(l, "size=\"", sz, sizeof(sz)) && !attr(l, "md5=\"", md, sizeof(md)) && !parse_md5(md, x->d.md5[x->d.n]))
			x->d.size[x->d.n++] = strtoul(sz, NULL, 10);
	} else if (strstr(l, "</game>") && x->d.n) {
		size_t nl = strlen(x->d.name);
		if (nl > 255) nl = 255;
		fputc(x->d.n, x->idx);
		fputc((int)nl, x->idx);
		fwrite(x->d.name, 1, nl, x->idx);
		for (int i = 0; i < x->d.n; i++) {
			uint32_t s = x->d.size[i];
			uint8_t b[4] = {(uint8_t)(s >> 24), (uint8_t)(s >> 16), (uint8_t)(s >> 8), (uint8_t)s};
			fwrite(b, 1, 4, x->idx);
			fwrite(x->d.md5[i], 1, 16, x->idx);
		}
		x->count++;
		x->d.n = 0;
	}
}

static void index_feed(Indexer *x, const uint8_t *p, int n)
{
	for (int i = 0; i < n; i++) {
		if (p[i] == '\n') {
			x->line[x->len] = 0;
			index_line(x);
			x->len = 0;
		} else if (x->len < (int)sizeof(x->line) - 1) {
			x->line[x->len++] = (char)p[i];
		}
	}
}

// Inflates the first (only) file of the zip and builds the index. Returns the disc count.
static int build_index(volatile uint32_t *done, volatile uint32_t *total, volatile int *abort)
{
	FILE *z = fopen(RD_ZIP, "rb");
	if (!z) return -5;
	uint8_t h[30];
	if (fread(h, 1, 30, z) != 30 || h[0] != 'P' || h[1] != 'K' || h[2] != 3 || h[3] != 4 || (h[8] | h[9] << 8) != 8) {
		fclose(z); return -5;   // not a deflated zip entry
	}
	fseek(z, 30 + (h[26] | h[27] << 8) + (h[28] | h[29] << 8), SEEK_SET);
	struct stat st;
	stat(RD_ZIP, &st);
	*total = (uint32_t)st.st_size; *done = 0;

	Indexer *x = (Indexer *)calloc(1, sizeof(Indexer));
	uint8_t *in = (uint8_t *)malloc(65536), *out = (uint8_t *)malloc(65536);
	x->idx = fopen(RD_IDX ".part", "wb");
	int ret = -6;
	z_stream zs;
	memset(&zs, 0, sizeof(zs));
	if (x && in && out && x->idx && inflateInit2(&zs, -MAX_WBITS) == Z_OK) {
		int zr = Z_OK;
		while (zr != Z_STREAM_END && !*abort) {
			if (!zs.avail_in) {
				zs.avail_in = fread(in, 1, 65536, z);
				zs.next_in = in;
				if (!zs.avail_in) break;
			}
			zs.next_out = out; zs.avail_out = 65536;
			zr = inflate(&zs, Z_NO_FLUSH);
			if (zr != Z_OK && zr != Z_STREAM_END) break;
			index_feed(x, out, 65536 - zs.avail_out);
			*done = (uint32_t)ftell(z);
		}
		inflateEnd(&zs);
		if (*abort) ret = -7;
		else if (zr == Z_STREAM_END && x->count > 0) ret = x->count;
	}
	fclose(z);
	if (x && x->idx) fclose(x->idx);
	if (ret > 0) {
		unlink(RD_IDX);
		rename(RD_IDX ".part", RD_IDX);
		FILE *v = fopen(RD_VER, "w");
		if (v) { fprintf(v, "%s\n%d\n", x->version, x->count); fclose(v); }
		rlog("redump index: %d discs, version %s", x->count, x->version);
	} else {
		unlink(RD_IDX ".part");
	}
	free(x); free(in); free(out);
	return ret;
}

int rd_download(volatile uint32_t *done, volatile uint32_t *total, volatile int *phase, volatile int *abort)
{
	mkdir(RD_DIR, 0777);
	*phase = 0; *done = 0; *total = 0;
	FILE *f = fopen(RD_ZIP, "wb");
	if (!f) return -4;
	DlCtx c = {f, done, abort};
	// Content-Length goes straight into *total, so the progress bar knows the size during the download
	int st = http_get("/datfile/psx/", file_sink, &c, (uint32_t *)total, NULL, 0);
	uint32_t clen = *total;
	fclose(f);
	rlog("redump download -> HTTP %d, %u bytes", st, *done);
	if (st != 200 || *abort || (clen && *done != clen)) {
		unlink(RD_ZIP);
		return *abort ? -7 : (st < 0 ? st : -3);
	}
	*phase = 1;
	int r = build_index(done, total, abort);
	unlink(RD_ZIP);
	return r;
}

int rd_local_by_size(uint64_t total, int ntracks, char (*names)[RD_NAME], int max)
{
	FILE *f = fopen(RD_IDX, "rb");
	if (!f) return -1;
	static char vbuf[65536];
	setvbuf(f, vbuf, _IOFBF, sizeof(vbuf));
	static RdDisc d;
	int hits = 0;
	while (read_rec(f, &d)) {
		if (d.n != ntracks) continue;
		uint64_t s = 0;
		for (int i = 0; i < d.n; i++) s += d.size[i];
		if (s != total) continue;
		if (hits < max) snprintf(names[hits], RD_NAME, "%s", d.name);
		hits++;
	}
	fclose(f);
	return hits;
}

int rd_local_verify(const uint8_t (*md5)[16], int n, char *name, size_t nn)
{
	FILE *f = fopen(RD_IDX, "rb");
	if (!f) return -1;
	static char vbuf[65536];
	setvbuf(f, vbuf, _IOFBF, sizeof(vbuf));
	static RdDisc d;
	int found = 0;
	while (!found && read_rec(f, &d)) {
		if (d.n != n) continue;
		int ok = 1;
		for (int i = 0; i < n && ok; i++) ok = !memcmp(d.md5[i], md5[i], 16);
		if (ok) { snprintf(name, nn, "%s", d.name); found = 1; }
	}
	fclose(f);
	return found;
}
