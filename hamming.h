/*
 * hamming.h  –  NABTS payload parity
 *
 * parity(b)  set bit 7 of a 7-bit value to achieve odd parity (CEA-516 §3.3).
 *
 * NABTS Hamming encoding for header bytes uses nabts_hamming_enc[] in nabts.h,
 * not this file.  hamming84() has been removed; it produced WST-format output
 * and is incorrect for NABTS.
 */
#ifndef HAMMING_H
#define HAMMING_H

#include <stdint.h>

uint8_t parity(uint8_t);

#endif /* HAMMING_H */
