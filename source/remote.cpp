// Remote view (see remote.h): MJPEG stream of the UI canvas, /status.json and a page to set the
// webhook address. System dialogs are drawn by the PS3 on top of the app and are not included.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/thread.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <net/netctl.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

#include "ui.h"
#include "lang.h"
#include "remote.h"

static RemoteHooks g_hooks;
static volatile int g_on = 0, g_listen = -1, g_running = 0;

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

// application/x-www-form-urlencoded value of `key` from a request body.
static void form_value(const char *body, const char *key, char *out, size_t n)
{
	out[0] = 0;
	size_t kl = strlen(key);
	for (const char *p = body; p && *p; ) {
		if (!strncmp(p, key, kl) && p[kl] == '=') {
			size_t o = 0;
			for (const char *v = p + kl + 1; *v && *v != '&' && o < n - 1; v++) {
				if (*v == '+') out[o++] = ' ';
				else if (*v == '%' && v[1] && v[2]) { char h[3] = {v[1], v[2], 0}; out[o++] = (char)strtol(h, NULL, 16); v += 2; }
				else out[o++] = *v;
			}
			out[o] = 0;
			return;
		}
		p = strchr(p, '&');
		if (p) p++;
	}
}

static void html_attr(char *out, size_t n, const char *s)
{
	size_t o = 0;
	for (; *s && o + 7 < n; s++) {
		if (*s == '"') o += snprintf(out + o, n - o, "&quot;");
		else if (*s == '&') o += snprintf(out + o, n - o, "&amp;");
		else if (*s == '<') o += snprintf(out + o, n - o, "&lt;");
		else out[o++] = *s;
	}
	out[o] = 0;
}

static void webhook_page(int s, const char *msg)
{
	char url[512] = "", esc[1024];
	if (g_hooks.get_webhook) g_hooks.get_webhook(url, sizeof(url));
	html_attr(esc, sizeof(esc), url);
	char *page = (char *)malloc(4096);
	if (!page) return;
	snprintf(page, 4096,
		"HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-store\r\n\r\n"
		"<!doctype html><meta name=viewport content='width=device-width'><title>PSX Ripper</title>"
		"<body style='font-family:sans-serif;background:#141826;color:#e8e8f0;padding:16px;max-width:640px'>"
		"<h2>PSX Ripper · Webhook</h2><p>%s</p>"
		"<form method=post action='/webhook'><input name=url value=\"%s\" placeholder='http://homeassistant.local:8123/api/webhook/...' "
		"style='width:100%%;padding:8px;font-size:16px;box-sizing:border-box'><p><button>%s</button></p></form>"
		"<form method=post action='/webhook/test'><button>%s</button></form>"
		"<p style='color:#f0c060'>%s</p><p><a style='color:#9ad0a8' href='/'>%s</a></p></body>",
		T(STR_WEB_HELP), esc, T(STR_WEB_SAVE), T(STR_WEB_TEST), msg ? msg : "", T(STR_WEB_BACK));
	send_str(s, page);
	free(page);
}

static void client_thread(void *arg)
{
	int s = (int)(long)arg;
	char req[2048];
	int n = 0;
	// read the headers and, for a POST, the (small) body
	for (;;) {
		int r = recv(s, req + n, sizeof(req) - 1 - n, 0);
		if (r <= 0) break;
		n += r; req[n] = 0;
		char *e = strstr(req, "\r\n\r\n");
		if (!e) { if (n >= (int)sizeof(req) - 1) break; continue; }
		const char *cl = strstr(req, "Content-Length:");
		int want = cl ? atoi(cl + 15) : 0;
		if ((int)(req + n - (e + 4)) >= want || n >= (int)sizeof(req) - 1) break;
	}
	req[n] = 0;
	const char *body = strstr(req, "\r\n\r\n");
	body = body ? body + 4 : "";

	if (!strncmp(req, "GET /stream", 11) || !strncmp(req, "GET /shot.jpg", 13)) {
		JpgBuf jb = {NULL, 0, 0};
		uint8_t *rgb = (uint8_t *)malloc(ui_w() / 2 * (ui_h() / 2) * 3);
		if (rgb && !strncmp(req, "GET /stream", 11)) {
			if (!send_str(s, "HTTP/1.0 200 OK\r\nCache-Control: no-cache\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n")) {
				while (g_on) {
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
	} else if (!strncmp(req, "GET /status.json", 16)) {
		char js[1024] = "{}";
		if (g_hooks.status_json) g_hooks.status_json(js, sizeof(js));
		char hdr[128];
		snprintf(hdr, sizeof(hdr), "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nContent-Length: %d\r\n\r\n", (int)strlen(js));
		if (!send_str(s, hdr)) send_str(s, js);
	} else if (!strncmp(req, "POST /webhook/test", 18)) {
		char msg[96];
		snprintf(msg, sizeof(msg), T(STR_WEB_TESTED), g_hooks.test_webhook ? g_hooks.test_webhook() : -1);
		webhook_page(s, msg);
	} else if (!strncmp(req, "POST /webhook", 13)) {
		char url[512];
		form_value(body, "url", url, sizeof(url));
		if (g_hooks.set_webhook) g_hooks.set_webhook(url);
		webhook_page(s, T(STR_WEB_SAVED));
	} else if (!strncmp(req, "GET /webhook", 12)) {
		webhook_page(s, NULL);
	} else {
		send_str(s,
			"HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n\r\n"
			"<!doctype html><meta name=viewport content='width=device-width'><title>PSX Ripper</title>"
			"<body style='margin:0;background:#111;font-family:sans-serif'><img src='/stream' style='width:100%;display:block'>"
			"<p style='padding:8px 16px'><a style='color:#9ad0a8' href='/webhook'>Webhook</a> · "
			"<a style='color:#9ad0a8' href='/status.json'>status.json</a></p></body>");
	}
	close(s);
	sysThreadExit(0);
}

static void server_thread(void *arg)
{
	int ls = socket(AF_INET, SOCK_STREAM, 0);
	int one = 1;
	setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(REMOTE_PORT);
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	if (ls < 0 || bind(ls, (struct sockaddr *)&sa, sizeof(sa)) || listen(ls, 4)) {
		if (ls >= 0) close(ls);
		g_running = 0;
		sysThreadExit(0);
	}
	g_listen = ls;
	while (g_on) {
		int s = accept(ls, NULL, NULL);
		if (s < 0) { if (!g_on) break; usleep(100 * 1000); continue; }
		sys_ppu_thread_t t;
		// low priority (higher number): never compete with the rip threads
		if (sysThreadCreate(&t, client_thread, (void *)(long)s, 2000, 0x8000, 0, (char *)"psxremote")) close(s);
	}
	g_listen = -1;
	g_running = 0;
	sysThreadExit(0);
}

void remote_init(const RemoteHooks *hooks)
{
	g_hooks = *hooks;
	netCtlInit();
}

void remote_set(int on)
{
	if (on && !g_running) {
		g_on = 1;
		g_running = 1;
		sys_ppu_thread_t t;
		if (sysThreadCreate(&t, server_thread, NULL, 2000, 0x4000, 0, (char *)"psxremoted")) g_running = 0;
	} else if (!on && g_on) {
		g_on = 0;                       // stream clients stop after their current frame
		int ls = g_listen;
		if (ls >= 0) { shutdown(ls, SHUT_RDWR); close(ls); }   // wakes accept()
	}
}
