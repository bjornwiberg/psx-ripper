// Remote view (see remote.h): MJPEG stream of the UI canvas, /status.json and a pad (POST /pad). System dialogs are drawn by the PS3 on top of the app and are not included.
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
		char js[1024] = "{}";
		if (g_hooks.status_json) g_hooks.status_json(js, sizeof(js));
		char hdr[128];
		snprintf(hdr, sizeof(hdr), "HTTP/1.0 200 OK\r\nContent-Type: application/json\r\nCache-Control: no-store\r\nContent-Length: %d\r\n\r\n", (int)strlen(js));
		if (!send_str(s, hdr)) send_str(s, js);
	} else if (!strncmp(req, "POST /pad", 9)) {   // a button of the web page's pad
		char b[16];
		size_t k = strcspn(body, " \r\n&");
		if (k >= sizeof(b)) k = sizeof(b) - 1;
		memcpy(b, body, k); b[k] = 0;
		int bad = !g_hooks.press || g_hooks.press(b);
		send_str(s, bad ? "HTTP/1.0 400 Bad Request\r\nContent-Length: 0\r\n\r\n" : "HTTP/1.0 204 No Content\r\n\r\n");
	} else {   // the page: the screen, and under it a pad whose buttons press the console's
		send_str(s, "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nCache-Control: no-cache\r\n\r\n");
		send_str(s,
			"<!doctype html><html lang=sv><meta charset=utf-8><meta name=viewport content='width=device-width,initial-scale=1'><title>PSX Ripper</title>\n"
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
			"<img src='/stream' alt=''>\n"
			"<div class=pad>\n"
			"<div class=row><div class=grp><button data-b=l2>L2</button><button data-b=l1>L1</button></div><div class=grp><button data-b=r1>R1</button><button data-b=r2>R2</button></div></div>\n"
			"<div class=dp><span></span><button data-b=up>&#9650;</button><span></span><button data-b=left>&#9664;</button><span></span><button data-b=right>&#9654;</button><span></span><button data-b=down>&#9660;</button><span></span></div>\n"
			"<div class=mid><button data-b=select>SELECT</button><button data-b=start>START</button></div>\n"
			"<div class=fa><span></span><button class=tr data-b=triangle>&#9651;</button><span></span><button class=sq data-b=square>&#9633;</button><span></span><button class=o data-b=circle>&#9675;</button><span></span><button class=c data-b=cross>&#10005;</button><span></span></div>\n"
			"</div>\n"
			"<p>Knapparna trycker på konsolens handkontroll. Tangentbord: pilar, Enter = &#10005;, Esc/Backspace = &#9675;, Q/E = L1/R1, 1/3 = L2/R2, T = &#9651;, S = &#9633;. <a href='/status.json'>status.json</a></p>\n"
			"<script>\n"
			"function press(b){var e=document.querySelector('[data-b='+b+']');if(e){e.classList.add('on');setTimeout(function(){e.classList.remove('on')},120)}fetch('/pad',{method:'POST',body:b}).catch(function(){})}\n"
			"document.querySelectorAll('[data-b]').forEach(function(e){e.addEventListener('click',function(){press(e.dataset.b)})});\n"
			"var K={ArrowUp:'up',ArrowDown:'down',ArrowLeft:'left',ArrowRight:'right',Enter:'cross',Escape:'circle',Backspace:'circle',q:'l1',e:'r1','1':'l2','3':'r2',t:'triangle',s:'square'};\n"
			"document.addEventListener('keydown',function(ev){var b=K[ev.key]||K[ev.key.toLowerCase()];if(b&&!ev.metaKey&&!ev.ctrlKey){ev.preventDefault();press(b)}});\n"
			"</script></html>\n");
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
