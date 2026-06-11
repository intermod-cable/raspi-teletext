/*
 * buffer.c  –  NABTS packet FIFO with pixel-clock-corrected bit expansion
 * Based on raspi-teletext buffer.c by Alistair Buxton <a.j.buxton@gmail.com>
 *
 * copy_packet():
 *   Expands the 33-byte Data Packet (264 bits, bytes [3..35] of a 36-byte
 *   line) into source pixels using the Bresenham width table nabts_px_width[].
 *
 *   Bit 0 of the Data Packet corresponds to global bit index 24 (after the
 *   24-bit preamble), so nabts_px_width[24..287] applies.
 *
 *   Each bit is written into 1 or 2 consecutive pixel columns, giving an
 *   effective on-wire bit rate of 5,724,928 Hz (−0.04% from the 5,727,272 Hz
 *   CEA-516 requirement — within the ±16 Hz tolerance of §1.3).
 *
 *   The pixel buffer passed in (dest) starts at column FIXED (29) of the
 *   framebuffer row and has room for NABTS_DATA_PIXELS (320) columns.
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

/*
 * ci_null  –  single CI counter for ALL channel-0x000 packets.
 *
 * Both get_packet() (filler path) and demo.c's push_null() must use the
 * same counter.  If they used separate counters the decoder would see
 * non-monotonic CI on channel 0x000 whenever the queue drains and the
 * filler path takes over, violating CEA-516 §3.2.4.
 *
 * Ownership lives here (buffer.c) because get_packet() is the lowest-level
 * emission point; demo.c reaches it through null_ci_next() exported in buffer.h.
 */
static nabts_ci_t ci_null_state = {0};

uint8_t null_ci_next(void)
{
    return nabts_ci_next(&ci_null_state);
}

/*
 * copy_packet  –  expand the 33-byte Data Packet from a 36-byte line buffer
 *                 into pixel-per-bit (variable width) format.
 *
 * src   full 36-byte NABTS Data Line (preamble + packet)
 * dest  pixel row at column FIXED; must have NABTS_DATA_PIXELS (320) columns
 *
 * The preamble bytes src[0..2] are already rendered into the FIXED region by
 * init() and are not touched here.
 *
 * We expand src[3..35] (33 bytes = 264 bits) using nabts_px_width[24..287]
 * (global bit positions 24–287 of the 288-bit Data Line).
 */
static void copy_packet(const uint8_t *src, uint8_t *dest)
{
    const uint8_t *pkt = src + NABTS_PREAMBLE_BYTES;  /* byte 3 */
    int col = 0;

    for (int byte_idx = 0; byte_idx < NABTS_PACKET_BYTES; byte_idx++) {
        uint8_t b = pkt[byte_idx];
        for (int bit = 0; bit < 8; bit++) {
            /* Global bit index = 24 (preamble bits) + byte_idx*8 + bit */
            int global_bit = NABTS_PREAMBLE_BYTES * 8 + byte_idx * 8 + bit;
            int width      = nabts_px_width[global_bit];
            uint8_t val    = (b >> bit) & 1u;
            dest[col]      = val;
            if (width == 2)
                dest[col + 1] = val;
            col += width;
        }
    }
    /* col == NABTS_DATA_PIXELS (320) at this point */
}

void get_packet(uint8_t *dest)
{
    if (buffer_head == buffer_tail) {
        /* Queue empty: emit a filler packet using the shared null-channel CI. */
        uint8_t zero_data[NABTS_DATA_BLOCK_BYTES];
        uint8_t fill[NABTS_LINE_BYTES];
        memset(zero_data, 0x00, sizeof(zero_data));
        nabts_build_packet(fill, 0x000, null_ci_next(), 0, 1, zero_data);
        copy_packet(fill, dest);
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
