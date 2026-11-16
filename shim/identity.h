#pragma once
#include <stdint.h>

/* The phone's serial number (androidboot.serialno= in LK's bootargs: 1..31 letters and digits) into the root
 * serial-number of the ADT, in place (the property must exist, 32 bytes). Returns its length, or 0 (left as it is). */
int adt_phone_serial(volatile uint8_t *adt, uint64_t adt_size, const volatile char *bootargs, uint32_t len);

/* /chosen/random-seed of the ADT, in place: SHA-256 over the property's old bytes and `entropy' (LK's kaslr-seed and
 * timer readings), a counter for each 32 bytes. Returns the bytes filled, or 0 (no /chosen/random-seed: left alone). */
uint32_t adt_random_seed(volatile uint8_t *adt, uint64_t adt_size, const uint8_t *entropy, uint32_t len);

/* SHA-256 of `n' bytes (for the seed; host-tested against FIPS 180-2's vectors). */
void sha256(const uint8_t *data, uint64_t n, uint8_t out[32]);
