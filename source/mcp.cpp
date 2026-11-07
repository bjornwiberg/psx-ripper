// MCP request handling (see mcp.h): a JSON-RPC 2.0 request in, one response out. The request is
// checked once by a small JSON reader (bounded depth, no allocation) and then read in place; the
// tool table drives both tools/list and the argument checks.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "mcp.h"

#define MAX_DEPTH 16
#define MAX_ID 128          // the raw id we echo (a string id with its quotes, or a number)
#define MAX_KEY 64
#define MAX_VALUE 1024

static McpHooks g_hooks;
static char g_version[48] = "?";

void mcp_init(const McpHooks *hooks, const char *version)
{
	if (hooks) g_hooks = *hooks;
	else memset(&g_hooks, 0, sizeof(g_hooks));
	snprintf(g_version, sizeof(g_version), "%s", version ? version : "?");
}

// ---- reading JSON: p..e is the text; every function stays inside it ----

static const char *j_ws(const char *p, const char *e)
{
	while (p < e && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) p++;
	return p;
}

static int hexval(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

static int hex4(const char *p, const char *e)
{
	if (e - p < 4) return -1;
	int v = 0;
	for (int i = 0; i < 4; i++) {
		int h = hexval(p[i]);
		if (h < 0) return -1;
		v = v * 16 + h;
	}
	return v;
}

// p at '"': the end of the string (after its closing quote), or NULL if it is not a valid one.
// A \u escape of a surrogate must be a complete pair.
static const char *j_string(const char *p, const char *e)
{
	if (p >= e || *p != '"') return NULL;
	for (p++; p < e; ) {
		unsigned char c = *p++;
		if (c == '"') return p;
		if (c < 0x20) return NULL;
		if (c >= 0x80) {
			// well-formed UTF-8 only (no overlong forms, surrogates or values past U+10FFFF), so
			// what reaches the hooks and comes back in a response is valid text
			int more = c >= 0xC2 && c <= 0xDF ? 1 : c >= 0xE0 && c <= 0xEF ? 2 : c >= 0xF0 && c <= 0xF4 ? 3 : -1;
			if (more < 0 || e - p < more) return NULL;
			unsigned char c1 = p[0];
			if ((c == 0xE0 && c1 < 0xA0) || (c == 0xED && c1 > 0x9F) || (c == 0xF0 && c1 < 0x90) || (c == 0xF4 && c1 > 0x8F)) return NULL;
			for (int i = 0; i < more; i++) if (((unsigned char)p[i] & 0xC0) != 0x80) return NULL;
			p += more;
			continue;
		}
		if (c != '\\') continue;
		if (p >= e) return NULL;
		c = *p++;
		if (c == 'u') {
			int u = hex4(p, e);
			if (u < 0) return NULL;
			p += 4;
			if (u >= 0xDC00 && u <= 0xDFFF) return NULL;
			if (u >= 0xD800 && u <= 0xDBFF) {
				if (e - p < 6 || p[0] != '\\' || p[1] != 'u') return NULL;
				int lo = hex4(p + 2, e);
				if (lo < 0xDC00 || lo > 0xDFFF) return NULL;
				p += 6;
			}
		} else if (!strchr("\"\\/bfnrt", c) || !c) return NULL;
	}
	return NULL;
}

static const char *j_digits(const char *p, const char *e)
{
	const char *s = p;
	while (p < e && *p >= '0' && *p <= '9') p++;
	return p > s ? p : NULL;
}

static const char *j_number(const char *p, const char *e)
{
	if (p < e && *p == '-') p++;
	if (p >= e) return NULL;
	if (*p == '0') p++;
	else if (!(p = j_digits(p, e))) return NULL;
	if (p < e && *p == '.' && !(p = j_digits(p + 1, e))) return NULL;
	if (p < e && (*p == 'e' || *p == 'E')) {
		p++;
		if (p < e && (*p == '+' || *p == '-')) p++;
		if (!(p = j_digits(p, e))) return NULL;
	}
	return p;
}

static const char *j_word(const char *p, const char *e, const char *w)
{
	int k = strlen(w);
	return e - p >= k && !memcmp(p, w, k) ? p + k : NULL;
}

// p at a value (no leading space): the end of the value, or NULL if it is not valid JSON or
// nests deeper than MAX_DEPTH (the value itself is at level depth).
static const char *j_value(const char *p, const char *e, int depth)
{
	if (p >= e) return NULL;
	if (*p == '{' || *p == '[') {
		if (depth > MAX_DEPTH) return NULL;
		char close = *p == '{' ? '}' : ']';
		p = j_ws(p + 1, e);
		if (p < e && *p == close) return p + 1;
		for (;;) {
			if (close == '}') {
				if (!(p = j_string(p, e))) return NULL;
				p = j_ws(p, e);
				if (p >= e || *p != ':') return NULL;
				p = j_ws(p + 1, e);
			}
			if (!(p = j_value(p, e, depth + 1))) return NULL;
			p = j_ws(p, e);
			if (p >= e) return NULL;
			if (*p == close) return p + 1;
			if (*p != ',') return NULL;
			p = j_ws(p + 1, e);
		}
	}
	if (*p == '"') return j_string(p, e);
	if (*p == 't') return j_word(p, e, "true");
	if (*p == 'f') return j_word(p, e, "false");
	if (*p == 'n') return j_word(p, e, "null");
	return j_number(p, e);
}

// The rest only reads text that j_value has accepted.
static const char *j_skip(const char *p, const char *e) { return j_value(p, e, 0); }

// Decodes the string at p into dst (UTF-8, NUL-terminated); its length, or -1 if it does not fit.
// Raw bytes are copied as they are (the client's UTF-8); escapes become their characters.
static int j_decode(const char *p, const char *e, char *dst, int cap)
{
	int k = 0;
	for (p++; p < e && *p != '"'; ) {
		char u[4];
		int m = 1;
		unsigned c = (unsigned char)*p++;
		u[0] = c;
		if (c == '\\') {
			c = (unsigned char)*p++;
			switch (c) {
			case 'b': u[0] = '\b'; break;
			case 'f': u[0] = '\f'; break;
			case 'n': u[0] = '\n'; break;
			case 'r': u[0] = '\r'; break;
			case 't': u[0] = '\t'; break;
			case 'u':
				c = hex4(p, e);
				p += 4;
				if (c >= 0xD800 && c <= 0xDBFF) {
					c = 0x10000 + ((c - 0xD800) << 10) + (hex4(p + 2, e) - 0xDC00);
					p += 6;
				}
				if (c < 0x80) u[0] = c;
				else if (c < 0x800) { u[0] = 0xC0 | (c >> 6); u[1] = 0x80 | (c & 63); m = 2; }
				else if (c < 0x10000) { u[0] = 0xE0 | (c >> 12); u[1] = 0x80 | ((c >> 6) & 63); u[2] = 0x80 | (c & 63); m = 3; }
				else { u[0] = 0xF0 | (c >> 18); u[1] = 0x80 | ((c >> 12) & 63); u[2] = 0x80 | ((c >> 6) & 63); u[3] = 0x80 | (c & 63); m = 4; }
				break;
			default: u[0] = c; break;   // " \ /
			}
		}
		if (k + m >= cap) return -1;
		memcpy(dst + k, u, m);
		k += m;
	}
	dst[k] = 0;
	return k;
}

static bool j_is(const char *p, const char *e, const char *lit)
{
	char b[MAX_KEY];
	int k = j_decode(p, e, b, sizeof(b));
	return k == (int)strlen(lit) && !memcmp(b, lit, k);
}

// Walks an object's members: obj at '{'.
struct JIt { const char *p, *e; };
static JIt j_members(const char *obj, const char *e) { JIt it = {j_ws(obj + 1, e), e}; return it; }
static bool j_next(JIt *it, const char **key, const char **val)
{
	if (it->p >= it->e || *it->p == '}') return false;
	*key = it->p;
	const char *q = j_ws(j_skip(it->p, it->e), it->e);   // at ':'
	*val = j_ws(q + 1, it->e);
	q = j_ws(j_skip(*val, it->e), it->e);
	if (q < it->e && *q == ',') q = j_ws(q + 1, it->e);
	it->p = q;
	return true;
}

// The member's value, or NULL; *dup is set when the key appears more than once.
static const char *j_get(const char *obj, const char *e, const char *key, bool *dup = NULL)
{
	JIt it = j_members(obj, e);
	const char *k, *v, *found = NULL;
	while (j_next(&it, &k, &v)) {
		if (!j_is(k, e, key)) continue;
		if (found) { if (dup) *dup = true; break; }
		found = v;
	}
	return found;
}

// ---- writing the response ----

struct W { char *p; int n, len; bool over; };

static void w_raw(W *w, const char *s, int k)
{
	if (w->over) return;
	if (w->len + k >= w->n) { w->over = true; return; }
	memcpy(w->p + w->len, s, k);
	w->len += k;
	w->p[w->len] = 0;
}
static void w_str(W *w, const char *s) { w_raw(w, s, strlen(s)); }
static void w_fmt(W *w, const char *f, ...)
{
	char b[256];
	va_list ap;
	va_start(ap, f);
	int k = vsnprintf(b, sizeof(b), f, ap);
	va_end(ap);
	if (k < 0 || k >= (int)sizeof(b)) { w->over = true; return; }
	w_raw(w, b, k);
}
// a JSON string with its quotes
static void w_esc(W *w, const char *s)
{
	w_raw(w, "\"", 1);
	for (const char *run = s; ; s++) {
		unsigned char c = *s;
		if (c >= 0x20 && c != '"' && c != '\\') continue;
		w_raw(w, run, s - run);
		if (!c) break;
		switch (c) {
		case '"': w_raw(w, "\\\"", 2); break;
		case '\\': w_raw(w, "\\\\", 2); break;
		case '\n': w_raw(w, "\\n", 2); break;
		case '\r': w_raw(w, "\\r", 2); break;
		case '\t': w_raw(w, "\\t", 2); break;
		default: w_fmt(w, "\\u%04x", c); break;
		}
		run = s + 1;
	}
	w_raw(w, "\"", 1);
}

static void w_base64(W *w, const unsigned char *d, int n)
{
	static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	if (w->len + (n + 2) / 3 * 4 >= w->n) { w->over = true; return; }
	char *o = w->p + w->len;
	int i = 0;
	for (; i + 2 < n; i += 3) {
		unsigned v = d[i] << 16 | d[i + 1] << 8 | d[i + 2];
		*o++ = t[v >> 18]; *o++ = t[(v >> 12) & 63]; *o++ = t[(v >> 6) & 63]; *o++ = t[v & 63];
	}
	if (i < n) {
		unsigned v = d[i] << 16 | (i + 1 < n ? d[i + 1] << 8 : 0);
		*o++ = t[v >> 18]; *o++ = t[(v >> 12) & 63];
		*o++ = i + 1 < n ? t[(v >> 6) & 63] : '=';
		*o++ = '=';
	}
	w->len = o - w->p;
	w->p[w->len] = 0;
}

// ---- the tool table ----

enum ArgType { A_DRIVE, A_SCREEN, A_BUTTON, A_KEY, A_VALUE, A_FROM, A_MAX, A_SOURCE, A_FILTER, A_OFFSET, A_LIMIT, A_ID, A_PATTERN, A_CONFIRM,
               A_SOURCE1 /* one source: not all */, A_EXPECT, A_QUERY };
struct Arg { const char *name; ArgType type; bool required; const char *desc; };
enum ToolKind { T_STATE, T_SHOT, T_DRIVES, T_LIST, T_SET, T_SCREEN, T_UI, T_LOG, T_GAMES, T_GAME, T_GACT };
#define NARGS 5   // a tool's arguments at most
struct Tool { const char *name; ToolKind kind; bool reads; const char *desc; Arg args[NARGS]; };

static const char *const k_screens[] = {"home", "settings", "drive_settings", "filename", "webhooks", "about", "audit_log", "remote_control", "games", NULL};
static const char *const k_buttons[] = {"cross", "circle", "triangle", "square", "up", "down", "left", "right", "start", "select", "l1", "r1", "l2", "r2", NULL};
// The game list's filter, as its panel has it: each criterion's values in the panel's order (the
// first is "any"; McpGameQuery has the index), and Show (games or the trash).
static const char *const k_f_format[] = {"any", "2352", "2048", NULL};
static const char *const k_f_redump[] = {"any", "verified", "unchecked", "no_match", "not_verified", NULL};
static const char *const k_f_errors[] = {"any", "with", "without", NULL};
static const char *const k_f_other[] = {"any", "broken", "duplicates", "sbi_missing", NULL};
static const char *const k_f_show[] = {"games", "trash", NULL};
static const struct { const char *name; const char *const *values; const char *desc; } k_filter[] = {
	{"format", k_f_format, "2352 (raw) or 2048 (data only) images"},
	{"redump", k_f_redump, "The Redump result: verified, not checked, does not match, or not verified (not checked or does not match)"},
	{"errors", k_f_errors, "With or without sectors the rip log still lists as wrong"},
	{"other", k_f_other, "Broken sets, duplicates, or LibCrypt games without their .sbi"},
	{"show", k_f_show, "The games (default) or the trash"}};

#define DRIVE_DESC "The drive: internal, or usbN by its port as the tabs show it"
#define DRIVE_ARG {"drive", A_DRIVE, true, DRIVE_DESC}
#define ID_ARG {"id", A_ID, true, "The game's id, as list_games gives it"}
#define SOURCE_DESC "The source: hdd, usbN (the USB drive's port) or all (default all)"
#define CHECK_POLL "Answers at once with started: poll get_state for the progress (screen game_check; it reads nothing), and call list_games for the result when it has ended."
static const Tool k_tools[] = {
	{"get_state", T_STATE, true, "What the screen shows: the screen, the selected tab and row, an open dialog with its buttons, and every drive's state.", {}},
	{"get_screenshot", T_SHOT, true, "A JPEG picture of the app's screen (system dialogs drawn by the console are not in it).", {}},
	{"get_drives", T_DRIVES, true, "Every drive with its disc and job, as status.json shows them.", {}},
	{"list_settings", T_LIST, true, "Every setting with its key, labels (en, sv), value, allowed values and scope; secret values are left out.", {}},
	{"set_setting", T_SET, false, "Changes a setting as its screen would (same checks and side effects) and returns the new value.", {
		{"key", A_KEY, true, "The setting's key, as list_settings gives it"},
		{"value", A_VALUE, true, "The new value, one of the setting's allowed values (any text for free-text settings); numbers and true/false are taken as their text (true = 1)"},
		{"drive", A_DRIVE, false, "The drive for a per-drive setting; leave out for the standard settings"}}},
	{"go_screen", T_SCREEN, false, "Opens one of the app's screens, the way the pad goes there, and returns the screen state. To change a setting use set_setting; to rip, check and so on use the action tools; for the game list, list_games, get_game and the game tools (check_game, rename_game, trash_game, ...).", {
		{"screen", A_SCREEN, true, "The screen to open"},
		{"drive", A_DRIVE, false, "The drive, only with drive_settings (and required there)"}}},
	{"go_tab", T_UI, false, "Shows a drive's tab on the home screen (going there first) and returns the screen state.", {DRIVE_ARG}},
	{"press_button", T_UI, false, "Fallback: presses one pad button in the app, as the pad would, and returns the screen state. Use it only to answer a dialog (cross = yes/OK, circle = no) or to move around a screen; rip, check, repair, cancel, skip_rereads, eject, set_setting and the game list's tools (check_game, check_games, cancel_check, rename_game, trash_game, restore_game, empty_trash) do their work directly. It never presses into the console's own on-screen keyboard, and circle on the home screen is refused (it would exit the app, and MCP with it).", {
		{"button", A_BUTTON, true, "The button"}}},
	{"rip", T_UI, false, "Starts ripping the disc in the drive (same checks as the pad, on its tab); returns the screen state or why not. Like the pad's cross, it replaces an existing rip at the drive's destination without asking. Use this instead of press_button.", {DRIVE_ARG}},
	{"repair", T_UI, false, "Re-reads the sectors the drive's rip lists as bad (same checks as the pad, on its tab); returns the screen state or why not. Use this instead of press_button.", {DRIVE_ARG}},
	{"check", T_UI, false, "Checks the drive's rip against Redump (same checks as the pad, on its tab); returns the screen state or why not. Use this instead of press_button.", {DRIVE_ARG}},
	{"cancel", T_UI, false, "Cancels the drive's running job: this call is the confirmation the pad asks for, and it is logged. Returns the screen state or why not. Use this instead of press_button.", {DRIVE_ARG}},
	{"skip_rereads", T_UI, false, "Stops re-reading the drive's bad sectors and finishes the rip with what it has; returns the screen state or why not. Use this instead of press_button.", {DRIVE_ARG}},
	{"eject", T_UI, false, "Ejects the drive's disc (same checks as the pad: not while a job runs on it); returns the screen state or why not. Use this instead of press_button.", {DRIVE_ARG}},
	{"read_log", T_LOG, true, "Lines of the app's log (psxrip.log) and the number of the next line.", {
		{"from_line", A_FROM, false, "The first line (0 = the log's first); leave out for the last lines"},
		{"max_lines", A_MAX, false, "How many lines, at most 500 (default 100)"}}},
	{"list_games", T_GAMES, true, "The game list: every game image in the PSXISO folder of the hard disk and the USB drives (not only the app's rips), "
		"read on its own without touching the screen. Gives the summary line's counts, the sources, and the games in alphabetical order, each with its "
		"id, name, disc ID, format, tracks, status, errors, Redump name, place, marks (duplicate, .sbi missing, in the trash) and the discs of a "
		"multi-disc game; and a game-list check's last result (to follow a running check, poll get_state instead: list_games may read a USB drive again). "
		"Page with offset and limit: next_offset is the next page's offset (null: no more); a page too large for one answer is cut there (cut: true). "
		"Use this instead of press_button on the games screen.", {
		{"source", A_SOURCE, false, SOURCE_DESC},
		{"filter", A_FILTER, false, "The filter panel's criteria; all must match (each one left out: any)"},
		{"query", A_QUERY, false, "Only games whose name has this text, in any case (A-Z; other letters as they are)"},
		{"offset", A_OFFSET, false, "The first game to give (0 = the first; default 0)"},
		{"limit", A_LIMIT, false, "How many games, at most 200 (default 50)"}}},
	{"get_game", T_GAME, true, "One game as its detail view shows it, per disc: status, format, disc ID, Redump name, when and on which drive it was ripped, "
		"size, place, cover, last check, LibCrypt, duplicate, tracks with their MD5, sectors with errors and its files. Read on its own without touching the screen.", {ID_ARG}},
	{"check_game", T_GACT, false, "Checks the game's discs against Redump from their files, as square in the game list does (Redump must be on; not a 2048 image, "
		"a broken set or a game a drive works on). " CHECK_POLL " Logged in the audit log. Use this instead of press_button.", {ID_ARG}},
	{"check_games", T_GACT, false, "Checks every game of a source and filter that has no Redump result yet and can be checked, as START in the game list does. "
		CHECK_POLL " Use this instead of press_button.", {
		{"source", A_SOURCE, false, SOURCE_DESC},
		{"filter", A_FILTER, false, "The filter panel's criteria; all must match (each one left out: any)"}}},
	{"cancel_check", T_GACT, false, "Stops the game list's running check, as circle on its progress screen does: the game being checked and those not "
		"reached yet keep the result they had. Use this instead of press_button.", {}},
	{"rename_game", T_GACT, false, "Renames every file of the game (all its discs, the .cue's FILE lines too) by a file name pattern, as START in the rename "
		"view does, with its checks: refused when the name stays the same, a game of that name is there, or a drive or a check uses the game. Logged in the "
		"audit log and undoable there. Returns the new names and ids. Use this instead of press_button.", {ID_ARG,
		{"pattern", A_PATTERN, true, "The pattern: %T the title, %N Redump's name, %D the disc ID, %R the region, other characters as they are "
			"(as the File name setting); empty = %T [%D]"}}},
	{"trash_game", T_GACT, false, "Moves the game, every file of it, to the trash of its source (PSXISO/.trash), as triangle in the game list does. Logged in "
		"the audit log and undoable there. Use this instead of press_button.", {ID_ARG}},
	{"restore_game", T_GACT, false, "Moves a game in the trash back to its folder, as triangle in the trash does. Use this instead of press_button.", {ID_ARG}},
	{"empty_trash", T_GACT, false, "Deletes every game in the trash of one source (never all at once), as START and yes in the trash do. This is final: "
		"the files cannot be restored, not even from the audit log. Refused unless confirm is true. List that trash first (list_games with the source "
		"and filter show: trash) and pass what you saw as expect: when the trash holds anything else by then, nothing is deleted. "
		"Use this instead of press_button.", {
		{"source", A_SOURCE1, true, "The source whose trash is emptied: hdd or usbN (the USB drive's port)"},
		{"confirm", A_CONFIRM, false, "Must be true: emptying the trash is final"},
		{"expect", A_EXPECT, false, "What list_games showed in that trash: the number of its games, or the id of every disc in it (a game's id, and for a game of several discs each id in its discs list; any order)"}}},
};
static const int k_ntools = sizeof(k_tools) / sizeof(k_tools[0]);

static void w_enum(W *w, const char *const *list)
{
	w_str(w, "\"enum\":[");
	for (int i = 0; list[i]; i++) { if (i) w_str(w, ","); w_esc(w, list[i]); }
	w_str(w, "]");
}

static void w_tools_list(W *w)
{
	w_str(w, "{\"tools\":[");
	for (int i = 0; i < k_ntools; i++) {
		const Tool &t = k_tools[i];
		if (i) w_str(w, ",");
		w_fmt(w, "{\"name\":\"%s\",\"description\":", t.name);
		w_esc(w, t.desc);
		w_str(w, ",\"inputSchema\":{\"type\":\"object\",\"properties\":{");
		int nreq = 0;
		for (int a = 0; a < NARGS && t.args[a].name; a++) {
			const Arg &g = t.args[a];
			if (a) w_str(w, ",");
			w_fmt(w, "\"%s\":{", g.name);
			switch (g.type) {
			case A_DRIVE: w_str(w, "\"type\":\"string\",\"pattern\":\"^(internal|usb[0-9]{1,3})$\""); break;
			case A_SCREEN: w_str(w, "\"type\":\"string\","); w_enum(w, k_screens); break;
			case A_BUTTON: w_str(w, "\"type\":\"string\","); w_enum(w, k_buttons); break;
			case A_KEY: w_str(w, "\"type\":\"string\",\"minLength\":1"); break;
			case A_VALUE: w_str(w, "\"type\":[\"string\",\"number\",\"boolean\"]"); break;
			case A_FROM: w_str(w, "\"type\":\"integer\",\"minimum\":0"); break;
			case A_MAX: w_fmt(w, "\"type\":\"integer\",\"minimum\":1,\"maximum\":%d", MCP_MAX_LOG_LINES); break;
			case A_SOURCE: w_str(w, "\"type\":\"string\",\"pattern\":\"^(all|hdd|usb[0-7])$\""); break;
			case A_SOURCE1: w_str(w, "\"type\":\"string\",\"pattern\":\"^(hdd|usb[0-7])$\""); break;
			case A_EXPECT: w_str(w, "\"type\":[\"integer\",\"array\"],\"minimum\":0,\"items\":{\"type\":\"string\",\"minLength\":1}"); break;
			case A_QUERY: w_fmt(w, "\"type\":\"string\",\"maxLength\":%d", MCP_MAX_QUERY); break;
			case A_FILTER:   // (no additionalProperties of its own: the tool's arguments have the one)
				w_str(w, "\"type\":\"object\",\"properties\":{");
				for (size_t f = 0; f < sizeof(k_filter) / sizeof(k_filter[0]); f++) {
					w_fmt(w, "%s\"%s\":{\"type\":\"string\",", f ? "," : "", k_filter[f].name);
					w_enum(w, k_filter[f].values);
					w_str(w, ",\"description\":");
					w_esc(w, k_filter[f].desc);
					w_str(w, "}");
				}
				w_str(w, "}");
				break;
			case A_OFFSET: w_str(w, "\"type\":\"integer\",\"minimum\":0"); break;
			case A_LIMIT: w_fmt(w, "\"type\":\"integer\",\"minimum\":1,\"maximum\":%d", MCP_MAX_GAMES); break;
			case A_ID: w_str(w, "\"type\":\"string\",\"minLength\":1"); break;
			case A_PATTERN: w_str(w, "\"type\":\"string\",\"maxLength\":95"); break;
			case A_CONFIRM: w_str(w, "\"type\":\"boolean\""); break;
			}
			w_str(w, ",\"description\":");
			w_esc(w, g.desc);
			w_str(w, "}");
			nreq += g.required;
		}
		w_str(w, "}");
		if (nreq) {
			w_str(w, ",\"required\":[");
			for (int a = 0, k = 0; a < NARGS && t.args[a].name; a++)
				if (t.args[a].required) w_fmt(w, "%s\"%s\"", k++ ? "," : "", t.args[a].name);
			w_str(w, "]");
		}
		w_fmt(w, ",\"additionalProperties\":false},\"annotations\":{\"readOnlyHint\":%s}}", t.reads ? "true" : "false");
	}
	w_str(w, "]}");
}

// ---- checking a call's arguments ----

struct Err { int code; char field[MAX_KEY]; char msg[200]; };

static int set_err(Err *er, int code, const char *field, const char *fmt, ...)
{
	er->code = code;
	snprintf(er->field, sizeof(er->field), "%s", field ? field : "");
	char m[160];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(m, sizeof(m), fmt, ap);
	va_end(ap);
	if (code == -32602 && field) snprintf(er->msg, sizeof(er->msg), "Invalid params: %s: %s", er->field, m);
	else snprintf(er->msg, sizeof(er->msg), "%s", m);
	return -1;
}

static bool in_list(const char *s, const char *const *list)
{
	for (int i = 0; list[i]; i++) if (!strcmp(s, list[i])) return true;
	return false;
}

static bool valid_drive(const char *s)
{
	if (!strcmp(s, "internal")) return true;
	if (strncmp(s, "usb", 3)) return false;
	int k = strlen(s + 3);
	if (k < 1 || k > 3) return false;
	for (const char *p = s + 3; *p; p++) if (*p < '0' || *p > '9') return false;
	return true;
}

struct Args {
	char str[NARGS][MAX_VALUE];   // the string arguments, by their place in the tool's table
	bool given[NARGS];
	int num[NARGS];
	McpGameQuery q;               // the game list's source, filter, offset and limit
};

static bool valid_source(const char *s)
{
	return !strcmp(s, "all") || !strcmp(s, "hdd") || (!strncmp(s, "usb", 3) && s[3] >= '0' && s[3] <= '7' && !s[4]);
}

// The filter object (at '{'), as the panel's criteria into q; -1 with the error.
static int check_filter(const char *v, const char *e, McpGameQuery *q, Err *er)
{
	if (*v != '{') return set_err(er, -32602, "filter", "must be an object");
	int *crit[] = {&q->fmt, &q->redump, &q->errors, &q->other, &q->trash};
	unsigned seen = 0;
	JIt it = j_members(v, e);
	const char *k, *fv;
	while (j_next(&it, &k, &fv)) {
		char name[MAX_KEY], val[MAX_KEY];
		int c = -1;
		if (j_decode(k, e, name, sizeof(name)) >= 0)
			for (size_t i = 0; i < sizeof(k_filter) / sizeof(k_filter[0]); i++) if (!strcmp(name, k_filter[i].name)) c = (int)i;
		if (c < 0) return set_err(er, -32602, "filter", "unknown criterion (format, redump, errors, other, show)");
		if (seen >> c & 1) return set_err(er, -32602, "filter", "%s given twice", k_filter[c].name);
		seen |= 1u << c;
		int x = -1;
		if (*fv == '"' && j_decode(fv, e, val, sizeof(val)) >= 0)
			for (int i = 0; k_filter[c].values[i]; i++) if (!strcmp(val, k_filter[c].values[i])) x = i;
		if (x < 0) {
			char al[120] = "";
			for (int i = 0; k_filter[c].values[i]; i++) snprintf(al + strlen(al), sizeof(al) - strlen(al), "%s%s", i ? ", " : "", k_filter[c].values[i]);
			return set_err(er, -32602, "filter", "%s must be one of %s", k_filter[c].name, al);
		}
		*crit[c] = x;
	}
	return 0;
}

// empty_trash's expect (at its value): a count (an integer of 0 or more) or the games' ids (an array
// of non-empty strings), into q; -1 with the error.
static int check_expect(const char *v, const char *e, McpGameQuery *q, Err *er)
{
	if (*v == '[') {
		q->expect = 0;
		q->expect_ids = 1;
		q->expect_hash = 0;
		for (const char *p = j_ws(v + 1, e); p < e && *p != ']'; ) {
			char id[MAX_VALUE];
			int len = *p == '"' ? j_decode(p, e, id, sizeof(id)) : -1;
			if (len <= 0 || (int)strlen(id) != len) return set_err(er, -32602, "expect", "the ids must be non-empty strings (as list_games gives them)");
			q->expect++;
			q->expect_hash += mcp_id_hash(id);
			p = j_ws(j_skip(p, e), e);
			if (p < e && *p == ',') p = j_ws(p + 1, e);
		}
		return 0;
	}
	const char *end = j_skip(v, e);
	int n = 0;
	bool ok = *v >= '0' && *v <= '9' && end - v <= 9;
	for (const char *p = v; ok && p < end; p++) {
		if (*p < '0' || *p > '9') ok = false;
		else n = n * 10 + (*p - '0');
	}
	if (!ok) return set_err(er, -32602, "expect", "must be the number of games (0 or more) or their ids");
	q->expect = n;
	q->expect_ids = 0;
	return 0;
}

static int check_args(const Tool &t, const char *obj, const char *e, Args *a, Err *er)
{
	memset(a->given, 0, sizeof(a->given));
	memset(&a->q, 0, sizeof(a->q));
	a->q.limit = MCP_DEFAULT_GAMES;
	a->q.expect = -1;
	if (obj) {
		// every key must be one of the tool's, once
		JIt it = j_members(obj, e);
		const char *k, *v;
		while (j_next(&it, &k, &v)) {
			bool known = false;
			for (int i = 0; i < NARGS && t.args[i].name; i++) if (j_is(k, e, t.args[i].name)) known = true;
			if (!known) {
				char name[MAX_KEY];
				if (j_decode(k, e, name, sizeof(name)) < 0) snprintf(name, sizeof(name), "(long name)");
				return set_err(er, -32602, name, "unknown argument");
			}
		}
	}
	for (int i = 0; i < NARGS && t.args[i].name; i++) {
		const Arg &g = t.args[i];
		bool dup = false;
		const char *v = obj ? j_get(obj, e, g.name, &dup) : NULL;
		if (dup) return set_err(er, -32602, g.name, "given twice");
		if (!v) {
			if (g.required) return set_err(er, -32602, g.name, "required");
			continue;
		}
		a->given[i] = true;
		if (g.type == A_FROM || g.type == A_MAX || g.type == A_OFFSET || g.type == A_LIMIT) {
			// a plain non-negative integer: no sign, fraction or exponent
			const char *end = j_skip(v, e);
			int n = 0, top = g.type == A_MAX ? MCP_MAX_LOG_LINES : MCP_MAX_GAMES;
			bool ok = *v >= '0' && *v <= '9' && end - v <= 9, ranged = g.type == A_MAX || g.type == A_LIMIT;
			for (const char *p = v; ok && p < end; p++) {
				if (*p < '0' || *p > '9') ok = false;
				else n = n * 10 + (*p - '0');
			}
			if (ranged && ok && (n < 1 || n > top)) ok = false;
			if (!ok) return ranged ? set_err(er, -32602, g.name, "must be an integer from 1 to %d", top)
			                       : set_err(er, -32602, g.name, "must be an integer of 0 or more");
			a->num[i] = n;
			if (g.type == A_OFFSET) a->q.offset = n;
			if (g.type == A_LIMIT) a->q.limit = n;
			continue;
		}
		if (g.type == A_FILTER) {
			if (check_filter(v, e, &a->q, er)) return -1;
			continue;
		}
		if (g.type == A_EXPECT) {
			if (check_expect(v, e, &a->q, er)) return -1;
			continue;
		}
		if (g.type == A_CONFIRM) {
			if (*v != 't' && *v != 'f') return set_err(er, -32602, g.name, "must be true or false");
			a->num[i] = *v == 't';
			continue;
		}
		if (g.type == A_VALUE && *v != '"') {
			// agents often send 4 or true for "4" or "1": the config's text form (on/off keys are 1/0);
			// a number's text goes on as it was sent, the app's own checks decide
			const char *end = j_skip(v, e);
			if (*v == 't') snprintf(a->str[i], MAX_VALUE, "1");
			else if (*v == 'f') snprintf(a->str[i], MAX_VALUE, "0");
			else if (*v == '-' || (*v >= '0' && *v <= '9')) {
				if (end - v >= MAX_VALUE) return set_err(er, -32602, g.name, "too long");
				memcpy(a->str[i], v, end - v);
				a->str[i][end - v] = 0;
			} else return set_err(er, -32602, g.name, "must be a string, a number or true/false");
			continue;
		}
		if (*v != '"') return set_err(er, -32602, g.name, "must be a string");
		int len = j_decode(v, e, a->str[i], g.type == A_VALUE || g.type == A_ID || g.type == A_PATTERN || g.type == A_QUERY ? MAX_VALUE : MAX_KEY);
		if (len < 0) return set_err(er, -32602, g.name, "too long");
		if ((int)strlen(a->str[i]) != len || strpbrk(a->str[i], "\r\n"))
			return set_err(er, -32602, g.name, "must not contain line breaks or NUL");
		switch (g.type) {
		case A_DRIVE:
			if (!valid_drive(a->str[i])) return set_err(er, -32602, g.name, "must be internal or usbN (N the port)");
			break;
		case A_SCREEN:
			if (!in_list(a->str[i], k_screens)) return set_err(er, -32602, g.name, "must be one of home, settings, drive_settings, filename, webhooks, about, audit_log, remote_control, games");
			break;
		case A_BUTTON:
			if (!in_list(a->str[i], k_buttons)) return set_err(er, -32602, g.name, "must be one of cross, circle, triangle, square, up, down, left, right, start, select, l1, r1, l2, r2");
			break;
		case A_KEY: case A_ID:
			if (!len) return set_err(er, -32602, g.name, "must not be empty");
			break;
		case A_SOURCE:
			if (!valid_source(a->str[i])) return set_err(er, -32602, g.name, "must be all, hdd or usbN (N the USB drive's port, 0 to 7)");
			snprintf(a->q.source, sizeof(a->q.source), "%s", a->str[i]);
			break;
		case A_SOURCE1:   // (a trash is emptied one source at a time: the one the agent listed)
			if (strcmp(a->str[i], "all") && valid_source(a->str[i])) { snprintf(a->q.source, sizeof(a->q.source), "%s", a->str[i]); break; }
			return set_err(er, -32602, g.name, "must be one source, hdd or usbN (N the USB drive's port, 0 to 7): a trash is emptied one source at a time");
		case A_QUERY:
			if (len > MCP_MAX_QUERY) return set_err(er, -32602, g.name, "too long (at most %d bytes)", MCP_MAX_QUERY);
			snprintf(a->q.query, sizeof(a->q.query), "%s", a->str[i]);
			break;
		default: break;
		}
	}
	if (t.kind == T_SCREEN) {
		bool ds = !strcmp(a->str[0], "drive_settings");
		if (ds && !a->given[1]) return set_err(er, -32602, "drive", "required for drive_settings");
		if (!ds && a->given[1]) return set_err(er, -32602, "drive", "only for drive_settings");
	}
	return 0;
}

// ---- tools/call ----

static void w_text_result(W *w, const char *text, bool error)
{
	w_str(w, "{\"content\":[{\"type\":\"text\",\"text\":");
	w_esc(w, text);
	w_fmt(w, "}],\"isError\":%s}", error ? "true" : "false");
}

// Writes the call's result into w, or returns -1 with a protocol error in er.
static int tools_call(const char *params, const char *e, W *w, Err *er)
{
	if (!params || *params != '{') return set_err(er, -32602, "params", "must be an object with the tool's name");
	const char *nm = j_get(params, e, "name");
	if (!nm || *nm != '"') return set_err(er, -32602, "name", "the tool's name (a string) is required");
	char name[MAX_KEY];
	int nlen = j_decode(nm, e, name, sizeof(name));
	// a NUL inside (\u0000) would cut the name short: such a name is no tool's
	if (nlen < 0 || (int)strlen(name) != nlen) return set_err(er, -32602, "name", "unknown tool");
	const Tool *t = NULL;
	for (int i = 0; i < k_ntools; i++) if (!strcmp(k_tools[i].name, name)) t = &k_tools[i];
	if (!t) return set_err(er, -32602, "name", "unknown tool %s", name);
	const char *args = j_get(params, e, "arguments");
	if (args && *args == 'n') args = NULL;   // null: no arguments
	if (args && *args != '{') return set_err(er, -32602, "arguments", "must be an object");
	Args *a = (Args *)malloc(sizeof(Args));
	if (!a) return set_err(er, -32603, NULL, "Out of memory");
	if (check_args(*t, args, e, a, er)) { free(a); return -1; }

	if (t->kind == T_SHOT) {
		unsigned char *jpg = NULL;
		int len = 0;
		if (!g_hooks.screenshot_jpeg) w_text_result(w, "This tool is not available.", true);
		else if (g_hooks.screenshot_jpeg(&jpg, &len) || !jpg || len <= 0) w_text_result(w, "No screenshot could be taken.", true);
		else {
			w_str(w, "{\"content\":[{\"type\":\"image\",\"data\":\"");
			w_base64(w, jpg, len);
			w_str(w, "\",\"mimeType\":\"image/jpeg\"}],\"isError\":false}");
		}
		free(jpg);
		free(a);
		return 0;
	}

	// the hook's answer goes into a buffer as large as the response, then escaped into it
	int n = w->n > 1 ? w->n : 1;
	char *buf = (char *)malloc(n);
	if (!buf) { free(a); return set_err(er, -32603, NULL, "Out of memory"); }
	buf[0] = 0;
	int r = -1;
	bool have = true;
	switch (t->kind) {
	case T_STATE: if ((have = g_hooks.state)) r = g_hooks.state(buf, n); break;
	case T_DRIVES: if ((have = g_hooks.drives)) r = g_hooks.drives(buf, n); break;
	case T_LIST: if ((have = g_hooks.list_settings)) r = g_hooks.list_settings(buf, n); break;
	case T_SET:
		if ((have = g_hooks.set_setting)) r = g_hooks.set_setting(a->str[0], a->str[1], a->given[2] ? a->str[2] : NULL, buf, n);
		break;
	case T_SCREEN:
		if ((have = g_hooks.ui)) {
			char arg[MAX_KEY * 2];
			if (a->given[1]) snprintf(arg, sizeof(arg), "%s:%s", a->str[0], a->str[1]);
			else snprintf(arg, sizeof(arg), "%s", a->str[0]);
			r = g_hooks.ui(t->name, arg, buf, n);
		}
		break;
	case T_UI: if ((have = g_hooks.ui)) r = g_hooks.ui(t->name, a->str[0], buf, n); break;
	case T_LOG:
		if ((have = g_hooks.read_log))
			r = g_hooks.read_log(a->given[0] ? a->num[0] : -1, a->given[1] ? a->num[1] : MCP_DEFAULT_LOG_LINES, buf, n);
		break;
	case T_GAMES: if ((have = g_hooks.list_games)) r = g_hooks.list_games(&a->q, buf, n); break;
	case T_GAME: if ((have = g_hooks.get_game)) r = g_hooks.get_game(a->str[0], buf, n); break;
	case T_GACT: {
		if (!(have = g_hooks.game)) break;
		const char *id = NULL, *pat = NULL;
		bool confirmed = false;
		for (int i = 0; i < NARGS && t->args[i].name; i++) {
			if (t->args[i].type == A_ID) id = a->str[i];
			if (t->args[i].type == A_PATTERN) pat = a->str[i];
			if (t->args[i].type == A_CONFIRM) confirmed = a->given[i] && a->num[i];
		}
		if (!strcmp(t->name, "empty_trash") && !confirmed) {   // final: never on a call that did not say so
			snprintf(buf, n, "Not emptied: emptying the trash deletes its games' files for good (they cannot be restored, not even from the audit log). "
			         "Call empty_trash again with confirm: true to empty it.");
			r = -1;
			break;
		}
		r = g_hooks.game(t->name, id, pat, &a->q, buf, n);
		break;
	}
	default: break;
	}
	buf[n - 1] = 0;
	if (!have) w_text_result(w, "This tool is not available.", true);
	else if (r) w_text_result(w, buf[0] ? buf : "Refused.", true);
	else w_text_result(w, buf, false);
	free(buf);
	free(a);
	return 0;
}

// ---- one request ----

static int finish_error(char *out, int n, const char *id, const Err *er, int status)
{
	W w = {out, n, 0, false};
	w_fmt(&w, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"error\":{\"code\":%d,\"message\":", id ? id : "null", er->code);
	w_esc(&w, er->msg);
	if (er->field[0]) { w_str(&w, ",\"data\":{\"field\":"); w_esc(&w, er->field); w_str(&w, "}"); }
	w_str(&w, "}}");
	if (w.over && n > 0) out[0] = 0;
	return status;
}

int mcp_handle(const char *body, int len, char *out, int n)
{
	if (n > 0) out[0] = 0;
	Err er;
	er.field[0] = 0;
	if (!body || len <= 0) { set_err(&er, -32700, NULL, "Parse error: empty request"); return finish_error(out, n, NULL, &er, 400); }
	if (len > MCP_MAX_REQUEST) {
		set_err(&er, -32600, NULL, "Request too large (at most %d bytes)", MCP_MAX_REQUEST);
		return finish_error(out, n, NULL, &er, 413);
	}
	const char *e = body + len;
	const char *p = j_ws(body, e);
	const char *end = j_value(p, e, 1);
	if (!end || j_ws(end, e) != e) { set_err(&er, -32700, NULL, "Parse error: not valid JSON (or nested deeper than %d)", MAX_DEPTH); return finish_error(out, n, NULL, &er, 400); }
	if (*p == '[') { set_err(&er, -32600, NULL, "Invalid request: batches are not supported"); return finish_error(out, n, NULL, &er, 400); }
	if (*p != '{') { set_err(&er, -32600, NULL, "Invalid request: not an object"); return finish_error(out, n, NULL, &er, 400); }

	// the id, echoed as it was sent; a message without one (a notification or a response) gets 202
	const char *idv = j_get(p, e, "id");
	if (!idv) return 202;
	char id[MAX_ID + 1];
	const char *idend = j_skip(idv, e);
	if ((*idv != '"' && *idv != '-' && (*idv < '0' || *idv > '9')) || idend - idv > MAX_ID) {
		set_err(&er, -32600, NULL, "Invalid request: the id must be a string or a number (at most %d characters)", MAX_ID);
		return finish_error(out, n, NULL, &er, 400);
	}
	memcpy(id, idv, idend - idv);
	id[idend - idv] = 0;

	const char *ver = j_get(p, e, "jsonrpc");
	if (!ver || *ver != '"' || !j_is(ver, e, "2.0")) { set_err(&er, -32600, NULL, "Invalid request: jsonrpc must be \"2.0\""); return finish_error(out, n, id, &er, 400); }
	const char *mv = j_get(p, e, "method");
	if (!mv) {
		if (j_get(p, e, "result") || j_get(p, e, "error")) return 202;
		set_err(&er, -32600, NULL, "Invalid request: no method");
		return finish_error(out, n, id, &er, 400);
	}
	if (*mv != '"') { set_err(&er, -32600, NULL, "Invalid request: the method must be a string"); return finish_error(out, n, id, &er, 400); }
	char method[MAX_KEY];
	int mlen = j_decode(mv, e, method, sizeof(method));
	if (mlen < 0 || (int)strlen(method) != mlen) method[0] = 0;   // too long, or a NUL inside: no method's name
	const char *params = j_get(p, e, "params");

	W w = {out, n, 0, false};
	w_fmt(&w, "{\"jsonrpc\":\"2.0\",\"id\":%s,\"result\":", id);
	if (!strcmp(method, "initialize")) {
		w_str(&w, "{\"protocolVersion\":\"" MCP_PROTOCOL_VERSION "\",\"capabilities\":{\"tools\":{}},\"serverInfo\":{\"name\":\"psx-ripper\",\"version\":");
		w_esc(&w, g_version);
		w_str(&w, "},\"instructions\":\"PSX RiPPER on a PS3. get_* tools, list_settings and read_log only read; "
		          "the actions (rip, check, repair, cancel, skip_rereads, eject) and set_setting do the work directly; list_games and get_game read the game list, "
		          "and check_game, check_games, cancel_check, rename_game, trash_game, restore_game and empty_trash change it as its screen would; go_* and press_button "
		          "move around the screens and answer dialogs. Each tool that changes the screen returns the screen state.\"}");
	} else if (!strcmp(method, "ping")) {
		w_str(&w, "{}");
	} else if (!strcmp(method, "tools/list")) {
		w_tools_list(&w);
	} else if (!strcmp(method, "tools/call")) {
		if (tools_call(params, e, &w, &er)) return finish_error(out, n, id, &er, 200);
	} else {
		set_err(&er, -32601, NULL, "Method not found: %s", method[0] ? method : "(unreadable)");
		return finish_error(out, n, id, &er, 200);
	}
	w_str(&w, "}");
	if (w.over) {
		set_err(&er, -32603, NULL, "Response too large");
		return finish_error(out, n, id, &er, 200);
	}
	return 200;
}
