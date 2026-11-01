// USB optical drives through an own USB driver (Bulk-Only Transport). See usbodd.cpp.
// Drives are numbered 0 .. USBODD_DRIVES-1 in the order they attached.
#ifndef PSXRIP_USBODD_H
#define PSXRIP_USBODD_H

#include <stdint.h>

#define USBODD_MAX 65536   // bytes per command (27 raw sectors with Q)
#define USBODD_DRIVES 2

int usbodd_init();
int usbodd_count();
int usbodd_attached(int drive);
void usbodd_status(char *out, int n);
int usbodd_reset(int drive);
int usbodd_cmd(int drive, const uint8_t *cdb, int cdblen, uint8_t *data, int len, int dir_in);
int usbodd_last_got(int drive);   // data bytes the last usbodd_cmd moved
int usbodd_identify(int drive);       // INQUIRY into usbodd_name
const char *usbodd_name(int drive);  // "Apple SuperDrive", "" before usbodd_identify
int usbodd_location(int drive, unsigned char *loc8);   // usbGetDeviceLocation bytes
int usbodd_port(int drive);   // USB port as the PS3 numbers them, -1 unknown
int usbodd_unlock(int drive);
int usbodd_ready(int drive, int timeout_ms);
int usbodd_eject(int drive);
int usbodd_scsi(int drive, const uint8_t *cdb, int cdblen, uint8_t *data, int len, int dir_in, int *status, int *got, char *log, int logn);

#endif
