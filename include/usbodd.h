// USB optical drives through an own USB driver (Bulk-Only Transport). See usbodd.cpp.
#ifndef PSXRIP_USBODD_H
#define PSXRIP_USBODD_H

#include <stdint.h>

int usbodd_init();
void usbodd_status(char *out, int n);
int usbodd_reset();
#define USBODD_MAX 65536   // bytes per command (27 raw sectors with Q)

int usbodd_attached();
int usbodd_cmd(const uint8_t *cdb, int cdblen, uint8_t *data, int len, int dir_in);
int usbodd_last_got();   // data bytes the last usbodd_cmd moved
int usbodd_unlock();
int usbodd_ready(int timeout_ms);
int usbodd_eject();
int usbodd_scsi(const uint8_t *cdb, int cdblen, uint8_t *data, int len, int dir_in, int *status, int *got, char *log, int logn);

#endif
