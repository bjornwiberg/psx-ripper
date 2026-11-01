// USB optical drives through an own USB driver (Bulk-Only Transport). See usbodd.cpp.
#ifndef PSXRIP_USBODD_H
#define PSXRIP_USBODD_H

#include <stdint.h>

int usbodd_init();
void usbodd_status(char *out, int n);
int usbodd_scsi(const uint8_t *cdb, int cdblen, uint8_t *data, int len, int dir_in, int *status, int *got, char *log, int logn);

#endif
