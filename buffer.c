/*
 * buffer.c  –  NABTS packet FIFO
 * Based on raspi-teletext buffer.c by Alistair Buxton <a.j.buxton@gmail.com>
 *
 * Each buffer slot holds one complete NABTS Data Line: NABTS_LINE_BYTES (36)
 * bytes = 3-byte preamble + 33-byte Data Packet (CEA-516 §2, §3).
 *
 * copy_packet():
 *   The fixed preamble (CS1, CS2, BS) is written once by teletext.c init()
 *   into the FIXED region of the framebuffer and is never rewritten here.
 *   copy_packet() expands only the 33-byte Data Packet (bytes [3..35])
 *   LSB-first into 264 pixel slots, matching the 264-bit Data Packet.
 *
 * Filler packet:
 *   When the queue is empty we emit a null Standard Packet on channel 0,
 *   CI=0, with a zero-filled Data Block.  This keeps the decoder's PLL
 *   locked between real data bursts.  No suffix is used (PS=0x15).
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "nabts.h"

#define NBUFFERS 64

static uint8_t tt_buffer[NBUFFERS][NABTS_LINE_BYTES];
static volatile uint8_t buffer_head = 0;
static volatile uint8_t buffer_tail = 0;

static uint8_t fill_buffer[NABTS_LINE_BYTES];

static void init_fill_buffer(void)
{
    static int done = 0;
    if (done) return;
    uint8_t zero_data[NABTS_DATA_BLOCK_BYTES];
    memset(zero_data, 0x00, sizeof(zero_data));
    /*
     * Standard packet, channel 0, CI=0, full=1, sync=0.
     * All data bytes 0x00.  No suffix.
     */
    nabts_build_packet(fill_buffer,
                       0x000,   /* channel 0 */
                       0,       /* CI = 0    */
                       0,       /* standard, not sync */
                       1,       /* full */
                       zero_data);
    done = 1;
}

/*
 * copy_packet  –  expand the 33-byte Data Packet portion of a 36-byte
 *                 line buffer into pixel-per-bit format.
 *
 * src  points to the full 36-byte line (preamble + packet).
 * dest points to the pixel row immediately after the FIXED preamble.
 *
 * We skip src[0..2] (preamble, already in the fixed region) and expand
 * src[3..35] (33 bytes = 264 bits) LSB-first.
 */
static void copy_packet(const uint8_t *src, uint8_t *dest)
{
    /* Data Packet starts at byte 3, immediately after the 3-byte preamble */
    const uint8_t *pkt = src + NABTS_PREAMBLE_BYTES;

    for (int n = 0; n < NABTS_PACKET_BYTES; n++) {
        uint8_t b = pkt[n];
        for (int m = 0; m < 8; m++) {
            *dest++ = b & 1u;
            b >>= 1;
        }
    }
}

void get_packet(uint8_t *dest)
{
    init_fill_buffer();
    if (buffer_head == buffer_tail) {
        copy_packet(fill_buffer, dest);
    } else {
        copy_packet(tt_buffer[buffer_tail], dest);
        buffer_tail = (buffer_tail + 1) % NBUFFERS;
    }
}

void push_packet(uint8_t *src)
{
    while (((buffer_head + 1) % NBUFFERS) == buffer_tail)
        usleep(20000);
    memcpy(tt_buffer[buffer_head], src, NABTS_LINE_BYTES);
    buffer_head = (buffer_head + 1) % NBUFFERS;
}

int read_packets(void)
{
    if (((buffer_head + 1) % NBUFFERS) == buffer_tail) {
        usleep(20000);
        return 1;
    }
    if (fread(tt_buffer[buffer_head], NABTS_LINE_BYTES, 1, stdin) == 1) {
        buffer_head = (buffer_head + 1) % NBUFFERS;
        return 1;
    }
    return 0;
}
