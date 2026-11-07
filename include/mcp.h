// MCP server (Model Context Protocol, JSON-RPC 2.0 over HTTP POST /mcp): the request handling and
// the tool table. String in, string out; everything app-specific goes through McpHooks, so the
// module runs on the host without main.cpp.
#ifndef PSXRIP_MCP_H
#define PSXRIP_MCP_H

#define MCP_MAX_REQUEST 16384          // larger request bodies are refused with HTTP 413
#define MCP_PROTOCOL_VERSION "2025-06-18"
#define MCP_MAX_LOG_LINES 500          // read_log's max_lines may not be larger
#define MCP_DEFAULT_LOG_LINES 100
#define MCP_MAX_RESPONSE (512 * 1024)   // the response buffer: a screenshot (base64) is the largest answer

// Every hook returns 0 and writes a JSON value (NUL-terminated, at most n bytes with the NUL)
// into out, or returns non-zero and writes a plain English message (why it was refused, "busy",
// ...) that the agent gets as a tool error. A NULL hook makes its tools answer "not available".
struct McpHooks {
	int (*state)(char *out, int n);                                   // get_state
	int (*screenshot_jpeg)(unsigned char **jpg, int *len);            // get_screenshot; caller frees *jpg with free()
	int (*drives)(char *out, int n);                                  // get_drives
	int (*list_settings)(char *out, int n);                           // list_settings
	int (*set_setting)(const char *key, const char *value, const char *drive, char *out, int n);   // drive NULL = standard
	// tool: go_screen, go_tab, press_button, rip, repair, check, cancel, skip_rereads, eject.
	// arg: go_screen the screen ("drive_settings:usb0" for a drive's settings), go_tab and the
	// actions the drive ("internal", "usb0", ...), press_button the button ("cross", "l2", ...).
	int (*ui)(const char *tool, const char *arg, char *out, int n);
	// from: first line (0 = the first line of the log), or -1 for the last max lines
	int (*read_log)(int from, int max, char *out, int n);
};

void mcp_init(const McpHooks *hooks, const char *version);
// One HTTP request body in, the response body out (NUL-terminated; empty for 202); returns the
// HTTP status (200, 202, 400, 413).
int mcp_handle(const char *body, int len, char *out, int n);

#endif
