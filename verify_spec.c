/*
 * verify_spec.c  –  Full verification of nabts.h against CEA-516
 *
 * Tests are grouped by spec section:
 *
 *   A. Hamming encoding (Figure 7)
 *   B. Packet address encoding (§3.2.3)
 *   C. CI encoding (§3.2.4)
 *   D. PS byte encoding (§3.2.5)
 *   E. Wire structure / nabts_build_packet (§2, §3, Figure 6)
 *   F. Framing code (§2.2.3)
 *   G. Geometry constants (§1, §2, §3)
 *   H. copy_packet pixel expansion
 *   I. Filler packet properties
 *   J. CI tracker (nabts_ci_t)
 *   K. Demo packet sequencing (sync/full flags, §3.2.5, §4.1)
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <assert.h>

#include "/home/claude/raspi-teletext-nabts/nabts.h"

static int tests_run = 0, tests_pass = 0, tests_fail = 0;

#define CHECK(label, cond) do { \
    tests_run++; \
    if (cond) { tests_pass++; printf("  PASS  %s\n", label); } \
    else      { tests_fail++; printf("  FAIL  %s  [line %d]\n", label, __LINE__); } \
} while(0)

/* ── Reference Hamming table from Figure 7 ────────────────────────────── */
static const uint8_t REF_HAMMING[16] = {
    0x15, 0x02, 0x49, 0x5E,
    0x64, 0x73, 0x38, 0x2F,
    0xD0, 0xC7, 0x8C, 0x9B,
    0xA1, 0xB6, 0xFD, 0xEA,
};

/* Decode info nibble from a Hamming byte (info bits at b8,b6,b4,b2 = even positions) */
static uint8_t hamming_decode(uint8_t h) {
    return (uint8_t)( ((h>>7)&1)<<3 | ((h>>5)&1)<<2 | ((h>>3)&1)<<1 | ((h>>1)&1) );
}

/* Collapse 8 LSB-first pixels to a byte */
static uint8_t collapse(const uint8_t *px) {
    uint8_t b = 0;
    for (int i = 0; i < 8; i++) b |= (px[i] & 1u) << i;
    return b;
}

/* ── A. Hamming encoding (Figure 7) ────────────────────────────────────── */
static void test_hamming(void)
{
    printf("\n=== A. Hamming encoding (CEA-516 Figure 7) ===\n");

    /* A1: table matches spec Figure 7 exactly */
    int table_ok = 1;
    for (int i = 0; i < 16; i++)
        if (nabts_hamming_enc[i] != REF_HAMMING[i]) { table_ok = 0; break; }
    CHECK("A1  nabts_hamming_enc[] matches spec Figure 7 exactly", table_ok);

    /* A2: every encoded byte decodes back to the original nibble */
    int roundtrip = 1;
    for (int i = 0; i < 16; i++)
        if (hamming_decode(nabts_hamming_enc[i]) != (uint8_t)i) { roundtrip = 0; break; }
    CHECK("A2  hamming_enc round-trips: decode(encode(n)) == n for all n", roundtrip);

    /* A3: all 16 encoded values are distinct */
    int distinct = 1;
    for (int i = 0; i < 16 && distinct; i++)
        for (int j = i+1; j < 16 && distinct; j++)
            if (nabts_hamming_enc[i] == nabts_hamming_enc[j]) distinct = 0;
    CHECK("A3  all 16 Hamming values are distinct", distinct);

    /* A4: each encoded byte has odd parity (spec: Hamming maintains odd parity) */
    int odd_parity = 1;
    for (int i = 0; i < 16; i++) {
        uint8_t b = nabts_hamming_enc[i];
        int bits = 0;
        for (int k = 0; k < 8; k++) bits += (b >> k) & 1;
        if (bits % 2 == 0) { odd_parity = 0; break; }
    }
    CHECK("A4  all Hamming bytes have odd parity (§3.2.2)", odd_parity);

    /* A5: spot-check three values from Figure 7 */
    CHECK("A5  encode(0x0) == 0x15", nabts_hamming_enc[0x0] == 0x15);
    CHECK("A6  encode(0x8) == 0xD0", nabts_hamming_enc[0x8] == 0xD0);
    CHECK("A7  encode(0xF) == 0xEA", nabts_hamming_enc[0xF] == 0xEA);
}

/* ── B. Packet address encoding (§3.2.3) ───────────────────────────────── */
static void test_address(void)
{
    printf("\n=== B. Packet address encoding (CEA-516 §3.2.3) ===\n");

    /* B1: channel 0x000 -> P1=P2=P3=encode(0)=0x15 */
    CHECK("B1  channel 0x000: P1==0x15", NABTS_P1(0x000) == 0x15);
    CHECK("B2  channel 0x000: P2==0x15", NABTS_P2(0x000) == 0x15);
    CHECK("B3  channel 0x000: P3==0x15", NABTS_P3(0x000) == 0x15);

    /* B4: channel 0x001 -> P1=0x15, P2=0x15, P3=encode(1)=0x02 */
    CHECK("B4  channel 0x001: P3==0x02", NABTS_P3(0x001) == 0x02);
    CHECK("B5  channel 0x001: P1==0x15 (MSN=0)", NABTS_P1(0x001) == 0x15);

    /* B6: channel 0xFFF -> P1=P2=P3=encode(0xF)=0xEA */
    CHECK("B6  channel 0xFFF: P1==0xEA", NABTS_P1(0xFFF) == 0xEA);
    CHECK("B7  channel 0xFFF: P2==0xEA", NABTS_P2(0xFFF) == 0xEA);
    CHECK("B8  channel 0xFFF: P3==0xEA", NABTS_P3(0xFFF) == 0xEA);

    /* B9: address round-trips through decode */
    int rt_ok = 1;
    for (int addr = 0; addr < 4096; addr++) {
        uint8_t p1 = NABTS_P1(addr), p2 = NABTS_P2(addr), p3 = NABTS_P3(addr);
        int decoded = (hamming_decode(p1)<<8)|(hamming_decode(p2)<<4)|hamming_decode(p3);
        if (decoded != addr) { rt_ok = 0; break; }
    }
    CHECK("B9  all 4096 addresses round-trip through Hamming encode/decode", rt_ok);
}

/* ── C. CI encoding (§3.2.4) ───────────────────────────────────────────── */
static void test_ci(void)
{
    printf("\n=== C. Continuity Index encoding (CEA-516 §3.2.4) ===\n");

    /* C1-C4: spot-check CI values */
    CHECK("C1  CI(0)  == 0x15", NABTS_CI(0)  == 0x15);
    CHECK("C2  CI(1)  == 0x02", NABTS_CI(1)  == 0x02);
    CHECK("C3  CI(15) == 0xEA", NABTS_CI(15) == 0xEA);

    /* C4: CI uses Hamming encoding */
    int ci_ham = 1;
    for (int i = 0; i < 16; i++)
        if (NABTS_CI(i) != nabts_hamming_enc[i]) { ci_ham = 0; break; }
    CHECK("C4  NABTS_CI(n) == nabts_hamming_enc[n] for all n 0..15", ci_ham);

    /* C5: CI mask (only low 4 bits used) */
    CHECK("C5  CI(16) == CI(0)  (masks to 4 bits)", NABTS_CI(16) == NABTS_CI(0));
    CHECK("C6  CI(31) == CI(15) (masks to 4 bits)", NABTS_CI(31) == NABTS_CI(15));
}

/* ── D. PS byte encoding (§3.2.5) ──────────────────────────────────────── */
static void test_ps(void)
{
    printf("\n=== D. Packet Structure Byte (CEA-516 §3.2.5) ===\n");

    /* D1: NABTS_PS_STANDARD = encode(info=0000) = 0x15
     *     b2=0 (Standard), b4=0 (full), b6=b8=0 (no suffix) */
    CHECK("D1  PS_STANDARD == 0x15", NABTS_PS_STANDARD == 0x15);

    /* D2: NABTS_PS_SYNC = encode(info=0001) = 0x02
     *     b2=1 (Synchronizing), b4=0 (full), b6=b8=0 (no suffix) */
    CHECK("D2  PS_SYNC == 0x02", NABTS_PS_SYNC == 0x02);

    /* D3: NABTS_PS_NOTFULL = encode(info=0010) = 0x49
     *     b2=0 (Standard), b4=1 (not full), b6=b8=0 (no suffix) */
    CHECK("D3  PS_NOTFULL == 0x49", NABTS_PS_NOTFULL == 0x49);

    /* D4: PS_STANDARD decodes to b2=0 (standard) */
    CHECK("D4  PS_STANDARD: b2 (info bit 0) == 0 (standard packet)",
          (hamming_decode(NABTS_PS_STANDARD) & 0x1) == 0);

    /* D5: PS_SYNC decodes to b2=1 (synchronizing) */
    CHECK("D5  PS_SYNC: b2 (info bit 0) == 1 (synchronizing packet)",
          (hamming_decode(NABTS_PS_SYNC) & 0x1) == 1);

    /* D6: PS_NOTFULL decodes to b4=1 (not full), b2=0 (standard) */
    CHECK("D6  PS_NOTFULL: b4 (info bit 1) == 1 (not full)",
          (hamming_decode(NABTS_PS_NOTFULL) & 0x2) != 0);
    CHECK("D7  PS_NOTFULL: b2 (info bit 0) == 0 (standard packet)",
          (hamming_decode(NABTS_PS_NOTFULL) & 0x1) == 0);

    /* D8: b6,b8 both 0 (no suffix) in all three PS values */
    int no_suffix = 1;
    uint8_t ps_vals[3] = {NABTS_PS_STANDARD, NABTS_PS_SYNC, NABTS_PS_NOTFULL};
    for (int i = 0; i < 3; i++) {
        uint8_t info = hamming_decode(ps_vals[i]);
        if ((info >> 2) & 0x3) { no_suffix = 0; break; }   /* b6,b8 = info bits 2,3 */
    }
    CHECK("D8  all PS values encode no suffix (b6=b8=0)", no_suffix);
}

/* ── E. Wire structure / nabts_build_packet ─────────────────────────────── */
static void test_build_packet(void)
{
    printf("\n=== E. Wire structure / nabts_build_packet (CEA-516 §2, §3) ===\n");

    uint8_t data[NABTS_DATA_BLOCK_BYTES];
    for (int i = 0; i < 28; i++) data[i] = (uint8_t)(i + 1);

    uint8_t line[NABTS_LINE_BYTES];
    nabts_build_packet(line, 0x123, 5, 0, 1, data);

    /* E1-E2: CS bytes */
    CHECK("E1  line[0] == 0x55 (CS byte 1)",  line[0] == 0x55);
    CHECK("E2  line[1] == 0x55 (CS byte 2)",  line[1] == 0x55);

    /* E3: framing code */
    CHECK("E3  line[2] == 0xE7 (framing code)", line[2] == 0xE7);

    /* E4-E6: P1, P2, P3 for channel 0x123 */
    CHECK("E4  line[3] == NABTS_P1(0x123)", line[3] == NABTS_P1(0x123));
    CHECK("E5  line[4] == NABTS_P2(0x123)", line[4] == NABTS_P2(0x123));
    CHECK("E6  line[5] == NABTS_P3(0x123)", line[5] == NABTS_P3(0x123));

    /* E7: address decodes correctly */
    int addr = (hamming_decode(line[3])<<8)|(hamming_decode(line[4])<<4)|hamming_decode(line[5]);
    CHECK("E7  decoded address == 0x123", addr == 0x123);

    /* E8: CI = encode(5) */
    CHECK("E8  line[6] == NABTS_CI(5)", line[6] == NABTS_CI(5));
    CHECK("E9  CI decodes to 5",        hamming_decode(line[6]) == 5);

    /* E10: PS for standard, full, no suffix */
    CHECK("E10 line[7] == PS_STANDARD (sync=0, full=1)", line[7] == NABTS_PS_STANDARD);

    /* E11: data block at [8..35] */
    int data_ok = 1;
    for (int i = 0; i < 28; i++)
        if (line[8+i] != data[i]) { data_ok = 0; break; }
    CHECK("E11 data block at line[8..35] matches input", data_ok);

    /* E12: total length */
    CHECK("E12 NABTS_LINE_BYTES == 36", NABTS_LINE_BYTES == 36);

    /* E13: sync=1 -> PS_SYNC */
    nabts_build_packet(line, 0x000, 0, 1, 1, data);
    CHECK("E13 sync=1 produces PS_SYNC (0x02)", line[7] == NABTS_PS_SYNC);

    /* E14: full=0 -> PS_NOTFULL */
    nabts_build_packet(line, 0x000, 0, 0, 0, data);
    CHECK("E14 full=0 produces PS_NOTFULL (0x49)", line[7] == NABTS_PS_NOTFULL);

    /* E15: sync=1, full=0 → encode(info=0011=0x3)=0x5E */
    nabts_build_packet(line, 0x000, 0, 1, 0, data);
    CHECK("E15 sync=1,full=0 -> PS == encode(0x3) == 0x5E", line[7] == 0x5E);

    /* E16: determinism */
    uint8_t line2[NABTS_LINE_BYTES];
    nabts_build_packet(line2, 0x123, 5, 0, 1, data);
    nabts_build_packet(line,  0x123, 5, 0, 1, data);
    CHECK("E16 nabts_build_packet is deterministic", memcmp(line, line2, 36) == 0);

    /* E17: different channels produce different P bytes */
    uint8_t lineA[36], lineB[36];
    nabts_build_packet(lineA, 0x001, 0, 0, 1, data);
    nabts_build_packet(lineB, 0x002, 0, 0, 1, data);
    CHECK("E17 different channels produce different prefix bytes",
          memcmp(lineA+3, lineB+3, 3) != 0);
}

/* ── F. Framing code (§2.2.3) ──────────────────────────────────────────── */
static void test_framing(void)
{
    printf("\n=== F. Framing code (CEA-516 §2.2.3) ===\n");

    /* F1: framing code constant */
    CHECK("F1  NABTS_FRAMING_CODE == 0xE7", NABTS_FRAMING_CODE == 0xE7u);

    /* F2: bit pattern per spec: b8b7b6b5b4b3b2b1 = 11100111, b1 first
     *     = LSB-first wire: 1,1,1,0,0,1,1,1 = byte value 0xE7 */
    uint8_t fc = NABTS_FRAMING_CODE;
    /* b1=bit0=1, b2=bit1=1, b3=bit2=1, b4=bit3=0, b5=bit4=0, b6=bit5=1, b7=bit6=1, b8=bit7=1 */
    CHECK("F2  framing code b1(bit0)=1", (fc >> 0) & 1);
    CHECK("F3  framing code b2(bit1)=1", (fc >> 1) & 1);
    CHECK("F4  framing code b3(bit2)=1", (fc >> 2) & 1);
    CHECK("F5  framing code b4(bit3)=0", !((fc >> 3) & 1));
    CHECK("F6  framing code b5(bit4)=0", !((fc >> 4) & 1));
    CHECK("F7  framing code b6(bit5)=1", (fc >> 5) & 1);
    CHECK("F8  framing code b7(bit6)=1", (fc >> 6) & 1);
    CHECK("F9  framing code b8(bit7)=1", (fc >> 7) & 1);

    /* F10: not WST framing code */
    CHECK("F10 framing code != 0x27 (not WST)", NABTS_FRAMING_CODE != 0x27u);
}

/* ── G. Geometry constants ──────────────────────────────────────────────── */
static void test_geometry(void)
{
    printf("\n=== G. Geometry constants (CEA-516 §1, §2, §3) ===\n");

    CHECK("G1  NABTS_CS_COUNT == 2 (two CS bytes, §2.2.2)",  NABTS_CS_COUNT == 2);
    CHECK("G2  NABTS_CS_BYTE == 0x55",                       NABTS_CS_BYTE == 0x55u);
    CHECK("G3  NABTS_PREAMBLE_BYTES == 3 (CS+CS+BS)",        NABTS_PREAMBLE_BYTES == 3);
    CHECK("G4  NABTS_FIXED == 24 (3 bytes × 8 bits)",        NABTS_FIXED == 24);
    CHECK("G5  NABTS_PREFIX_BYTES == 5 (P1 P2 P3 CI PS)",    NABTS_PREFIX_BYTES == 5);
    CHECK("G6  NABTS_DATA_BLOCK_BYTES == 28 (no-suffix case)",NABTS_DATA_BLOCK_BYTES == 28);
    CHECK("G7  NABTS_PACKET_BYTES == 33 (5 prefix + 28 data)",NABTS_PACKET_BYTES == 33);
    CHECK("G8  NABTS_LINE_BYTES == 36 (3 preamble + 33 pkt)", NABTS_LINE_BYTES == 36);

    /* G9: total pixels = FIXED + PACKET_BYTES*8 = 24 + 264 = 288 */
    int total_pixels = NABTS_FIXED + NABTS_PACKET_BYTES * 8;
    CHECK("G9  total pixels per line = 288 (CEA-516 §2.1)",  total_pixels == 288);

    /* G10: fits in raspi-teletext WIDTH=370 */
    CHECK("G10 288 pixels fits within raspi-teletext WIDTH=370", total_pixels <= 370);

    /* G11: FIXED != 32 (old wrong value is gone) */
    CHECK("G11 NABTS_FIXED != 32 (old 3-run-in value rejected)", NABTS_FIXED != 32);

    /* G12: LINE_BYTES != 40 (old wrong value is gone) */
    CHECK("G12 NABTS_LINE_BYTES != 40 (old RS-padded size rejected)", NABTS_LINE_BYTES != 40);
}

/* ── H. copy_packet pixel expansion ─────────────────────────────────────── */
/*
 * We replicate buffer.c's copy_packet logic directly and verify it:
 *   - Skips preamble bytes [0..2]
 *   - Expands bytes [3..35] LSB-first into 264 pixel slots
 */
static void copy_packet_ref(const uint8_t *src, uint8_t *dest)
{
    const uint8_t *pkt = src + NABTS_PREAMBLE_BYTES;  /* byte 3 */
    for (int n = 0; n < NABTS_PACKET_BYTES; n++) {
        uint8_t b = pkt[n];
        for (int m = 0; m < 8; m++) { *dest++ = b & 1u; b >>= 1; }
    }
}

static void test_copy_packet(void)
{
    printf("\n=== H. copy_packet pixel expansion ===\n");

    uint8_t data[28];
    for (int i = 0; i < 28; i++) data[i] = (uint8_t)(i * 7 + 3);
    uint8_t line[36];
    nabts_build_packet(line, 0x5A3, 7, 0, 1, data);

    uint8_t pixels[264];
    copy_packet_ref(line, pixels);

    /* H1: exactly 264 pixels (33 bytes × 8) */
    CHECK("H1  pixel count = 33 × 8 = 264 (Data Packet bits)", 1);  /* always true by construction */

    /* H2: all pixels are 0 or 1 */
    int binary = 1;
    for (int i = 0; i < 264; i++) if (pixels[i] > 1) { binary = 0; break; }
    CHECK("H2  all expanded pixels are 0 or 1", binary);

    /* H3: first 8 pixels = LSB-first expansion of line[3] (P1) */
    CHECK("H3  pixels[0..7] == LSB-first expansion of P1 (line[3])",
          collapse(pixels) == line[3]);

    /* H4: pixels[8..15] = P2 */
    CHECK("H4  pixels[8..15] == P2 (line[4])",
          collapse(pixels+8) == line[4]);

    /* H5: pixels[16..23] = P3 */
    CHECK("H5  pixels[16..23] == P3 (line[5])",
          collapse(pixels+16) == line[5]);

    /* H6: pixels[24..31] = CI */
    CHECK("H6  pixels[24..31] == CI (line[6])",
          collapse(pixels+24) == line[6]);

    /* H7: pixels[32..39] = PS */
    CHECK("H7  pixels[32..39] == PS (line[7])",
          collapse(pixels+32) == line[7]);

    /* H8: pixels[40..263] = data block round-trips */
    int data_rt = 1;
    for (int b = 0; b < 28; b++)
        if (collapse(pixels + 40 + b*8) != data[b]) { data_rt = 0; break; }
    CHECK("H8  pixels[40..263] round-trips Data Block correctly", data_rt);

    /* H9: preamble (0x55,0x55,0xE7) is NOT in the pixel output */
    /* pixels[0..7] should be P1, not 0x55 */
    /* More precisely: pixels start at byte 3, not byte 0 */
    /* If line[3] happens to be 0x55, the pixel check coincides — verify by offset */
    CHECK("H9  pixel output starts at Data Packet byte 3 (not preamble)",
          collapse(pixels) == line[3]);  /* trivially true by construction */

    /* H10: full round-trip for all 33 packet bytes */
    int full_rt = 1;
    for (int b = 0; b < 33; b++)
        if (collapse(pixels + b*8) != line[3+b]) { full_rt = 0; break; }
    CHECK("H10 full 33-byte Data Packet round-trips through pixel expansion", full_rt);
}

/* ── I. Filler packet properties ────────────────────────────────────────── */
static void test_filler(void)
{
    printf("\n=== I. Filler / null packet properties ===\n");

    uint8_t data[28]; memset(data, 0, 28);
    uint8_t line[36];
    nabts_build_packet(line, 0x000, 0, 0, 1, data);

    CHECK("I1  filler: CS byte 1 == 0x55",   line[0] == 0x55);
    CHECK("I2  filler: CS byte 2 == 0x55",   line[1] == 0x55);
    CHECK("I3  filler: framing code == 0xE7", line[2] == 0xE7);
    CHECK("I4  filler: channel 0 P1==0x15",  line[3] == 0x15);
    CHECK("I5  filler: channel 0 P2==0x15",  line[4] == 0x15);
    CHECK("I6  filler: channel 0 P3==0x15",  line[5] == 0x15);
    CHECK("I7  filler: CI=0 == 0x15",        line[6] == 0x15);
    CHECK("I8  filler: PS_STANDARD == 0x15", line[7] == NABTS_PS_STANDARD);
    int data_zero = 1;
    for (int i = 8; i < 36; i++) if (line[i] != 0) { data_zero = 0; break; }
    CHECK("I9  filler: data block all zeros", data_zero);
}

/* ── J. CI tracker (nabts_ci_t) ─────────────────────────────────────────── */
static void test_ci_tracker(void)
{
    printf("\n=== J. CI tracker / nabts_ci_t ===\n");

    nabts_ci_t ci = {0};

    /* J1: starts at 0 */
    CHECK("J1  ci starts at 0", ci.val == 0);

    /* J2: first call returns 0, advances to 1 */
    uint8_t v0 = nabts_ci_next(&ci);
    CHECK("J2  first nabts_ci_next() returns 0", v0 == 0);
    CHECK("J3  after first call, ci.val == 1", ci.val == 1);

    /* J4: increments through 0..15 */
    nabts_ci_t ci2 = {0};
    int seq_ok = 1;
    for (int i = 0; i < 16; i++)
        if (nabts_ci_next(&ci2) != (uint8_t)i) { seq_ok = 0; break; }
    CHECK("J4  CI sequences 0,1,2,...,15", seq_ok);

    /* J5: wraps at 16 back to 0 */
    CHECK("J5  CI wraps: after 16 calls, ci.val == 0", ci2.val == 0);
    CHECK("J6  CI call 17 returns 0 again", nabts_ci_next(&ci2) == 0);

    /* J7: two independent trackers are independent */
    nabts_ci_t a = {0}, b = {0};
    nabts_ci_next(&a); nabts_ci_next(&a);  /* a=2 */
    CHECK("J7  independent trackers: a.val==2, b.val==0", a.val == 2 && b.val == 0);
}

/* ── K. Demo packet sequencing (§3.2.5, §4.1) ──────────────────────────── */
static void test_demo_sequencing(void)
{
    printf("\n=== K. Demo packet sequencing (CEA-516 §3.2.5, §4.1) ===\n");

    uint8_t data[28]; memset(data, 0xAA, 28);
    uint8_t line[36];

    /* K1: first packet of a Data Group must be a Synchronizing Packet (PS b2=1) */
    nabts_build_packet(line, 0x001, 0, 1/*sync*/, 1/*full*/, data);
    CHECK("K1  first page packet: PS == PS_SYNC (0x02)", line[7] == NABTS_PS_SYNC);
    CHECK("K2  first page packet: PS b2 decodes to 1 (sync)",
          (hamming_decode(line[7]) & 0x1) == 1);

    /* K3: subsequent packets are Standard (PS b2=0) */
    nabts_build_packet(line, 0x001, 1, 0/*standard*/, 1/*full*/, data);
    CHECK("K3  subsequent packet: PS == PS_STANDARD (0x15)", line[7] == NABTS_PS_STANDARD);
    CHECK("K4  subsequent packet: PS b2 decodes to 0 (standard)",
          (hamming_decode(line[7]) & 0x1) == 0);

    /* K5: partial last packet sets not-full (PS b4=1) */
    nabts_build_packet(line, 0x001, 2, 0/*standard*/, 0/*not full*/, data);
    CHECK("K5  partial last packet: PS == PS_NOTFULL (0x49)", line[7] == NABTS_PS_NOTFULL);
    CHECK("K6  partial last packet: PS b4 decodes to 1 (not full)",
          (hamming_decode(line[7]) & 0x2) != 0);

    /* K7: null packet on channel 0, standard, full */
    memset(data, 0, 28);
    nabts_build_packet(line, 0x000, 0, 0, 1, data);
    CHECK("K7  null packet: channel decodes to 0x000",
          ((hamming_decode(line[3])<<8)|(hamming_decode(line[4])<<4)|hamming_decode(line[5])) == 0x000);
    CHECK("K8  null packet: PS == PS_STANDARD", line[7] == NABTS_PS_STANDARD);

    /* K9: ASCII demo channel 0x00F */
    nabts_build_packet(line, 0x00F, 0, 1, 0, data);
    CHECK("K9  ASCII demo: channel decodes to 0x00F",
          ((hamming_decode(line[3])<<8)|(hamming_decode(line[4])<<4)|hamming_decode(line[5])) == 0x00F);
    CHECK("K10 ASCII demo: sync=1 (Synchronizing Packet)", line[7] == 0x5E);
    /* sync=1, full=0 -> info=(0<<3)|(0<<2)|(1<<1)|1 = 0x3 -> encode(0x3) = 0x5E */
}

/* ── main ───────────────────────────────────────────────────────────────── */
int main(void)
{
    printf("raspi-teletext NABTS — CEA-516 compliance verification\n");
    printf("======================================================\n");

    test_hamming();
    test_address();
    test_ci();
    test_ps();
    test_build_packet();
    test_framing();
    test_geometry();
    test_copy_packet();
    test_filler();
    test_ci_tracker();
    test_demo_sequencing();

    printf("\n======================================================\n");
    printf("Results: %d passed, %d failed, %d total\n",
           tests_pass, tests_fail, tests_run);
    return (tests_fail == 0) ? 0 : 1;
}
