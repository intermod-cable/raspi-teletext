#ifndef NABTS_H
#define NABTS_H

#include <stdint.h>

/*
 * nabts.h  –  NABTS packet framing per CEA-516 (May 1988)
 *
 * ── Wire structure (CEA-516 §2, §3, Figure 6) ───────────────────────────
 *
 *  Each VBI Data Line = 288 bits = 36 bytes, LSB first:
 *
 *  Synchronization Sequence (24 bits = 3 bytes):
 *    [0]   0x55   CS byte 1  –  Clock Sync, alternating 1/0, first bit=1
 *    [1]   0x55   CS byte 2
 *    [2]   0xE7   BS         –  Framing Code (§2.2.3: 11100111, b1 first)
 *
 *  Data Packet (264 bits = 33 bytes):
 *    [3]   P1     Hamming-encoded Packet Address byte 1 (most significant)
 *    [4]   P2     Hamming-encoded Packet Address byte 2
 *    [5]   P3     Hamming-encoded Packet Address byte 3 (least significant)
 *              → 12 info bits total = Data Channel number (0x000..0xFFF)
 *    [6]   CI     Hamming-encoded Continuity Index (0..15, increments per pkt)
 *    [7]   PS     Hamming-encoded Packet Structure Byte
 *              b2=1: Synchronizing Packet (starts a Data Group)
 *              b2=0: Standard Packet
 *              b4=1: Data Block not full of useful data
 *              b6,b8: suffix length (00=none, 01=1 byte, 10=2 bytes, 11=28 bytes)
 *    [8..35]  Data Block (0, 26, 27 or 28 bytes depending on suffix)
 *    [35]     Optional suffix (see §3.4); for no-suffix case byte 35 = data
 *
 *  No-suffix case (FSS baseline per §8.3.4):
 *    Data Block = 28 bytes [8..35]
 *    No suffix bytes
 *    Total = 36 bytes ✓
 *
 * ── Hamming encoding (CEA-516 Figure 7) ─────────────────────────────────
 *
 *  Each Hamming byte carries 4 information bits in bit positions b8,b6,b4,b2
 *  (i.e. the even-numbered bits when bits are labelled b1..b8).
 *  Bits b7,b5,b3,b1 are Hamming parity/protection bits.
 *  Allows single-bit error correction, double-bit error detection.
 *
 * ── Longitudinal parity suffix (CEA-516 §3.4, §8.3.4) ───────────────────
 *
 *  FSS receivers must implement the 1-byte suffix.
 *  The Longitudinal Parity Check (LPC) byte is appended after the Data Block.
 *  XOR of all Data Block bytes combined with LPC must equal 0xFF (odd parity).
 *  Therefore: LPC = XOR(data[0..27]) ^ 0xFF
 *
 *  With a 1-byte suffix the Data Block shrinks to 27 bytes.
 *  PS bits b6=1, b8=0 signal a 1-byte suffix.
 *  For simplicity this implementation uses no suffix (PS b6=b8=0, 28-byte block).
 *  Decoders are required to accept no-suffix packets (§3.4: suffix is optional).
 *
 * ── Signal parameters (CEA-516 §1) ──────────────────────────────────────
 *
 *  Bit rate:   5,727,272 bps ± 16 bps  (= 8/5 × colour subcarrier)
 *  Logic 1:    70 ± 2 IRE
 *  Logic 0:    0 ± 2 IRE (blanking level)
 *  Modulation: NRZ
 *  Bit order:  LSB first
 *
 * ── VBI lines (CEA-516 §1.1.1) ──────────────────────────────────────────
 *
 *  NTSC 525-line: lines 10 through 21, both fields.
 *  Full-field:    lines 10 through 262, both fields.
 *
 * ── raspi-teletext framebuffer mapping ──────────────────────────────────
 *
 *  The 288 bits of each Data Line are expanded to 349 source pixels using
 *  a Bresenham bit-width table to correct for the Pi's composite pixel clock
 *  (see "Pixel clock correction" section below).
 *
 *  FIXED = 29 pixels: the 24-bit preamble (CS+BS) occupies the first 29
 *  pixel columns; written once by init() and never overwritten.
 *  copy_packet() renders the 264-bit Data Packet into the 320 pixels that
 *  follow.
 *  Total = 29 + 320 = 349 pixels per line (effective bit rate 5,724,928 Hz).
 *  With OFFSET=8 the signal starts at 10.55 µs after the sync leading edge,
 *  within the CEA-516 §1.3 requirement of 10.48 ± 0.34 µs [10.14, 10.82] µs.
 */

/* ── Wire constants ─────────────────────────────────────────────────────── */

#define NABTS_CS_BYTE         0x55u   /* clock sync byte (both CS bytes)      */
#define NABTS_CS_COUNT        2       /* two CS bytes (CEA-516 §2.2.2)        */
#define NABTS_FRAMING_CODE    0xE7u   /* byte sync / framing code (§2.2.3)    */

/* Preamble = CS + BS = 3 bytes */
#define NABTS_PREAMBLE_BYTES  (NABTS_CS_COUNT + 1)   /* 3  */
/* NABTS_FIXED is defined below in the pixel clock section (value: 29) */

/* Data Packet structure (§3) */
#define NABTS_PREFIX_BYTES    5    /* P1 P2 P3 CI PS                          */
#define NABTS_DATA_BLOCK_BYTES 28  /* Data Block, no-suffix case              */
#define NABTS_PACKET_BYTES    (NABTS_PREFIX_BYTES + NABTS_DATA_BLOCK_BYTES) /* 33 */

/* Total on-wire bytes per Data Line */
#define NABTS_LINE_BYTES      (NABTS_PREAMBLE_BYTES + NABTS_PACKET_BYTES) /* 36 */

/* ── Hamming encoding table (CEA-516 Figure 7) ──────────────────────────── */
/*
 * hamming_enc[n] gives the Hamming-encoded byte for info nibble n (0x0..0xF).
 * Info bits occupy positions b8,b6,b4,b2 of the result byte.
 */
static const uint8_t nabts_hamming_enc[16] = {
    0x15, 0x02, 0x49, 0x5E,   /* 0x0 .. 0x3 */
    0x64, 0x73, 0x38, 0x2F,   /* 0x4 .. 0x7 */
    0xD0, 0xC7, 0x8C, 0x9B,   /* 0x8 .. 0xB */
    0xA1, 0xB6, 0xFD, 0xEA,   /* 0xC .. 0xF */
};

/* ── PS byte constants (Packet Structure) ───────────────────────────────── */
/*
 * PS info nibble bit layout:
 *   bit 3 (b8): suffix length high bit
 *   bit 2 (b6): suffix length low bit
 *   bit 1 (b4): 1 = Data Block not full
 *   bit 0 (b2): 1 = Synchronizing Packet (starts Data Group), 0 = Standard
 *
 * Suffix length encoding (b8,b6):
 *   0,0 = no suffix  (28-byte Data Block)
 *   0,1 = 1-byte LPC suffix (27-byte Data Block)
 *   1,0 = 2-byte suffix (26-byte Data Block)
 *   1,1 = 28-byte suffix (0-byte Data Block, reserved future use)
 */

/* Standard packet, Data Block full, no suffix */
#define NABTS_PS_STANDARD  (nabts_hamming_enc[0x0])  /* 0x15 */

/* Synchronizing packet (starts a Data Group), Data Block full, no suffix */
#define NABTS_PS_SYNC      (nabts_hamming_enc[0x1])  /* 0x02 */

/* Standard packet, Data Block NOT full, no suffix */
#define NABTS_PS_NOTFULL   (nabts_hamming_enc[0x2])  /* 0x49 */

/* ── CI convenience macro ───────────────────────────────────────────────── */
/* Encode a Continuity Index value (0..15) to its Hamming byte */
#define NABTS_CI(n)  (nabts_hamming_enc[(n) & 0xF])

/* ── Packet address encoding ─────────────────────────────────────────────
 *
 * A 12-bit Data Channel address is split into three 4-bit nibbles,
 * each Hamming-encoded into one byte.
 * P1 = most-significant nibble, P3 = least-significant.
 */
#define NABTS_P1(addr)  (nabts_hamming_enc[((addr) >> 8) & 0xF])
#define NABTS_P2(addr)  (nabts_hamming_enc[((addr) >> 4) & 0xF])
#define NABTS_P3(addr)  (nabts_hamming_enc[((addr) >> 0) & 0xF])

/* ── Packet builder ──────────────────────────────────────────────────────
 *
 * nabts_build_packet  –  assemble one complete 36-byte NABTS Data Line.
 *
 * @out      output buffer, at least NABTS_LINE_BYTES (36) bytes
 * @channel  12-bit Data Channel / Packet Address (0x000..0xFFF)
 * @ci       Continuity Index value (0..15); caller increments per packet
 *           per channel.  Use nabts_ci_next() to manage automatically.
 * @sync     1 = Synchronizing Packet (first packet of a Data Group)
 *           0 = Standard Packet
 * @full     1 = Data Block is fully occupied by useful data  (b4=0 in PS)
 *           0 = Data Block is not full                        (b4=1 in PS)
 * @data     pointer to exactly NABTS_DATA_BLOCK_BYTES (28) bytes of payload;
 *           caller pads unused bytes to 0x00
 *
 * Wire layout of out[0..35]:
 *   [0]      0x55  CS1
 *   [1]      0x55  CS2
 *   [2]      0xE7  BS (framing code)
 *   [3]      P1    Hamming(channel[11:8])
 *   [4]      P2    Hamming(channel[7:4])
 *   [5]      P3    Hamming(channel[3:0])
 *   [6]      CI    Hamming(ci & 0xF)
 *   [7]      PS    Hamming(sync | ((!full)<<1))
 *   [8..35]  data[0..27]
 */
static inline void nabts_build_packet(uint8_t *out,
                                      uint16_t channel,
                                      uint8_t  ci,
                                      int      sync,
                                      int      full,
                                      const uint8_t data[NABTS_DATA_BLOCK_BYTES])
{
    /* Synchronisation sequence */
    out[0] = NABTS_CS_BYTE;
    out[1] = NABTS_CS_BYTE;
    out[2] = NABTS_FRAMING_CODE;

    /* Packet Prefix: P1, P2, P3 (address), CI, PS */
    out[3] = NABTS_P1(channel);
    out[4] = NABTS_P2(channel);
    out[5] = NABTS_P3(channel);
    out[6] = NABTS_CI(ci);

    /* PS: info nibble = (b8=0, b6=0, b4=!full, b2=sync)
     *                 = (0 << 3) | (0 << 2) | ((!full) << 1) | sync */
    uint8_t ps_info = (uint8_t)(((full ? 0 : 1) << 1) | (sync ? 1 : 0));
    out[7] = nabts_hamming_enc[ps_info & 0xF];

    /* Data Block */
    for (int i = 0; i < NABTS_DATA_BLOCK_BYTES; i++)
        out[8 + i] = data[i];
}

/* ── Continuity Index tracker ────────────────────────────────────────────
 *
 * The spec requires CI to increment by 1 (mod 16) for each packet
 * transmitted on a given Data Channel (§3.2.4).
 *
 * Declare one nabts_ci_t per channel and call nabts_ci_next() to get
 * the value to pass to nabts_build_packet().
 */
typedef struct { uint8_t val; } nabts_ci_t;

static inline uint8_t nabts_ci_next(nabts_ci_t *ci)
{
    uint8_t v = ci->val;
    ci->val = (ci->val + 1) & 0xF;
    return v;
}


/* ── Pixel clock correction for Raspberry Pi composite output ────────────
 *
 * raspi-teletext works by placing pixel data in a dispmanx framebuffer
 * which is stretched from WIDTH source pixels to 720 pixels by the
 * hardware compositor before reaching the Pi's VEC (Video Encoder Chip).
 *
 * Effective source pixel rate = VEC_clock × WIDTH / 720
 *                             = 13.5 MHz × 370 / 720
 *                             = 6.9375 MHz
 *
 * This rate matches WST (6.9375 MHz) but is 21% too fast for NABTS.
 * CEA-516 §1.3 requires 5,727,272 bps.
 *
 * Fix: use Bresenham (accumulator) bit expansion so that 288 NABTS bits
 * are spread across 349 source pixels instead of 288.
 *
 *   Effective bit rate = 6.9375 MHz × 288/349 = 5,724,928 Hz
 *   Error vs spec:      −2,344 Hz  (146× the ±16 Hz tolerance of §1.3)
 *
 * NOTE: The ±16 Hz tolerance in §1.3 is derived from PLL-locking the
 * transmitter to the NTSC colour subcarrier (§1.3 note: "may be 8/5 of
 * the color sub-carrier frequency … and may be frequency locked").  The
 * Pi VEC in software composite mode is NOT subcarrier-locked; its pixel
 * clock is a free-running PLL from the 19.2 MHz crystal (±50 ppm typical
 * = ±286 Hz on the resulting bit rate), so the absolute ±16 Hz limit is
 * unachievable regardless of pixel count.  No integer N at WIDTH=370
 * meets the ±16 Hz window; N=349 is the closest achievable value.
 * In practice NABTS decoder PLLs have capture ranges far wider than
 * 16 Hz and lock reliably on this signal.
 *
 * 227 bits are rendered as 1 pixel wide; 61 bits as 2 pixels wide.
 * The doubled bits are distributed evenly by the Bresenham pattern.
 *
 * Consequences:
 *   NABTS_FIXED        = 29   (pixels for the 24-bit preamble)
 *   NABTS_DATA_PIXELS  = 320  (pixels for the 264-bit Data Packet)
 *   NABTS_TOTAL_PIXELS = 349  (total per line; fits in WIDTH=370 with OFFSET=8)
 */

#define NABTS_TOTAL_BITS    288   /* bits per Data Line (CEA-516 §2.1) */
#define NABTS_TOTAL_PIXELS  349   /* source pixels per Data Line (Pi clock-corrected) */
#define NABTS_FIXED         29    /* source pixels for 24-bit preamble */
#define NABTS_DATA_PIXELS   320   /* source pixels for 264-bit Data Packet */

/*
 * nabts_px_width[b] = number of source pixels for bit b (0..287).
 * Generated by Bresenham accumulator: b * 349 / 288.
 * Values are 1 or 2 only.  sum = 349.
 */
static const uint8_t nabts_px_width[NABTS_TOTAL_BITS] = {
    1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1,
    1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1,
    1, 2, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2,
    1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1,
    1, 1, 2, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1,
    2, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1,
    1, 1, 1, 2, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1,
    1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 2, 1, 1, 1, 1, 2,
    1, 1, 1, 1, 2, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1,
    1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 2, 1, 1, 1, 1,
    2, 1, 1, 1, 1, 2, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1,
    1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 2, 1, 1, 1,
    1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 2,
    1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 2, 1, 1,
    1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1,
    2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 2, 1,
    1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1,
    1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 1, 2, 1, 1, 1, 2,
};

/* ── FSS Data Group size limits (CEA-516 §8.4.2.5) ─────────────────────────
 *
 * In the FSS service the maximum S value (S1,S2 decoded) is 67, meaning at
 * most 68 Data Packets per Data Group.  With a 28-byte Data Block and no
 * suffix the maximum NAPLPS payload is:
 *   first packet:  28 - 8 (DG header) - 5 (Record Header) = 15 bytes
 *   packets 2-68:  67 × 28                                 = 1876 bytes
 *   total:                                                    1891 bytes
 *
 * Note: the Record Header (NABTS_REC_HDR_BYTES = 5) always occupies part of
 * the first Data Block alongside the DG header, leaving only 15 bytes for
 * NAPLPS in packet 1.  The previous value of 1896 was off by 5 and could
 * allow push_page() to generate 69 packets (exceeding the FSS limit of 68).
 */
#define NABTS_FSS_MAX_PACKETS  68
#define NABTS_FSS_MAX_NAPLPS \
    ((NABTS_DATA_BLOCK_BYTES - 8 - NABTS_REC_HDR_BYTES) + \
     (NABTS_FSS_MAX_PACKETS - 1) * NABTS_DATA_BLOCK_BYTES)   /* 1891 */

#endif /* NABTS_H */
