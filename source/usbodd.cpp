// USB optical drives through an own USB driver (PSL1GHT usbd): the PS3 system lists USB drives as
// storage but does not pass SCSI commands to them, so a SuperDrive cannot even be unlocked that
// way. This driver claims the drive by vendor/product id and talks USB Mass Storage Bulk-Only
// Transport itself: CBW (command), data, CSW (status). The ripper uses it as an alternative drive
// /dbg can send single commands for diagnosis. Up to USBODD_DRIVES drives,
// each with its own pipes, transfer memory and lock, so they can be used at the same time.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
extern "C" {   // usb.h has no C++ guard
#include <usb/usb.h>
}
#include <sys/mutex.h>
#include <sysmodule/sysmodule.h>
#include "usbodd.h"

#define LOG_PATH "/dev_hdd0/tmp/psxrip.log"
static void ulog(const char *fmt, ...)
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

struct UsbDrive;
struct Xfer { UsbDrive *d; unsigned seq; };   // callback argument: which drive, which transfer

struct UsbDrive {
	volatile s32 dev, ctrl, in, out;
	uint8_t ep_in, ep_out, ifnum;
	volatile int attached;
	unsigned tag;
	sys_mutex_t lock;              // one command at a time (rip thread, presence poll, /dbg)
	void *cbw, *csw, *buf;         // transfer memory from usbAllocateMemory
	volatile int done;             // completion of the transfer in flight
	volatile unsigned seq;         // number of that transfer; a late completion of an earlier one is ignored
	Xfer xf[16];
	usbEndpointDescriptor ein, eout;   // copies, to reopen the bulk pipes
	volatile s32 res, cnt;
	volatile int last_got;
	usbDeviceRequest req;          // control request in flight (must stay valid until done)
	int apple;                     // 05AC:1500
	unsigned char loc[8];          // usbGetDeviceLocation: root port, then hub ports
	char name[40];                 // "Apple SuperDrive" from INQUIRY, empty until asked
	char trace[1024];              // step trace of the last command (the callers that want it pass their own)
};
static UsbDrive g_drv[USBODD_DRIVES];
static volatile int g_probes = 0, g_attaches = 0;
static int g_init_rc = 1, g_reg_rc = 1;

static void done_cb(s32 result, s32 count, void *arg)
{
	Xfer *x = (Xfer *)arg;
	UsbDrive *d = x->d;
	if (x->seq != d->seq) return;   // a transfer we gave up waiting for: its answer must not end the current one
	d->res = result; d->cnt = count; d->done = 1;
}

// Prepares the completion for the next transfer and returns its callback argument.
static void *next_xfer(UsbDrive *d)
{
	d->seq++;
	Xfer *x = &d->xf[d->seq & 15];
	x->d = d;
	x->seq = d->seq;
	d->done = 0;
	return x;
}

static uint64_t ms_now()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int wait_done(UsbDrive *d, int ms)   // by the clock: usleep granularity varies
{
	uint64_t end = ms_now() + ms;
	while (!d->done && ms_now() < end) usleep(2000);
	return d->done ? 0 : -1;
}

static UsbDrive *drive(int i) { return i >= 0 && i < USBODD_DRIVES && g_drv[i].attached ? &g_drv[i] : NULL; }

static s32 odd_probe(s32 dev)
{
	g_probes++;
	ulog("usbodd: probe dev %d", dev);
	return USB_PROBE_SUCCEEDED;
}

static s32 odd_attach(s32 dev)
{
	g_attaches++;
	int slot = -1;
	for (int i = 0; i < USBODD_DRIVES && slot < 0; i++) if (!g_drv[i].attached) slot = i;
	if (slot < 0) { ulog("usbodd: attach dev %d: no free slot", dev); return USB_ATTACH_FAILED; }
	UsbDrive *d = &g_drv[slot];
	usbInterfaceDescriptor *ifc = (usbInterfaceDescriptor *)usbScanStaticDescriptor(dev, NULL, USB_DESCRIPTOR_TYPE_INTERFACE);
	if (!ifc) { ulog("usbodd: attach dev %d: no interface", dev); return USB_ATTACH_FAILED; }
	ulog("usbodd: attach dev %d (drive %d): class %02x/%02x/%02x, %d endpoints", dev, slot, ifc->bInterfaceClass,
	     ifc->bInterfaceSubClass, ifc->bInterfaceProtocol, ifc->bNumEndpoints);
	usbEndpointDescriptor *ein = NULL, *eout = NULL;
	void *p = ifc;
	for (int i = 0; i < 8; i++) {
		usbEndpointDescriptor *e = (usbEndpointDescriptor *)usbScanStaticDescriptor(dev, p, USB_DESCRIPTOR_TYPE_ENDPOINT);
		if (!e) break;
		p = e;
		if ((e->bmAttributes & USB_ENDPOINT_TRANSFER_TYPE_BITS) != USB_ENDPOINT_TRANSFER_TYPE_BULK) continue;
		if (e->bEndpointAddress & USB_ENDPOINT_DIRECTION_IN) ein = e; else eout = e;
	}
	if (!ein || !eout) { ulog("usbodd: attach dev %d: no bulk endpoints", dev); return USB_ATTACH_FAILED; }
	d->ctrl = usbOpenPipe(dev, NULL);
	// select the configuration, or the bulk endpoints stay inactive (the answer comes later, in the
	// callback; nothing waits for it here because attach runs on the USB thread)
	const uint8_t *cfg = (const uint8_t *)usbScanStaticDescriptor(dev, NULL, USB_DESCRIPTOR_TYPE_CONFIG);
	if (d->ctrl >= 0 && cfg) {
		d->req.bmRequestType = USB_REQTYPE_DIR_TO_DEVICE | USB_REQTYPE_TYPE_STANDARD | USB_REQTYPE_RECIP_DEVICE;
		d->req.bRequest = USB_REQUEST_SET_CONFIGURATION;
		d->req.wValue = cfg[5];   // bConfigurationValue
		d->req.wIndex = 0;
		d->req.wLength = 0;
		s32 rc = usbControlTransfer(d->ctrl, &d->req, NULL, done_cb, next_xfer(d));
		ulog("usbodd: set configuration %u -> 0x%x", cfg[5], rc);
	}
	d->ein = *ein; d->eout = *eout;
	d->in = usbOpenPipe(dev, &d->ein);
	d->out = usbOpenPipe(dev, &d->eout);
	d->ep_in = ein->bEndpointAddress; d->ep_out = eout->bEndpointAddress; d->ifnum = ifc->bInterfaceNumber;
	d->dev = dev;
	d->apple = 1;   // the only id registered so far
	memset(d->loc, 0, sizeof(d->loc));
	s32 lr = usbGetDeviceLocation(dev, d->loc);
	ulog("usbodd: dev %d location -> 0x%x %02x %02x %02x %02x %02x %02x %02x", dev, lr, d->loc[0], d->loc[1], d->loc[2], d->loc[3],
	     d->loc[4], d->loc[5], d->loc[6]);
	if (!d->tag) d->tag = 1;
	d->attached = d->ctrl >= 0 && d->in >= 0 && d->out >= 0;
	ulog("usbodd: attach dev %d: pipes ctrl %d in %d (ep %02x) out %d (ep %02x)", dev, d->ctrl, d->in, ein->bEndpointAddress,
	     d->out, eout->bEndpointAddress);
	return d->attached ? USB_ATTACH_SUCCEEDED : USB_ATTACH_FAILED;
}

static s32 odd_detach(s32 dev)
{
	ulog("usbodd: detach dev %d", dev);
	for (int i = 0; i < USBODD_DRIVES; i++)
		if (g_drv[i].attached && g_drv[i].dev == dev) {   // transfer memory and lock are kept for the next drive
			g_drv[i].attached = 0; g_drv[i].dev = -1; g_drv[i].name[0] = 0; g_drv[i].ctrl = g_drv[i].in = g_drv[i].out = -1;
		}
	return 0;
}

static usbLddOps g_ops = {"psxrip_odd", odd_probe, odd_attach, odd_detach};

int usbodd_init()
{
	if (g_init_rc == 0) return 0;
	int m = sysModuleLoad(SYSMODULE_USB);
	g_init_rc = usbInit();
	for (int i = 0; i < USBODD_DRIVES; i++) {
		g_drv[i].dev = g_drv[i].ctrl = g_drv[i].in = g_drv[i].out = -1;
		sys_mutex_attr_t ma;
		sysMutexAttrInitialize(ma);
		sysMutexCreate(&g_drv[i].lock, &ma);
	}
	// Apple USB SuperDrive
	g_reg_rc = usbRegisterExtraLdd(&g_ops, 0x05AC, 0x1500);
	ulog("usbodd: module 0x%x, usbInit 0x%x, register 0x%x", m, g_init_rc, g_reg_rc);
	return g_init_rc;
}

int usbodd_count()
{
	int n = 0;
	for (int i = 0; i < USBODD_DRIVES; i++) n += g_drv[i].attached;
	return n;
}

int usbodd_attached(int i) { return drive(i) != NULL; }

void usbodd_status(char *out, int n)
{
	int o = snprintf(out, n, "usbInit=0x%x register=0x%x probes=%d attaches=%d\n", g_init_rc, g_reg_rc, g_probes, g_attaches);
	for (int i = 0; i < USBODD_DRIVES && o < n; i++) {
		UsbDrive *d = &g_drv[i];
		o += snprintf(out + o, n - o, "drive %d: attached=%d dev=%d pipes ctrl=%d in=%d out=%d\n", i, d->attached, d->dev, d->ctrl, d->in, d->out);
	}
}

// Control request on the default pipe, waited for. Returns the transfer result (0 = ok).
static int control(UsbDrive *d, uint8_t type, uint8_t req, uint16_t value, uint16_t index)
{
	d->req.bmRequestType = type; d->req.bRequest = req; d->req.wValue = value; d->req.wIndex = index; d->req.wLength = 0;
		s32 rc = usbControlTransfer(d->ctrl, &d->req, NULL, done_cb, next_xfer(d));
	if (rc) return rc;
	return wait_done(d, 2000) ? -1 : d->res;
}

// A stalled endpoint stays blocked until the host clears the halt (USB 2.0 9.4.1).
static int clear_halt(UsbDrive *d, uint8_t ep)
{
	int r = control(d, USB_REQTYPE_DIR_TO_DEVICE | USB_REQTYPE_TYPE_STANDARD | USB_REQTYPE_RECIP_ENDPOINT, USB_REQUEST_CLEAR_FEATURE, 0, ep);
	if (r) ulog("usbodd: clear halt ep %02x -> 0x%x", ep, r);
	return r;
}

// A transfer that timed out is still queued on its pipe and would take the next answer (a bulk-in
// request waiting for a CSW gets the next command's data). Closing the pipe cancels it.
static void reopen_pipes(UsbDrive *d)
{
	d->seq++;   // whatever still completes belongs to the cancelled transfers
	usbClosePipe(d->in);
	usbClosePipe(d->out);
	d->in = usbOpenPipe(d->dev, &d->ein);
	d->out = usbOpenPipe(d->dev, &d->eout);
	ulog("usbodd: reopened pipes in %d out %d", d->in, d->out);
}

// Bulk-Only Mass Storage Reset (class request 0xFF) followed by clearing both halts (BOT 5.3.4).
static int reset_locked(UsbDrive *d)
{
	int r = control(d, USB_REQTYPE_DIR_TO_DEVICE | USB_REQTYPE_TYPE_CLASS | USB_REQTYPE_RECIP_INTERFACE, 0xFF, 0, d->ifnum);
	ulog("usbodd: bulk-only reset -> 0x%x", r);
	clear_halt(d, d->ep_in);
	clear_halt(d, d->ep_out);
	return r;
}

int usbodd_reset(int i)
{
	UsbDrive *d = drive(i);
	if (!d) return -1;
	sysMutexLock(d->lock, 0);
	int r = reset_locked(d);
	sysMutexUnlock(d->lock);
	return r;
}

static void put_le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t get_le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void *usb_mem(size_t n)
{
	void *p = NULL;
	usbAllocateMemory(&p, n);
	// the library stores a 32-bit address in the 64-bit pointer: on this big-endian CPU it lands
	// in the upper half (0x200a000000000000 for 0x200a0000)
	return (void *)((uintptr_t)p >> 32);
}

// One SCSI command over Bulk-Only Transport, with the drive locked. Returns 0 when the CSW
// arrived; *status = CSW status (0 good, 1 failed, 2 phase error), *got = data bytes moved.
static int scsi_locked(UsbDrive *d, const uint8_t *cdb, int cdblen, uint8_t *data, int len, int dir_in, int *status, int *got,
                       char *log, int logn)
{
	if (!d->cbw) {
		d->cbw = usb_mem(64); d->csw = usb_mem(64); d->buf = usb_mem(USBODD_MAX);
		ulog("usbodd: transfer memory %p %p %p", d->cbw, d->csw, d->buf);
	}
	*got = 0;
	if (!d->cbw || !d->csw || !d->buf || len > USBODD_MAX) { snprintf(log, logn, "no USB memory\n"); return -2; }
	uint8_t *c = (uint8_t *)d->cbw;
	memset(c, 0, 31);
	c[0] = 'U'; c[1] = 'S'; c[2] = 'B'; c[3] = 'C';
	put_le32(c + 4, d->tag++);
	put_le32(c + 8, len);
	c[12] = dir_in ? 0x80 : 0x00;
	c[13] = 0;                     // LUN
	c[14] = cdblen;
	memcpy(c + 15, cdb, cdblen);
	int o = 0;
#define TR(...) do { if (o < logn) o += snprintf(log + o, logn - o, __VA_ARGS__); } while (0)
	s32 r = usbBulkTransfer(d->out, d->cbw, 31, done_cb, next_xfer(d));
	int w = wait_done(d, 3000);
	TR("CBW -> 0x%x, done %d res 0x%x cnt %d\n", r, !w, d->res, d->cnt);
	if (r || w || d->res) {
		if (w) reopen_pipes(d);
		int rr = reset_locked(d);
		TR("CBW failed: reset -> 0x%x\n", rr);
		return -3;
	}
	if (len) {
		if (!dir_in) memcpy(d->buf, data, len);
		r = usbBulkTransfer(dir_in ? d->in : d->out, d->buf, len, done_cb, next_xfer(d));
		w = wait_done(d, 20000);   // a scratched sector can keep the drive retrying for a while
		TR("data -> 0x%x, done %d res 0x%x cnt %d\n", r, !w, d->res, d->cnt);
		if (!r && !w && !d->res) { *got = d->cnt; if (dir_in && d->cnt > 0) memcpy(data, d->buf, d->cnt < len ? d->cnt : len); }
		else if (w) {   // no data in time: cancel and recover, the CSW will not come either
			reopen_pipes(d);
			int rr = reset_locked(d);
			TR("data timeout: reset -> 0x%x\n", rr);
			return -5;
		}
		else if (!r && d->res) {   // the device stalled the data phase: clear it, then the CSW tells why
			int ch = clear_halt(d, dir_in ? d->ep_in : d->ep_out);
			TR("data stalled: clear halt -> 0x%x\n", ch);
		}
	}
	r = usbBulkTransfer(d->in, d->csw, 13, done_cb, next_xfer(d));
	w = wait_done(d, 5000);
	if (!r && !w && d->res) {   // after a short data phase the device stalls bulk-in: clear it, read the CSW again (BOT 6.7.2)
		int ch = clear_halt(d, d->ep_in);
		TR("CSW stalled: clear halt -> 0x%x\n", ch);
		r = usbBulkTransfer(d->in, d->csw, 13, done_cb, next_xfer(d));
		w = wait_done(d, 5000);
	}
	const uint8_t *s = (const uint8_t *)d->csw;
	TR("CSW -> 0x%x, done %d res 0x%x cnt %d sig %c%c%c%c tag %u residue %u status %u\n", r, !w, d->res, d->cnt,
	   s[0], s[1], s[2], s[3], get_le32(s + 4), get_le32(s + 8), s[12]);
	if (r || w || d->res) {   // no valid CSW: recover with a reset so the next command works again
		if (w) reopen_pipes(d);
		int rr = reset_locked(d);
		TR("no CSW: bulk-only reset -> 0x%x\n", rr);
		return -4;
	}
#undef TR
	*status = s[12];
	return 0;
}

int usbodd_scsi(int i, const uint8_t *cdb, int cdblen, uint8_t *data, int len, int dir_in, int *status, int *got, char *log, int logn)
{
	UsbDrive *d = drive(i);
	if (!d) { if (log) snprintf(log, logn, "drive %d not attached\n", i); return -1; }
	sysMutexLock(d->lock, 0);
	int r = scsi_locked(d, cdb, cdblen, data, len, dir_in, status, got, log ? log : d->trace, log ? logn : (int)sizeof(d->trace));
	sysMutexUnlock(d->lock);
	return r;
}

int usbodd_last_got(int i) { return i >= 0 && i < USBODD_DRIVES ? g_drv[i].last_got : 0; }

// A command and, when it fails, REQUEST SENSE, with the drive locked over both so no other
// command can clear the sense data in between. Returns 0 on success, -1 when the transport
// failed, otherwise 0x8D000000 | sense key << 16 | ASC << 8 | ASCQ.
int usbodd_cmd(int i, const uint8_t *cdb, int cdblen, uint8_t *data, int len, int dir_in)
{
	UsbDrive *d = drive(i);
	if (!d) return -1;
	sysMutexLock(d->lock, 0);
	int st = -1, got = 0;
	int r = scsi_locked(d, cdb, cdblen, data, len, dir_in, &st, &got, d->trace, sizeof(d->trace));
	d->last_got = got;   // a short answer is fine (TOC, sense); READ CD checks it
	if (!r && st) {
		uint8_t sense[18];
		const uint8_t rs[6] = {0x03, 0, 0, 0, sizeof(sense), 0};
		memset(sense, 0, sizeof(sense));
		int sg = 0, ss = -1;
		r = scsi_locked(d, rs, 6, sense, sizeof(sense), 1, &ss, &sg, d->trace, sizeof(d->trace));
		r = (r || ss) ? 0x8D000000 : 0x8D000000 | (sense[2] & 0x0F) << 16 | sense[12] << 8 | sense[13];
	} else if (r) {
		r = -1;
	}
	sysMutexUnlock(d->lock);
	return r;
}

// Apple's vendor command that lets the SuperDrive take a disc (without it the slot stays shut).
int usbodd_unlock(int i)
{
	const uint8_t cdb[12] = {0xEA, 0, 0, 0, 0, 0, 0x01, 0, 0, 0, 0, 0};
	int r = usbodd_cmd(i, cdb, 12, NULL, 0, 0);
	ulog("usbodd: drive %d unlock -> 0x%x", i, r);
	return r;
}

// TEST UNIT READY until the drive is ready. After a disc goes in the drive spins up for up to
// a minute on bus power (06/29, 02/04/01, 06/28 on the way). Returns 0 = ready, 1 = no disc,
// -1 = not attached / no answer / timeout.
int usbodd_ready(int i, int timeout_ms)
{
	const uint8_t tur[6] = {0};
	uint64_t end = ms_now() + timeout_ms;
	for (;;) {
		if (!drive(i)) return -1;
		int r = usbodd_cmd(i, tur, 6, NULL, 0, 0);
		if (r == 0) return 0;
		if (r == -1) return -1;
		int key = r >> 16 & 0x0F, asc = r >> 8 & 0xFF;
		if (key == 0x02 && asc == 0x3A) return 1;     // medium not present
		if (ms_now() >= end) return -1;
		usleep(key == 0x06 ? 50000 : 500000);         // unit attention: ask again at once
	}
}

// START STOP UNIT with LoEj: the slot-loading drive pushes the disc out (it has no button).
int usbodd_eject(int i)
{
	const uint8_t allow[6] = {0x1E, 0, 0, 0, 0, 0}, ej[6] = {0x1B, 0, 0, 0, 0x02, 0};
	usbodd_cmd(i, allow, 6, NULL, 0, 0);
	int r = usbodd_cmd(i, ej, 6, NULL, 0, 0);
	ulog("usbodd: drive %d eject -> 0x%x", i, r);
	return r;
}

// INQUIRY: vendor and product ("Apple SuperDrive") for the drive list. Not done in attach (that
// runs on the USB thread, which also delivers the transfer callbacks).
int usbodd_identify(int i)
{
	UsbDrive *d = drive(i);
	if (!d) return -1;
	uint8_t inq[36];
	const uint8_t cdb[6] = {0x12, 0, 0, 0, sizeof(inq), 0};
	memset(inq, 0, sizeof(inq));
	int r = usbodd_cmd(i, cdb, 6, inq, sizeof(inq), 1);
	if (d->apple) {   // INQUIRY names the mechanism inside ("HL-DT-ST DVDRW GX30N"): use the product's name
		snprintf(d->name, sizeof(d->name), "Apple SuperDrive");
		return 0;
	}
	if (r) { snprintf(d->name, sizeof(d->name), "CD/DVD"); return r; }
	char v[9], p[17];
	memcpy(v, inq + 8, 8); v[8] = 0;
	memcpy(p, inq + 16, 16); p[16] = 0;
	for (int k = 7; k >= 0 && v[k] == ' '; k--) v[k] = 0;
	for (int k = 15; k >= 0 && p[k] == ' '; k--) p[k] = 0;
	snprintf(d->name, sizeof(d->name), "%s%s%s", v, v[0] && p[0] ? " " : "", p);
	if (!d->name[0]) snprintf(d->name, sizeof(d->name), "CD/DVD");
	return 0;
}

const char *usbodd_name(int i) { return i >= 0 && i < USBODD_DRIVES ? g_drv[i].name : ""; }

int usbodd_location(int i, unsigned char *loc8)
{
	if (!drive(i)) return -1;
	memcpy(loc8, g_drv[i].loc, 8);
	return 0;
}

// Root port as the PS3 numbers its USB ports (location counts from 1), -1 when unknown.
int usbodd_port(int i)
{
	if (!drive(i) || !g_drv[i].loc[0]) return -1;
	return g_drv[i].loc[0] - 1;
}
