#pragma once
#include <stdint.h>

/* The phone's serial number (androidboot.serialno= in LK's bootargs: 1..31 letters and digits) into the root
 * serial-number of the ADT, in place (the property must exist, 32 bytes). Returns its length, or 0 (left as it is). */
int adt_phone_serial(volatile uint8_t *adt, uint64_t adt_size, const volatile char *bootargs, uint32_t len);
