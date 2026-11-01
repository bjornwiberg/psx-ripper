// USB optical drives through an own USB driver (PSL1GHT usbd): the PS3 system lists USB drives as
// storage but does not pass SCSI commands to them, so a SuperDrive cannot even be unlocked that
// way. This driver claims the drive by vendor/product id and talks USB Mass Storage Bulk-Only
// Transport itself: CBW (command), data, CSW (status). Development stage: used from /dbg.
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

static volatile s32 g_dev = -1, g_ctrl = -1, g_in = -1, g_out = -1;
static volatile int g_attached = 0, g_probes = 0, g_attaches = 0;
static int g_init_rc = 1, g_reg_rc = 1;
static unsigned g_tag = 1;

static volatile int g_done;
static volatile s32 g_res, g_cnt;
static void done_cb(s32 result, s32 count, void *arg) { g_res = result; g_cnt = count; g_done = 1; }

static uint64_t ms_now()
{
	struct timeval tv;
	gettimeofday(&tv, NULL);
	return (uint64_t)tv.tv_sec * 1000 + tv.tv_usec / 1000;
}

static int wait_done(int ms)   // by the clock: usleep granularity varies
{
	uint64_t end = ms_now() + ms;
	while (!g_done && ms_now() < end) usleep(2000);
	return g_done ? 0 : -1;
}

static volatile int g_cfg_done = 0;
static volatile s32 g_cfg_res = 1;
static void cfg_cb(s32 result, s32 count, void *arg) { g_cfg_res = result; g_cfg_done = 1; }

static s32 odd_probe(s32 dev)
{
	g_probes++;
	ulog("usbodd: probe dev %d", dev);
	return USB_PROBE_SUCCEEDED;
}

static s32 odd_attach(s32 dev)
{
	g_attaches++;
	usbInterfaceDescriptor *ifc = (usbInterfaceDescriptor *)usbScanStaticDescriptor(dev, NULL, USB_DESCRIPTOR_TYPE_INTERFACE);
	if (!ifc) { ulog("usbodd: attach dev %d: no interface", dev); return USB_ATTACH_FAILED; }
	ulog("usbodd: attach dev %d: class %02x/%02x/%02x, %d endpoints", dev, ifc->bInterfaceClass, ifc->bInterfaceSubClass,
	     ifc->bInterfaceProtocol, ifc->bNumEndpoints);
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
	g_ctrl = usbOpenPipe(dev, NULL);
	// select the configuration, or the bulk endpoints stay inactive
	const uint8_t *cfg = (const uint8_t *)usbScanStaticDescriptor(dev, NULL, USB_DESCRIPTOR_TYPE_CONFIG);
	if (g_ctrl >= 0 && cfg) {
		static usbDeviceRequest req;
		req.bmRequestType = USB_REQTYPE_DIR_TO_DEVICE | USB_REQTYPE_TYPE_STANDARD | USB_REQTYPE_RECIP_DEVICE;
		req.bRequest = USB_REQUEST_SET_CONFIGURATION;
		req.wValue = cfg[5];   // bConfigurationValue
		req.wIndex = 0;
		req.wLength = 0;
		s32 rc = usbControlTransfer(g_ctrl, &req, NULL, cfg_cb, NULL);
		ulog("usbodd: set configuration %u -> 0x%x", cfg[5], rc);
	}
	g_in = usbOpenPipe(dev, ein);
	g_out = usbOpenPipe(dev, eout);
	g_dev = dev;
	g_attached = g_ctrl >= 0 && g_in >= 0 && g_out >= 0;
	ulog("usbodd: attach dev %d: pipes ctrl %d in %d (ep %02x) out %d (ep %02x)", dev, g_ctrl, g_in, ein->bEndpointAddress,
	     g_out, eout->bEndpointAddress);
	return g_attached ? USB_ATTACH_SUCCEEDED : USB_ATTACH_FAILED;
}

static s32 odd_detach(s32 dev)
{
	ulog("usbodd: detach dev %d", dev);
	g_attached = 0; g_dev = -1; g_ctrl = g_in = g_out = -1;
	return 0;
}

static usbLddOps g_ops = {"psxrip_odd", odd_probe, odd_attach, odd_detach};

int usbodd_init()
{
	if (g_init_rc == 0) return 0;
	int m = sysModuleLoad(SYSMODULE_USB);
	g_init_rc = usbInit();
	// Apple USB SuperDrive
	g_reg_rc = usbRegisterExtraLdd(&g_ops, 0x05AC, 0x1500);
	ulog("usbodd: module 0x%x, usbInit 0x%x, register 0x%x", m, g_init_rc, g_reg_rc);
	return g_init_rc;
}

void usbodd_status(char *out, int n)
{
	snprintf(out, n, "usbInit=0x%x register=0x%x probes=%d attaches=%d attached=%d dev=%d pipes ctrl=%d in=%d out=%d set_config done=%d res=0x%x\n",
	         g_init_rc, g_reg_rc, g_probes, g_attaches, g_attached, g_dev, g_ctrl, g_in, g_out, g_cfg_done, g_cfg_res);
}

static void put_le32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static uint32_t get_le32(const uint8_t *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

// One SCSI command over Bulk-Only Transport. Returns 0 when the CSW arrived; *status = CSW status
// (0 good, 1 failed, 2 phase error), *got = data bytes moved.
int usbodd_scsi(const uint8_t *cdb, int cdblen, uint8_t *data, int len, int dir_in, int *status, int *got, char *log, int logn)
{
	if (!g_attached) { snprintf(log, logn, "not attached\n"); return -1; }
	static void *cbw, *csw, *buf;
	if (!cbw) {
		ulog("usbodd: allocating transfer memory");
		s32 a1 = usbAllocateMemory(&cbw, 64), a2 = usbAllocateMemory(&csw, 64), a3 = usbAllocateMemory(&buf, 65536);
		// the library stores a 32-bit address in the 64-bit pointer: on this big-endian CPU it lands
		// in the upper half (0x200a000000000000 for 0x200a0000)
		cbw = (void *)((uintptr_t)cbw >> 32); csw = (void *)((uintptr_t)csw >> 32); buf = (void *)((uintptr_t)buf >> 32);
		ulog("usbodd: allocate -> 0x%x 0x%x 0x%x (%p %p %p)", a1, a2, a3, cbw, csw, buf);
	}
	if (!cbw || !csw || !buf || len > 65536) { snprintf(log, logn, "no USB memory\n"); return -2; }
	uint8_t *c = (uint8_t *)cbw;
	memset(c, 0, 31);
	c[0] = 'U'; c[1] = 'S'; c[2] = 'B'; c[3] = 'C';
	put_le32(c + 4, g_tag++);
	put_le32(c + 8, len);
	c[12] = dir_in ? 0x80 : 0x00;
	c[13] = 0;                     // LUN
	c[14] = cdblen;
	memcpy(c + 15, cdb, cdblen);
	size_t o = 0;
	g_done = 0;
	ulog("usbodd: CBW submit (out pipe %d)", g_out);
	s32 r = usbBulkTransfer(g_out, cbw, 31, done_cb, NULL);
	ulog("usbodd: CBW submitted -> 0x%x", r);
	int w = wait_done(3000);
	ulog("usbodd: CBW wait -> %d (res 0x%x cnt %d)", w, g_res, g_cnt);
	o += snprintf(log + o, logn - o, "CBW -> 0x%x, done %d res 0x%x cnt %d\n", r, !w, g_res, g_cnt);
	if (r || w || g_res) return -3;
	*got = 0;
	if (len) {
		if (!dir_in) memcpy(buf, data, len);
		g_done = 0;
		r = usbBulkTransfer(dir_in ? g_in : g_out, buf, len, done_cb, NULL);
		w = wait_done(10000);
		o += snprintf(log + o, logn - o, "data -> 0x%x, done %d res 0x%x cnt %d\n", r, !w, g_res, g_cnt);
		if (!r && !w) { *got = g_cnt; if (dir_in && g_cnt > 0) memcpy(data, buf, g_cnt < len ? g_cnt : len); }
	}
	g_done = 0;
	r = usbBulkTransfer(g_in, csw, 13, done_cb, NULL);
	w = wait_done(5000);
	const uint8_t *s = (const uint8_t *)csw;
	o += snprintf(log + o, logn - o, "CSW -> 0x%x, done %d res 0x%x cnt %d sig %c%c%c%c tag %u residue %u status %u\n", r, !w, g_res, g_cnt,
	              s[0], s[1], s[2], s[3], get_le32(s + 4), get_le32(s + 8), s[12]);
	if (r || w) return -4;
	*status = s[12];
	return 0;
}
