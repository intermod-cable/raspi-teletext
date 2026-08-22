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
 * ── Page addressing (CEA-516 §7.5.2) ────────────────────────────────────
 *
 *  Page 100 in Magazine 1:
 *    Short Record Address = 100 decimal = 0x064 hex
 *      A1 = 0x0, A2 = 0x6, A3 = 0x4
 *    Data Channel (= Magazine number) = 0x100
 *
 *  The Record Header (§5.2) is the first thing in the Data Group Data,
 *  immediately after the 8-byte Data Group Header.  A minimal Record
 *  Header for a cyclic presentation page (§5.2.2.2) consists of:
 *    RT  – Record Type = 0  (Presentation Record, cyclic teletext)
 *    RD  – Record Header Designator (no optional sub-groups present)
 *    A1  – most-significant address nibble  (0x0)
 *    A2  – middle address nibble            (0x6)
 *    A3  – least-significant address nibble (0x4)
 *  All five bytes are Hamming-encoded (§5.2.1).
 *  NAPLPS presentation data follows immediately after A3.
 *
 * ── CEA-516 packet usage ─────────────────────────────────────────────────
 *
 *  channel 0x000  –  null / filler packets (Standard, full, CI incrementing)
 *  channel 0x100  –  all demo data (graphics and ascii), page 100
 *
 *  Data Group structure (CEA-516 §4):
 *    First packet of a Data Group:  sync=1 (Synchronizing Packet, b2=1 in PS)
 *    Subsequent packets:            sync=0 (Standard Packet)
 *    Last packet if Data Block not completely full: full=0 (b4=1 in PS)
 *
 *  Continuity Index (CI, §3.2.4):
 *    Increments by 1 (mod 16) for each packet on a given channel.
 *    Managed per-channel with nabts_ci_t / nabts_ci_next().
 *    Kept continuous across Data Groups (not reset per page), matching the
 *    recommendation in §8.3.2.4: "It is recommended that the continuity
 *    Index be continuous across Data Groups."
 *
 *  Packet-rate pacing: §8.3.2.3 restricts full-field teletext to groups of
 *  <= 32 Data Packets / <= 12 Synchronizing Packets separated by >= 230
 *  scan lines, but that restriction is explicit about VBI: "In VBI
 *  teletext, interspacing of Data Packets is not required. All Data
 *  Packets in the VBI may have the same Packet Address." This encoder only
 *  transmits on VBI lines 10-21 (never full-field/active-video lines), so
 *  no packet-group pacing logic is required here.
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
 *  nl_page NAPLPS bytes (nl_len):
 *    clear(1) + set_bg(3) + 5 bars(85) + title(21) + box(17) + counter(19)
 *    = 146 bytes
 *  (The 5-byte Record Header is added by push_page() inside the Data Block,
 *   not stored in nl_page.  It was previously double-counted here as part
 *   of the 151-byte total.)
 *
 *  Packet 1 (sync): 8-byte DG header + 5-byte Rec Hdr + 15 NAPLPS = 28, full
 *  Packets 2-5:     28 NAPLPS bytes each = 112 bytes
 *  Packet 6:        19 NAPLPS bytes + 9 × 0x80 padding  (final_bytes = 19)
 *  Total = 6 packets; NL_PAGE_MAX = 168 bytes (6 × 28)
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <unistd.h>

#include "buffer.h"
#include "hamming.h"
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
    /*
     * CEA-516 §3.3: all Data Block bytes for Data Group Type 0 must be
     * transmitted with odd parity (bit b8 is the parity bit).
     * parity() sets bit 7 so that the total number of 1-bits is odd.
     */
    if (nl_len < NL_PAGE_MAX) nl_page[nl_len++] = parity(b);
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

/* ── Page 100 addressing (CEA-516 §7.5.2) ──────────────────────────────── */
/*
 * User page number 100 → Short Record Address 0x064 → A1=0x0, A2=0x6, A3=0x4
 * Magazine 1 → Data Channel 0x100
 *
 * Record Header (§5.2.1): RT, RD, A1, A2, A3 — all Hamming-encoded.
 *   RT = 0x0  (Record Type 0: cyclic Presentation Record, §5.2.2.2)
 *   RD = 0x0  (no optional sub-groups: no address extension, no link,
 *               no classification sequence, no header extension, §5.2.3)
 *   A1 = 0x0, A2 = 0x6, A3 = 0x4
 */
#define PAGE_CHANNEL  0x100u   /* Data Channel = Magazine 1       */
#define PAGE_ADDR_A1  0x0u     /* most-significant nibble of 0x064 */
#define PAGE_ADDR_A2  0x6u     /* middle nibble                    */
#define PAGE_ADDR_A3  0x4u     /* least-significant nibble         */

/* NABTS_REC_HDR_BYTES is defined in nabts.h (CEA-516 §5.2.1 constant) */

/* ── Per-channel CI trackers ───────────────────────────────────────────── */

/* channel 0x000 null CI is owned by buffer.c and shared via null_ci_next() */
static nabts_ci_t ci_data  = {0};   /* channel 0x100 (page 100), both demos */

/* ── Packet helpers ────────────────────────────────────────────────────── */

/*
 * push_null  –  Standard filler packet on channel 0x000.
 * Uses null_ci_next() (buffer.c) so the CI is shared with the get_packet()
 * filler path — both paths emit on channel 0x000 and must use one counter.
 */
static void push_null(void)
{
    uint8_t line[NABTS_LINE_BYTES];
    uint8_t data[NABTS_DATA_BLOCK_BYTES];
    /* CEA-516 §3.3: use 0x80 (odd parity of zero), not 0x00 (even parity). */
    memset(data, 0x80, sizeof(data));
    nabts_build_packet(line, 0x000, null_ci_next(), 0, 1, data);
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
 * make_rec_header  –  build the 5-byte Hamming-encoded Record Header
 *                     (CEA-516 §5.2.1) into dest[0..4].
 *
 * Emits: RT(0=cyclic presentation), RD(0=no optional fields), A1, A2, A3.
 * All bytes are Hamming-encoded (§5.2.1 "five Hamming-encoded bytes").
 */
static void make_rec_header(uint8_t *dest,
                             uint8_t a1, uint8_t a2, uint8_t a3)
{
    dest[0] = nabts_hamming_enc[0x0 & 0xF];   /* RT = 0 (cyclic presentation) */
    dest[1] = nabts_hamming_enc[0x0 & 0xF];   /* RD = 0 (no optional fields)  */
    dest[2] = nabts_hamming_enc[a1  & 0xF];   /* A1                           */
    dest[3] = nabts_hamming_enc[a2  & 0xF];   /* A2                           */
    dest[4] = nabts_hamming_enc[a3  & 0xF];   /* A3                           */
}

/*
 * push_page  –  slice nl_page[0..nl_len) into NABTS packets on PAGE_CHANNEL.
 *
 * CEA-516 §4.1: a Data Group begins with a Synchronizing Packet (PS b2=1).
 * CEA-516 §4.2: the Data Block of the Synchronizing Packet starts with the
 *   8-byte Hamming-encoded Data Group Header (GT,GC,GR,S1,S2,F1,F2,GN).
 * CEA-516 §5.2: the Record Header immediately follows the Data Group Header.
 *   Minimal Record Header = RT + RD + A1 + A2 + A3 = 5 Hamming bytes.
 *   NAPLPS payload follows immediately after A3.
 *
 * Packet layout:
 *   Packet 0 (sync=1): [8-byte DG hdr][5-byte rec hdr][up to 15 bytes NAPLPS]
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
    /*
     * CEA-516 §8.4.2.5: FSS Data Groups may not exceed 68 packets (S ≤ 67).
     * With 28-byte Data Blocks and the 5-byte Record Header always present
     * in packet 1, this limits NAPLPS payload to 1891 bytes.
     * Truncate silently and warn on stderr so callers can detect the issue.
     */
    if (nl_len > NABTS_FSS_MAX_NAPLPS) {
        fprintf(stderr,
                "nabts: push_page: page truncated from %d to %d bytes "
                "(FSS max §8.4.2.5)\n",
                nl_len, NABTS_FSS_MAX_NAPLPS);
        nl_len = NABTS_FSS_MAX_NAPLPS;
    }

    /*
     * First packet holds the DG header (8) + Record Header (5) + NAPLPS.
     * Space for NAPLPS in the first packet = 28 - 8 - 5 = 15 bytes.
     */
    int first_payload = NABTS_DATA_BLOCK_BYTES - 8 - NABTS_REC_HDR_BYTES;  /* 15 */
    int num_packets, final_bytes;
    if (nl_len <= first_payload) {
        num_packets  = 1;
        /* DG header(8) + rec header(5) + NAPLPS data = total useful bytes */
        final_bytes  = 8 + NABTS_REC_HDR_BYTES + nl_len;
    } else {
        int remaining = nl_len - first_payload;
        int extra     = (remaining + NABTS_DATA_BLOCK_BYTES - 1) / NABTS_DATA_BLOCK_BYTES;
        num_packets   = 1 + extra;
        int last_chunk = remaining - (extra - 1) * NABTS_DATA_BLOCK_BYTES;
        final_bytes   = last_chunk;
        if (final_bytes == 0) final_bytes = NABTS_DATA_BLOCK_BYTES;
    }

    int naplps_offset = 0;
    int first = 1;

    while (naplps_offset < nl_len || first) {
        uint8_t data[NABTS_DATA_BLOCK_BYTES];
        /*
         * CEA-516 §3.3: ALL bytes in a Data Block must have odd parity.
         * §8.3.4 (FSS) is explicit about exactly this case: "If [...] the
         * Data Packet is not completely full of useful data, then the
         * error correction-and-detection schemes used by the Suffix shall
         * also apply to the extra bytes. The extra bytes shall also have
         * odd parity." 0x00 has even parity; 0x80 = parity(0x00) is the
         * correct null byte. This matters here: the padding bytes at the
         * end of the last (non-full) packet are not overwritten by the
         * memcpy below and are transmitted as-is.
         */
        memset(data, 0x80, sizeof(data));
        int chunk, is_full;

        if (first) {
            /* Synchronizing Packet: DG header (8) + Record Header (5) */
            make_dg_header(data, page_gc, 0, num_packets, final_bytes);
            make_rec_header(data + 8,
                            PAGE_ADDR_A1, PAGE_ADDR_A2, PAGE_ADDR_A3);
            int space = NABTS_DATA_BLOCK_BYTES - 8 - NABTS_REC_HDR_BYTES; /* 15 */
            int avail = nl_len - naplps_offset;
            chunk     = (avail >= space) ? space : avail;
            if (chunk > 0)
                memcpy(data + 8 + NABTS_REC_HDR_BYTES,
                       nl_page + naplps_offset, (size_t)chunk);
            is_full   = ((8 + NABTS_REC_HDR_BYTES + chunk) == NABTS_DATA_BLOCK_BYTES);
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
                           PAGE_CHANNEL,
                           nabts_ci_next(&ci_data),
                           first,
                           is_full,
                           data);
        push_packet(line);
        push_null();
        first = 0;

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
 * Emits a single-packet Data Group on PAGE_CHANNEL (0x100) carrying a plain
 * ASCII identification string every ~1 second, with null packets between
 * bursts.
 *
 * Like demo_graphics() this uses PAGE_CHANNEL and includes the 5-byte Record
 * Header (§5.2) so the decoder associates the packet with page 100.  The
 * ci_data CI tracker is shared with demo_graphics() since both transmit on
 * the same channel; whichever demo is running owns ci_data exclusively.
 *
 * Data Block layout (CEA-516 §4.2 / §5.2):
 *   bytes [0..7]  = 8-byte Hamming-encoded Data Group Header
 *   bytes [8..12] = 5-byte Hamming-encoded Record Header (RT,RD,A1,A2,A3)
 *   bytes [13..27]= ASCII payload (15 bytes, odd-parity, §3.3)
 * Total = 28 bytes = exactly full, so full=1.
 *
 * DG Header fields:
 *   GT=0, GC=page_gc (shared), GR=0
 *   S1=S2=0  (no Data Blocks follow the Synchronizing Packet)
 *   F1,F2 → decoded F=28  (final block fully used)
 *   GN=0
 *
 * Usage: sudo ./teletext -d ascii
 */
#define ASCII_NULLS  299

void demo_ascii(void)
{
    while (1) {
        uint8_t data[NABTS_DATA_BLOCK_BYTES];
        /* CEA-516 §3.3: pad with 0x80 (odd parity of zero), not 0x00.
         * All 28 bytes are overwritten below, so this is defensive. */
        memset(data, 0x80, sizeof(data));

        /* Data Group Header: 1 packet, final block = 28 bytes (full) */
        make_dg_header(data, page_gc, 0, 1, NABTS_DATA_BLOCK_BYTES);

        /* Record Header: ties this packet to page 100 on Magazine 1 */
        make_rec_header(data + 8,
                        PAGE_ADDR_A1, PAGE_ADDR_A2, PAGE_ADDR_A3);

        /* ASCII payload after header + Record Header.
         * CEA-516 §3.3: each payload byte must have odd parity. */
        const char *str = "NABTS raspi-teletext p100";
        int space = NABTS_DATA_BLOCK_BYTES - 8 - NABTS_REC_HDR_BYTES;  /* 15 */
        int len   = (int)strlen(str);
        if (len > space) len = space;
        for (int k = 0; k < len; k++)
            data[8 + NABTS_REC_HDR_BYTES + k] = parity((uint8_t)str[k]);

        uint8_t line[NABTS_LINE_BYTES];
        /* sync=1 (Synchronizing Packet), full=1 (all 28 bytes used) */
        nabts_build_packet(line, PAGE_CHANNEL,
                           nabts_ci_next(&ci_data),
                           1,    /* sync */
                           1,    /* full: 8+5+15 = 28 */
                           data);
        push_packet(line);

        for (int i = 0; i < ASCII_NULLS; i++) {
            push_null();
            usleep(3333);
        }

        page_gc = (page_gc + 1) & 0xF;
    }
}

void demo(void) { demo_graphics(); }
