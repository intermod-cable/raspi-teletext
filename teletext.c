/*
 * teletext.c  –  NABTS VBI output for Raspberry Pi (NTSC)
 * Based on raspi-teletext by Alistair Buxton <a.j.buxton@gmail.com>
 *
 * Implements CEA-516 (North American Basic Teletext Specification).
 *
 * ── NTSC VBI geometry ───────────────────────────────────────────────────
 *
 *  525-line, 59.94 Hz.  VBI data lines: 10–21 both fields (§1.1.1).
 *  Pi NTSC_ON register shifts the framebuffer so the top rows land in
 *  the blanking interval.  height=32 gives 16 rows per field (interleaved).
 *
 * ── Preamble (FIXED region) ─────────────────────────────────────────────
 *
 *  FIXED = 24 pixels = 3 bytes × 8 bits:
 *    bytes 0-1: 0x55 0x55  (Clock Synchronization Sequence, CEA-516 §2.2.2)
 *    byte  2:   0xE7       (Byte Synchronization / Framing Code, §2.2.3)
 *
 *  The 24-bit preamble word emitted LSB-first = 0xE75555:
 *    bits  0- 7: 0x55 (CS byte 1)
 *    bits  8-15: 0x55 (CS byte 2)
 *    bits 16-23: 0xE7 (framing code)
 *
 *  This is written once in init() and never overwritten by copy_packet().
 *
 * ── Data Packet (variable region) ───────────────────────────────────────
 *
 *  copy_packet() renders the 33-byte Data Packet (P1 P2 P3 CI PS + 28-byte
 *  Data Block) into 264 pixels immediately after the FIXED region.
 *  Total per line: 24 + 264 = 288 pixels = 288 bits. ✓
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>

#include "bcm_host.h"
#include "render.h"
#include "buffer.h"
#include "nabts.h"
#include "demo.h"

#define WIDTH   370
#define OFFSET  8

/* FIXED = 24 pixels: the 3-byte synchronisation sequence */
#define FIXED   NABTS_FIXED   /* 24 */

#define ROW(i, n) ((i) + (PITCH(WIDTH) * (n)) + OFFSET)

int      height = 32;
uint16_t line_mask[2];


void draw(uint8_t *image, int next_resource)
{
    int m = line_mask[next_resource];
    for (int n = 0; n < height; n += 2) {
        if (!(m & 1)) get_packet(ROW(image, n + next_resource) + FIXED);
        m >>= 1;
    }
}


void init(uint8_t *image)
{
    /*
     * Write the 3-byte NABTS synchronisation sequence into every VBI row.
     *
     * Preamble (24 bits, LSB-first emission):
     *   bits  0- 7: 0x55  (CS byte 1)
     *   bits  8-15: 0x55  (CS byte 2)
     *   bits 16-23: 0xE7  (framing code)
     *
     * We pack this as a 32-bit word and shift out the low 24 bits LSB-first.
     * The top 8 bits are unused.
     */
    uint32_t preamble = 0x00E75555UL;   /* bits 0-23 = 0x55 0x55 0xE7 LSB-first */
    int n, m, even, odd;

    for (m = 0; m < FIXED; m++) {
        even = line_mask[0];
        odd  = line_mask[1];
        for (n = 0; n < height; n += 2) {
            if (!(even & 1)) ROW(image, n    )[m] = (preamble >> m) & 1u;
            if (!(odd  & 1)) ROW(image, n + 1)[m] = (preamble >> m) & 1u;
            even >>= 1;
            odd  >>= 1;
        }
    }

    draw(image, 0);
}


int main(int argc, char *argv[])
{
    int   c, level = 100;
    char *mvalue   = NULL;
    char *ovalue   = NULL;
    DemoMode dmode = DEMO_GRAPHICS;

    /*
     * Options:
     *   -l <n>     white level 0-100 (default 100; §1.6 specifies 70 IRE)
     *   -m <mask>  even-field line mask (16-bit hex)
     *   -o <mask>  odd-field line mask  (16-bit hex)
     *   -d ascii   select ASCII demo mode (default: graphics)
     */
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

    /* '-' as final argument: read raw 36-byte packets from stdin */
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
