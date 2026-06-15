/*
 * buffer.h  –  NABTS packet FIFO interface
 *
 * get_packet(dest)   copy next packet (or filler) LSB-first into pixel dest
 * push_packet(src)   enqueue one NABTS_LINE_BYTES packet for transmission
 * read_packets()     read one NABTS_LINE_BYTES packet from stdin into queue
 * null_ci_next()     return the next CI value for channel 0x000 and advance
 *                    the counter; shared by push_null() and get_packet() so
 *                    the decoder sees a monotonically incrementing CI on the
 *                    null channel regardless of which code path emits the packet
 *
 * Each packet slot is exactly NABTS_LINE_BYTES (36) bytes:
 *   3 preamble bytes + 33 Data Packet bytes
 */
#ifndef BUFFER_H
#define BUFFER_H

#include <stdint.h>
#include "nabts.h"

void    get_packet(uint8_t *dest);
void    push_packet(uint8_t *src);
int     read_packets(void);
uint8_t null_ci_next(void);   /* shared CI counter for channel 0x000 */

#endif /* BUFFER_H */
