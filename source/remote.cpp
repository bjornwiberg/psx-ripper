// Remote control (see remote.h): MJPEG stream of the UI canvas and a pad (POST /pad), POST /mcp and
// /status.json, each path only while its switch is on. System dialogs are drawn by the PS3 on top
// of the app and are not included.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <sys/thread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <time.h>
#include <netinet/in.h>
#include <net/netctl.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

#include "ui.h"
#include "lang.h"
#include "remote.h"
#include "mcp.h"

static RemoteHooks g_hooks;
#ifdef HOST_REMOTE
volatile int g_remote_stop_delay_ms = 0;   // tests: the server thread lingers once it decided to stop
volatile int g_remote_sndbuf = 0;          // tests: a small send buffer, so an unread answer blocks (a computer's grows to MBs)
#endif
static volatile int g_paths = 0, g_listen = -1, g_running = 0;
static volatile int g_clients = 0;    // client threads serving a request (at most REMOTE_MAX_CLIENTS)
static volatile int g_streams = 0;    // client threads sending the stream (at most REMOTE_MAX_STREAMS)
static volatile int g_mcp_busy = 0;   // one MCP request at a time (two clients: one waits)

int remote_ip(char *out, int n)
{
	union net_ctl_info info;
	if (netCtlGetInfo(NET_CTL_INFO_IP_ADDRESS, &info)) { snprintf(out, n, "?"); return -1; }
	snprintf(out, n, "%s", info.ip_address);
	return 0;
}

struct JpgBuf { uint8_t *p; int n, cap; };
static void jpg_sink(void *ctx, void *data, int size)
{
	JpgBuf *b = (JpgBuf *)ctx;
	if (b->n + size > b->cap) {
		int cap = (b->n + size) * 2;
		uint8_t *np = (uint8_t *)realloc(b->p, cap);
		if (!np) return;
		b->p = np; b->cap = cap;
	}
	memcpy(b->p + b->n, data, size);
	b->n += size;
}

// Canvas at half size (2x2 average), JPEG quality 70.
static int encode_frame(JpgBuf *out, uint8_t *rgb)
{
	const uint32_t *c = ui_canvas();
	int W = ui_w(), H = ui_h(), w = W / 2, h = H / 2;
	for (int y = 0; y < h; y++) {
		const uint32_t *r0 = c + (y * 2) * W, *r1 = r0 + W;
		uint8_t *o = rgb + y * w * 3;
		for (int x = 0; x < w; x++) {
			uint32_t a = r0[x * 2], b = r0[x * 2 + 1], d = r1[x * 2], e = r1[x * 2 + 1];
			o[x * 3]     = (((a >> 16) & 255) + ((b >> 16) & 255) + ((d >> 16) & 255) + ((e >> 16) & 255)) >> 2;
			o[x * 3 + 1] = (((a >> 8) & 255) + ((b >> 8) & 255) + ((d >> 8) & 255) + ((e >> 8) & 255)) >> 2;
			o[x * 3 + 2] = ((a & 255) + (b & 255) + (d & 255) + (e & 255)) >> 2;
		}
	}
	out->n = 0;
	return stbi_write_jpg_to_func(jpg_sink, out, w, h, 3, rgb, 70) ? out->n : 0;
}

int remote_jpeg(unsigned char **jpg, int *len)
{
	*jpg = NULL;
	*len = 0;
	JpgBuf jb = {NULL, 0, 0};
	uint8_t *rgb = (uint8_t *)malloc(ui_w() / 2 * (ui_h() / 2) * 3);
	if (!rgb) return -1;
	int n = encode_frame(&jb, rgb);
	free(rgb);
	if (n <= 0) { free(jb.p); return -1; }
	*jpg = jb.p;
	*len = n;
	return 0;
}

static int send_all(int s, const void *p, int n)
{
	const char *c = (const char *)p;
	while (n > 0) {
		int r = send(s, c, n, 0);
		if (r <= 0) return -1;
		c += r; n -= r;
	}
	return 0;
}

static int send_str(int s, const char *t) { return send_all(s, t, strlen(t)); }

// recv that gives up at `deadline`: each wait is cut to the time left, so a client sending a byte
// now and then cannot stretch the request past it. 0 at the deadline (like a closed connection).
static int recv_by(int s, char *p, int n, time_t deadline)
{
	time_t left = deadline - time(NULL);
	if (left <= 0) return 0;
	struct timeval rt = {(long)left, 0};
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &rt, sizeof(rt));
	return recv(s, p, n, 0);
}

// The value of header `name` (given with its colon; any case, as HTTP allows) in the header block
// up to hend, without the spaces before it; NULL when the request has no such header.
static const char *header(const char *req, const char *hend, const char *name)
{
	size_t nl = strlen(name);
	for (const char *l = strstr(req, "\r\n"); l && l + 2 < hend; l = strstr(l + 2, "\r\n")) {
		if (strncasecmp(l + 2, name, nl)) continue;
		const char *v = l + 2 + nl;
		while (*v == ' ' || *v == '\t') v++;
		return v;
	}
	return NULL;
}

// The Content-Length of a request, 0 when it has none; a value too large to be real comes back
// as INT_MAX.
static int content_length(const char *req, const char *hend)
{
	const char *v = header(req, hend, "Content-Length:");
	long x = 0;
	for (; v && *v >= '0' && *v <= '9'; v++) if ((x = x * 10 + (*v - '0')) > 0x7FFFFFFF / 10) return 0x7FFFFFFF;
	return (int)x;
}

// A browser sends Origin with a request a page makes: only the console's own pages may reach
// /mcp and /pad (a page elsewhere could otherwise use the browser to control the app). No Origin:
// a program such as an MCP client, allowed.
static int origin_ok(const char *req, const char *hend)
{
	const char *v = header(req, hend, "Origin:");
	if (!v) return 1;
	char o[96], ip[20], me[64];
	size_t k = strcspn(v, " \t\r\n");
	if (k >= sizeof(o)) return 0;
	memcpy(o, v, k); o[k] = 0;
	if (remote_ip(ip, sizeof(ip))) ip[0] = 0;
	// exactly the page's origin (scheme, host and this port): anything else, "null" included, is
	// another site
#ifdef HOST_REMOTE
	const char *hosts[] = {ip, "localhost", "127.0.0.1"};   // the simulator: its page is on this computer
#else
	const char *hosts[] = {ip};                              // the console: only its own address
#endif
	for (int i = 0; i < (int)(sizeof(hosts) / sizeof(hosts[0])); i++) {
		if (!hosts[i][0]) continue;
		snprintf(me, sizeof(me), "http://%s:%d", hosts[i], REMOTE_PORT);
		if (!strcasecmp(o, me)) return 1;
	}
	return 0;
}

// The request line is "<method> /mcp" (or "/mcp?..."): MCP's path, whatever the method.
static int mcp_path(const char *req)
{
	const char *p = strchr(req, ' ');
	return p && !strncmp(p + 1, "/mcp", 4) && (p[5] == ' ' || p[5] == '?');
}

static const char MCP_OFF_TEXT[] = "MCP is off on the console. Turn it on in PSX RiPPER: Settings → Remote control → MCP.";

static const char *http_reason(int st)
{
	return st == 200 ? "OK" : st == 202 ? "Accepted" : st == 403 ? "Forbidden" : st == 413 ? "Payload Too Large" : "Bad Request";
}

static void send_json(int s, int status, const char *js)
{
	char hdr[160];
	snprintf(hdr, sizeof(hdr), "HTTP/1.0 %d %s\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nContent-Length: %d\r\n\r\n",
	         status, http_reason(status), (int)strlen(js));
	if (!send_str(s, hdr)) send_str(s, js);
}

// A JSON-RPC error without an id (the request was never handled), with HTTP status `status`.
static void send_rpc_error(int s, int status, int code, const char *msg)
{
	char js[256];
	snprintf(js, sizeof(js), "{\"jsonrpc\":\"2.0\",\"id\":null,\"error\":{\"code\":%d,\"message\":\"%s\"}}", code, msg);
	send_json(s, status, js);
}

// POST /mcp: the body (at most MCP_MAX_REQUEST bytes, by its Content-Length; part of it may have
// come with the headers) through mcp_handle, its answer with the status mcp_handle gives. The
// whole body must come by `deadline`; one that ends early is refused, never handled.
static void mcp_request(int s, const char *got, int have, int want, time_t deadline)
{
	if (want > MCP_MAX_REQUEST) {   // refused before reading it
		char m[64];
		snprintf(m, sizeof(m), "Request too large (at most %d bytes)", MCP_MAX_REQUEST);
		send_rpc_error(s, 413, -32600, m);
		return;
	}
	char *body = (char *)malloc(want + 1);
	if (!body) return;
	int n = have < want ? have : want;
	memcpy(body, got, n);
	while (n < want) {
		int r = recv_by(s, body + n, want - n, deadline);
		if (r <= 0) break;
		n += r;
	}
	if (n < want) {   // the client stopped sending, or too slowly: never handled
		free(body);
		send_rpc_error(s, 400, -32700, "Parse error: the request ended before its Content-Length");
		return;
	}
	body[n] = 0;
	while (__sync_lock_test_and_set(&g_mcp_busy, 1)) usleep(5 * 1000);
	// only now: a request waiting for another one holds no answer buffer, and one that waited
	// while MCP was turned off on the console is not handled
	const int on = g_paths & REMOTE_MCP;
	char *out = on ? (char *)malloc(MCP_MAX_RESPONSE) : NULL;
	int st = out ? mcp_handle(body, n, out, MCP_MAX_RESPONSE) : 0;
	__sync_lock_release(&g_mcp_busy);
	if (!on) send_rpc_error(s, 403, -32000, MCP_OFF_TEXT);
	else if (st == 202) send_str(s, "HTTP/1.0 202 Accepted\r\nContent-Length: 0\r\n\r\n");
	else if (out) send_json(s, st, out);   // no memory: the connection closes without an answer
	free(body);
	free(out);
}

static void client_thread(void *arg)
{
	int s = (int)(long)arg;
	int counted = 1;   // in g_clients (a stream moves to g_streams)
	// a client that stops reading (a phone asleep, a tab put away, a stuck agent) must not hold the
	// thread and its slot either: a send that cannot go on for this long gives up
	struct timeval st = {REMOTE_SEND_SECONDS, 0};
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &st, sizeof(st));
#ifdef HOST_REMOTE
	if (g_remote_sndbuf) setsockopt(s, SOL_SOCKET, SO_SNDBUF, (const void *)&g_remote_sndbuf, sizeof(int));
#endif
	// a client that stops sending, or sends a byte now and then, must not hold the thread: the
	// whole request (headers and body) must have come within REMOTE_READ_SECONDS
	const time_t deadline = time(NULL) + REMOTE_READ_SECONDS;
	char req[2048];
	int n = 0;
	// read the headers and, for a POST, the (small) body; /mcp reads its own (larger) body
	const char *hend = NULL;
	for (;;) {
		int r = recv_by(s, req + n, sizeof(req) - 1 - n, deadline);
		if (r <= 0) break;
		n += r; req[n] = 0;
		char *e = strstr(req, "\r\n\r\n");
		if (!e) { if (n >= (int)sizeof(req) - 1) break; continue; }
		hend = e + 4;
		if (mcp_path(req) || (int)(req + n - hend) >= content_length(req, hend) || n >= (int)sizeof(req) - 1) break;
	}
	req[n] = 0;
	const char *body = hend ? hend : "";
	const int view = g_paths & REMOTE_VIEW;
	const char *not_found = "HTTP/1.0 404 Not Found\r\nContent-Length: 0\r\n\r\n";

	if (mcp_path(req)) {
		if (!(g_paths & REMOTE_MCP))   // the agent learns why, and what to do about it
			send_rpc_error(s, 403, -32000, MCP_OFF_TEXT);
		else if (!hend) send_rpc_error(s, 400, -32700, "Parse error: incomplete request headers (or over 2 KB)");
		else if (!origin_ok(req, hend)) send_rpc_error(s, 403, -32000, "Origin not allowed: only the console's own pages may use MCP");
		else if (strncmp(req, "POST ", 5))   // no event stream (GET): only POST
			send_str(s, "HTTP/1.0 405 Method Not Allowed\r\nAllow: POST\r\nContent-Length: 0\r\n\r\n");
		else mcp_request(s, hend, req + n - hend, content_length(req, hend), deadline);
	} else if (!strncmp(req, "GET /status.json", 16) && !(g_paths & REMOTE_STATUS)) {
		send_str(s, not_found);
	} else if (!view && strncmp(req, "GET /status.json", 16)) {   // the remote view's paths, and anything else
		send_str(s, not_found);
	} else if (!strncmp(req, "GET /stream", 11) || !strncmp(req, "GET /shot.jpg", 13)) {
		JpgBuf jb = {NULL, 0, 0};
		const int stream = !strncmp(req, "GET /stream", 11);
		// a stream lasts as long as its tab is open: it leaves the requests' slots to the requests
		// and counts against its own limit (a phone that sleeps keeps it until the send timeout)
		if (stream && __sync_fetch_and_add(&g_streams, 1) >= REMOTE_MAX_STREAMS) {
			__sync_fetch_and_sub(&g_streams, 1);
			send_str(s, "HTTP/1.0 503 Service Unavailable\r\nRetry-After: 5\r\nContent-Length: 0\r\n\r\n");
		} else if (stream) {
			__sync_fetch_and_sub(&g_clients, 1);
			counted = 0;
		}
		uint8_t *rgb = stream && counted ? NULL : (uint8_t *)malloc(ui_w() / 2 * (ui_h() / 2) * 3);
		if (rgb && stream) {
			if (!send_str(s, "HTTP/1.0 200 OK\r\nCache-Control: no-cache\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n")) {
				while (g_paths & REMOTE_VIEW) {
					int len = encode_frame(&jb, rgb);
					char ph[128];
					int k = snprintf(ph, sizeof(ph), "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", len);
					if (!len || send_all(s, ph, k) || send_all(s, jb.p, len) || send_all(s, "\r\n", 2)) break;
					usleep(500 * 1000);   // about 2 frames per second
				}
			}
		} else if (rgb) {
			int len = encode_frame(&jb, rgb);
			char ph[160];
			int k = snprintf(ph, sizeof(ph), "HTTP/1.0 200 OK\r\nCache-Control: no-cache\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", len);
			if (len && !send_all(s, ph, k)) send_all(s, jb.p, len);
		}
		free(jb.p);
		free(rgb);
		if (stream && !counted) __sync_fetch_and_sub(&g_streams, 1);
	} else if (!strncmp(req, "GET /dbg?", 9) && g_hooks.debug) {
		char q[600];
		size_t k = strcspn(req + 9, " \r\n");
		if (k >= sizeof(q)) k = sizeof(q) - 1;
		memcpy(q, req + 9, k); q[k] = 0;
		char *out = (char *)malloc(16384);
		if (out) {
			out[0] = 0;
			g_hooks.debug(q, out, 16384);
			char hdr[128];
			snprintf(hdr, sizeof(hdr), "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nCache-Control: no-store\r\nContent-Length: %d\r\n\r\n", (int)strlen(out));
			if (!send_str(s, hdr)) send_str(s, out);
			free(out);
		}
	} else if (!strncmp(req, "GET /status.json", 16)) {
		const int JN = 16384;   // a few drives with their jobs and events; each client its own buffer
		char *js = (char *)malloc(JN);
		if (js) {
			snprintf(js, JN, "{}");
			if (g_hooks.status_json) g_hooks.status_json(js, JN);
			char hdr[128];
			snprintf(hdr, sizeof(hdr), "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nContent-Length: %d\r\n\r\n", (int)strlen(js));
			if (!send_str(s, hdr)) send_str(s, js);
			free(js);
		}
	} else if (!strncmp(req, "POST /pad", 9) && hend && !origin_ok(req, hend)) {   // a page elsewhere
		send_str(s, "HTTP/1.0 403 Forbidden\r\nContent-Length: 0\r\n\r\n");
	} else if (!strncmp(req, "POST /pad", 9)) {   // a button of the web page's pad
		char b[16];
		size_t k = strcspn(body, " \r\n&");
		if (k >= sizeof(b)) k = sizeof(b) - 1;
		memcpy(b, body, k); b[k] = 0;
		int bad = !g_hooks.press || g_hooks.press(b);
		send_str(s, bad ? "HTTP/1.0 400 Bad Request\r\nContent-Length: 0\r\n\r\n" : "HTTP/1.0 204 No Content\r\n\r\n");
	} else if (strncmp(req, "GET ", 4)) {   // POST /mcpx and the like: no such thing
		send_str(s, not_found);
	} else {   // the page: the screen, and under it a pad whose buttons press the console's
		send_str(s, "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-cache\r\n\r\n");
		send_str(s,
			"<!doctype html><html lang=");
		send_str(s, T(STR_HTML_LANG));   // the page in the console's language
		send_str(s,
			"><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>PSX RiPPER</title>\n"
			"<style>\n"
			":root{--bg:#111;--k:#262b3a;--kb:#3a4160;--t:#dfe3f0;--d:#8890a8;--b:min(52px,10vw)}\n"
			"*{box-sizing:border-box}\n"
			"html,body{overflow-x:hidden}\n"
			"body{margin:0;background:var(--bg);color:var(--t);font-family:system-ui,sans-serif}\n"
			"img{width:100%;max-width:1280px;display:block;margin:0 auto}\n"
			".pad{max-width:760px;margin:12px auto;padding:0 16px;display:grid;grid-template-columns:1fr auto 1fr;gap:8px;align-items:center;user-select:none;-webkit-user-select:none}\n"
			".row{grid-column:1/4;display:flex;justify-content:space-between}\n"
			".grp{display:flex;gap:8px}\n"
			"button{background:var(--k);color:var(--t);border:1px solid var(--kb);border-radius:10px;font:inherit;font-size:calc(var(--b)*.36);min-width:calc(var(--b)*1.1);height:calc(var(--b)*.85);padding:0;cursor:pointer;touch-action:manipulation}\n"
			"button:active,button.on{background:var(--kb)}\n"
			".dp,.fa{display:grid;grid-template-columns:repeat(3,var(--b));grid-template-rows:repeat(3,var(--b));gap:4px}\n"
			".dp{justify-self:start}.fa{justify-self:end}\n"
			".dp button,.fa button{min-width:0;width:var(--b);height:var(--b);border-radius:50%;font-size:calc(var(--b)*.42)}\n"
			".mid{display:flex;flex-direction:column;gap:8px;align-items:center}\n"
			".mid button{font-size:calc(var(--b)*.25);height:calc(var(--b)*.62);border-radius:999px;min-width:calc(var(--b)*1.6)}\n"
			".c{color:#7cc4ff}.o{color:#ff8a80}.tr{color:#7ce0a0}.sq{color:#e8a0e0}\n"
			"p{max-width:760px;margin:8px auto 16px;padding:0 16px;color:var(--d);font-size:13px}a{color:#9ad0a8}\n"
			"</style>\n"
			"<img id=v src='/stream' alt=''>\n"
			"<div class=pad>\n"
			"<div class=row><div class=grp><button data-b=l2>L2</button><button data-b=l1>L1</button></div><div class=grp><button data-b=r1>R1</button><button data-b=r2>R2</button></div></div>\n"
			"<div class=dp><span></span><button data-b=up>&#9650;</button><span></span><button data-b=left>&#9664;</button><span></span><button data-b=right>&#9654;</button><span></span><button data-b=down>&#9660;</button><span></span></div>\n"
			"<div class=mid><button data-b=select>SELECT</button><button data-b=start>START</button></div>\n"
			"<div class=fa><span></span><button class=tr data-b=triangle>&#9651;</button><span></span><button class=sq data-b=square>&#9633;</button><span></span><button class=o data-b=circle>&#9675;</button><span></span><button class=c data-b=cross>&#10005;</button><span></span></div>\n"
			"</div>\n"
			"<p>");
		send_str(s, T(STR_REMOTE_PAD_HELP));
		send_str(s,
			" <a href='/status.json'>status.json</a></p>\n"
			"<script>\n"
			"function press(b){var e=document.querySelector('[data-b='+b+']');if(e){e.classList.add('on');setTimeout(function(){e.classList.remove('on')},120)}fetch('/pad',{method:'POST',body:b}).catch(function(){})}\n"
			// the stream dies when the tab sleeps (phone locked, other app); close it while hidden and
			// open a fresh one when the page is visible again, and retry if it breaks
			"var v=document.getElementById('v'),rt;\n"
			"function live(){clearTimeout(rt);v.src='/stream?'+Date.now()}\n"
			"function stop(){clearTimeout(rt);v.removeAttribute('src')}\n"
			"v.addEventListener('error',function(){if(!document.hidden){clearTimeout(rt);rt=setTimeout(live,2000)}});\n"
			"document.addEventListener('visibilitychange',function(){document.hidden?stop():live()});\n"
			"addEventListener('pageshow',function(e){if(e.persisted)live()});\n"
			"document.querySelectorAll('[data-b]').forEach(function(e){e.addEventListener('click',function(){press(e.dataset.b)})});\n"
			"var K={ArrowUp:'up',ArrowDown:'down',ArrowLeft:'left',ArrowRight:'right',Enter:'cross',Escape:'circle',Backspace:'circle',q:'l1',e:'r1','1':'l2','3':'r2',t:'triangle',s:'square'};\n"
			"document.addEventListener('keydown',function(ev){var b=K[ev.key]||K[ev.key.toLowerCase()];if(b&&!ev.metaKey&&!ev.ctrlKey){ev.preventDefault();press(b)}});\n"
			"</script></html>\n");
	}
	close(s);
	if (counted) __sync_fetch_and_sub(&g_clients, 1);
	sysThreadExit(0);
}

static int listen_socket()
{
	int ls = socket(AF_INET, SOCK_STREAM, 0);
	if (ls < 0) return -1;
	int one = 1;
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(REMOTE_PORT);
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(ls, (struct sockaddr *)&sa, sizeof(sa)) || listen(ls, 4)) { close(ls); return -1; }
	return ls;
}

// Listens while any path is on. remote_set(0) takes the listening socket (g_listen) and closes
// it; switched on again before this thread noticed, the thread listens anew instead of ending.
static void server_thread(void *arg)
{
	for (;;) {
		while (g_paths) {
			int ls = -1;
			for (int i = 0; i < 20 && g_paths && (ls = listen_socket()) < 0; i++) usleep(100 * 1000);   // the old socket may linger a moment
			if (ls < 0) break;
			g_listen = ls;
			while (g_paths && g_listen == ls) {
				int s = accept(ls, NULL, NULL);
				if (s < 0) { if (!g_paths || g_listen != ls) break; usleep(100 * 1000); continue; }
				if (__sync_fetch_and_add(&g_clients, 1) >= REMOTE_MAX_CLIENTS) {   // full: try again later
					__sync_fetch_and_sub(&g_clients, 1);
					send_str(s, "HTTP/1.0 503 Service Unavailable\r\nRetry-After: 5\r\nContent-Length: 0\r\n\r\n");
					close(s);
					continue;
				}
				sys_ppu_thread_t t;
				// low priority (higher number): never compete with the rip threads
				if (sysThreadCreate(&t, client_thread, (void *)(long)s, 2000, 0x10000, 0, (char *)"psxremote")) {
					close(s);
					__sync_fetch_and_sub(&g_clients, 1);
				}
			}
			if (__sync_bool_compare_and_swap(&g_listen, ls, -1)) close(ls);   // not taken by remote_set
		}
#ifdef HOST_REMOTE
		if (g_remote_stop_delay_ms) usleep(g_remote_stop_delay_ms * 1000);
#endif
		g_running = 0;
		__sync_synchronize();
		// switched on while this thread was ending: it goes on (remote_set saw it still running)
		if (!g_paths || !__sync_bool_compare_and_swap(&g_running, 0, 1)) break;
	}
	sysThreadExit(0);
}

void remote_init(const RemoteHooks *hooks)
{
	g_hooks = *hooks;
	netCtlInit();
}

void remote_set(int paths)
{
	g_paths = paths;   // a path whose switch went off stops answering at once
	__sync_synchronize();
	if (paths && __sync_bool_compare_and_swap(&g_running, 0, 1)) {
		sys_ppu_thread_t t;
		if (sysThreadCreate(&t, server_thread, NULL, 2000, 0x4000, 0, (char *)"psxremoted")) g_running = 0;
	} else if (!paths) {
		// stream clients stop after their current frame
		int ls = __sync_lock_test_and_set(&g_listen, -1);
		if (ls >= 0) { shutdown(ls, SHUT_RDWR); close(ls); }   // wakes accept()
	}
}
