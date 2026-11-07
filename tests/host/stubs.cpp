// Host build (UI tests): the PS3 system, the USB drives, the file system and the network, as far
// as the screens need them. Nothing here talks to hardware; tests set the host_* values.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <sys/stat.h>
#include <dirent.h>
#include <time.h>
#include "ppu-types.h"
#include "io/pad.h"
#include "sys/thread.h"
#include "sys/mutex.h"
#include "sys/memory.h"
#include "sysutil/sysutil.h"
#include "sysutil/osk.h"
#include "sysutil/disc.h"
#include "net/net.h"
#ifdef HOST_REMOTE
#include "net/netctl.h"
#endif
#include "rsxutil.h"
#include "usbodd.h"
#include "vfs.h"
#include "redump.h"
#include "remote.h"
#include "webhook.h"
#include "host.h"

void (*host_io)() = 0;   // called by the drive scans and free-space queries (a test checks which locks are held)
// lv2: free space on a drive (syscall 840) is a hard disk with room; nothing else happens
void host_lv2_3(int n, u64, u64 total, u64 avail)
{
	if (n == 840 && host_io) host_io();
	if (n == 840) { *(uint64_t *)(uintptr_t)total = 465ull << 30; *(uint64_t *)(uintptr_t)avail = 338ull << 30; }
}

// the screen
u32 display_width = 1920, display_height = 1080, color_pitch = 1920 * 4, curr_fb = 0;
u32 *color_buffer[2];
int flip_timeouts = 0;
gcmContextData *context = 0;
void (*host_frame)() = 0;
void flip_sync() { if (host_frame) host_frame(); }
void flip() {}
#ifdef HOST_REMOTE
void waitflip() { usleep(16 * 1000); }   // about 60 frames per second, as on the console
#else
void waitflip() {}
#endif
void init_screen(void *, u32) {}

// the pad: the buttons of the next frame
HostPad host_pad;
int ioPadInit(u32) { return 0; }
int ioPadGetInfo(padInfo *i) { memset(i, 0, sizeof(*i)); i->status[0] = 1; return 0; }
int ioPadGetData(u32 port, padData *d)
{
	memset(d, 0, sizeof(*d));
	if (port) return 0;
	d->len = 1;
	d->BTN_CROSS = host_pad.cross; d->BTN_CIRCLE = host_pad.circle; d->BTN_TRIANGLE = host_pad.triangle; d->BTN_SQUARE = host_pad.square;
	d->BTN_UP = host_pad.up; d->BTN_DOWN = host_pad.down; d->BTN_LEFT = host_pad.left; d->BTN_RIGHT = host_pad.right;
	d->BTN_START = host_pad.start; d->BTN_SELECT = host_pad.select; d->BTN_L1 = host_pad.l1; d->BTN_R1 = host_pad.r1;
	d->BTN_L2 = host_pad.l2; d->BTN_R2 = host_pad.r2;
	return 0;
}

// threads and locks: the screens are drawn on one thread; nothing runs in the background.
// The remote view simulator (HOST_REMOTE) runs the real web server: its threads are real.
#ifdef HOST_REMOTE
#include <pthread.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <arpa/inet.h>
int sysThreadCreate(sys_ppu_thread_t *id, void (*entry)(void *), void *arg, int, u64, u64, char *name)
{
	*id = 0;
	if (!name || strncmp(name, "psxremote", 9)) return -1;   // rips, identify, USB polling: not here
	pthread_t t;
	if (pthread_create(&t, NULL, (void *(*)(void *))entry, arg)) return -1;
	pthread_detach(t);
	return 0;
}
void sysThreadExit(u64) { pthread_exit(NULL); }   // only the server's threads call it here
#else
int host_webman_calls = 0;   // webMAN requests the app started (a popup, ...): counted, not sent
int sysThreadCreate(sys_ppu_thread_t *id, void (*)(void *), void *, int, u64, u64, char *name)
{
	*id = 0;
	if (name && !strcmp(name, "psxwebman")) __sync_fetch_and_add(&host_webman_calls, 1);
	return -1;
}
void sysThreadExit(u64) {}
#endif
int sysThreadJoin(sys_ppu_thread_t, u64 *) { return 0; }
// Real mutexes, recursive when the attributes say so (as lv2's): a test can run a screen and MCP's
// hooks on two threads. A handle is an index; 0 was never created (a lock on it fails, as on the console).
#include <pthread.h>
#define HOST_MUTEXES 32
static pthread_mutex_t g_host_mx[HOST_MUTEXES];
static pthread_t g_host_owner[HOST_MUTEXES];
static int g_host_depth[HOST_MUTEXES], g_host_mx_n = 1;
int sysMutexCreate(sys_mutex_t *m, const sys_mutex_attr_t *a)
{
	int i = __sync_fetch_and_add(&g_host_mx_n, 1);
	if (i >= HOST_MUTEXES) return -1;
	pthread_mutexattr_t at;
	pthread_mutexattr_init(&at);
	pthread_mutexattr_settype(&at, a && a->attr_recursive == SYS_MUTEX_ATTR_RECURSIVE ? PTHREAD_MUTEX_RECURSIVE : PTHREAD_MUTEX_ERRORCHECK);
	pthread_mutex_init(&g_host_mx[i], &at);
	pthread_mutexattr_destroy(&at);
	*m = i;
	return 0;
}
int sysMutexLock(sys_mutex_t m, u64)
{
	if (m < 1 || m >= (sys_mutex_t)g_host_mx_n) return -1;
	int r = pthread_mutex_lock(&g_host_mx[m]);
	if (r) { fprintf(stderr, "FAIL mutex %u: lock error %d (a second lock of a lock that is not recursive)\n", m, r); abort(); }
	g_host_owner[m] = pthread_self();
	g_host_depth[m]++;
	return 0;
}
int sysMutexUnlock(sys_mutex_t m)
{
	if (m < 1 || m >= (sys_mutex_t)g_host_mx_n) return -1;
	if (!--g_host_depth[m]) g_host_owner[m] = pthread_t();
	return pthread_mutex_unlock(&g_host_mx[m]);
}
int host_mutex_held(sys_mutex_t m)   // by the calling thread
{
	return m >= 1 && m < (sys_mutex_t)g_host_mx_n && g_host_depth[m] > 0 && pthread_equal(g_host_owner[m], pthread_self());
}
int sysMemContainerCreate(sys_mem_container_t *c, u64) { *c = 1; return 0; }
int sysMemContainerDestroy(sys_mem_container_t) { return 0; }

// system utilities
int host_lang = PS3_LANG_SWEDISH;
int sysUtilRegisterCallback(int, sysutilCallback, void *) { return 0; }
int sysUtilCheckCallback() { return 0; }
int sysUtilGetSystemParamInt(int id, s32 *v) { *v = id == SYSUTIL_SYSTEMPARAM_ID_LANG ? host_lang : 0; return 0; }
int oskSetKeyLayoutOption(u32) { return 0; }
int oskLoadAsync(sys_mem_container_t, const oskParam *, const oskInputFieldInfo *) { return -1; }
int oskUnloadAsync(oskCallbackReturnParam *) { return 0; }
int sysDiscRegisterDiscChangeCallback(void (*)(void), void (*)(u32, char *)) { return 0; }
int sysDiscUnregisterDiscChangeCallback() { return 0; }
int netInitialize() { return 0; }

// USB optical drives: as many as a test connects
HostUsb host_usb[USBODD_DRIVES];
int usbodd_init() { return 0; }
int usbodd_count() { int n = 0; for (int i = 0; i < USBODD_DRIVES; i++) n += host_usb[i].attached; return n; }
int usbodd_attached(int d) { return d >= 0 && d < USBODD_DRIVES && host_usb[d].attached; }
void usbodd_status(char *out, int n) { snprintf(out, n, "host\n"); }
int usbodd_reset(int) { return 0; }
int usbodd_cmd(int, const uint8_t *, int, uint8_t *, int, int) { return -1; }
int usbodd_last_got(int) { return 0; }
int usbodd_identify(int) { return 0; }
const char *usbodd_name(int d) { return d >= 0 && d < USBODD_DRIVES ? host_usb[d].name : ""; }
int usbodd_location(int, unsigned char *loc8) { memset(loc8, 0, 8); return -1; }
int usbodd_port(int d) { return d >= 0 && d < USBODD_DRIVES && host_usb[d].attached ? host_usb[d].port : -1; }
int usbodd_unlock(int) { return 0; }
int usbodd_ready(int, int) { return -1; }
int usbodd_eject(int) { return 0; }
int usbodd_scsi(int, const uint8_t *, int, uint8_t *, int, int, int *, int *, char *log, int logn) { if (log) snprintf(log, logn, "host\n"); return -1; }

// files: real files through stdio, so image writes and checks can be tested on the host; the app's
// paths (/dev_hdd0/..., exfatN:/...) are looked up under host_root, empty by default, so the screens
// see no files, as on an empty console
char host_root[256];
int host_exfat[8];
char host_label[8][24];
static const char *hp(const char *p, char *buf, size_t n)
{
	if (!host_root[0]) return p;
	if (!strncmp(p, "/dev_", 5)) { snprintf(buf, n, "%s%s", host_root, p); return buf; }
	if (!strncmp(p, "exfat", 5) && p[5] >= '0' && p[5] <= '7' && p[6] == ':') { snprintf(buf, n, "%s/exfat%c%s", host_root, p[5], p + 7); return buf; }
	return p;
}
struct VFile { FILE *f; };
int vf_exists(const char *path) { char b[512]; struct stat st; return stat(hp(path, b, sizeof b), &st) == 0; }
VFile *vf_open(const char *path, const char *mode)
{
	char b[512];
	FILE *f = fopen(hp(path, b, sizeof b), mode);
	if (!f) return 0;
	VFile *v = (VFile *)malloc(sizeof(VFile));
	v->f = f;
	return v;
}
size_t vf_write(VFile *f, const void *p, size_t n) { return fwrite(p, 1, n, f->f); }
size_t vf_read(VFile *f, void *p, size_t n) { return fread(p, 1, n, f->f); }
int vf_seek(VFile *f, uint64_t pos) { return fseeko(f->f, (off_t)pos, SEEK_SET); }
int vf_printf(VFile *f, const char *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	int n = vfprintf(f->f, fmt, ap);
	va_end(ap);
	return n;
}
int vf_close(VFile *f) { int r = fclose(f->f); free(f); return r; }
int vf_unlink(const char *path) { char b[512]; return unlink(hp(path, b, sizeof b)); }
int (*host_rename_hook)(const char *from, const char *to);   // nonzero: the rename fails (the tests' blocked or failing renames)
int vf_rename(const char *from, const char *to) { char a[512], b[512]; if (host_rename_hook && host_rename_hook(from, to)) return -1; return rename(hp(from, a, sizeof a), hp(to, b, sizeof b)); }
int vf_mkdir(const char *path) { char b[512]; struct stat st; const char *p = hp(path, b, sizeof b); return mkdir(p, 0777) == 0 || (stat(p, &st) == 0 && S_ISDIR(st.st_mode)) ? 0 : -1; }
int vf_scan(VfVolume *out, int max)
{
	if (host_io) host_io();
	int n = 0;
	for (int port = 0; port < 8; port++)
		if (host_exfat[port]) {
			if (n < max) { snprintf(out[n].root, sizeof(out[n].root), "exfat%d:", port); out[n].port = port; out[n].type = VF_EXFAT; out[n].free_mb = 0; }
			n++;
		}
	return n;
}
int vf_free_mb(const char *, uint64_t *mb) { if (host_io) host_io(); *mb = 338 * 1024; return 0; }   // a hard disk with room
int vf_list(const char *, void (*)(const char *, void *), void *) { return 0; }
const char *host_listdir_extra;   // a name every listing adds (a file a case-sensitive file system has too)
int vf_listdir(const char *dir, void (*cb)(const char *, void *), void *ctx)
{
	char b[512];
	DIR *d = opendir(hp(dir, b, sizeof b));
	if (!d) return -1;
	int n = 0;
	struct dirent *de;
	while ((de = readdir(d))) if (de->d_name[0] != '.') { cb(de->d_name, ctx); n++; }
	closedir(d);
	if (host_listdir_extra) { cb(host_listdir_extra, ctx); n++; }
	return n;
}
int vf_size(const char *path, uint64_t *size) { char b[512]; struct stat st; if (stat(hp(path, b, sizeof b), &st)) return -1; *size = (uint64_t)st.st_size; return 0; }
int vf_mtime(const char *path, time_t *t)
{
	if (strncmp(path, "/dev_hdd0", 9)) return -1;   // as on the console: only the hard disk keeps folder dates
	char b[512];
	struct stat st;
	if (stat(hp(path, b, sizeof b), &st)) return -1;
	*t = st.st_mtime;
	return 0;
}
int vf_drive_present(int port)
{
	if (port < 0 || port > 7) return 0;
	char dev[32], b[512];
	snprintf(dev, sizeof dev, "/dev_usb%03d", port);
	struct stat st;
	return host_exfat[port] || (host_root[0] && stat(hp(dev, b, sizeof b), &st) == 0);
}
int vf_label(int port, char *out, size_t n) { if (port < 0 || port > 7) return -1; snprintf(out, n, "%s", host_label[port]); return 0; }

// Redump, the remote view and webhooks: no network on the host
int rd_local_info(char *ver, size_t n, time_t *when) { snprintf(ver, n, "2026-06-15"); *when = 0; return 1; }
int rd_local_by_size(uint64_t, int, char (*)[RD_NAME], int) { return -1; }
int host_rd_local;
unsigned char host_rd_md5[16];
char host_rd_name[160];
int rd_local_verify(const uint8_t (*md5)[16], int n, char *name, size_t nn)
{
	if (!host_rd_local) return -1;
	if (n != 1 || memcmp(md5[0], host_rd_md5, 16)) return 0;
	snprintf(name, nn, "%s", host_rd_name);
	return 1;
}
int rd_online(const char *, RdDisc *, int) { return -1; }
int rd_download(volatile uint32_t *, volatile uint32_t *, volatile int *, volatile int *) { return -1; }
#ifdef HOST_REMOTE
int netCtlInit() { return 0; }
int netCtlGetInfo(int, union net_ctl_info *info)   // this computer's first LAN address
{
	struct ifaddrs *ifs, *i;
	snprintf(info->ip_address, sizeof(info->ip_address), "127.0.0.1");
	if (getifaddrs(&ifs)) return 0;
	for (i = ifs; i; i = i->ifa_next)
		if (i->ifa_addr && i->ifa_addr->sa_family == AF_INET) {
			const char *a = inet_ntoa(((struct sockaddr_in *)i->ifa_addr)->sin_addr);
			if (strncmp(a, "127.", 4)) { snprintf(info->ip_address, sizeof(info->ip_address), "%s", a); break; }
		}
	freeifaddrs(ifs);
	return 0;
}
#else
void remote_init(const RemoteHooks *) {}
int host_remote_flags = -1;   // what the app last told the web server to serve (remote_set)
void remote_set(int flags) { host_remote_flags = flags; }
int remote_ip(char *out, int n) { snprintf(out, n, "192.168.1.50"); return 0; }
int remote_jpeg(unsigned char **jpg, int *len) { *jpg = NULL; *len = 0; return -1; }   // the encoder is remote.cpp's (remote_test)
#endif
void json_str(char *out, size_t n, const char *s) { size_t o = strlen(out); snprintf(out + o, n - o, "\"%s\"", s); }
int webhook_event_index(const char *) { return -1; }
void webhook_host(const char *url, char *out, size_t n) { snprintf(out, n, "%s", url); }
int webhook_post(const char *, const char *, const char *) { return -1; }
void webhook_send_async(const char *, const char *, const char *) {}
