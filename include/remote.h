// Remote control: a small web server on port 8090. Its paths each have a switch in the settings
// (Settings → Remote control): the remote view (an MJPEG stream of the app's screen and a page with
// a pad whose buttons press the console's), MCP (POST /mcp, see mcp.h) and /status.json.
#ifndef PSXRIP_REMOTE_H
#define PSXRIP_REMOTE_H

#define REMOTE_PORT 8090
#ifndef REMOTE_READ_SECONDS
#define REMOTE_READ_SECONDS 10   // a request (headers and body) must have come whole by then
#endif
#define REMOTE_MAX_CLIENTS 4     // connections served at once; one more gets 503

struct RemoteHooks {
	int (*status_json)(char *out, int n);         // current state as JSON
	int (*debug)(const char *query, char *out, int n);   // /dbg?... (only with the debug flag file)
	int (*press)(const char *button);                     // POST /pad "cross", "l2", ...: a button press; 0 = known
};

void remote_init(const RemoteHooks *hooks);
enum { REMOTE_VIEW = 1, REMOTE_MCP = 2, REMOTE_STATUS = 4 };
void remote_set(int paths);                       // the paths that answer (REMOTE_*); the server runs while any does
int remote_ip(char *out, int n);                  // the console's IP address, 0 on success

#endif
