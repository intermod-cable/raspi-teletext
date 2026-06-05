/*
 * demo.c  –  NABTS demo transmitter
 * Based on raspi-teletext by Alistair Buxton <a.j.buxton@gmail.com>
 *
 * Implements two demo modes:
 *
 *   demo_graphics()  –  NAPLPS presentation page with colour bars, title,
 *                       bouncing box and frame counter (default).
 *   demo_ascii()     –  plain ASCII identification packets; useful for
 *                       verifying decoder lock before attempting NAPLPS.
 *
 * ── CEA-516 packet usage ─────────────────────────────────────────────────
 *
 *  channel 0x000  –  null / filler packets (Standard, full, CI incrementing)
 *  channel 0x001  –  NAPLPS presentation data (graphics demo)
 *  channel 0x00F  –  ASCII identification packets (ascii demo)
 *
 *  Data Group structure (CEA-516 §4):
 *    First packet of a Data Group:  sync=1 (Synchronizing Packet, b2=1 in PS)
 *    Subsequent packets:            sync=0 (Standard Packet)
 *    Last packet if Data Block not completely full: full=0 (b4=1 in PS)
 *
 *  Continuity Index (CI, §3.2.4):
 *    Increments by 1 (mod 16) for each packet on a given channel.
 *    Managed per-channel with nabts_ci_t / nabts_ci_next().
 *
 * ── NAPLPS encoding ──────────────────────────────────────────────────────
 *
 *  Presentation data (CEA-516 §6.1) conforms to ANSI/CSA T1.502 (NAPLPS).
 *  Only the 7-bit code environment is supported (§6.1 note).
 *  All bytes emitted here are 7-bit clean (bit 7 = 0).
 *
 *  Coordinate encoding (NAPLPS domain/fraction):
 *    Each ordinate: 3 bytes.  Byte 1 (domain): bits[5:0] = floor(coord×64).
 *    Bytes 2-3 (fraction): 0x00 0x00 (integer resolution for this demo).
 *    Range 0..63 maps to normalised screen 0.0..1.0.
 *
 *  Colour byte (used with ESC 0x24 set-FG and ESC 0x26 set-BG):
 *    bits[7:4] = colour index: 0=black 1=red 2=green 3=yellow
 *                              4=blue  5=magenta 6=cyan 7=white
 *    bits[3:0] = 0x00
 *
 * ── Byte budget per graphics frame ──────────────────────────────────────
 *
 *  clear(1) + set_bg(3) + 5 bars(85) + title(21) + box(17) + counter(19) = 146
 *  146 bytes / 28 bytes per packet = 6 packets (last has 6 data + 22 padding)
 *  NL_PAGE_MAX = 168 bytes (6 × 28)
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "buffer.h"
#include "nabts.h"

/* ── NAPLPS byte constants ─────────────────────────────────────────────── */

#define NL_FF      0x0Cu   /* Form Feed: clear screen              */
#define NL_ESC     0x1Bu   /* Escape prefix                        */
#define NL_SET_FG  0x24u   /* ESC 0x24 <col>  set foreground       */
#define NL_SET_BG  0x26u   /* ESC 0x26 <col>  set background       */
#define NL_RECT    0x63u   /* ESC 0x63 x1 y1 x2 y2  filled rect   */
#define NL_MOVE    0x61u   /* ESC 0x61 x y  move (no draw)         */

#define COL_BLACK   0x00u
#define COL_RED     0x10u
#define COL_GREEN   0x20u
#define COL_YELLOW  0x30u
#define COL_BLUE    0x40u
#define COL_MAGENTA 0x50u
#define COL_CYAN    0x60u
#define COL_WHITE   0x70u

/* ── Page assembly ─────────────────────────────────────────────────────── */

#define NL_PAGE_MAX  168   /* 6 packets × 28 bytes */

static uint8_t nl_page[NL_PAGE_MAX];
static int     nl_len;

static void nl_byte(uint8_t b)
{
    if (nl_len < NL_PAGE_MAX) nl_page[nl_len++] = b;
}
static void nl_colour(uint8_t cmd, uint8_t col)
    { nl_byte(NL_ESC); nl_byte(cmd); nl_byte(col); }
static void nl_rect(uint8_t x1, uint8_t y1, uint8_t x2, uint8_t y2) {
    nl_byte(NL_ESC); nl_byte(NL_RECT);
    nl_byte(x1); nl_byte(0); nl_byte(0);
    nl_byte(y1); nl_byte(0); nl_byte(0);
    nl_byte(x2); nl_byte(0); nl_byte(0);
    nl_byte(y2); nl_byte(0); nl_byte(0);
}
static void nl_move(uint8_t x, uint8_t y) {
    nl_byte(NL_ESC); nl_byte(NL_MOVE);
    nl_byte(x); nl_byte(0); nl_byte(0);
    nl_byte(y); nl_byte(0); nl_byte(0);
}
static void nl_text(const char *s) { while (*s) nl_byte((uint8_t)*s++); }

/* ── Per-channel CI trackers ───────────────────────────────────────────── */

static nabts_ci_t ci_null  = {0};   /* channel 0x000 */
static nabts_ci_t ci_data  = {0};   /* channel 0x001 */
static nabts_ci_t ci_ident = {0};   /* channel 0x00F */

/* ── Packet helpers ────────────────────────────────────────────────────── */

/*
 * push_null  –  Standard filler packet on channel 0.
 * Keeps decoder PLL locked between data bursts.
 */
static void push_null(void)
{
    uint8_t line[NABTS_LINE_BYTES];
    uint8_t data[NABTS_DATA_BLOCK_BYTES];
    memset(data, 0x00, sizeof(data));
    nabts_build_packet(line, 0x000, nabts_ci_next(&ci_null), 0, 1, data);
    push_packet(line);
}

/*
 * push_page  –  slice nl_page[0..nl_len) into 28-byte NABTS packets on
 *               channel 0x001, correctly setting sync and full flags.
 *
 *  First packet in the sequence:   sync=1  (Synchronizing Packet, §4.1)
 *  Middle packets:                 sync=0, full=1
 *  Last packet if remainder < 28:  sync=0, full=0  (Data Block not full)
 *  Last packet if remainder == 28: sync=0, full=1
 *
 *  One null packet is interleaved after each data packet so channel 0
 *  nulls continue flowing during the burst.
 */
static void push_page(void)
{
    int offset  = 0;
    int first   = 1;

    while (offset < nl_len) {
        uint8_t data[NABTS_DATA_BLOCK_BYTES];
        memset(data, 0x00, sizeof(data));

        int remaining = nl_len - offset;
        int chunk     = (remaining >= NABTS_DATA_BLOCK_BYTES)
                        ? NABTS_DATA_BLOCK_BYTES : remaining;
        memcpy(data, nl_page + offset, (size_t)chunk);

        int is_full = (chunk == NABTS_DATA_BLOCK_BYTES);

        uint8_t line[NABTS_LINE_BYTES];
        nabts_build_packet(line,
                           0x001,
                           nabts_ci_next(&ci_data),
                           first,      /* sync flag */
                           is_full,    /* full flag */
                           data);
        push_packet(line);
        push_null();

        offset += chunk;
        first   = 0;
    }
}

/* ── Scene elements ────────────────────────────────────────────────────── */

/* Five colour bars across the top 22% of the screen (y 0..14) */
static void build_colour_bars(void)
{
    static const uint8_t cols[5] = {
        COL_WHITE, COL_YELLOW, COL_CYAN, COL_GREEN, COL_MAGENTA
    };
    for (int i = 0; i < 5; i++) {
        nl_colour(NL_SET_FG, cols[i]);
        nl_rect((uint8_t)(i * 12), 0, (uint8_t)(i * 12 + 11), 14);
    }
}

/* "NABTS DEMO" centred in the middle band */
static void build_title(void)
{
    nl_colour(NL_SET_FG, COL_WHITE);
    nl_move(0x0B, 0x1C);
    nl_text("NABTS DEMO");
}

/* Bouncing box tracing a rectangular path in the lower half */
#define BOX_TRAVEL  25
#define BOX_SIZE     5

static void build_bouncing_box(int frame)
{
    static const uint8_t cols[4] = {
        COL_RED, COL_GREEN, COL_BLUE, COL_YELLOW
    };
    int period = 4 * BOX_TRAVEL;
    int pos    = frame % period;
    uint8_t bx, by;

    if      (pos < BOX_TRAVEL)     { bx = (uint8_t)(3 + pos);               by = 33; }
    else if (pos < 2*BOX_TRAVEL)   { bx = (uint8_t)(3 + BOX_TRAVEL - 1);    by = (uint8_t)(33 + pos - BOX_TRAVEL); }
    else if (pos < 3*BOX_TRAVEL)   { bx = (uint8_t)(3 + BOX_TRAVEL - 1 - (pos - 2*BOX_TRAVEL)); by = (uint8_t)(33 + BOX_TRAVEL - 1); }
    else                            { bx = 3;                                by = (uint8_t)(33 + BOX_TRAVEL - 1 - (pos - 3*BOX_TRAVEL)); }

    nl_colour(NL_SET_FG, cols[(frame / BOX_TRAVEL) % 4]);
    nl_rect(bx, by, (uint8_t)(bx + BOX_SIZE), (uint8_t)(by + BOX_SIZE));
}

/* "FRM:nnnn" counter at bottom-left */
static void build_frame_counter(int frame)
{
    nl_colour(NL_SET_FG, COL_CYAN);
    nl_move(0x02, 0x3A);
    int f = frame % 10000;
    char buf[9];
    buf[0]='F'; buf[1]='R'; buf[2]='M'; buf[3]=':';
    buf[4]=(char)('0'+f/1000); buf[5]=(char)('0'+(f/100)%10);
    buf[6]=(char)('0'+(f/10)%10); buf[7]=(char)('0'+f%10); buf[8]='\0';
    nl_text(buf);
}

/* ── demo_graphics ─────────────────────────────────────────────────────── */

#define NULLS_BETWEEN_PAGES  288   /* ~1 second at 300 packets/sec */

void demo_graphics(void)
{
    int frame = 0;
    while (1) {
        nl_len = 0;
        nl_byte(NL_FF);
        nl_colour(NL_SET_BG, COL_BLACK);
        build_colour_bars();
        build_title();
        build_bouncing_box(frame);
        build_frame_counter(frame);
        push_page();
        for (int i = 0; i < NULLS_BETWEEN_PAGES; i++) {
            push_null();
            usleep(3333);
        }
        frame++;
    }
}

/* ── demo_ascii ────────────────────────────────────────────────────────── */

/*
 * Emits a Synchronizing Packet on channel 0x00F carrying a plain ASCII
 * string every ~1 second, with null packets between bursts.
 *
 * This is the minimal signal for verifying decoder lock before NAPLPS.
 * The payload is placed raw in the Data Block; it is not NAPLPS-encoded.
 *
 * Usage: sudo ./teletext -d ascii
 */
#define ASCII_NULLS  299

void demo_ascii(void)
{
    while (1) {
        /* Build a single-packet Data Group on channel 0x00F */
        uint8_t data[NABTS_DATA_BLOCK_BYTES];
        memset(data, 0x00, sizeof(data));
        const char *str = "NABTS raspi-teletext";
        int len = (int)strlen(str);
        if (len > NABTS_DATA_BLOCK_BYTES) len = NABTS_DATA_BLOCK_BYTES;
        memcpy(data, str, (size_t)len);

        uint8_t line[NABTS_LINE_BYTES];
        /* sync=1 (start of Data Group), full=0 (string < 28 bytes) */
        nabts_build_packet(line, 0x00F,
                           nabts_ci_next(&ci_ident),
                           1,   /* sync */
                           0,   /* not full */
                           data);
        push_packet(line);

        for (int i = 0; i < ASCII_NULLS; i++) {
            push_null();
            usleep(3333);
        }
    }
}

void demo(void) { demo_graphics(); }
