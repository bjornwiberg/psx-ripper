// Development aid: streams the app's screen as MJPEG over HTTP, so a session can be followed
// from a browser (http://<ps3>:8090/). Only built with DEV=1 and only started when the flag file
// /dev_hdd0/tmp/psxrip.stream exists. It shows the app's own canvas; system dialogs are drawn by
// the PS3 on top and are not included.
#ifdef DEV_STREAM
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/thread.h>
#include <sys/socket.h>
#include <netinet/in.h>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#define STBI_WRITE_NO_STDIO
#include "stb_image_write.h"

#include "ui.h"
#include "devstream.h"

#define STREAM_PORT 8090

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

static void client_thread(void *arg)
{
	int s = (int)(long)arg;
	char req[512];
	int n = recv(s, req, sizeof(req) - 1, 0);
	req[n > 0 ? n : 0] = 0;
	JpgBuf jb = {NULL, 0, 0};
	uint8_t *rgb = (uint8_t *)malloc(ui_w() / 2 * (ui_h() / 2) * 3);
	if (!rgb) { close(s); sysThreadExit(0); }
	if (!strncmp(req, "GET /stream", 11)) {
		const char *hdr = "HTTP/1.0 200 OK\r\nCache-Control: no-cache\r\nContent-Type: multipart/x-mixed-replace; boundary=frame\r\n\r\n";
		if (send_all(s, hdr, strlen(hdr)) == 0) {
			for (;;) {
				int len = encode_frame(&jb, rgb);
				char ph[128];
				int k = snprintf(ph, sizeof(ph), "--frame\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", len);
				if (!len || send_all(s, ph, k) || send_all(s, jb.p, len) || send_all(s, "\r\n", 2)) break;
				usleep(500 * 1000);   // about 2 frames per second
			}
		}
	} else if (!strncmp(req, "GET /shot.jpg", 13)) {
		int len = encode_frame(&jb, rgb);
		char ph[160];
		int k = snprintf(ph, sizeof(ph), "HTTP/1.0 200 OK\r\nCache-Control: no-cache\r\nContent-Type: image/jpeg\r\nContent-Length: %d\r\n\r\n", len);
		if (len && !send_all(s, ph, k)) send_all(s, jb.p, len);
	} else {
		const char *page = "HTTP/1.0 200 OK\r\nContent-Type: text/html; charset=utf-8\r\n\r\n"
			"<!doctype html><meta name=viewport content='width=device-width'><title>PSX Ripper</title>"
			"<body style='margin:0;background:#111'><img src='/stream' style='width:100%'></body>";
		send_all(s, page, strlen(page));
	}
	free(jb.p);
	free(rgb);
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
	sa.sin_port = htons(STREAM_PORT);
	sa.sin_addr.s_addr = htonl(INADDR_ANY);
	if (ls < 0 || bind(ls, (struct sockaddr *)&sa, sizeof(sa)) || listen(ls, 4)) sysThreadExit(0);
	for (;;) {
		int s = accept(ls, NULL, NULL);
		if (s < 0) { usleep(100 * 1000); continue; }
		sys_ppu_thread_t t;
		// low priority (higher number): never compete with the rip threads
		if (sysThreadCreate(&t, client_thread, (void *)(long)s, 2000, 0x8000, 0, (char *)"psxstream")) close(s);
	}
}

void devstream_start()
{
	sys_ppu_thread_t t;
	sysThreadCreate(&t, server_thread, NULL, 2000, 0x4000, 0, (char *)"psxstreamd");
}
#endif
