// The web server (source/remote.cpp) on this computer, over loopback: which paths answer for which
// switches (remote view, MCP, status.json), and how POST /mcp reads its body and answers.
// Run: make -C tests/host   (uses port 8090; exit status 1 when a check fails)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <pthread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <string>
#include "ppu-types.h"
#include "sys/thread.h"
#include "net/netctl.h"
#include "lang.h"
#include "ui.h"
#include "remote.h"
#include "mcp.h"

// what remote.cpp needs from the app and the system
static uint32_t g_canvas[64 * 36];
const uint32_t *ui_canvas() { return g_canvas; }
int ui_w() { return 64; }
int ui_h() { return 36; }
const char *T(StrId) { return "text"; }
int netCtlInit() { return 0; }
int netCtlGetInfo(int, union net_ctl_info *info) { snprintf(info->ip_address, sizeof(info->ip_address), "127.0.0.1"); return 0; }
int sysThreadCreate(sys_ppu_thread_t *id, void (*entry)(void *), void *arg, int, u64, u64, char *)
{
	*id = 0;
	pthread_t t;
	if (pthread_create(&t, NULL, (void *(*)(void *))entry, arg)) return -1;
	pthread_detach(t);
	return 0;
}
void sysThreadExit(u64) { pthread_exit(NULL); }

// remote.cpp (HOST_REMOTE): how long the server thread waits once it has decided to stop, so a
// test can switch on again in exactly that moment
extern volatile int g_remote_stop_delay_ms;

static int status_hook(char *out, int n) { snprintf(out, n, "{\"drives\":[]}"); return 0; }
static int press_hook(const char *b) { return strcmp(b, "cross") != 0; }

struct Resp { int status; std::string head, body; };

// A connection to the server, -1 when it does not answer (it may still be starting: a few tries).
static int dial()
{
	for (int i = 0; i < 40; i++) {
		int s = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in a;
		memset(&a, 0, sizeof(a));
		a.sin_family = AF_INET;
		a.sin_port = htons(REMOTE_PORT);
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (!connect(s, (struct sockaddr *)&a, sizeof(a))) return s;
		close(s);
		usleep(50 * 1000);
	}
	return -1;
}

// The whole answer on connection s (HTTP/1.0: the server closes), waiting at most `secs`.
static Resp answer(int s, int secs = 3)
{
	Resp r = {0, "", ""};
	struct timeval tv = {secs, 0};
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	std::string all;
	char b[4096];
	int k;
	time_t t0 = time(NULL);
	while ((k = recv(s, b, sizeof(b), 0)) > 0) {
		all.append(b, k);
		if (all.size() > 200000 || time(NULL) - t0 > secs) break;   // the stream would go on
	}
	close(s);
	size_t e = all.find("\r\n\r\n");
	if (e == std::string::npos) return r;
	r.head = all.substr(0, e);
	r.body = all.substr(e + 4);
	sscanf(all.c_str(), "HTTP/1.%*d %d", &r.status);
	return r;
}

// One request over a fresh connection.
static Resp http(const std::string &req)
{
	int s = dial();
	if (s < 0) return Resp{0, "", ""};
	send(s, req.data(), req.size(), 0);
	shutdown(s, SHUT_WR);   // all sent: a short body ends here instead of waiting
	return answer(s);
}

static std::string post(const char *path, const std::string &body, const char *cl = "Content-Length")
{
	char h[256];
	snprintf(h, sizeof(h), "POST %s HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n%s: %zu\r\n\r\n", path, cl, body.size());
	return h + body;
}

static double now_s()
{
	struct timespec t;
	clock_gettime(CLOCK_MONOTONIC, &t);
	return t.tv_sec + t.tv_nsec / 1e9;
}

// A slow client: sends `start`, then one byte of `drip` every 300 ms (never finishing the request)
// until the server answers or closes, or `max_secs` pass. The answer, and how long it took.
static Resp slow_client(const std::string &start, char drip, int max_secs, double *took)
{
	int s = dial();
	if (s < 0) { *took = 0; return Resp{0, "", ""}; }
	const double t0 = now_s();
	send(s, start.data(), start.size(), 0);
	char b;
	for (;;) {
		usleep(300 * 1000);
		if (recv(s, &b, 1, MSG_PEEK | MSG_DONTWAIT) >= 0 || now_s() - t0 > max_secs) break;   // answered or closed
		if (send(s, &drip, 1, 0) < 0) break;
	}
	Resp r = answer(s, 1);
	*took = now_s() - t0;
	return r;
}

static std::string with_origin(const char *origin, const std::string &body)
{
	char h[256];
	snprintf(h, sizeof(h), "POST /mcp HTTP/1.1\r\nHost: x\r\nOrigin: %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n", origin, body.size());
	return h + body;
}

static int g_fail = 0;
static void expect(bool ok, const char *what, const Resp &r)
{
	if (ok) { printf("ok   %s\n", what); return; }
	printf("FAIL %s (status %d, body \"%.200s\")\n", what, r.status, r.body.c_str());
	g_fail++;
}

int main()
{
	signal(SIGPIPE, SIG_IGN);
	setvbuf(stdout, NULL, _IONBF, 0);
	static const RemoteHooks hooks = {status_hook, NULL, press_hook};
	static const McpHooks mh = {};
	remote_init(&hooks);
	mcp_init(&mh, "test");
	const std::string init = "{\"jsonrpc\":\"2.0\",\"id\":1,\"method\":\"initialize\",\"params\":{}}";

	remote_set(REMOTE_VIEW | REMOTE_MCP | REMOTE_STATUS);
	Resp r = http("GET / HTTP/1.1\r\nHost: x\r\n\r\n");
	expect(r.status == 200 && r.body.find("/pad") != std::string::npos, "all on: the page", r);
	r = http("GET /status.json HTTP/1.1\r\n\r\n");
	expect(r.status == 200 && r.body == "{\"drives\":[]}", "all on: status.json", r);
	r = http(post("/pad", "cross"));
	expect(r.status == 204, "all on: the pad", r);
	r = http(post("/mcp", init));
	expect(r.status == 200 && r.body.find("\"protocolVersion\":\"2025-06-18\"") != std::string::npos && r.body.find("\"id\":1") != std::string::npos,
	       "all on: POST /mcp initialize", r);
	{
		char cl[64];
		snprintf(cl, sizeof(cl), "Content-Length: %zu", r.body.size());
		expect(r.head.find("Content-Type: application/json") != std::string::npos && r.head.find(cl) != std::string::npos,
		       "POST /mcp: Content-Type application/json and the body's Content-Length", r);
	}
	r = http(post("/mcp", init, "content-length"));   // fetch and most HTTP clients send it in lower case
	expect(r.status == 200 && r.body.find("protocolVersion") != std::string::npos, "POST /mcp: content-length in lower case", r);
	r = http(post("/mcp", "{\"jsonrpc\":\"2.0\",\"method\":\"notifications/initialized\"}"));
	expect(r.status == 202 && r.body.empty(), "POST /mcp: a notification is 202 without a body", r);
	{   // a body well over the old 2 KB request buffer (but under 16 KB) is read whole
		std::string big = "{\"jsonrpc\":\"2.0\",\"id\":7,\"method\":\"ping\",\"params\":{\"pad\":\"" + std::string(12000, 'x') + "\"}}";
		r = http(post("/mcp", big));
		expect(r.status == 200 && r.body.find("\"id\":7") != std::string::npos && r.body.find("\"result\":{}") != std::string::npos, "POST /mcp: a 12 KB request", r);
		std::string max = "{\"jsonrpc\":\"2.0\",\"id\":8,\"method\":\"ping\",\"params\":{\"pad\":\"";
		max += std::string(MCP_MAX_REQUEST - max.size() - 3, 'y') + "\"}}";
		r = http(post("/mcp", max));
		expect(max.size() == MCP_MAX_REQUEST && r.status == 200 && r.body.find("\"id\":8") != std::string::npos, "POST /mcp: exactly 16 KB", r);
	}
	{   // over 16 KB: 413, without reading the body
		char h[160];
		snprintf(h, sizeof(h), "POST /mcp HTTP/1.1\r\nContent-Length: %d\r\n\r\n{\"jsonrpc\"", MCP_MAX_REQUEST + 1);
		r = http(h);
		expect(r.status == 413 && r.body.find("\"error\"") != std::string::npos, "POST /mcp over 16 KB: 413", r);
		r = http("POST /mcp HTTP/1.1\r\nContent-Length: 99999999999999999999\r\n\r\n{}");
		expect(r.status == 413, "POST /mcp with an absurd Content-Length: 413", r);
	}
	r = http("POST /mcp HTTP/1.1\r\nContent-Length: 40\r\n\r\n{\"jsonrpc\":\"2.0\"");   // the client stops sending
	expect(r.status == 400, "POST /mcp with a body shorter than its Content-Length: 400", r);
	r = http(post("/mcp", "{\"jsonrpc\":\"2.0\",\"id\":3,\"method\":\"ping\"}") + "trailing");
	expect(r.status == 200 && r.body.find("\"id\":3") != std::string::npos, "POST /mcp: bytes after the body are not part of it", r);
	{   // a body that ends before its Content-Length is never handled, even when it is valid JSON
		const std::string ping = "{\"jsonrpc\":\"2.0\",\"id\":4,\"method\":\"ping\"}";
		char h[128];
		snprintf(h, sizeof(h), "POST /mcp HTTP/1.1\r\nContent-Length: %zu\r\n\r\n", ping.size() + 20);
		r = http(h + ping);
		expect(r.status == 400 && r.body.find("\"id\":4") == std::string::npos, "POST /mcp: a valid JSON body shorter than its Content-Length is 400", r);
	}

	// Origin: a browser page elsewhere (DNS rebinding) must not reach MCP; the console's own may
	r = http(with_origin("http://evil.example", init));
	expect(r.status == 403 && r.body.find("\"error\"") != std::string::npos && r.body.find("protocolVersion") == std::string::npos, "Origin elsewhere: 403", r);
	r = http(with_origin("http://192.168.1.5:8090", init));
	expect(r.status == 403, "Origin of another LAN address: 403", r);
	r = http(with_origin("http://127.0.0.1:8090.evil.example", init));
	expect(r.status == 403, "Origin that only starts like the console's: 403", r);
	r = http(with_origin("http://127.0.0.1:3000", init));
	expect(r.status == 403, "Origin of the console's address on another port: 403", r);
	r = http(with_origin("null", init));
	expect(r.status == 403, "Origin null: 403", r);
	r = http(with_origin("http://127.0.0.1:8090", init));   // the test's console address (netCtlGetInfo above)
	expect(r.status == 200 && r.body.find("protocolVersion") != std::string::npos, "Origin of the console itself: allowed", r);
	r = http(with_origin("http://localhost:8090", init));
	expect(r.status == 200 && r.body.find("protocolVersion") != std::string::npos, "Origin localhost: allowed", r);

	// other methods on /mcp: 405 with Allow: POST; /mcp is the exact path
	r = http("GET /mcp HTTP/1.1\r\nHost: x\r\n\r\n");
	expect(r.status == 405 && r.head.find("Allow: POST") != std::string::npos, "GET /mcp: 405 with Allow: POST", r);
	r = http("DELETE /mcp HTTP/1.1\r\nHost: x\r\n\r\n");
	expect(r.status == 405 && r.head.find("Allow: POST") != std::string::npos, "DELETE /mcp: 405 with Allow: POST", r);
	r = http("PUT /mcp HTTP/1.1\r\nContent-Length: 2\r\n\r\n{}");
	expect(r.status == 405, "PUT /mcp: 405", r);
	r = http(post("/mcpx", init));
	expect(r.status == 404, "POST /mcpx: 404 (not MCP, not the page)", r);

	{   // slow clients: the whole request (headers and body) must come within REMOTE_READ_SECONDS
		double took;
		r = slow_client("GET /status.json HTTP/1.1\r\nX-Slow: ", 'a', REMOTE_READ_SECONDS + 6, &took);
		expect(took < REMOTE_READ_SECONDS + 2, "headers sent a byte at a time: closed by the deadline", r);
		r = slow_client("POST /mcp HTTP/1.1\r\nContent-Length: 400\r\n\r\n{", ' ', REMOTE_READ_SECONDS + 6, &took);
		expect(took < REMOTE_READ_SECONDS + 2 && r.status == 400, "MCP body sent a byte at a time: 400 by the deadline", r);
	}
	{   // at most REMOTE_MAX_CLIENTS connections at once; one more is 503 and closed
		int hold[REMOTE_MAX_CLIENTS];
		for (int i = 0; i < REMOTE_MAX_CLIENTS; i++) hold[i] = dial();
		usleep(300 * 1000);   // each has its thread now, waiting for its request
		r = http(post("/mcp", init));
		expect(r.status == 503, "one connection over the limit: 503", r);
		for (int i = 0; i < REMOTE_MAX_CLIENTS; i++) if (hold[i] >= 0) close(hold[i]);
		usleep(300 * 1000);
		r = http(post("/mcp", init));
		expect(r.status == 200, "after they closed: served again", r);
	}

	{   // switched off and on again at once (or nearly): the server still answers
		int dead = 0;
		for (int i = 0; i < 20 && !dead; i++) {
			remote_set(0);
			if (i & 1) usleep(i * 1000);
			remote_set(REMOTE_VIEW | REMOTE_MCP | REMOTE_STATUS);
			r = http(post("/mcp", init));
			if (r.status != 200) dead = 1;
		}
		expect(!dead, "off then on quickly, 20 times: the server answers each time", r);
		// on again while the server thread is ending (it has seen "off" but not yet stopped)
		g_remote_stop_delay_ms = 300;
		remote_set(0);
		usleep(100 * 1000);
		remote_set(REMOTE_VIEW | REMOTE_MCP | REMOTE_STATUS);
		usleep(400 * 1000);
		g_remote_stop_delay_ms = 0;
		r = http(post("/mcp", init));
		expect(r.status == 200, "on again while the server thread is ending: the server answers", r);
	}

	remote_set(REMOTE_MCP);   // MCP only
	r = http("GET / HTTP/1.1\r\n\r\n");
	expect(r.status == 404, "MCP only: the page is 404", r);
	r = http("GET /stream HTTP/1.1\r\n\r\n");
	expect(r.status == 404, "MCP only: the stream is 404", r);
	r = http("GET /shot.jpg HTTP/1.1\r\n\r\n");
	expect(r.status == 404, "MCP only: shot.jpg is 404", r);
	r = http(post("/pad", "cross"));
	expect(r.status == 404, "MCP only: the pad is 404", r);
	r = http("GET /status.json HTTP/1.1\r\n\r\n");
	expect(r.status == 404, "MCP only: status.json is 404", r);
	r = http(post("/mcp", init));
	expect(r.status == 200 && r.body.find("protocolVersion") != std::string::npos, "MCP only: POST /mcp", r);

	remote_set(REMOTE_VIEW);   // remote view only
	r = http(post("/mcp", init));
	expect(r.status == 403 && r.body.find("\"jsonrpc\":\"2.0\"") != std::string::npos && r.body.find("\"id\":null") != std::string::npos &&
	       r.body.find("\"error\"") != std::string::npos && r.body.find("MCP is off") != std::string::npos && r.body.find("Remote control") != std::string::npos,
	       "MCP off: a JSON-RPC error saying how to turn it on", r);
	r = http("GET /status.json HTTP/1.1\r\n\r\n");
	expect(r.status == 404, "remote view only: status.json is 404", r);
	r = http("GET / HTTP/1.1\r\n\r\n");
	expect(r.status == 200, "remote view only: the page", r);

	remote_set(REMOTE_STATUS);   // status.json only
	r = http("GET /status.json HTTP/1.1\r\n\r\n");
	expect(r.status == 200, "status.json only: status.json", r);
	r = http("GET / HTTP/1.1\r\n\r\n");
	expect(r.status == 404, "status.json only: the page is 404", r);

	remote_set(0);
	usleep(300 * 1000);
	r = http("GET /status.json HTTP/1.1\r\n\r\n");
	expect(r.status == 0, "all off: the server does not answer", r);

	printf("%s: %d problem(s)\n", g_fail ? "FAILED" : "passed", g_fail);
	return g_fail ? 1 : 0;
}
