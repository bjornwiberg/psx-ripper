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
#define MCP_MAX_GAMES 200              // list_games' limit may not be larger
#define MCP_DEFAULT_GAMES 50

#define MCP_MAX_QUERY 95                // list_games' query: at most this many bytes

#include <stdint.h>

// The game list's query (list_games, check_games, empty_trash): the source ("hdd", "usbN", "all";
// "" = all) and the filter panel's criteria as the config's games_filter has them (0 = any:
// fmt 1 2352, 2 2048; redump 1 verified, 2 not checked, 3 no match, 4 not verified; errors 1 with,
// 2 without; other 1 broken, 2 duplicates, 3 .sbi missing), trash = the trash instead of the games.
// query (list_games): a part of the game's name, any case ("" = any). expect (empty_trash): how
// many games the agent saw in that trash (-1 = not given); with expect_ids, expect_hash is the sum
// of mcp_id_hash() over their ids, so the set of ids is compared in any order.
struct McpGameQuery { char source[8]; int fmt, redump, errors, other, trash; int offset, limit;
                      char query[MCP_MAX_QUERY + 1]; int expect, expect_ids; uint64_t expect_hash; };

// A game id's hash (64-bit FNV-1a) for empty_trash's expect.
static inline uint64_t mcp_id_hash(const char *s)
{
	uint64_t h = 14695981039346656037ULL;
	for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; }
	return h;
}

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
	// The game list. list_games and get_game only read (their own scan, never the screen's).
	int (*list_games)(const McpGameQuery *q, char *out, int n);
	int (*get_game)(const char *id, char *out, int n);
	// tool: check_game, check_games, cancel_check, rename_game, trash_game, restore_game, empty_trash,
	// carried out by the game list screen as the pad would. id: the game (check_game, rename_game,
	// trash_game, restore_game); pattern: rename_game's; q: check_games' and empty_trash's source and
	// filter, empty_trash's expect. empty_trash is called only when the agent confirmed it (confirm:
	// true) and named one source (never all).
	int (*game)(const char *tool, const char *id, const char *pattern, const McpGameQuery *q, char *out, int n);
};

void mcp_init(const McpHooks *hooks, const char *version);
// One HTTP request body in, the response body out (NUL-terminated; empty for 202); returns the
// HTTP status (200, 202, 400, 413).
int mcp_handle(const char *body, int len, char *out, int n);

#endif
