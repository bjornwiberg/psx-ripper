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
int netCtlGetInfo(int, union net_ctl_info *info) { snprintf(info->ip_address, sizeof(info->ip_address), "192.168.1.50"); return 0; }
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
// and the send buffer of its client sockets (0: the system's, which grows to megabytes here)
extern volatile int g_remote_sndbuf;

static int status_hook(char *out, int n) { snprintf(out, n, "{\"drives\":[]}"); return 0; }
// get_state answers with a large text: a client that never reads it fills the socket buffers
static volatile int g_state_delay_ms = 0;   // get_state takes this long (a slow tool)
static int big_state(char *out, int n)
{
	if (g_state_delay_ms) { usleep(g_state_delay_ms * 1000); snprintf(out, n, "slow"); return 0; }
	int k = n / 2 - 300;
	if (k > 250000) k = 250000;
	memset(out, '"', k);   // each one escaped: twice as long in the answer
	out[k] = 0;
	return 0;
}
static volatile int g_presses = 0;   // buttons the pad pressed
static int press_hook(const char *b) { g_presses++; return strcmp(b, "cross") != 0; }

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

static void *http_thread(void *arg)
{
	std::pair<std::string, Resp> *x = (std::pair<std::string, Resp> *)arg;
	x->second = http(x->first);
	return NULL;
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

static std::string with_origin(const char *origin, const std::string &body, const char *path = "/mcp")
{
	char h[256];
	snprintf(h, sizeof(h), "POST %s HTTP/1.1\r\nHost: x\r\nOrigin: %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n\r\n", path, origin, body.size());
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
	static const McpHooks mh = {big_state, remote_jpeg};
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
	r = http(with_origin("http://192.168.1.50:8090.evil.example", init));
	expect(r.status == 403, "Origin that only starts like the console's: 403", r);
	r = http(with_origin("http://192.168.1.50:3000", init));
	expect(r.status == 403, "Origin of the console's address on another port: 403", r);
	r = http(with_origin("http://127.0.0.1:3000", init));
	expect(r.status == 403, "Origin of the simulator's address on another port: 403", r);
	r = http(with_origin("null", init));
	expect(r.status == 403, "Origin null: 403", r);
	r = http(with_origin("http://192.168.1.50:8090", init));   // the test's console address (netCtlGetInfo above)
	expect(r.status == 200 && r.body.find("protocolVersion") != std::string::npos, "Origin of the console itself: allowed", r);
	// the simulator's page (HOST_REMOTE only; the console's build allows only its own address)
	r = http(with_origin("http://localhost:8090", init));
	expect(r.status == 200 && r.body.find("protocolVersion") != std::string::npos, "simulator: Origin localhost allowed", r);
	r = http(with_origin("http://127.0.0.1:8090", init));
	expect(r.status == 200 && r.body.find("protocolVersion") != std::string::npos, "simulator: Origin 127.0.0.1 allowed", r);

	// the pad: a page elsewhere must not press the console's buttons either; its own page and a
	// program (no Origin) may
	const int presses = g_presses;
	r = http(with_origin("http://evil.example", "cross", "/pad"));
	expect(r.status == 403, "POST /pad with an Origin elsewhere: 403", r);
	r = http(with_origin("null", "cross", "/pad"));
	expect(r.status == 403, "POST /pad with Origin null: 403", r);
	r = http(with_origin("http://192.168.1.50:3000", "cross", "/pad"));
	expect(r.status == 403, "POST /pad from the console's address on another port: 403", r);
	expect(g_presses == presses, "POST /pad refused by Origin: nothing pressed", r);
	r = http(with_origin("http://192.168.1.50:8090", "cross", "/pad"));
	expect(r.status == 204, "POST /pad from the console's own page: pressed", r);
	r = http(post("/pad", "cross"));
	expect(r.status == 204, "POST /pad without Origin: pressed", r);

	{   // a request waiting behind a slow one is not handled once MCP was turned off meanwhile
		g_state_delay_ms = 1000;
		std::pair<std::string, Resp> slow(post("/mcp", "{\"jsonrpc\":\"2.0\",\"id\":21,\"method\":\"tools/call\",\"params\":{\"name\":\"get_state\"}}"), Resp());
		std::pair<std::string, Resp> waiting(post("/mcp", "{\"jsonrpc\":\"2.0\",\"id\":22,\"method\":\"ping\"}"), Resp());
		pthread_t a, b;
		pthread_create(&a, NULL, http_thread, &slow);
		usleep(300 * 1000);
		pthread_create(&b, NULL, http_thread, &waiting);
		usleep(300 * 1000);
		remote_set(REMOTE_VIEW | REMOTE_STATUS);   // MCP off on the console while the second waits
		pthread_join(a, NULL);
		pthread_join(b, NULL);
		g_state_delay_ms = 0;
		expect(slow.second.status == 200 && slow.second.body.find("\"id\":21") != std::string::npos, "MCP off mid-request: the request being handled is answered", slow.second);
		expect(waiting.second.status == 403 && waiting.second.body.find("MCP is off") != std::string::npos && waiting.second.body.find("\"id\":22") == std::string::npos,
		       "MCP off while a request waits for another: 403, not handled", waiting.second);
		remote_set(REMOTE_VIEW | REMOTE_MCP | REMOTE_STATUS);
	}

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

	{   // a client that asks and then never reads the answer is dropped after REMOTE_SEND_SECONDS:
		// it cannot hold a slot (and its buffers) for ever
		const std::string ask = "{\"jsonrpc\":\"2.0\",\"id\":9,\"method\":\"tools/call\",\"params\":{\"name\":\"get_state\"}}";
		const std::string req = post("/mcp", ask);
		g_remote_sndbuf = 8192;
		int hold[REMOTE_MAX_CLIENTS];
		for (int i = 0; i < REMOTE_MAX_CLIENTS; i++) {
			hold[i] = socket(AF_INET, SOCK_STREAM, 0);
			int small = 2048;
			setsockopt(hold[i], SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));   // a small window: the answer cannot all be sent
			struct sockaddr_in a;
			memset(&a, 0, sizeof(a));
			a.sin_family = AF_INET; a.sin_port = htons(REMOTE_PORT); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
			if (connect(hold[i], (struct sockaddr *)&a, sizeof(a))) { close(hold[i]); hold[i] = -1; continue; }
			send(hold[i], req.data(), req.size(), 0);
		}
		usleep(500 * 1000);
		r = http(post("/mcp", init));
		expect(r.status == 503, "clients that do not read their answers hold every slot for now", r);
		usleep((REMOTE_SEND_SECONDS + 2) * 1000 * 1000);
		r = http(post("/mcp", init));
		expect(r.status == 200, "after the send timeout their slots are free again", r);
		g_remote_sndbuf = 0;
		for (int i = 0; i < REMOTE_MAX_CLIENTS; i++) if (hold[i] >= 0) close(hold[i]);
		usleep(200 * 1000);
	}
	{   // remote view streams have their own limit: open tabs never make the requests answer 503
		int st[REMOTE_MAX_STREAMS + 1], idle[REMOTE_MAX_CLIENTS - 1];
		const char *get = "GET /stream HTTP/1.1\r\nHost: x\r\n\r\n";
		for (int i = 0; i < REMOTE_MAX_STREAMS; i++) { st[i] = dial(); if (st[i] >= 0) send(st[i], get, strlen(get), 0); }
		usleep(400 * 1000);   // each is a stream now
		st[REMOTE_MAX_STREAMS] = dial();
		send(st[REMOTE_MAX_STREAMS], get, strlen(get), 0);
		Resp over = answer(st[REMOTE_MAX_STREAMS], 2);
		expect(over.status == 503, "a stream over the streams' limit: 503", over);
		for (int i = 0; i < REMOTE_MAX_CLIENTS - 1; i++) idle[i] = dial();   // and the requests' slots all but one taken
		usleep(300 * 1000);
		r = http(post("/mcp", init));
		expect(r.status == 200, "with the streams open and other clients waiting, /mcp still answers", r);
		for (int i = 0; i < REMOTE_MAX_CLIENTS - 1; i++) if (idle[i] >= 0) close(idle[i]);
		for (int i = 0; i < REMOTE_MAX_STREAMS; i++) if (st[i] >= 0) close(st[i]);
		usleep(REMOTE_SEND_SECONDS * 1000 * 1000 + 700 * 1000);   // a closed stream ends at its next frame
		int s2 = dial();
		send(s2, get, strlen(get), 0);
		Resp again = answer(s2, 1);
		expect(again.status == 200 && again.head.find("multipart/x-mixed-replace") != std::string::npos, "after they closed a stream is served again", again);
	}
	{   // get_screenshot: the remote view's encoder, as image content
		unsigned char *jpg = NULL;
		int len = 0;
		Resp none = {0, "", ""};
		int rc = remote_jpeg(&jpg, &len);
		expect(rc == 0 && len > 4 && jpg[0] == 0xFF && jpg[1] == 0xD8 && jpg[len - 2] == 0xFF && jpg[len - 1] == 0xD9, "remote_jpeg: a whole JPEG", none);
		free(jpg);
		r = http(post("/mcp", "{\"jsonrpc\":\"2.0\",\"id\":11,\"method\":\"tools/call\",\"params\":{\"name\":\"get_screenshot\"}}"));
		expect(r.status == 200 && r.body.find("\"type\":\"image\"") != std::string::npos && r.body.find("\"data\":\"/9j/") != std::string::npos &&
		       r.body.find("image/jpeg") != std::string::npos, "get_screenshot over MCP: image content", r);
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
