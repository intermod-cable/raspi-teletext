/* Parity calculation for NABTS payload bytes (CEA-516 §3.3) */

/* Copyright 2015 Alistair Buxton <a.j.buxton@gmail.com> */

#include <stdint.h>

/*
 * parity  –  set bit 7 of a 7-bit value so the total number of 1-bits is odd.
 *
 * CEA-516 §3.3 requires each byte in a NABTS Data Block to have odd parity.
 * Input must be 7-bit clean (bits[6:0]); bit 7 of the input is ignored.
 * Output has the same bits[6:0] with bit 7 set to achieve odd parity.
 *
 * Note: NABTS Hamming-encoded header bytes (nabts_hamming_enc[]) already
 * satisfy odd parity by construction — do NOT apply parity() to them.
 */
uint8_t parity(uint8_t d)
{
    d &= 0x7f;
    uint8_t p = 1;
    uint8_t t = d;
    int i;
    for (i=0; i<7; i++) {
        p += t&1;
        t = t>>1;
    }
    p &= 1;
    return d|(p<<7);
}
