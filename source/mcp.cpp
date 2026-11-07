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

enum ArgType { A_DRIVE, A_SCREEN, A_BUTTON, A_KEY, A_VALUE, A_FROM, A_MAX };
struct Arg { const char *name; ArgType type; bool required; const char *desc; };
enum ToolKind { T_STATE, T_SHOT, T_DRIVES, T_LIST, T_SET, T_SCREEN, T_UI, T_LOG };
struct Tool { const char *name; ToolKind kind; bool reads; const char *desc; Arg args[3]; };

static const char *const k_screens[] = {"home", "settings", "drive_settings", "filename", "webhooks", "about", "audit_log", "remote_control", NULL};
static const char *const k_buttons[] = {"cross", "circle", "triangle", "square", "up", "down", "left", "right", "start", "select", "l1", "r1", "l2", "r2", NULL};

#define DRIVE_DESC "The drive: internal, or usbN by its port as the tabs show it"
#define DRIVE_ARG {"drive", A_DRIVE, true, DRIVE_DESC}
static const Tool k_tools[] = {
	{"get_state", T_STATE, true, "What the screen shows: the screen, the selected tab and row, an open dialog with its buttons, and every drive's state.", {}},
	{"get_screenshot", T_SHOT, true, "A JPEG picture of the app's screen (system dialogs drawn by the console are not in it).", {}},
	{"get_drives", T_DRIVES, true, "Every drive with its disc and job, as status.json shows them.", {}},
	{"list_settings", T_LIST, true, "Every setting with its key, labels (en, sv), value, allowed values and scope; secret values are left out.", {}},
	{"set_setting", T_SET, false, "Changes a setting as its screen would (same checks and side effects) and returns the new value.", {
		{"key", A_KEY, true, "The setting's key, as list_settings gives it"},
		{"value", A_VALUE, true, "The new value, one of the setting's allowed values (any text for free-text settings); numbers and true/false are taken as their text (true = 1)"},
		{"drive", A_DRIVE, false, "The drive for a per-drive setting; leave out for the standard settings"}}},
	{"go_screen", T_SCREEN, false, "Opens one of the app's screens, the way the pad goes there, and returns the screen state. To change a setting use set_setting; to rip, check and so on use the action tools.", {
		{"screen", A_SCREEN, true, "The screen to open"},
		{"drive", A_DRIVE, false, "The drive, only with drive_settings (and required there)"}}},
	{"go_tab", T_UI, false, "Shows a drive's tab on the home screen (going there first) and returns the screen state.", {DRIVE_ARG}},
	{"press_button", T_UI, false, "Fallback: presses one pad button in the app, as the pad would, and returns the screen state. Use it only to answer a dialog (cross = yes/OK, circle = no) or to move around a screen; rip, check, repair, cancel, skip_rereads, eject and set_setting do their work directly. It never presses into the console's own on-screen keyboard, and circle on the home screen is refused (it would exit the app, and MCP with it).", {
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
		for (int a = 0; a < 3 && t.args[a].name; a++) {
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
			}
			w_str(w, ",\"description\":");
			w_esc(w, g.desc);
			w_str(w, "}");
			nreq += g.required;
		}
		w_str(w, "}");
		if (nreq) {
			w_str(w, ",\"required\":[");
			for (int a = 0, k = 0; a < 3 && t.args[a].name; a++)
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
	char str[3][MAX_VALUE];   // the string arguments, by their place in the tool's table
	bool given[3];
	int num[3];
};

static int check_args(const Tool &t, const char *obj, const char *e, Args *a, Err *er)
{
	memset(a->given, 0, sizeof(a->given));
	if (obj) {
		// every key must be one of the tool's, once
		JIt it = j_members(obj, e);
		const char *k, *v;
		while (j_next(&it, &k, &v)) {
			bool known = false;
			for (int i = 0; i < 3 && t.args[i].name; i++) if (j_is(k, e, t.args[i].name)) known = true;
			if (!known) {
				char name[MAX_KEY];
				if (j_decode(k, e, name, sizeof(name)) < 0) snprintf(name, sizeof(name), "(long name)");
				return set_err(er, -32602, name, "unknown argument");
			}
		}
	}
	for (int i = 0; i < 3 && t.args[i].name; i++) {
		const Arg &g = t.args[i];
		bool dup = false;
		const char *v = obj ? j_get(obj, e, g.name, &dup) : NULL;
		if (dup) return set_err(er, -32602, g.name, "given twice");
		if (!v) {
			if (g.required) return set_err(er, -32602, g.name, "required");
			continue;
		}
		a->given[i] = true;
		if (g.type == A_FROM || g.type == A_MAX) {
			// a plain non-negative integer: no sign, fraction or exponent
			const char *end = j_skip(v, e);
			int n = 0;
			bool ok = *v >= '0' && *v <= '9' && end - v <= 9;
			for (const char *p = v; ok && p < end; p++) {
				if (*p < '0' || *p > '9') ok = false;
				else n = n * 10 + (*p - '0');
			}
			if (g.type == A_MAX && ok && (n < 1 || n > MCP_MAX_LOG_LINES)) ok = false;
			if (!ok) return g.type == A_MAX ? set_err(er, -32602, g.name, "must be an integer from 1 to %d", MCP_MAX_LOG_LINES)
			                                 : set_err(er, -32602, g.name, "must be an integer of 0 or more");
			a->num[i] = n;
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
		int len = j_decode(v, e, a->str[i], g.type == A_VALUE ? MAX_VALUE : MAX_KEY);
		if (len < 0) return set_err(er, -32602, g.name, "too long");
		if ((int)strlen(a->str[i]) != len || strpbrk(a->str[i], "\r\n"))
			return set_err(er, -32602, g.name, "must not contain line breaks or NUL");
		switch (g.type) {
		case A_DRIVE:
			if (!valid_drive(a->str[i])) return set_err(er, -32602, g.name, "must be internal or usbN (N the port)");
			break;
		case A_SCREEN:
			if (!in_list(a->str[i], k_screens)) return set_err(er, -32602, g.name, "must be one of home, settings, drive_settings, filename, webhooks, about, audit_log, remote_control");
			break;
		case A_BUTTON:
			if (!in_list(a->str[i], k_buttons)) return set_err(er, -32602, g.name, "must be one of cross, circle, triangle, square, up, down, left, right, start, select, l1, r1, l2, r2");
			break;
		case A_KEY:
			if (!len) return set_err(er, -32602, g.name, "must not be empty");
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
		          "the actions (rip, check, repair, cancel, skip_rereads, eject) and set_setting do the work directly; go_* and press_button "
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
