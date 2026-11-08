// Host tests of the MCP request handling (source/mcp.cpp). Run: make -C tests/host
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include "../../include/mcp.h"

static int g_fail;
#define CHECK(c) do { if (!(c)) { printf("  %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; bad = 1; } } while (0)

// ---- an independent JSON checker (so the module's own reader does not judge its output) ----
static const char *jv_value(const char *p, int depth);
static const char *jv_ws(const char *p) { while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') p++; return p; }
static const char *jv_string(const char *p)
{
	if (*p++ != '"') return NULL;
	for (;;) {
		unsigned char c = *p++;
		if (!c || c < 0x20) return NULL;
		if (c == '"') return p;
		if (c == '\\') {
			c = *p++;
			if (c == 'u') { for (int i = 0; i < 4; i++) if (!isxdigit((unsigned char)*p++)) return NULL; }
			else if (!strchr("\"\\/bfnrt", c) || !c) return NULL;
		}
	}
}
static const char *jv_value(const char *p, int depth)
{
	if (depth > 64) return NULL;
	p = jv_ws(p);
	if (*p == '"') return jv_string(p);
	if (*p == '{' || *p == '[') {
		char close = *p == '{' ? '}' : ']';
		p = jv_ws(p + 1);
		if (*p == close) return p + 1;
		for (;;) {
			if (close == '}') { p = jv_string(jv_ws(p)); if (!p) return NULL; p = jv_ws(p); if (*p++ != ':') return NULL; }
			p = jv_value(p, depth + 1);
			if (!p) return NULL;
			p = jv_ws(p);
			if (*p == ',') { p++; continue; }
			if (*p == close) return p + 1;
			return NULL;
		}
	}
	if (!strncmp(p, "true", 4)) return p + 4;
	if (!strncmp(p, "false", 5)) return p + 5;
	if (!strncmp(p, "null", 4)) return p + 4;
	const char *s = p;
	if (*p == '-') p++;
	if (!isdigit((unsigned char)*p)) return NULL;
	while (isdigit((unsigned char)*p)) p++;
	if (*p == '.') { p++; if (!isdigit((unsigned char)*p)) return NULL; while (isdigit((unsigned char)*p)) p++; }
	if (*p == 'e' || *p == 'E') { p++; if (*p == '+' || *p == '-') p++; if (!isdigit((unsigned char)*p)) return NULL; while (isdigit((unsigned char)*p)) p++; }
	return p > s ? p : NULL;
}
static bool valid_json(const char *s) { const char *e = jv_value(s, 0); return e && !*jv_ws(e); }
static int count(const char *s, const char *what) { int k = 0; for (const char *p = s; (p = strstr(p, what)); p++) k++; return k; }

// ---- stub hooks: record the last call, answer what the test set ----
static std::string g_call;           // "tool|arg" or "set|key|value|drive" or "log|from|max"
static int g_ret;                    // what the next hook returns
static const char *g_answer = "{\"ok\":true}";
static unsigned char g_jpg[] = {0xff, 0xd8, 0xff, 0xe0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0xff, 0xd9};
static int g_jpg_frees;
static int answer(char *out, int n) { snprintf(out, n, "%s", g_answer); return g_ret; }
static int h_state(char *out, int n) { g_call = "state"; return answer(out, n); }
static int h_drives(char *out, int n) { g_call = "drives"; return answer(out, n); }
static int h_list(char *out, int n) { g_call = "list_settings"; return answer(out, n); }
static int h_set(const char *k, const char *v, const char *d, char *out, int n)
{
	g_call = std::string("set|") + k + "|" + v + "|" + (d ? d : "(null)");
	return answer(out, n);
}
static int h_ui(const char *tool, const char *arg, char *out, int n) { g_call = std::string(tool) + "|" + arg; return answer(out, n); }
static int h_log(int from, int max, char *out, int n) { char b[64]; snprintf(b, sizeof(b), "log|%d|%d", from, max); g_call = b; return answer(out, n); }
static int h_shot(unsigned char **jpg, int *len)
{
	g_call = "shot";
	if (g_ret) return g_ret;
	*jpg = (unsigned char *)malloc(sizeof(g_jpg));
	memcpy(*jpg, g_jpg, sizeof(g_jpg));
	*len = sizeof(g_jpg);
	return 0;
}
static const McpHooks g_hooks = {h_state, h_shot, h_drives, h_list, h_set, h_ui, h_log};

static char g_out[256 * 1024];
static int call(const char *body) { g_call = ""; return mcp_handle(body, strlen(body), g_out, sizeof(g_out)); }
static int tool(const char *name, const char *args)
{
	std::string b = std::string("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":\"") + name + "\"";
	if (args) b += std::string(",\"arguments\":") + args;
	b += "}}";
	return call(b.c_str());
}
static bool has(const char *s) { return strstr(g_out, s) != NULL; }

static void initialize(int &bad)
{
	CHECK(call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{},\"clientInfo\":{\"name\":\"t\",\"version\":\"1\"}}}") == 200);
	CHECK(valid_json(g_out));
	CHECK(has("\"jsonrpc\":\"2.0\""));
	CHECK(has("\"id\":1,"));
	CHECK(has("\"protocolVersion\":\"2025-06-18\""));
	CHECK(has("\"serverInfo\":{\"name\":\"psx-ripper\",\"version\":\"1.2.3-test\"}"));
	CHECK(has("\"capabilities\":{\"tools\":{}}"));
	// a string id comes back as it was sent
	CHECK(call("{\"id\":\"a\\\"b\",\"jsonrpc\":\"2.0\",\"method\":\"initialize\"}") == 200);
	CHECK(has("\"id\":\"a\\\"b\""));
	CHECK(valid_json(g_out));
}

static void ping(int &bad)
{
	CHECK(call("{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"ping\"}") == 200);
	CHECK(!strcmp(g_out, "{\"jsonrpc\":\"2.0\",\"id\":2,\"result\":{}}"));
	CHECK(call(" \n{\"jsonrpc\":\"2.0\",\"id\":-3.5e2,\"method\":\"ping\",\"params\":{}}\r\n") == 200);
	CHECK(has("\"id\":-3.5e2,"));
}

static const char *k_tools[] = {"get_state", "get_screenshot", "get_drives", "list_settings", "set_setting",
	"go_screen", "go_tab", "press_button", "rip", "repair", "check", "cancel", "skip_rereads", "eject", "read_log",
	"list_games", "get_game", "check_game", "check_games", "cancel_check", "rename_game", "trash_game", "restore_game", "empty_trash", "show_games",
	"get_app_info", "audit_log", "preview_name", "audit_undo", "audit_redo", "reset_settings", "redump_download", "webhook_test", "show_game"};
static const int k_ntools = sizeof(k_tools) / sizeof(k_tools[0]);

static void tools_list(int &bad)
{
	CHECK(call("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\"}") == 200);
	CHECK(valid_json(g_out));
	CHECK(has("\"result\":{\"tools\":["));
	for (int i = 0; i < k_ntools; i++) {
		char want[64];
		snprintf(want, sizeof(want), "{\"name\":\"%s\",\"description\":\"", k_tools[i]);
		if (!has(want)) { printf("  missing %s\n", k_tools[i]); CHECK(has(want)); }
	}
	CHECK(count(g_out, "\"name\":") == k_ntools);
	CHECK(count(g_out, "\"inputSchema\":{\"type\":\"object\"") == k_ntools);
	CHECK(count(g_out, "\"additionalProperties\":false") == k_ntools);
	// arguments are described
	CHECK(has("\"required\":[\"key\",\"value\"]"));
	CHECK(has("\"value\":{\"type\":[\"string\",\"number\",\"boolean\"]"));
	CHECK(has("\"enum\":[\"cross\",\"circle\",\"triangle\",\"square\",\"up\",\"down\",\"left\",\"right\",\"start\",\"select\",\"l1\",\"r1\",\"l2\",\"r2\"]"));
	CHECK(has("\"enum\":[\"home\",\"settings\",\"drive_settings\",\"filename\",\"webhooks\",\"about\",\"audit_log\",\"remote_control\",\"games\"]"));
	CHECK(has("\"max_lines\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":500"));
	// the descriptions steer agents to the action tools first; press_button and go_* are the fallback
	CHECK(has("{\"name\":\"press_button\",\"description\":\"Fallback:"));
	CHECK(has("only to answer a dialog"));
	CHECK(has("circle on the home screen is refused"));   // it would exit the app, and MCP with it
	{   // rip replaces an earlier rip at the drive's destination without asking, as the pad's X does
		const char *d = strstr(g_out, "{\"name\":\"rip\",\"description\":\"");
		const char *end = d ? strstr(d, "\",\"inputSchema\"") : NULL;
		bool says = d && end && std::string(d, end - d).find("replaces an existing rip at the drive's destination without asking") != std::string::npos;
		if (!says) printf("  rip does not say it replaces an existing rip\n");
		CHECK(says);
	}
	for (const char *a : {"rip", "repair", "check", "cancel", "skip_rereads", "eject"}) {
		char want[64];
		snprintf(want, sizeof(want), "{\"name\":\"%s\",\"description\":\"", a);
		const char *d = strstr(g_out, want);
		const char *end = d ? strstr(d + strlen(want), "\",\"inputSchema\"") : NULL;
		bool steer = d && end;
		if (steer) { std::string s(d, end - d); steer = s.find("instead of press_button") != std::string::npos; }
		if (!steer) printf("  %s does not steer away from press_button\n", a);
		CHECK(steer);
	}
	// a params object with a cursor is fine
	CHECK(call("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\",\"params\":{\"cursor\":\"x\"}}") == 200);
	CHECK(has("\"tools\":["));
}

// a successful call's text content is the hook's JSON, escaped as a string
static bool text_ok(int st, const char *escaped)
{
	char want[512];
	snprintf(want, sizeof(want), "\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"%s\"}],\"isError\":false}", escaped);
	return st == 200 && strstr(g_out, want) && valid_json(g_out);
}

static void routing(int &bad)
{
	g_ret = 0; g_answer = "{\"screen\":\"home\",\"n\":\"a\\\\b\"}";
	const char *esc = "{\\\"screen\\\":\\\"home\\\",\\\"n\\\":\\\"a\\\\\\\\b\\\"}";
	CHECK(text_ok(tool("get_state", NULL), esc)); CHECK(g_call == "state");
	CHECK(text_ok(tool("get_state", "{}"), esc));
	CHECK(text_ok(tool("get_drives", "{}"), esc)); CHECK(g_call == "drives");
	CHECK(text_ok(tool("list_settings", "{}"), esc)); CHECK(g_call == "list_settings");
	CHECK(text_ok(tool("set_setting", "{\"key\":\"speed\",\"value\":\"4\"}"), esc)); CHECK(g_call == "set|speed|4|(null)");
	CHECK(text_ok(tool("set_setting", "{\"drive\":\"usb1\",\"key\":\"speed\",\"value\":\"\"}"), esc)); CHECK(g_call == "set|speed||usb1");
	CHECK(text_ok(tool("go_screen", "{\"screen\":\"settings\"}"), esc)); CHECK(g_call == "go_screen|settings");
	CHECK(text_ok(tool("go_screen", "{\"screen\":\"drive_settings\",\"drive\":\"usb0\"}"), esc)); CHECK(g_call == "go_screen|drive_settings:usb0");
	CHECK(text_ok(tool("go_tab", "{\"drive\":\"internal\"}"), esc)); CHECK(g_call == "go_tab|internal");
	CHECK(text_ok(tool("press_button", "{\"button\":\"l2\"}"), esc)); CHECK(g_call == "press_button|l2");
	const char *actions[] = {"rip", "repair", "check", "cancel", "skip_rereads", "eject"};
	for (const char *a : actions) {
		CHECK(text_ok(tool(a, "{\"drive\":\"usb12\"}"), esc));
		CHECK(g_call == std::string(a) + "|usb12");
	}
	CHECK(text_ok(tool("read_log", "{}"), esc)); CHECK(g_call == "log|-1|100");
	CHECK(text_ok(tool("read_log", NULL), esc)); CHECK(g_call == "log|-1|100");
	CHECK(text_ok(tool("read_log", "{\"from_line\":0,\"max_lines\":500}"), esc)); CHECK(g_call == "log|0|500");
	CHECK(text_ok(tool("read_log", "{\"from_line\":123456,\"max_lines\":1}"), esc)); CHECK(g_call == "log|123456|1");
	// a number or true/false as the value: its text form (on/off settings are 1/0)
	CHECK(text_ok(tool("set_setting", "{\"key\":\"speed\",\"value\":4}"), esc)); CHECK(g_call == "set|speed|4|(null)");
	CHECK(text_ok(tool("set_setting", "{\"key\":\"eject\",\"value\":true}"), esc)); CHECK(g_call == "set|eject|1|(null)");
	CHECK(text_ok(tool("set_setting", "{\"key\":\"eject\",\"value\":false,\"drive\":\"usb0\"}"), esc)); CHECK(g_call == "set|eject|0|usb0");
	CHECK(text_ok(tool("set_setting", "{\"key\":\"speed\",\"value\":-1}"), esc)); CHECK(g_call == "set|speed|-1|(null)");
	CHECK(text_ok(tool("set_setting", "{\"key\":\"speed\",\"value\":1.5}"), esc)); CHECK(g_call == "set|speed|1.5|(null)");
	// escapes in arguments arrive decoded as UTF-8
	CHECK(text_ok(tool("set_setting", "{\"key\":\"name_pattern\",\"value\":\"a\\\"b\\\\c\\/\\u00e9\\ud83d\\ude00\\t\"}"), esc));
	CHECK(g_call == "set|name_pattern|a\"b\\c/\xc3\xa9\xf0\x9f\x98\x80\t|(null)");
	// control characters and non-ASCII in the hook's answer are escaped / kept
	g_answer = "{\"t\":\"x\ny\x01\xc3\xa9\"}";
	CHECK(text_ok(tool("get_state", NULL), "{\\\"t\\\":\\\"x\\ny\\u0001\xc3\xa9\\\"}"));
	g_answer = "{\"ok\":true}";
}

// a JSON-RPC invalid-params error naming the field
static bool param_err(int st, const char *field)
{
	char want[96];
	snprintf(want, sizeof(want), "\"data\":{\"field\":\"%s\"}", field);
	return st == 200 && strstr(g_out, "\"error\":{\"code\":-32602,") && strstr(g_out, want) && valid_json(g_out) && g_call.empty();
}

static void bad_arguments(int &bad)
{
	g_ret = 0;
	CHECK(param_err(tool("rip", NULL), "drive"));
	CHECK(param_err(tool("rip", "{}"), "drive"));
	CHECK(param_err(tool("rip", "{\"drive\":1}"), "drive"));
	CHECK(param_err(tool("rip", "{\"drive\":null}"), "drive"));
	CHECK(param_err(tool("rip", "{\"drive\":\"usb\"}"), "drive"));
	CHECK(param_err(tool("rip", "{\"drive\":\"usb0x\"}"), "drive"));
	CHECK(param_err(tool("rip", "{\"drive\":\"Internal\"}"), "drive"));
	CHECK(param_err(tool("rip", "{\"drive\":\"usb0\",\"force\":true}"), "force"));
	CHECK(param_err(tool("eject", "{\"drive\":[\"usb0\"]}"), "drive"));
	CHECK(param_err(tool("go_tab", "{\"tab\":\"usb0\"}"), "tab"));
	CHECK(param_err(tool("go_screen", "{}"), "screen"));
	CHECK(param_err(tool("go_screen", "{\"screen\":\"dbg\"}"), "screen"));
	CHECK(param_err(tool("go_screen", "{\"screen\":\"drive_settings\"}"), "drive"));
	CHECK(param_err(tool("go_screen", "{\"screen\":\"settings\",\"drive\":\"usb0\"}"), "drive"));
	CHECK(param_err(tool("press_button", "{\"button\":\"ps\"}"), "button"));
	CHECK(param_err(tool("press_button", "{\"button\":true}"), "button"));
	CHECK(param_err(tool("set_setting", "{\"value\":\"1\"}"), "key"));
	CHECK(param_err(tool("set_setting", "{\"key\":\"\",\"value\":\"1\"}"), "key"));
	CHECK(param_err(tool("set_setting", "{\"key\":\"speed\"}"), "value"));
	CHECK(param_err(tool("set_setting", "{\"key\":\"speed\",\"value\":null}"), "value"));
	CHECK(param_err(tool("set_setting", "{\"key\":\"speed\",\"value\":[4]}"), "value"));
	CHECK(param_err(tool("set_setting", "{\"key\":\"speed\",\"value\":{\"v\":4}}"), "value"));
	CHECK(param_err(tool("set_setting", "{\"key\":\"speed\",\"value\":\"4\\nwebhook=x\"}"), "value"));
	CHECK(param_err(tool("set_setting", "{\"key\":\"sp\\reed\",\"value\":\"4\"}"), "key"));
	CHECK(param_err(tool("set_setting", "{\"key\":\"speed\",\"value\":\"a\\u0000b\"}"), "value"));
	CHECK(param_err(tool("set_setting", "{\"key\":\"speed\",\"value\":\"4\",\"drive\":\"cd\"}"), "drive"));
	CHECK(param_err(tool("read_log", "{\"max_lines\":501}"), "max_lines"));
	CHECK(param_err(tool("read_log", "{\"max_lines\":0}"), "max_lines"));
	CHECK(param_err(tool("read_log", "{\"max_lines\":\"5\"}"), "max_lines"));
	CHECK(param_err(tool("read_log", "{\"from_line\":-1}"), "from_line"));
	CHECK(param_err(tool("read_log", "{\"from_line\":1.5}"), "from_line"));
	CHECK(param_err(tool("read_log", "{\"from_line\":1e3}"), "from_line"));
	CHECK(param_err(tool("read_log", "{\"from_line\":99999999999999999999}"), "from_line"));
	CHECK(param_err(tool("get_state", "{\"x\":1}"), "x"));
	CHECK(param_err(tool("get_state", "[]"), "arguments"));
	CHECK(param_err(tool("get_state", "\"x\""), "arguments"));
	CHECK(param_err(tool("format_disc", "{}"), "name"));
	CHECK(param_err(tool("get_state\\u0000junk", "{}"), "name"));   // a NUL does not cut the name short
	CHECK(param_err(call("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{}}"), "name"));
	CHECK(param_err(call("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":{\"name\":5}}"), "name"));
	CHECK(param_err(call("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\"}"), "params"));
	CHECK(param_err(call("{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"tools/call\",\"params\":[1]}"), "params"));
	// the message names the field too
	tool("rip", "{\"drive\":1}");
	CHECK(has("\"message\":\"Invalid params: drive"));
	// a key given twice: the agent's intent is unclear
	CHECK(param_err(tool("rip", "{\"drive\":\"usb0\",\"drive\":\"usb1\"}"), "drive"));
}

static void tool_errors(int &bad)
{
	// a refusal is a tool result with isError and the plain message
	g_ret = -1; g_answer = "No disc in the drive.";
	CHECK(tool("rip", "{\"drive\":\"usb0\"}") == 200);
	CHECK(has("\"result\":{\"content\":[{\"type\":\"text\",\"text\":\"No disc in the drive.\"}],\"isError\":true}"));
	CHECK(valid_json(g_out));
	CHECK(tool("get_screenshot", NULL) == 200);
	CHECK(has("\"isError\":true"));
	g_ret = 0; g_answer = "{\"ok\":true}";
	// before mcp_init / without a hook
	McpHooks none = {};
	mcp_init(&none, "1.2.3-test");
	for (int i = 0; i < k_ntools; i++) {
		const char *args = !strcmp(k_tools[i], "set_setting") ? "{\"key\":\"a\",\"value\":\"b\"}"
			: !strcmp(k_tools[i], "go_screen") ? "{\"screen\":\"home\"}"
			: !strcmp(k_tools[i], "press_button") ? "{\"button\":\"cross\"}"
			: !strcmp(k_tools[i], "get_game") || !strcmp(k_tools[i], "check_game") || !strcmp(k_tools[i], "trash_game") || !strcmp(k_tools[i], "restore_game") ? "{\"id\":\"hdd:/a\"}"
			: !strcmp(k_tools[i], "rename_game") ? "{\"id\":\"hdd:/a\",\"pattern\":\"%T\"}"
			: !strcmp(k_tools[i], "empty_trash") ? "{\"source\":\"hdd\",\"confirm\":true}"
			: !strcmp(k_tools[i], "preview_name") ? "{\"pattern\":\"%T\"}"
			: !strcmp(k_tools[i], "audit_log") || !strcmp(k_tools[i], "redump_download") || !strcmp(k_tools[i], "webhook_test") ? "{}"
			: !strcmp(k_tools[i], "audit_undo") || !strcmp(k_tools[i], "audit_redo") ? "{\"row\":1}"
			: !strcmp(k_tools[i], "show_game") ? "{\"id\":\"hdd:/a\"}"
			: (!strncmp(k_tools[i], "get_", 4) || !strcmp(k_tools[i], "list_settings") || !strcmp(k_tools[i], "read_log") || !strcmp(k_tools[i], "list_games") ||
			   !strcmp(k_tools[i], "check_games") || !strcmp(k_tools[i], "cancel_check") || !strcmp(k_tools[i], "show_games")) ? "{}"
			: "{\"drive\":\"usb0\"}";
		CHECK(tool(k_tools[i], args) == 200);
		if (!has("\"isError\":true") || !has("not available")) { printf("  %s\n", k_tools[i]); CHECK(0); }
	}
	mcp_init(&g_hooks, "1.2.3-test");
}

static void screenshot(int &bad)
{
	g_ret = 0;
	CHECK(tool("get_screenshot", "{}") == 200);
	CHECK(g_call == "shot");
	CHECK(valid_json(g_out));
	CHECK(has("\"result\":{\"content\":[{\"type\":\"image\",\"data\":\"/9j/4AAQSkZJRgD/2Q==\",\"mimeType\":\"image/jpeg\"}],\"isError\":false}"));
	// a response buffer too small for the picture: a tool error, still valid JSON
	const char *req = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"get_screenshot\"}}";
	char small[100];
	int st = mcp_handle(req, strlen(req), small, sizeof(small));
	CHECK(st == 200 || st == 400);
	CHECK(strlen(small) < 100);
	CHECK(valid_json(small));
}

static void error_shape(int &bad, int st, int want_st, int code, const char *id)
{
	char want[64];
	snprintf(want, sizeof(want), "\"error\":{\"code\":%d,", code);
	char wid[64];
	snprintf(wid, sizeof(wid), "\"id\":%s,", id);
	if (st != want_st || !has(want) || !has(wid) || !valid_json(g_out)) { printf("  got %d %s\n", st, g_out); CHECK(0); }
}

static void protocol_errors(int &bad)
{
	error_shape(bad, call("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"resources/list\"}"), 200, -32601, "4");
	error_shape(bad, call("{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":5}"), 400, -32600, "4");
	error_shape(bad, call("{\"jsonrpc\":\"2.0\",\"id\":4}"), 400, -32600, "4");
	error_shape(bad, call("{\"jsonrpc\":\"1.0\",\"id\":4,\"method\":\"ping\"}"), 400, -32600, "4");
	error_shape(bad, call("{\"id\":4,\"method\":\"ping\"}"), 400, -32600, "4");
	error_shape(bad, call("{\"jsonrpc\":\"2.0\",\"id\":{},\"method\":\"ping\"}"), 400, -32600, "null");
	error_shape(bad, call("{\"jsonrpc\":\"2.0\",\"id\":true,\"method\":\"ping\"}"), 400, -32600, "null");
	error_shape(bad, call("[{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"}]"), 400, -32600, "null");
	error_shape(bad, call("\"ping\""), 400, -32600, "null");
	error_shape(bad, call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\""), 400, -32700, "null");
	error_shape(bad, call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\"} x"), 400, -32700, "null");
	error_shape(bad, call("{\"jsonrpc\":\"2.0\",\"id\":01,\"method\":\"ping\"}"), 400, -32700, "null");
	error_shape(bad, call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"pi\\x\"}"), 400, -32700, "null");
	error_shape(bad, call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"\\ud800\"}"), 400, -32700, "null");
	error_shape(bad, call("{'jsonrpc':'2.0'}"), 400, -32700, "null");
	error_shape(bad, call(""), 400, -32700, "null");
	// a raw control character inside a string
	error_shape(bad, call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"pi\nng\"}"), 400, -32700, "null");
	// text that is not UTF-8: a stray byte, an overlong form, a surrogate, a cut sequence
	error_shape(bad, call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"pi\xffng\"}"), 400, -32700, "null");
	error_shape(bad, call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"\xc0\xafx\"}"), 400, -32700, "null");
	error_shape(bad, call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"\xed\xa0\x80x\"}"), 400, -32700, "null");
	error_shape(bad, call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"\xe2\x82\"}"), 400, -32700, "null");
	// well-formed UTF-8 is fine (and comes back in the message)
	error_shape(bad, call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"r\xc3\xa4kna \xf0\x9f\x98\x80\"}"), 200, -32601, "1");
	CHECK(has("r\xc3\xa4kna \xf0\x9f\x98\x80"));
	// a NUL inside the method name does not cut it short
	error_shape(bad, call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\\u0000junk\"}"), 200, -32601, "1");
	// an embedded NUL byte
	CHECK(mcp_handle("{\"jsonrpc\":\"2.0\",\0\"id\":1,\"method\":\"ping\"}", 41, g_out, sizeof(g_out)) == 400);
	CHECK(has("-32700"));
	// the body length is what counts, not a NUL after it
	CHECK(mcp_handle("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"ping\"}garbage", 40, g_out, sizeof(g_out)) == 200);
	CHECK(has("\"id\":9,"));
	// an id too long to echo
	std::string longid = "{\"jsonrpc\":\"2.0\",\"id\":\"" + std::string(300, 'a') + "\",\"method\":\"ping\"}";
	error_shape(bad, call(longid.c_str()), 400, -32600, "null");
	// "id": null is allowed by JSON-RPC (though discouraged): handled, and echoed as null
	CHECK(call("{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"ping\"}") == 200);
	CHECK(has("\"id\":null,\"result\":{}"));
	error_shape(bad, call("{\"jsonrpc\":\"2.0\",\"id\":null,\"method\":\"resources/list\"}"), 200, -32601, "null");
	error_shape(bad, call("{\"jsonrpc\":\"2.0\",\"id\":nul,\"method\":\"ping\"}"), 400, -32700, "null");
}

static void notifications(int &bad)
{
	g_out[0] = 'x';
	CHECK(call("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}") == 202);
	CHECK(g_out[0] == 0);
	CHECK(call("{\"jsonrpc\":\"2.0\",\"method\":\"notifications/cancelled\",\"params\":{\"requestId\":1}}") == 202);
	// a message without an id is a notification even when the method is unknown
	CHECK(call("{\"jsonrpc\":\"2.0\",\"method\":\"tools/call\",\"params\":{\"name\":\"rip\",\"arguments\":{\"drive\":\"usb0\"}}}") == 202);
	CHECK(g_call.empty());   // a notification never runs a tool
	// a response from the client
	CHECK(call("{\"jsonrpc\":\"2.0\",\"id\":5,\"result\":{}}") == 202);
	CHECK(g_out[0] == 0);
}

static void too_large(int &bad)
{
	std::string big = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"params\":{\"x\":\"" + std::string(17000, 'a') + "\"}}";
	CHECK(call(big.c_str()) == 413);
	CHECK(has("\"code\":-32600,"));
	CHECK(valid_json(g_out));
	// just at the limit is fine
	std::string pre = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"params\":{\"x\":\"", post = "\"}}";
	std::string edge = pre + std::string(MCP_MAX_REQUEST - pre.size() - post.size(), 'a') + post;
	CHECK((int)edge.size() == MCP_MAX_REQUEST);
	CHECK(call(edge.c_str()) == 200);
	edge.insert(pre.size(), "a");
	CHECK(call(edge.c_str()) == 413);
}

static void depth(int &bad)
{
	// the request object is level 1, params 2: 14 more arrays make 16 levels, allowed
	std::string ok = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"params\":{\"x\":" + std::string(14, '[') + std::string(14, ']') + "}}";
	CHECK(call(ok.c_str()) == 200);
	std::string deep = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"params\":{\"x\":" + std::string(15, '[') + std::string(15, ']') + "}}";
	CHECK(call(deep.c_str()) == 400);
	CHECK(has("-32700"));
	std::string huge(MCP_MAX_REQUEST, '[');
	CHECK(call(huge.c_str()) == 400);
	std::string objs;
	for (int i = 0; i < 2000; i++) objs += "{\"a\":";
	CHECK(call(objs.c_str()) == 400);
	// many small values (more than any token table would hold)
	std::string wide = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"ping\",\"params\":{\"x\":[";
	while (wide.size() < MCP_MAX_REQUEST - 10) wide += "0,";
	wide += "0]}}";
	int st = call(wide.c_str());
	CHECK(st == 200 || st == 400);
	CHECK(valid_json(g_out));
}

// every response: a known status, NUL-terminated inside the buffer, valid JSON unless 202
static bool sane(const char *body0, int len, int n)
{
	char *body = (char *)malloc(len + 64);   // the body without a NUL after it, and guard bytes
	if (len) memcpy(body, body0, len);
	memset(body + len, '"', 64);
	// exactly n, then guard bytes: a write past n shows here even without AddressSanitizer
	char *out = (char *)malloc(n + 64);
	memset(out, 'Z', n + 64);
	int st = mcp_handle(body, len, out, n);
	bool ok = st == 200 || st == 202 || st == 400 || st == 413;
	for (int i = n; i < n + 64; i++) if (out[i] != 'Z') ok = false;
	if (ok && n > 0) ok = memchr(out, 0, n) != NULL;
	if (ok && st == 202 && n > 0) ok = out[0] == 0;
	if (ok && st != 202 && n >= 256) ok = valid_json(out);
	if (!ok) printf("  status %d, n %d, len %d: %.*s\n", st, n, len, n < 300 ? n : 300, out);
	free(out);
	free(body);
	return ok;
}

static void fuzz(int &bad)
{
	g_ret = 0;
	const char *valid[] = {
		"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{\"protocolVersion\":\"2025-06-18\",\"capabilities\":{}}}",
		"{\"jsonrpc\":\"2.0\",\"id\":\"x\\u00e9\",\"method\":\"tools/list\"}",
		"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"set_setting\",\"arguments\":{\"key\":\"name_pattern\",\"value\":\"\\ud83d\\ude00 \\\"x\\\" [y]\",\"drive\":\"usb0\"}}}",
		"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"read_log\",\"arguments\":{\"from_line\":10,\"max_lines\":20}}}",
		"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"get_screenshot\",\"arguments\":{}}}",
		"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"tools/call\",\"params\":{\"name\":\"go_screen\",\"arguments\":{\"screen\":\"drive_settings\",\"drive\":\"internal\"}}}",
		"{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\",\"params\":{\"a\":[1,-2.5e-3,true,false,null]}}",
	};
	const int sizes[] = {0, 1, 16, 64, 100, 256, 4096, 256 * 1024};
	int bad_runs = 0;
	unsigned seed = 12345;
	auto rnd = [&]() { seed = seed * 1103515245 + 12345; return (seed >> 16) & 0x7fff; };
	for (const char *v : valid) {
		int len = strlen(v);
		CHECK(sane(v, len, sizeof(g_out)));
		for (int cut = 0; cut <= len; cut++)
			for (int n : sizes) if (!sane(v, cut, n)) { if (!bad_runs++) printf("  cut %d n %d: %.*s\n", cut, n, cut, v); }
		std::string m;
		for (int i = 0; i < 3000; i++) {
			m = v;
			int edits = 1 + rnd() % 4;
			for (int e = 0; e < edits; e++) {
				int at = rnd() % m.size();
				switch (rnd() % 4) {
				case 0: m[at] = (char)rnd(); break;
				case 1: m.erase(at, 1 + rnd() % 8); if (m.empty()) m = "{"; break;
				case 2: m.insert(at, std::string(1 + rnd() % 20, "{[\"\\:,"[rnd() % 6])); break;
				case 3: m.insert(at, m.substr(rnd() % m.size(), 1 + rnd() % 30)); break;
				}
			}
			if (!sane(m.data(), m.size(), sizes[rnd() % 8]) && !bad_runs++) printf("  mutation: %s\n", m.c_str());
		}
	}
	for (int i = 0; i < 3000; i++) {
		char r[600];
		int len = rnd() % sizeof(r);
		for (int k = 0; k < len; k++) r[k] = (char)rnd();
		if (!sane(r, len, sizes[rnd() % 8]) && !bad_runs++) printf("  random bytes %d\n", i);
	}
	CHECK(bad_runs == 0);
	// a NULL body
	CHECK(mcp_handle(NULL, 0, g_out, sizeof(g_out)) == 400);
}

static void small_buffer(int &bad)
{
	// every buffer size from nothing to the whole answer: never a byte past the buffer
	g_ret = 0;
	const char *reqs[] = {
		"{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}",
		"{\"jsonrpc\":\"2.0\",\"id\":\"x\",\"method\":\"tools/list\"}",
		"{\"jsonrpc\":\"2.0\",\"id\":2,\"method\":\"tools/call\",\"params\":{\"name\":\"get_state\"}}",
		"{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/call\",\"params\":{\"name\":\"get_screenshot\"}}",
		"{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"tools/call\",\"params\":{\"name\":\"rip\"}}",
		"{\"jsonrpc\":\"2.0\",\"id\":5,\"method\":\"nope\"}",
		"[1]",
	};
	for (const char *r : reqs) {
		int full = mcp_handle(r, strlen(r), g_out, sizeof(g_out)) == 202 ? 0 : strlen(g_out);
		int fails = 0;
		for (int n = 0; n <= full + 2; n++) if (!sane(r, strlen(r), n)) fails++;
		CHECK(fails == 0);
	}

	// a hook answer bigger than the response buffer: an internal error that still fits
	static char big[8000];
	memset(big, 'a', sizeof(big) - 3);
	big[0] = '"'; big[sizeof(big) - 3] = '"'; big[sizeof(big) - 2] = 0;
	g_ret = 0; g_answer = big;
	char out[1024];
	const char *req = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"tools/call\",\"params\":{\"name\":\"get_state\"}}";
	int st = mcp_handle(req, strlen(req), out, sizeof(out));
	CHECK(st == 200);
	CHECK(valid_json(out));
	CHECK(strstr(out, "\"id\":1,") != NULL);
	CHECK(strstr(out, "-32603") != NULL);
	g_answer = "{\"ok\":true}";
}

// ---- the game list's tools: their arguments reach the hooks as the app needs them; empty_trash is
// never called without confirm: true
static std::string q_text(const McpGameQuery *q)
{
	char b[160];
	snprintf(b, sizeof(b), "%s|%d,%d,%d,%d,%d|%d|%d", q->source, q->fmt, q->redump, q->errors, q->other, q->trash, q->offset, q->limit);
	return b;
}
static McpGameQuery g_q;   // the last query a game hook got
static int h_games(const McpGameQuery *q, char *out, int n) { g_q = *q; g_call = "games|" + q_text(q); return answer(out, n); }
static int h_game(const char *id, char *out, int n) { g_call = std::string("game|") + id; return answer(out, n); }
static int h_gact(const char *tool, const char *id, const char *pat, const McpGameQuery *q, char *out, int n)
{
	g_q = *q;
	g_call = std::string(tool) + "|" + (id ? id : "(null)") + "|" + (pat ? pat : "(null)") + "|" + q_text(q);
	return answer(out, n);
}
// tools/list: the tool's description has text
static bool has_desc(const char *name, const char *text)
{
	call("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\"}");
	char want[64];
	snprintf(want, sizeof(want), "{\"name\":\"%s\",\"description\":\"", name);
	const char *d = strstr(g_out, want);
	const char *end = d ? strstr(d, "\",\"inputSchema\"") : NULL;
	return d && end && std::string(d, end - d).find(text) != std::string::npos;
}
// tools/list: empty_trash takes one source, required (all is not one)
static bool has_schema_source1()
{
	call("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\"}");
	const char *d = strstr(g_out, "{\"name\":\"empty_trash\",");
	const char *end = d ? strstr(d + 1, "{\"name\":") : NULL;
	std::string t = d ? std::string(d, end ? end - d : strlen(d)) : "";
	return t.find("\"source\":{\"type\":\"string\",\"pattern\":\"^(hdd|usb[0-7])$\"") != std::string::npos &&
	       t.find("\"required\":[\"source\"]") != std::string::npos && t.find("\"expect\":{") != std::string::npos;
}
static void games(int &bad)
{
	McpHooks hk = g_hooks;
	hk.list_games = h_games; hk.get_game = h_game; hk.game = h_gact;
	mcp_init(&hk, "1.2.3-test");
	g_ret = 0; g_answer = "{\"ok\":true}";
	const char *esc = "{\\\"ok\\\":true}";
	// the schema: the source, the filter panel's criteria, the page
	CHECK(call("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\"}") == 200);
	CHECK(valid_json(g_out));
	CHECK(has("\"limit\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":200"));
	CHECK(has("\"source\":{\"type\":\"string\",\"pattern\":\"^(all|hdd|usb[0-7])$\""));
	CHECK(has("\"redump\":{\"type\":\"string\",\"enum\":[\"any\",\"verified\",\"unchecked\",\"no_match\",\"not_verified\"]"));
	CHECK(has("\"show\":{\"type\":\"string\",\"enum\":[\"games\",\"trash\"]"));
	CHECK(has("\"confirm\":{\"type\":\"boolean\""));
	for (const char *a : {"check_game", "check_games", "cancel_check", "rename_game", "trash_game", "restore_game", "empty_trash"}) {
		char want[64];
		snprintf(want, sizeof(want), "{\"name\":\"%s\",\"description\":\"", a);
		const char *d = strstr(g_out, want);
		const char *end = d ? strstr(d + strlen(want), "\",\"inputSchema\"") : NULL;
		bool steer = d && end && std::string(d, end - d).find("instead of press_button") != std::string::npos;
		if (!steer) printf("  %s does not steer away from press_button\n", a);
		CHECK(steer);
	}
	{
		const char *d = strstr(g_out, "{\"name\":\"empty_trash\",\"description\":\"");
		const char *end = d ? strstr(d, "\",\"inputSchema\"") : NULL;
		CHECK(d && end && std::string(d, end - d).find("This is final") != std::string::npos);
	}
	CHECK(has("{\"name\":\"list_games\",") && has("{\"name\":\"get_game\","));
	CHECK(has("and the game list's tools (check_game"));   // press_button: the game tools do their work directly
	CHECK(call("{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\"}") == 200);
	CHECK(has("list_games and get_game read the game list"));
	// routing: what each tool hands its hook
	CHECK(text_ok(tool("list_games", NULL), esc)); CHECK(g_call == "games||0,0,0,0,0|0|50");
	CHECK(text_ok(tool("list_games", "{\"source\":\"usb3\",\"filter\":{\"format\":\"2048\",\"redump\":\"not_verified\",\"errors\":\"with\",\"other\":\"sbi_missing\",\"show\":\"trash\"},\"offset\":10,\"limit\":200}"), esc));
	CHECK(g_call == "games|usb3|2,4,1,3,1|10|200");
	CHECK(text_ok(tool("list_games", "{\"filter\":{\"redump\":\"unchecked\"},\"source\":\"all\"}"), esc)); CHECK(g_call == "games|all|0,2,0,0,0|0|50");
	CHECK(text_ok(tool("get_game", "{\"id\":\"hdd:/dev_hdd0/PSXISO/\\u00c9chapp\\u00e9e\"}"), esc)); CHECK(g_call == "game|hdd:/dev_hdd0/PSXISO/\xc3\x89" "chapp\xc3\xa9" "e");
	CHECK(text_ok(tool("check_game", "{\"id\":\"hdd:/a\"}"), esc)); CHECK(g_call == "check_game|hdd:/a|(null)||0,0,0,0,0|0|50");
	CHECK(text_ok(tool("check_games", "{\"source\":\"hdd\",\"filter\":{\"format\":\"2352\"}}"), esc)); CHECK(g_call == "check_games|(null)|(null)|hdd|1,0,0,0,0|0|50");
	CHECK(text_ok(tool("cancel_check", NULL), esc)); CHECK(g_call == "cancel_check|(null)|(null)||0,0,0,0,0|0|50");
	CHECK(text_ok(tool("rename_game", "{\"id\":\"usb0:/b\",\"pattern\":\"%N [%D]\"}"), esc)); CHECK(g_call == "rename_game|usb0:/b|%N [%D]||0,0,0,0,0|0|50");
	CHECK(text_ok(tool("rename_game", "{\"id\":\"usb0:/b\",\"pattern\":\"\"}"), esc)); CHECK(g_call == "rename_game|usb0:/b|||0,0,0,0,0|0|50");
	CHECK(text_ok(tool("trash_game", "{\"id\":\"hdd:/a\"}"), esc)); CHECK(g_call == "trash_game|hdd:/a|(null)||0,0,0,0,0|0|50");
	CHECK(text_ok(tool("restore_game", "{\"id\":\"hdd:/.trash/a\"}"), esc)); CHECK(g_call == "restore_game|hdd:/.trash/a|(null)||0,0,0,0,0|0|50");
	CHECK(text_ok(tool("empty_trash", "{\"confirm\":true,\"source\":\"usb1\"}"), esc)); CHECK(g_call == "empty_trash|(null)|(null)|usb1|0,0,0,0,0|0|50");
	CHECK(g_q.expect == -1);
	// empty_trash: one source (required, never all); expect = the games the agent saw in that trash,
	// a number or their ids (hashed: any order)
	CHECK(has_schema_source1());
	CHECK(text_ok(tool("empty_trash", "{\"confirm\":true,\"source\":\"hdd\",\"expect\":3}"), esc));
	CHECK(g_call == "empty_trash|(null)|(null)|hdd|0,0,0,0,0|0|50" && g_q.expect == 3 && !g_q.expect_ids);
	CHECK(text_ok(tool("empty_trash", "{\"confirm\":true,\"source\":\"hdd\",\"expect\":0}"), esc));
	CHECK(g_q.expect == 0 && !g_q.expect_ids);
	CHECK(text_ok(tool("empty_trash", "{\"confirm\":true,\"source\":\"usb2\",\"expect\":[\"usb2:/a\",\"usb2:/\\u00e9\"]}"), esc));
	CHECK(g_q.expect == 2 && g_q.expect_ids && g_q.expect_hash == mcp_id_hash("usb2:/a") + mcp_id_hash("usb2:/\xc3\xa9"));
	CHECK(text_ok(tool("empty_trash", "{\"confirm\":true,\"source\":\"usb2\",\"expect\":[\"usb2:/\\u00e9\",\"usb2:/a\"]}"), esc));
	CHECK(g_q.expect == 2 && g_q.expect_hash == mcp_id_hash("usb2:/a") + mcp_id_hash("usb2:/\xc3\xa9"));
	CHECK(text_ok(tool("empty_trash", "{\"confirm\":true,\"source\":\"usb2\",\"expect\":[]}"), esc));
	CHECK(g_q.expect == 0 && g_q.expect_ids && g_q.expect_hash == 0);
	CHECK(param_err(tool("empty_trash", "{\"confirm\":true}"), "source"));
	CHECK(param_err(tool("empty_trash", "{\"confirm\":true,\"source\":\"all\"}"), "source"));
	CHECK(param_err(tool("empty_trash", "{\"confirm\":true,\"source\":\"hdd\",\"expect\":-1}"), "expect"));
	CHECK(param_err(tool("empty_trash", "{\"confirm\":true,\"source\":\"hdd\",\"expect\":\"3\"}"), "expect"));
	CHECK(param_err(tool("empty_trash", "{\"confirm\":true,\"source\":\"hdd\",\"expect\":1.5}"), "expect"));
	CHECK(param_err(tool("empty_trash", "{\"confirm\":true,\"source\":\"hdd\",\"expect\":[1]}"), "expect"));
	CHECK(param_err(tool("empty_trash", "{\"confirm\":true,\"source\":\"hdd\",\"expect\":[\"\"]}"), "expect"));
	CHECK(param_err(tool("empty_trash", "{\"confirm\":true,\"source\":\"hdd\",\"expect\":[\"a\\u0000b\"]}"), "expect"));
	CHECK(param_err(tool("empty_trash", "{\"confirm\":true,\"source\":\"hdd\",\"expect\":{\"n\":1}}"), "expect"));
	CHECK(param_err(tool("check_games", "{\"expect\":1}"), "expect"));
	// list_games' query: a part of the name
	CHECK(text_ok(tool("list_games", "{\"query\":\"Croc\"}"), esc));
	CHECK(!strcmp(g_q.query, "Croc"));
	CHECK(text_ok(tool("list_games", NULL), esc));
	CHECK(g_q.query[0] == 0);
	CHECK(param_err(tool("list_games", "{\"query\":5}"), "query"));
	CHECK(param_err(tool("list_games", (std::string("{\"query\":\"") + std::string(96, 'x') + "\"}").c_str()), "query"));
	// progress is polled with get_state; list_games gives the result
	CHECK(has_desc("check_game", "poll get_state") && has_desc("check_games", "poll get_state"));
	// empty_trash without confirm: true is refused and the hook is not called (it is final)
	for (const char *args : {"{\"source\":\"hdd\"}", "{\"source\":\"hdd\",\"confirm\":false}"}) {
		g_call = "";
		CHECK(tool("empty_trash", args) == 200);
		CHECK(has("\"isError\":true") && has("confirm: true") && has("cannot be restored") && g_call.empty() && valid_json(g_out));
	}
	// bad arguments: a protocol error naming the field, no hook called
	CHECK(param_err(tool("list_games", "{\"limit\":201}"), "limit"));
	CHECK(param_err(tool("list_games", "{\"limit\":0}"), "limit"));
	CHECK(param_err(tool("list_games", "{\"offset\":-1}"), "offset"));
	CHECK(param_err(tool("list_games", "{\"offset\":\"5\"}"), "offset"));
	CHECK(param_err(tool("list_games", "{\"source\":\"usb8\"}"), "source"));
	CHECK(param_err(tool("list_games", "{\"source\":\"dvd\"}"), "source"));
	CHECK(param_err(tool("list_games", "{\"source\":\"usb0x\"}"), "source"));
	CHECK(param_err(tool("list_games", "{\"filter\":\"2048\"}"), "filter"));
	CHECK(param_err(tool("list_games", "{\"filter\":{\"status\":\"verified\"}}"), "filter"));
	CHECK(param_err(tool("list_games", "{\"filter\":{\"format\":\"4096\"}}"), "filter"));
	CHECK(param_err(tool("list_games", "{\"filter\":{\"format\":2048}}"), "filter"));
	CHECK(param_err(tool("list_games", "{\"filter\":{\"format\":\"2048\",\"format\":\"2352\"}}"), "filter"));
	CHECK(param_err(tool("list_games", "{\"page\":2}"), "page"));
	CHECK(param_err(tool("get_game", "{}"), "id"));
	CHECK(param_err(tool("get_game", "{\"id\":\"\"}"), "id"));
	CHECK(param_err(tool("get_game", "{\"id\":5}"), "id"));
	CHECK(param_err(tool("trash_game", "{\"id\":\"a\\nb\"}"), "id"));
	CHECK(param_err(tool("rename_game", "{\"id\":\"hdd:/a\"}"), "pattern"));
	CHECK(param_err(tool("empty_trash", "{\"source\":\"hdd\",\"confirm\":\"yes\"}"), "confirm"));   // (the source is required: given)
	CHECK(param_err(tool("cancel_check", "{\"id\":\"hdd:/a\"}"), "id"));
	// show_games: the list's source and filter set in one step (M10). Criteria not given keep their
	// value (fgiven says which were given); "clear" or {} clears them, as L1 in the panel
	CHECK(has_desc("show_games", "instead of press_button") && has_desc("list_games", "use show_games"));
	// the source and filter are not saved (M12, M13): the description says they last until the app restarts
	CHECK(has_desc("show_games", "last until the app restarts") && !has_desc("show_games", "saved as they save"));
	{
		call("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\"}");
		const char *d = strstr(g_out, "{\"name\":\"show_games\",");
		const char *end = d ? strstr(d + 1, "{\"name\":") : NULL;
		std::string t = d ? std::string(d, end ? end - d : strlen(d)) : "";
		CHECK(t.find("\"source\":{\"type\":\"string\",\"pattern\":\"^(all|hdd|usb[0-7])$\"") != std::string::npos);
		CHECK(t.find("\"filter\":{\"anyOf\":[{\"type\":\"object\",\"properties\":{\"format\":") != std::string::npos);
		CHECK(t.find("{\"type\":\"string\",\"enum\":[\"clear\"]}]") != std::string::npos);
		CHECK(t.find("\"required\"") == std::string::npos && t.find("\"query\"") == std::string::npos);
	}
	CHECK(text_ok(tool("show_games", NULL), esc)); CHECK(g_call == "show_games|(null)|(null)||0,0,0,0,0|0|50" && g_q.fgiven == 0 && !g_q.fclear);
	CHECK(text_ok(tool("show_games", "{\"source\":\"usb1\"}"), esc)); CHECK(g_call == "show_games|(null)|(null)|usb1|0,0,0,0,0|0|50" && g_q.fgiven == 0 && !g_q.fclear);
	CHECK(text_ok(tool("show_games", "{\"filter\":{\"format\":\"2048\"}}"), esc)); CHECK(g_call == "show_games|(null)|(null)||2,0,0,0,0|0|50" && g_q.fgiven == 1);
	CHECK(text_ok(tool("show_games", "{\"filter\":{\"redump\":\"no_match\"}}"), esc)); CHECK(g_call == "show_games|(null)|(null)||0,3,0,0,0|0|50" && g_q.fgiven == 2);
	CHECK(text_ok(tool("show_games", "{\"filter\":{\"errors\":\"without\"}}"), esc)); CHECK(g_call == "show_games|(null)|(null)||0,0,2,0,0|0|50" && g_q.fgiven == 4);
	CHECK(text_ok(tool("show_games", "{\"filter\":{\"other\":\"duplicates\"}}"), esc)); CHECK(g_call == "show_games|(null)|(null)||0,0,0,2,0|0|50" && g_q.fgiven == 8);
	CHECK(text_ok(tool("show_games", "{\"filter\":{\"show\":\"trash\"}}"), esc)); CHECK(g_call == "show_games|(null)|(null)||0,0,0,0,1|0|50" && g_q.fgiven == 16);
	CHECK(text_ok(tool("show_games", "{\"filter\":{\"format\":\"any\",\"show\":\"games\"}}"), esc)); CHECK(g_q.fgiven == 17 && !g_q.fclear);
	CHECK(text_ok(tool("show_games", "{\"source\":\"all\",\"filter\":{\"other\":\"duplicates\",\"redump\":\"verified\"}}"), esc));
	CHECK(g_call == "show_games|(null)|(null)|all|0,1,0,2,0|0|50" && g_q.fgiven == 10 && !g_q.fclear);
	CHECK(text_ok(tool("show_games", "{\"filter\":\"clear\"}"), esc)); CHECK(g_q.fclear && g_q.fgiven == 0);
	CHECK(text_ok(tool("show_games", "{\"filter\":{}}"), esc)); CHECK(g_q.fclear && g_q.fgiven == 0);
	// list_games takes no "clear" and its filter never asks to clear
	CHECK(param_err(tool("list_games", "{\"filter\":\"clear\"}"), "filter"));
	CHECK(text_ok(tool("list_games", "{\"filter\":{}}"), esc)); CHECK(!g_q.fclear);
	// refusals list the allowed values; no hook is called
	g_call = "";
	CHECK(param_err(tool("show_games", "{\"filter\":\"none\"}"), "filter") && has("clear") && g_call.empty());
	CHECK(param_err(tool("show_games", "{\"filter\":{\"status\":\"verified\"}}"), "filter") && has("format, redump, errors, other, show"));
	CHECK(param_err(tool("show_games", "{\"filter\":{\"format\":\"4096\"}}"), "filter") && has("any, 2352, 2048"));
	CHECK(param_err(tool("show_games", "{\"filter\":{\"show\":\"all\"}}"), "filter") && has("games, trash"));
	CHECK(param_err(tool("show_games", "{\"filter\":5}"), "filter"));
	CHECK(param_err(tool("show_games", "{\"source\":\"usb8\"}"), "source") && has("all, hdd or usbN"));
	CHECK(param_err(tool("show_games", "{\"query\":\"croc\"}"), "query"));   // (the screen cannot filter by name)
	CHECK(param_err(tool("show_games", "{\"offset\":1}"), "offset"));
	CHECK(g_call.empty());
	// a refusal is the app's text
	g_ret = -1; g_answer = "The game list is checking this rip";
	CHECK(tool("trash_game", "{\"id\":\"hdd:/a\"}") == 200);
	CHECK(has("\"text\":\"The game list is checking this rip\"}],\"isError\":true}"));
	g_ret = 0; g_answer = "{\"ok\":true}";
	mcp_init(&g_hooks, "1.2.3-test");
}

// ---- the app's other reading tools: get_app_info, audit_log (the query reaches the hook as the
// screen's levels need it), preview_name
static McpAuditQuery g_aq;   // the last audit query a hook got
static std::string aq_text(const McpAuditQuery *q)
{
	char b[200];
	snprintf(b, sizeof(b), "%s|%s|%u|%u|%d|%d|%d", q->domain, q->disc, (unsigned)q->row, (unsigned)q->job, q->offset, q->limit, q->confirm);
	return b;
}
static int h_info(char *out, int n) { g_call = "app_info"; return answer(out, n); }
static int h_alog(const McpAuditQuery *q, char *out, int n) { g_aq = *q; g_call = "audit_log|" + aq_text(q); return answer(out, n); }
static int h_preview(const char *pat, const char *drive, const char *id, char *out, int n)
{
	g_call = std::string("preview|") + pat + "|" + (drive ? drive : "(null)") + "|" + (id ? id : "(null)");
	return answer(out, n);
}
static void app_reads(int &bad)
{
	McpHooks hk = g_hooks;
	hk.app_info = h_info; hk.audit_log = h_alog; hk.preview_name = h_preview;
	mcp_init(&hk, "1.2.3-test");
	g_ret = 0; g_answer = "{\"ok\":true}";
	const char *esc = "{\\\"ok\\\":true}";
	CHECK(call("{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"tools/list\"}") == 200 && valid_json(g_out));
	CHECK(has("\"domain\":{\"type\":\"string\",\"enum\":[\"rips\",\"settings\",\"files\"]"));
	CHECK(has("\"job\":{\"type\":\"integer\",\"minimum\":1"));
	for (const char *t : {"get_app_info", "audit_log", "preview_name"}) {
		char want[80];
		snprintf(want, sizeof(want), "{\"name\":\"%s\",", t);
		const char *d = strstr(g_out, want), *e = d ? strstr(d, "\"annotations\":{\"readOnlyHint\":true}") : NULL, *nx = d ? strstr(d + 1, "{\"name\":") : NULL;
		CHECK(d && e && (!nx || e < nx));   // reading tools
	}
	CHECK(has_desc("audit_log", "never") && has_desc("audit_log", "secret"));
	CHECK(has_desc("preview_name", "set_setting") && has_desc("preview_name", "rename_game"));
	// get_app_info
	CHECK(text_ok(tool("get_app_info", NULL), esc)); CHECK(g_call == "app_info");
	CHECK(param_err(tool("get_app_info", "{\"x\":1}"), "x"));
	// audit_log: the domains; a domain's discs or events; a disc's events; a job's rows; a page
	CHECK(text_ok(tool("audit_log", NULL), esc)); CHECK(g_call == "audit_log|||0|0|0|50|0");
	CHECK(text_ok(tool("audit_log", "{\"domain\":\"settings\"}"), esc)); CHECK(g_call == "audit_log|settings||0|0|0|50|0");
	CHECK(text_ok(tool("audit_log", "{\"domain\":\"rips\",\"disc\":\"SLES_011.35\",\"offset\":9,\"limit\":200}"), esc));
	CHECK(g_call == "audit_log|rips|SLES_011.35|0|0|9|200|0");
	CHECK(text_ok(tool("audit_log", "{\"domain\":\"files\",\"disc\":\".trash\"}"), esc)); CHECK(g_call == "audit_log|files|.trash|0|0|0|50|0");
	CHECK(text_ok(tool("audit_log", "{\"job\":42,\"limit\":1}"), esc)); CHECK(g_call == "audit_log|||0|42|0|1|0");
	CHECK(param_err(tool("audit_log", "{\"domain\":\"mcp\"}"), "domain"));
	CHECK(param_err(tool("audit_log", "{\"disc\":\"SLES_011.35\"}"), "domain"));   // a disc key is a domain's
	CHECK(param_err(tool("audit_log", "{\"domain\":\"settings\",\"disc\":\"x\"}"), "disc"));
	CHECK(param_err(tool("audit_log", "{\"job\":3,\"domain\":\"rips\"}"), "job"));
	CHECK(param_err(tool("audit_log", "{\"job\":0}"), "job"));
	CHECK(param_err(tool("audit_log", "{\"job\":\"3\"}"), "job"));
	CHECK(param_err(tool("audit_log", "{\"job\":-3}"), "job"));
	CHECK(param_err(tool("audit_log", "{\"domain\":\"rips\",\"disc\":\"\"}"), "disc"));
	CHECK(param_err(tool("audit_log", "{\"limit\":201}"), "limit"));
	{   // a disc key longer than the log keeps (95 bytes)
		std::string a = "{\"domain\":\"rips\",\"disc\":\"" + std::string(96, 'x') + "\"}";
		CHECK(param_err(tool("audit_log", a.c_str()), "disc"));
	}
	// preview_name: the pattern, for the drives, one drive or one game
	CHECK(text_ok(tool("preview_name", "{\"pattern\":\"%T (%R)\"}"), esc)); CHECK(g_call == "preview|%T (%R)|(null)|(null)");
	CHECK(text_ok(tool("preview_name", "{\"pattern\":\"\",\"drive\":\"usb0\"}"), esc)); CHECK(g_call == "preview||usb0|(null)");
	CHECK(text_ok(tool("preview_name", "{\"pattern\":\"%N\",\"id\":\"hdd:/a\"}"), esc)); CHECK(g_call == "preview|%N|(null)|hdd:/a");
	CHECK(param_err(tool("preview_name", "{}"), "pattern"));
	CHECK(param_err(tool("preview_name", "{\"pattern\":\"%T\",\"drive\":\"usb0\",\"id\":\"hdd:/a\"}"), "id"));
	CHECK(param_err(tool("preview_name", "{\"pattern\":\"%T\",\"drive\":\"cd\"}"), "drive"));
	// a refusal is the app's text
	g_ret = -1; g_answer = "No job 42 in the audit log.";
	CHECK(tool("audit_log", "{\"job\":42}") == 200 && has("\"text\":\"No job 42 in the audit log.\"}],\"isError\":true}"));
	g_ret = 0; g_answer = "{\"ok\":true}";
	mcp_init(&g_hooks, "1.2.3-test");
}

// ---- the app's other changing tools: audit_undo and audit_redo (a whole disc only with confirm),
// reset_settings (all of them only with confirm), redump_download, webhook_test, show_game
static int h_undo(int redo, const McpAuditQuery *q, char *out, int n) { g_aq = *q; g_call = std::string(redo ? "redo|" : "undo|") + aq_text(q); return answer(out, n); }
static int h_whtest(char *out, int n) { g_call = "webhook_test"; return answer(out, n); }
static void app_actions(int &bad)
{
	McpHooks hk = g_hooks;
	hk.audit_undo = h_undo; hk.webhook_test = h_whtest; hk.game = h_gact;
	mcp_init(&hk, "1.2.3-test");
	g_ret = 0; g_answer = "{\"ok\":true}";
	const char *esc = "{\\\"ok\\\":true}";
	for (const char *t : {"audit_undo", "audit_redo", "reset_settings", "redump_download", "show_game"})
		if (!has_desc(t, "instead of press_button")) { printf("  %s does not steer away from press_button\n", t); CHECK(0); }
	CHECK(has_desc("audit_undo", "audit_redo") && has_desc("audit_redo", "audit_undo") && has_desc("audit_undo", "confirm"));
	CHECK(has_desc("reset_settings", "MCP stays on") && has_desc("redump_download", "get_state") && has_desc("webhook_test", "never"));
	CHECK(has_desc("set_setting", "audit_mb") && has_desc("press_button", "audit_undo") && has_desc("press_button", "reset_settings"));
	// audit_undo / audit_redo: exactly one of row, job and disc; a disc only with confirm: true
	CHECK(text_ok(tool("audit_undo", "{\"row\":12}"), esc)); CHECK(g_call == "undo|||12|0|0|50|0");
	CHECK(text_ok(tool("audit_redo", "{\"job\":7}"), esc)); CHECK(g_call == "redo|||0|7|0|50|0");
	CHECK(text_ok(tool("audit_undo", "{\"disc\":\"SLES_011.35\",\"confirm\":true}"), esc)); CHECK(g_call == "undo||SLES_011.35|0|0|0|50|1");
	CHECK(text_ok(tool("audit_redo", "{\"disc\":\"SLES_011.35\",\"confirm\":true}"), esc)); CHECK(g_call == "redo||SLES_011.35|0|0|0|50|1");
	for (const char *a : {"{\"disc\":\"SLES_011.35\"}", "{\"disc\":\"SLES_011.35\",\"confirm\":false}"}) {
		CHECK(tool("audit_undo", a) == 200 && has("\"isError\":true") && has("confirm: true") && g_call.empty());
	}
	CHECK(param_err(tool("audit_undo", "{}"), "row"));
	CHECK(param_err(tool("audit_undo", "{\"row\":1,\"job\":2}"), "row"));
	CHECK(param_err(tool("audit_redo", "{\"job\":2,\"disc\":\"x\",\"confirm\":true}"), "row"));
	CHECK(param_err(tool("audit_undo", "{\"row\":0}"), "row"));
	CHECK(param_err(tool("audit_undo", "{\"row\":\"5\"}"), "row"));
	CHECK(param_err(tool("audit_undo", "{\"row\":5,\"confirm\":1}"), "confirm"));
	// reset_settings: a drive's own settings at once; all of them only with confirm: true
	CHECK(text_ok(tool("reset_settings", "{\"drive\":\"usb0\"}"), esc)); CHECK(g_call == "reset_settings|usb0");
	CHECK(text_ok(tool("reset_settings", "{\"confirm\":true}"), esc)); CHECK(g_call == "reset_settings|");
	for (const char *a : {"{}", "{\"confirm\":false}"}) {
		CHECK(tool("reset_settings", a) == 200 && has("\"isError\":true") && has("confirm: true") && g_call.empty());
	}
	CHECK(param_err(tool("reset_settings", "{\"drive\":\"cd\"}"), "drive"));
	// redump_download: start, or stop with cancel
	CHECK(text_ok(tool("redump_download", NULL), esc)); CHECK(g_call == "redump_download|");
	CHECK(text_ok(tool("redump_download", "{\"cancel\":false}"), esc)); CHECK(g_call == "redump_download|");
	CHECK(text_ok(tool("redump_download", "{\"cancel\":true}"), esc)); CHECK(g_call == "redump_download|cancel");
	CHECK(param_err(tool("redump_download", "{\"cancel\":\"yes\"}"), "cancel"));
	// webhook_test, show_game
	CHECK(text_ok(tool("webhook_test", NULL), esc)); CHECK(g_call == "webhook_test");
	CHECK(param_err(tool("webhook_test", "{\"url\":\"http://x\"}"), "url"));
	CHECK(text_ok(tool("show_game", "{\"id\":\"hdd:/a\"}"), esc)); CHECK(g_call.compare(0, 18, "show_game|hdd:/a|(") == 0);
	CHECK(param_err(tool("show_game", "{}"), "id"));
	mcp_init(&g_hooks, "1.2.3-test");
}

int main()
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	mcp_init(&g_hooks, "1.2.3-test");
	struct { const char *name; void (*fn)(int &); } tests[] = {
		{"initialize", initialize}, {"ping", ping}, {"tools_list", tools_list}, {"routing", routing},
		{"bad_arguments", bad_arguments}, {"tool_errors", tool_errors}, {"screenshot", screenshot},
		{"protocol_errors", protocol_errors}, {"notifications", notifications}, {"too_large", too_large},
		{"depth", depth}, {"fuzz", fuzz}, {"small_buffer", small_buffer}, {"games", games},
		{"app_reads", app_reads}, {"app_actions", app_actions},
	};
	for (auto &t : tests) {
		int bad = 0;
		t.fn(bad);
		printf("%s %s\n", bad ? "FAIL" : "ok  ", t.name);
	}
	printf("mcp_test: %d problem(s)\n", g_fail);
	return g_fail ? 1 : 0;
}
