// Verification against Redump (redump.org). Nothing from Redump ships with PSX Ripper:
// the data is fetched at runtime, either per disc (online) or as a local copy that the user
// downloads to the console from the settings screen.
#ifndef PSXRIP_REDUMP_H
#define PSXRIP_REDUMP_H

#include <stdint.h>
#include <stddef.h>
#include <time.h>

enum { RD_OFF, RD_ONLINE, RD_LOCAL };

#define RD_NAME 160
#define RD_MAX_TRACKS 99

struct RdDisc {
	char name[RD_NAME];
	int n;                              // tracks (.bin files)
	uint32_t size[RD_MAX_TRACKS];       // 0 when unknown (online lookups)
	uint8_t md5[RD_MAX_TRACKS][16];
};

// Local copy. rd_local_info() returns the number of discs in the index (0 = none), its version
// and when it was downloaded (UTC, may be NULL).
int rd_local_info(char *version, size_t n, time_t *fetched);
// Downloads the dat and builds the index. *done/*total report progress (bytes, phase 0 = download,
// 1 = index); *abort cancels. Returns the number of discs, or a negative error.
int rd_download(volatile uint32_t *done, volatile uint32_t *total, volatile int *phase, volatile int *abort);
// Discs whose track count and total size match. Returns the number of matches; up to max names.
int rd_local_by_size(uint64_t total, int ntracks, char (*names)[RD_NAME], int max);
// 1 = every track MD5 matches a disc (name filled in), 0 = no match, -1 = no local copy.
int rd_local_verify(const uint8_t (*md5)[16], int n, char *name, size_t nn);

// Online: discs with this serial (PSX form, e.g. "SLES_011.35"). Returns the count, -1 on error.
int rd_online(const char *serial, RdDisc *out, int max);

#endif
