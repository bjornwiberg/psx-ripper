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

static int status_hook(char *out, int n) { snprintf(out, n, "{\"drives\":[]}"); return 0; }
static int press_hook(const char *b) { return strcmp(b, "cross") != 0; }

struct Resp { int status; std::string head, body; };

// One request over a fresh connection; the whole answer (HTTP/1.0: the server closes).
static Resp http(const std::string &req)
{
	Resp r = {0, "", ""};
	int s = -1;
	for (int i = 0; i < 40; i++) {   // the server may still be starting
		s = socket(AF_INET, SOCK_STREAM, 0);
		struct sockaddr_in a;
		memset(&a, 0, sizeof(a));
		a.sin_family = AF_INET;
		a.sin_port = htons(REMOTE_PORT);
		a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
		if (!connect(s, (struct sockaddr *)&a, sizeof(a))) break;
		close(s);
		s = -1;
		usleep(50 * 1000);
	}
	if (s < 0) return r;
	struct timeval tv = {3, 0};
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	send(s, req.data(), req.size(), 0);
	shutdown(s, SHUT_WR);   // all sent: a short body ends here instead of waiting
	std::string all;
	char b[4096];
	int k;
	time_t t0 = time(NULL);
	while ((k = recv(s, b, sizeof(b), 0)) > 0) {
		all.append(b, k);
		if (all.size() > 200000 || time(NULL) - t0 > 3) break;   // the stream would go on
	}
	close(s);
	size_t e = all.find("\r\n\r\n");
	if (e == std::string::npos) return r;
	r.head = all.substr(0, e);
	r.body = all.substr(e + 4);
	sscanf(all.c_str(), "HTTP/1.%*d %d", &r.status);
	return r;
}

static std::string post(const char *path, const std::string &body, const char *cl = "Content-Length")
{
	char h[256];
	snprintf(h, sizeof(h), "POST %s HTTP/1.1\r\nHost: x\r\nContent-Type: application/json\r\n%s: %zu\r\n\r\n", path, cl, body.size());
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
