#pragma once

#include <stdint.h>

/* SHA-256 (FIPS 180-4), for OMEGA_SHA256: a ROM file on the Omega's card hashed in pieces. The
 * compression runs from IWRAM on the GBA. */

typedef struct {
    uint32_t h[8];
    uint64_t len; /* bytes hashed */
    uint8_t buf[64];
    uint32_t used; /* bytes in buf */
} HpSha256;

void hp_sha256_init(HpSha256* s);
void hp_sha256_update(HpSha256* s, const uint8_t* p, uint32_t n);
void hp_sha256_final(HpSha256* s, uint8_t out[32]);
