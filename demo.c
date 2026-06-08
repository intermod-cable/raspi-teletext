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
 * make_dg_header  –  build the 8-byte Hamming-encoded Data Group Header
 *                    (CEA-516 §4.2) into the first 8 bytes of dest[].
 *
 * @gc          Data Group Continuity (0..15, §4.2.3)
 * @gr          Data Group Repetition (0=not repeated, §4.2.4)
 * @num_packets total number of Data Packets in this Data Group
 * @final_bytes number of useful bytes in the last Data Block (1..28)
 *
 * Field layout (§4.2.1, Figure 9):
 *   GT  = 0x0  (Data Group Type 0 = broadcast teletext, §4.2.2)
 *   GC  = gc   (continuity counter)
 *   GR  = gr   (repetition indicator)
 *   S1,S2 = num_packets-1 split into two nibbles (§4.2.5)
 *   F1,F2 = final_bytes   split into two nibbles (§4.2.6)
 *   GN  = 0x0  (network routing, §4.2.7)
 *
 * All 8 bytes are Hamming-encoded per Figure 7.
 */
static void make_dg_header(uint8_t *dest,
                           uint8_t gc, uint8_t gr,
                           int num_packets, int final_bytes)
{
    int S = num_packets - 1;   /* blocks following the sync packet's block */
    int F = final_bytes;
    dest[0] = nabts_hamming_enc[0x0];          /* GT = 0 */
    dest[1] = nabts_hamming_enc[gc  & 0xF];    /* GC     */
    dest[2] = nabts_hamming_enc[gr  & 0xF];    /* GR     */
    dest[3] = nabts_hamming_enc[(S >> 4) & 0xF]; /* S1   */
    dest[4] = nabts_hamming_enc[S        & 0xF]; /* S2   */
    dest[5] = nabts_hamming_enc[(F >> 4) & 0xF]; /* F1   */
    dest[6] = nabts_hamming_enc[F        & 0xF]; /* F2   */
    dest[7] = nabts_hamming_enc[0x0];          /* GN = 0 */
}

/*
 * push_page  –  slice nl_page[0..nl_len) into NABTS packets on channel 0x001.
 *
 * CEA-516 §4.1: a Data Group begins with a Synchronizing Packet (PS b2=1).
 * CEA-516 §4.2: the Data Block of the Synchronizing Packet starts with the
 *   8-byte Hamming-encoded Data Group Header (GT,GC,GR,S1,S2,F1,F2,GN).
 *   The NAPLPS payload follows immediately after the header.
 *
 * Packet layout:
 *   Packet 0 (sync=1): [8-byte DG header][up to 20 bytes NAPLPS]
 *   Packets 1..N-1:    [up to 28 bytes NAPLPS]
 *   Last packet sets full=0 if Data Block is not completely filled.
 *
 * One null packet is interleaved after each data packet.
 *
 * GC is incremented each call so receivers can detect lost Data Groups.
 */
static uint8_t page_gc = 0;   /* Data Group Continuity counter */

static void push_page(void)
{
    /* Pre-calculate packet count and final block size for DG header */
    /* First packet holds 28-8=20 NAPLPS bytes; rest hold 28 each   */
    int first_payload = NABTS_DATA_BLOCK_BYTES - 8;  /* 20 */
    int num_packets, final_bytes;
    if (nl_len <= first_payload) {
        num_packets  = 1;
        final_bytes  = (nl_len > 0) ? nl_len + 8 : 8; /* DG header + data */
    } else {
        int remaining = nl_len - first_payload;
        int extra     = (remaining + NABTS_DATA_BLOCK_BYTES - 1) / NABTS_DATA_BLOCK_BYTES;
        num_packets   = 1 + extra;
        int last_chunk = remaining - (extra - 1) * NABTS_DATA_BLOCK_BYTES;
        final_bytes   = last_chunk;  /* useful bytes in last block */
        if (final_bytes == 0) final_bytes = NABTS_DATA_BLOCK_BYTES;
    }

    int naplps_offset = 0;
    int first = 1;

    while (naplps_offset < nl_len || first) {
        uint8_t data[NABTS_DATA_BLOCK_BYTES];
        memset(data, 0x00, sizeof(data));
        int chunk, is_full;

        if (first) {
            /* Synchronizing Packet: DG header occupies first 8 bytes */
            make_dg_header(data, page_gc, 0, num_packets, final_bytes);
            int space    = NABTS_DATA_BLOCK_BYTES - 8;
            int avail    = nl_len - naplps_offset;
            chunk        = (avail >= space) ? space : avail;
            if (chunk > 0)
                memcpy(data + 8, nl_page + naplps_offset, (size_t)chunk);
            is_full      = ((8 + chunk) == NABTS_DATA_BLOCK_BYTES);
            naplps_offset += chunk;
        } else {
            /* Standard Packet: pure NAPLPS payload */
            int avail = nl_len - naplps_offset;
            chunk     = (avail >= NABTS_DATA_BLOCK_BYTES)
                        ? NABTS_DATA_BLOCK_BYTES : avail;
            memcpy(data, nl_page + naplps_offset, (size_t)chunk);
            is_full   = (chunk == NABTS_DATA_BLOCK_BYTES);
            naplps_offset += chunk;
        }

        uint8_t line[NABTS_LINE_BYTES];
        nabts_build_packet(line,
                           0x001,
                           nabts_ci_next(&ci_data),
                           first,
                           is_full,
                           data);
        push_packet(line);
        push_null();
        first = 0;

        /* Exit after processing the last chunk (avoids extra empty packet) */
        if (naplps_offset >= nl_len && !is_full) break;
        if (naplps_offset >= nl_len) break;
    }

    page_gc = (page_gc + 1) & 0xF;
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
    /*
     * Single-packet Data Group on channel 0x00F.
     *
     * Data Block layout (CEA-516 §4.2):
     *   bytes [0..7]  = 8-byte Hamming-encoded Data Group Header
     *   bytes [8..27] = ASCII payload (20 bytes = "NABTS raspi-teletext")
     * Total = 28 bytes = exactly full, so full=1.
     *
     * DG Header fields for a 1-packet group:
     *   GT=0, GC=ident_gc, GR=0, S1=S2=0 (0 blocks after sync), F1=0,F2=0x1C (28), GN=0
     *   S=0 because there are no Data Blocks following the Synchronizing Packet.
     *   F=28 because the final (only) block is fully used.
     */
    uint8_t ident_gc = 0;

    while (1) {
        uint8_t data[NABTS_DATA_BLOCK_BYTES];
        memset(data, 0x00, sizeof(data));

        /* Data Group Header: 1 packet, final block = 28 bytes */
        make_dg_header(data, ident_gc, 0, 1, NABTS_DATA_BLOCK_BYTES);

        /* ASCII payload after the 8-byte header */
        const char *str = "NABTS raspi-teletext";
        int len = (int)strlen(str);
        int space = NABTS_DATA_BLOCK_BYTES - 8;   /* 20 bytes available */
        if (len > space) len = space;
        memcpy(data + 8, str, (size_t)len);

        uint8_t line[NABTS_LINE_BYTES];
        /* sync=1 (Synchronizing Packet), full=1 (28 bytes used) */
        nabts_build_packet(line, 0x00F,
                           nabts_ci_next(&ci_ident),
                           1,    /* sync */
                           1,    /* full: header(8) + payload(20) = 28 */
                           data);
        push_packet(line);

        for (int i = 0; i < ASCII_NULLS; i++) {
            push_null();
            usleep(3333);
        }

        ident_gc = (ident_gc + 1) & 0xF;
    }
}

void demo(void) { demo_graphics(); }
