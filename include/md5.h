// MD5 (RFC 1321), written for PSX Ripper.
#ifndef PSXRIP_MD5_H
#define PSXRIP_MD5_H

#include <stdint.h>
#include <stddef.h>

struct Md5 {
	uint32_t s[4];
	uint64_t len;
	uint8_t buf[64];
};

void md5_init(Md5 *m);
void md5_update(Md5 *m, const void *data, size_t n);
void md5_final(Md5 *m, uint8_t out[16]);
void md5_hex(const uint8_t d[16], char out[33]);

#endif
