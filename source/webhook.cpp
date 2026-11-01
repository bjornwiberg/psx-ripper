// Webhooks, see webhook.h.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/thread.h>
#include <sys/time.h>
#include <net/netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include "webhook.h"

#define LOG_PATH "/dev_hdd0/tmp/psxrip.log"

static void wlog(const char *fmt, ...)
{
	FILE *f = fopen(LOG_PATH, "a");
	if (!f) return;
	va_list ap;
	va_start(ap, fmt);
	vfprintf(f, fmt, ap);
	va_end(ap);
	fputc('\n', f);
	fclose(f);
}

// Splits http://host[:port][/path]. Returns 0 on success.
static int parse_url(const char *url, char *host, size_t hn, int *port, char *path, size_t pn)
{
	if (strncmp(url, "http://", 7)) return -1;   // https would need a TLS library
	const char *h = url + 7, *e = h + strcspn(h, ":/");
	size_t k = (size_t)(e - h);
	if (!k || k >= hn) return -1;
	memcpy(host, h, k); host[k] = 0;
	*port = 80;
	if (*e == ':') { *port = atoi(e + 1); e += strcspn(e, "/"); }
	snprintf(path, pn, "%s", *e ? e : "/");
	return *port > 0 && *port < 65536 ? 0 : -1;
}

void webhook_host(const char *url, char *out, size_t n)
{
	char host[128], path[512];
	int port;
	if (parse_url(url, host, sizeof(host), &port, path, sizeof(path))) { snprintf(out, n, "?"); return; }
	if (port == 80) snprintf(out, n, "%s", host); else snprintf(out, n, "%s:%d", host, port);
}

int webhook_post(const char *url, const char *json, const char *headers)
{
	char host[128], path[512];
	int port;
	if (parse_url(url, host, sizeof(host), &port, path, sizeof(path))) { wlog("webhook: unsupported address (only http://)"); return -1; }
	struct hostent *h = gethostbyname(host);
	if (!h || !h->h_addr_list || !h->h_addr_list[0]) { wlog("webhook: cannot resolve %s", host); return -2; }
	int s = socket(AF_INET, SOCK_STREAM, 0);
	if (s < 0) return -3;
	struct timeval tv = {5, 0};
	setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
	setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
	struct sockaddr_in sa;
	memset(&sa, 0, sizeof(sa));
	sa.sin_family = AF_INET;
	sa.sin_port = htons(port);
	memcpy(&sa.sin_addr, h->h_addr_list[0], 4);
	int status = -4;
	if (connect(s, (struct sockaddr *)&sa, sizeof(sa)) == 0) {
		char hdr[2048];
		int bl = strlen(json);
		int n = snprintf(hdr, sizeof(hdr), "POST %s HTTP/1.0\r\nHost: %s\r\nUser-Agent: PSXRipper\r\nContent-Type: application/json\r\n"
		                 "%sContent-Length: %d\r\nConnection: close\r\n\r\n", path, host, headers ? headers : "", bl);
		if (n >= (int)sizeof(hdr)) n = sizeof(hdr) - 1;
		if (send(s, hdr, n, 0) == n && send(s, json, bl, 0) == bl) {
			char resp[64];
			int r = recv(s, resp, sizeof(resp) - 1, 0);
			if (r > 0) { resp[r] = 0; sscanf(resp, "HTTP/%*s %d", &status); }
			else status = -5;
		}
	}
	close(s);
	return status;
}

struct Job { char url[512]; char hdrs[1024]; char *json; };
static void send_thread(void *arg)
{
	Job *j = (Job *)arg;
	char host[160];
	webhook_host(j->url, host, sizeof(host));
	int st = webhook_post(j->url, j->json, j->hdrs);
	wlog("webhook to %s -> %d", host, st);   // the path (often a secret id) is never logged
	free(j->json);
	free(j);
	sysThreadExit(0);
}

void webhook_send_async(const char *url, const char *json, const char *headers)
{
	if (!url || !url[0]) return;
	Job *j = (Job *)malloc(sizeof(Job));
	if (!j) return;
	snprintf(j->url, sizeof(j->url), "%s", url);
	snprintf(j->hdrs, sizeof(j->hdrs), "%s", headers ? headers : "");
	j->json = strdup(json);
	sys_ppu_thread_t t;
	if (!j->json || sysThreadCreate(&t, send_thread, j, 1500, 0x4000, 0, (char *)"psxhook")) { free(j->json); free(j); }
}

void json_str(char *out, size_t n, const char *s)
{
	size_t o = strlen(out);
	if (o + 2 >= n) return;
	out[o++] = '"';
	for (; *s && o + 7 < n; s++) {
		unsigned char c = (unsigned char)*s;
		if (c == '"' || c == '\\') { out[o++] = '\\'; out[o++] = c; }
		else if (c < 0x20) o += snprintf(out + o, n - o, "\\u%04x", c);
		else out[o++] = c;
	}
	out[o++] = '"';
	out[o] = 0;
}

int webhook_event_index(const char *name)
{
	for (int i = 0; i < WH_COUNT; i++) if (!strcmp(name, WEBHOOK_EVENTS[i])) return i;
	return -1;
}
