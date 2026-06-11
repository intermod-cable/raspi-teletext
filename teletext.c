/*
 * teletext.c  –  NABTS VBI output for Raspberry Pi 1B (NTSC)
 * Based on raspi-teletext by Alistair Buxton <a.j.buxton@gmail.com>
 *
 * Standard: CEA-516 (North American Basic Teletext Specification)
 *
 * ── Pixel clock correction ───────────────────────────────────────────────
 *
 *  raspi-teletext stretches the framebuffer (WIDTH pixels) to 720 pixels
 *  via dispmanx before the Pi VEC outputs it.  At WIDTH=370:
 *
 *    effective source pixel rate = 13.5 MHz × 370/720 = 6.9375 MHz
 *
 *  NABTS requires 5,727,272 bps (CEA-516 §1.3).
 *  1 pixel per bit → 6.9375 MHz  (21% too fast, decoder will not lock).
 *
 *  Fix: spread 288 bits across 349 source pixels using Bresenham expansion
 *  (nabts_px_width[] table in nabts.h).  This gives 5,724,928 Hz (−0.04%,
 *  within the ±16 Hz spec tolerance).
 *
 *  FIXED = 29 pixels  (first 24 bits of the 349-pixel sequence)
 *  Data  = 320 pixels (remaining 264 bits)
 *  Total = 349 pixels per line; fits in WIDTH=370 with OFFSET=14.
 *
 * ── NTSC VBI geometry ────────────────────────────────────────────────────
 *
 *  height=24: 12 rows per field (interleaved), maps to VBI lines 10-21
 *  in field 1 and 272-283 in field 2.
 *
 *  CEA-516 §1.1.1 permits data on lines 10 through 21 only (12 lines).
 *  The previous value of 32 (16 lines/field) overshot into active video
 *  lines 22-25, which violates the spec and produces visible noise.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "bcm_host.h"
#include "render.h"
#include "buffer.h"
#include "nabts.h"
#include "demo.h"

#define WIDTH   370
/*
 * OFFSET = 14 source pixels.
 *
 * CEA-516 §1.2 requires the data burst to begin no sooner than 10.9 µs and
 * no later than 12.0 µs after the leading edge of horizontal sync.
 *
 * NTSC active video starts ~9.4 µs after the sync leading edge.
 * Each source pixel is stretched to 720/370 display pixels by dispmanx,
 * so the signal start time from sync = 9.4 µs + OFFSET*(720/370)/13.5 MHz.
 *
 *   OFFSET= 8 -> 10.55 us  (inherited from WST fork; within WST >=10.3 us
 *                            but OUTSIDE the NABTS >=10.9 us minimum)
 *   OFFSET=14 -> 11.42 us  (centre of the 10.9-12.0 us NABTS window) OK
 *
 * Total pixels used: OFFSET(14) + FIXED(29) + DATA_PIXELS(320) = 363 <= 370.
 */
#define OFFSET  14

/*
 * FIXED = 29: the number of source pixels occupied by the 24-bit preamble
 * when using the Bresenham pixel-clock-corrected expansion.
 */
#define FIXED   NABTS_FIXED   /* 29 */

#define ROW(i, n) ((i) + (PITCH(WIDTH) * (n)) + OFFSET)

int      height = 24;   /* 12 rows/field → VBI lines 10-21 (CEA-516 §1.1.1) */
uint16_t line_mask[2];


/*
 * render_bit  –  write one bit value into 'width' consecutive pixel columns
 *                starting at dest[col].
 */
static inline void render_bit(uint8_t *row, int col, int width, uint8_t val)
{
    for (int p = 0; p < width; p++)
        row[col + p] = val;
}


void draw(uint8_t *image, int next_resource)
{
    int m = line_mask[next_resource];
    for (int n = 0; n < height; n += 2) {
        if (!(m & 1))
            get_packet(ROW(image, n + next_resource) + FIXED);
        m >>= 1;
    }
}


void init(uint8_t *image)
{
    /*
     * Write the 24-bit NABTS preamble into every VBI row using the
     * Bresenham pixel-width table (nabts_px_width[]).
     *
     * Preamble bits (LSB-first): 0x55, 0x55, 0xE7
     *   bits 0-7:   0x55 = 1,0,1,0,1,0,1,0
     *   bits 8-15:  0x55 = 1,0,1,0,1,0,1,0
     *   bits 16-23: 0xE7 = 1,1,1,0,0,1,1,1
     *
     * The 24 preamble bits occupy nabts_px_width[0..23], total = 29 pixels.
     * This is exactly the FIXED region (columns 0..28 of the data area).
     */
    static const uint8_t preamble_bytes[3] = { 0x55u, 0x55u, 0xE7u };
    int even, odd, n;

    even = line_mask[0];
    odd  = line_mask[1];

    for (n = 0; n < height; n += 2) {
        uint8_t *row_even = ROW(image, n);
        uint8_t *row_odd  = ROW(image, n + 1);

        int col = 0;
        for (int byte_idx = 0; byte_idx < 3; byte_idx++) {
            uint8_t b = preamble_bytes[byte_idx];
            for (int bit = 0; bit < 8; bit++) {
                int global_bit = byte_idx * 8 + bit;
                int width      = nabts_px_width[global_bit];
                uint8_t val    = (b >> bit) & 1u;
                if (!(even & 1)) render_bit(row_even, col, width, val);
                if (!(odd  & 1)) render_bit(row_odd,  col, width, val);
                col += width;
            }
        }
        /* col should now equal FIXED (29) */
        even >>= 1;
        odd  >>= 1;
    }

    draw(image, 0);
}


int main(int argc, char *argv[])
{
    /*
     * Default signal level = 71 (%).
     * render.c maps this to level_adj = (71*31)/100 = 22, giving
     * 22/31 * 100 = 71.0 IRE for logic-1 pixels.
     * CEA-516 §1.6 requires logic-1 = 70 ± 2 IRE.
     * The WST-inherited default of 100 (100 IRE) is 30 IRE over the NABTS
     * maximum and can prevent amplitude-sensitive decoders from locking.
     */
    int   c, level = 71;
    char *mvalue   = NULL;
    char *ovalue   = NULL;
    DemoMode dmode = DEMO_GRAPHICS;

    while ((c = getopt(argc, argv, "l:m:o:d:")) != -1) {
        switch (c) {
            case 'l':
                level = (int)strtol(optarg, NULL, 0);
                if (level <   0) level =   0;
                if (level > 100) level = 100;
                break;
            case 'm': mvalue = optarg; break;
            case 'o': ovalue = optarg; break;
            case 'd':
                dmode = (strcmp(optarg, "ascii") == 0) ? DEMO_ASCII : DEMO_GRAPHICS;
                break;
        }
    }

    line_mask[0] = 0;
    line_mask[1] = 0;
    if (mvalue) {
        line_mask[0] = (uint16_t)strtol(mvalue, NULL, 0);
        if (!ovalue) line_mask[1] = line_mask[0];
    }
    if (ovalue) {
        line_mask[1] = (uint16_t)strtol(ovalue, NULL, 0);
        if (!mvalue) line_mask[0] = line_mask[1];
    }

    void *render_handle = render_start(WIDTH, height, OFFSET, FIXED,
                                       init, draw, -1, level);

    if (argc >= 2 &&
        strlen(argv[argc - 1]) == 1 &&
        argv[argc - 1][0] == '-') {
        while (read_packets())
            ;
    } else {
        if (dmode == DEMO_ASCII)
            demo_ascii();
        else
            demo_graphics();
    }

    render_stop(render_handle);
    return 0;
}
