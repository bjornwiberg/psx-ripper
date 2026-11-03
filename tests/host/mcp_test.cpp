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
	"go_screen", "go_tab", "press_button", "rip", "repair", "check", "cancel", "skip_rereads", "eject", "read_log"};
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
	CHECK(has("\"enum\":[\"cross\",\"circle\",\"triangle\",\"square\",\"up\",\"down\",\"left\",\"right\",\"start\",\"select\",\"l1\",\"r1\",\"l2\",\"r2\"]"));
	CHECK(has("\"enum\":[\"home\",\"settings\",\"drive_settings\",\"filename\",\"webhooks\",\"about\",\"audit_log\",\"remote_control\"]"));
	CHECK(has("\"max_lines\":{\"type\":\"integer\",\"minimum\":1,\"maximum\":500"));
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
	CHECK(param_err(tool("set_setting", "{\"key\":\"speed\",\"value\":4}"), "value"));
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
			: (!strncmp(k_tools[i], "get_", 4) || !strcmp(k_tools[i], "list_settings") || !strcmp(k_tools[i], "read_log")) ? "{}"
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
	// an embedded NUL byte
	CHECK(mcp_handle("{\"jsonrpc\":\"2.0\",\0\"id\":1,\"method\":\"ping\"}", 41, g_out, sizeof(g_out)) == 400);
	CHECK(has("-32700"));
	// the body length is what counts, not a NUL after it
	CHECK(mcp_handle("{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"ping\"}garbage", 40, g_out, sizeof(g_out)) == 200);
	CHECK(has("\"id\":9,"));
	// an id too long to echo
	std::string longid = "{\"jsonrpc\":\"2.0\",\"id\":\"" + std::string(300, 'a') + "\",\"method\":\"ping\"}";
	error_shape(bad, call(longid.c_str()), 400, -32600, "null");
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

int main()
{
	setvbuf(stdout, NULL, _IOLBF, 0);
	mcp_init(&g_hooks, "1.2.3-test");
	struct { const char *name; void (*fn)(int &); } tests[] = {
		{"initialize", initialize}, {"ping", ping}, {"tools_list", tools_list}, {"routing", routing},
		{"bad_arguments", bad_arguments}, {"tool_errors", tool_errors}, {"screenshot", screenshot},
		{"protocol_errors", protocol_errors}, {"notifications", notifications}, {"too_large", too_large},
		{"depth", depth}, {"fuzz", fuzz}, {"small_buffer", small_buffer},
	};
	for (auto &t : tests) {
		int bad = 0;
		t.fn(bad);
		printf("%s %s\n", bad ? "FAIL" : "ok  ", t.name);
	}
	printf("mcp_test: %d problem(s)\n", g_fail);
	return g_fail ? 1 : 0;
}
