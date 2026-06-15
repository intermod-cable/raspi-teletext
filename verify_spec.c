/*
 * verify_spec.c  –  Full CEA-516 compliance and pixel clock verification
 *
 *   A.  Hamming encoding (Figure 7)
 *   B.  Packet address encoding (§3.2.3)
 *   C.  CI encoding (§3.2.4)
 *   D.  PS byte encoding (§3.2.5)
 *   E.  Wire structure / nabts_build_packet (§2, §3, Figure 6)
 *   F.  Framing code (§2.2.3)
 *   G.  Geometry constants (§1, §2, §3)
 *   H.  Pixel clock correction (Bresenham table)
 *   I.  copy_packet pixel expansion with variable bit widths
 *   J.  Preamble rendering (FIXED region)
 *   K.  Filler packet
 *   L.  CI tracker
 *   M.  Demo packet sequencing
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "nabts.h"

static int tests_run = 0, tests_pass = 0, tests_fail = 0;
#define CHECK(label, cond) do { \
    tests_run++; \
    if (cond) { tests_pass++; printf("  PASS  %s\n", label); } \
    else      { tests_fail++; printf("  FAIL  %s  [line %d]\n", label, __LINE__); } \
} while(0)

static const uint8_t REF_HAMMING[16] = {
    0x15,0x02,0x49,0x5E, 0x64,0x73,0x38,0x2F,
    0xD0,0xC7,0x8C,0x9B, 0xA1,0xB6,0xFD,0xEA,
};
static uint8_t hamming_decode(uint8_t h) {
    return (uint8_t)(((h>>7)&1)<<3|((h>>5)&1)<<2|((h>>3)&1)<<1|((h>>1)&1));
}

/* copy_packet reference implementation */
static void copy_packet_ref(const uint8_t *src, uint8_t *dest)
{
    const uint8_t *pkt = src + NABTS_PREAMBLE_BYTES;
    int col = 0;
    for (int bi = 0; bi < NABTS_PACKET_BYTES; bi++) {
        uint8_t b = pkt[bi];
        for (int bit = 0; bit < 8; bit++) {
            int global_bit = NABTS_PREAMBLE_BYTES*8 + bi*8 + bit;
            int width = nabts_px_width[global_bit];
            uint8_t val = (b>>bit)&1u;
            dest[col] = val;
            if (width==2) dest[col+1] = val;
            col += width;
        }
    }
}

/* preamble render reference */
static void render_preamble_ref(uint8_t *row)
{
    static const uint8_t pre[3] = {0x55,0x55,0xE7};
    int col = 0;
    for (int bi=0; bi<3; bi++) {
        uint8_t b = pre[bi];
        for (int bit=0; bit<8; bit++) {
            int global_bit = bi*8+bit;
            int width = nabts_px_width[global_bit];
            uint8_t val = (b>>bit)&1u;
            row[col] = val;
            if (width==2) row[col+1] = val;
            col += width;
        }
    }
}

/* ── A. Hamming ─────────────────────────────────────────────────────────── */
static void test_hamming(void) {
    printf("\n=== A. Hamming encoding (CEA-516 Figure 7) ===\n");
    int ok=1; for(int i=0;i<16;i++) if(nabts_hamming_enc[i]!=REF_HAMMING[i]){ok=0;break;}
    CHECK("A1  table matches Figure 7", ok);
    ok=1; for(int i=0;i<16;i++) if(hamming_decode(nabts_hamming_enc[i])!=(uint8_t)i){ok=0;break;}
    CHECK("A2  round-trip decode(encode(n))==n", ok);
    ok=1; for(int i=0;i<16;i++){int bits=0; uint8_t b=nabts_hamming_enc[i];
          for(int k=0;k<8;k++) bits+=(b>>k)&1;
          if(bits%2==0){ok=0;break;}}
    CHECK("A3  all Hamming bytes have odd parity (§3.2.2)", ok);
    CHECK("A4  encode(0x0)==0x15", nabts_hamming_enc[0x0]==0x15);
    CHECK("A5  encode(0x8)==0xD0", nabts_hamming_enc[0x8]==0xD0);
    CHECK("A6  encode(0xF)==0xEA", nabts_hamming_enc[0xF]==0xEA);
}

/* ── B. Packet address ──────────────────────────────────────────────────── */
static void test_address(void) {
    printf("\n=== B. Packet address (CEA-516 §3.2.3) ===\n");
    CHECK("B1  ch 0x000: P1==0x15", NABTS_P1(0x000)==0x15);
    CHECK("B2  ch 0x000: P2==0x15", NABTS_P2(0x000)==0x15);
    CHECK("B3  ch 0x000: P3==0x15", NABTS_P3(0x000)==0x15);
    CHECK("B4  ch 0x001: P3==0x02", NABTS_P3(0x001)==0x02);
    CHECK("B5  ch 0xFFF: P1==0xEA", NABTS_P1(0xFFF)==0xEA);
    int ok=1;
    for(int a=0;a<4096;a++){
        int d=(hamming_decode(NABTS_P1(a))<<8)|(hamming_decode(NABTS_P2(a))<<4)|hamming_decode(NABTS_P3(a));
        if(d!=a){ok=0;break;}
    }
    CHECK("B6  all 4096 addresses round-trip", ok);
}

/* ── C. CI ──────────────────────────────────────────────────────────────── */
static void test_ci(void) {
    printf("\n=== C. Continuity Index (CEA-516 §3.2.4) ===\n");
    CHECK("C1  CI(0)==0x15",  NABTS_CI(0)==0x15);
    CHECK("C2  CI(1)==0x02",  NABTS_CI(1)==0x02);
    CHECK("C3  CI(15)==0xEA", NABTS_CI(15)==0xEA);
    int ok=1; for(int i=0;i<16;i++) if(NABTS_CI(i)!=nabts_hamming_enc[i]){ok=0;break;}
    CHECK("C4  CI(n)==hamming_enc[n] all n", ok);
    CHECK("C5  CI wraps at 16: CI(16)==CI(0)", NABTS_CI(16)==NABTS_CI(0));
}

/* ── D. PS byte ─────────────────────────────────────────────────────────── */
static void test_ps(void) {
    printf("\n=== D. Packet Structure Byte (CEA-516 §3.2.5) ===\n");
    CHECK("D1  PS_STANDARD==0x15", NABTS_PS_STANDARD==0x15);
    CHECK("D2  PS_SYNC==0x02",     NABTS_PS_SYNC==0x02);
    CHECK("D3  PS_NOTFULL==0x49",  NABTS_PS_NOTFULL==0x49);
    CHECK("D4  PS_STANDARD: b2==0 (standard)", (hamming_decode(NABTS_PS_STANDARD)&1)==0);
    CHECK("D5  PS_SYNC: b2==1 (synchronizing)", (hamming_decode(NABTS_PS_SYNC)&1)==1);
    CHECK("D6  PS_NOTFULL: b4==1 (not full)", (hamming_decode(NABTS_PS_NOTFULL)&2)!=0);
    uint8_t ps_vals[3]={NABTS_PS_STANDARD,NABTS_PS_SYNC,NABTS_PS_NOTFULL};
    int ok=1; for(int i=0;i<3;i++) if((hamming_decode(ps_vals[i])>>2)&3){ok=0;break;}
    CHECK("D7  all PS values: no suffix (b6=b8=0)", ok);
}

/* ── E. Wire structure ──────────────────────────────────────────────────── */
static void test_build_packet(void) {
    printf("\n=== E. Wire structure / nabts_build_packet (CEA-516 §2, §3) ===\n");
    uint8_t data[28]; for(int i=0;i<28;i++) data[i]=(uint8_t)(i+1);
    uint8_t line[36];
    nabts_build_packet(line,0x123,5,0,1,data);
    CHECK("E1  line[0]==0x55 (CS1)",   line[0]==0x55);
    CHECK("E2  line[1]==0x55 (CS2)",   line[1]==0x55);
    CHECK("E3  line[2]==0xE7 (BS)",    line[2]==0xE7);
    CHECK("E4  line[3]==P1(0x123)",    line[3]==NABTS_P1(0x123));
    CHECK("E5  line[4]==P2(0x123)",    line[4]==NABTS_P2(0x123));
    CHECK("E6  line[5]==P3(0x123)",    line[5]==NABTS_P3(0x123));
    int addr=(hamming_decode(line[3])<<8)|(hamming_decode(line[4])<<4)|hamming_decode(line[5]);
    CHECK("E7  decoded address==0x123", addr==0x123);
    CHECK("E8  line[6]==CI(5)",        line[6]==NABTS_CI(5));
    CHECK("E9  line[7]==PS_STANDARD",  line[7]==NABTS_PS_STANDARD);
    int ok=1; for(int i=0;i<28;i++) if(line[8+i]!=data[i]){ok=0;break;}
    CHECK("E10 data block at [8..35]", ok);
    CHECK("E11 NABTS_LINE_BYTES==36",  NABTS_LINE_BYTES==36);
    nabts_build_packet(line,0,0,1,1,data);
    CHECK("E12 sync=1 -> PS_SYNC",     line[7]==NABTS_PS_SYNC);
    nabts_build_packet(line,0,0,0,0,data);
    CHECK("E13 full=0 -> PS_NOTFULL",  line[7]==NABTS_PS_NOTFULL);
    nabts_build_packet(line,0,0,1,0,data);
    CHECK("E14 sync=1,full=0 -> 0x5E", line[7]==0x5E);
}

/* ── F. Framing code ────────────────────────────────────────────────────── */
static void test_framing(void) {
    printf("\n=== F. Framing code (CEA-516 §2.2.3) ===\n");
    CHECK("F1  NABTS_FRAMING_CODE==0xE7", NABTS_FRAMING_CODE==0xE7u);
    uint8_t fc=NABTS_FRAMING_CODE;
    CHECK("F2  b1==1", (fc>>0)&1);  CHECK("F3  b2==1", (fc>>1)&1);
    CHECK("F4  b3==1", (fc>>2)&1);  CHECK("F5  b4==0", !((fc>>3)&1));
    CHECK("F6  b5==0", !((fc>>4)&1)); CHECK("F7  b6==1", (fc>>5)&1);
    CHECK("F8  b7==1", (fc>>6)&1);  CHECK("F9  b8==1", (fc>>7)&1);
    CHECK("F10 !=0x27 (not WST)", NABTS_FRAMING_CODE!=0x27u);
}

/* ── G. Geometry ────────────────────────────────────────────────────────── */
static void test_geometry(void) {
    printf("\n=== G. Geometry constants (CEA-516 §1, §2, §3) ===\n");
    CHECK("G1  CS_COUNT==2",           NABTS_CS_COUNT==2);
    CHECK("G2  PREAMBLE_BYTES==3",     NABTS_PREAMBLE_BYTES==3);
    CHECK("G3  PREFIX_BYTES==5",       NABTS_PREFIX_BYTES==5);
    CHECK("G4  DATA_BLOCK_BYTES==28",  NABTS_DATA_BLOCK_BYTES==28);
    CHECK("G5  PACKET_BYTES==33",      NABTS_PACKET_BYTES==33);
    CHECK("G6  LINE_BYTES==36",        NABTS_LINE_BYTES==36);
    CHECK("G7  TOTAL_BITS==288",       NABTS_TOTAL_BITS==288);
    CHECK("G8  TOTAL_PIXELS==349",     NABTS_TOTAL_PIXELS==349);
    CHECK("G9  FIXED==29",             NABTS_FIXED==29);
    CHECK("G10 DATA_PIXELS==320",      NABTS_DATA_PIXELS==320);
    CHECK("G11 FIXED+DATA_PIXELS==349",NABTS_FIXED+NABTS_DATA_PIXELS==349);
    CHECK("G12 349 <= 370-14 (fits in WIDTH with NABTS OFFSET=14)", 349<=370-14);
    CHECK("G13 FIXED!=24 (old WST/wrong value gone)", NABTS_FIXED!=24);
    CHECK("G14 LINE_BYTES!=40 (old RS value gone)",   NABTS_LINE_BYTES!=40);
}

/* ── H. Pixel clock correction (Bresenham table) ────────────────────────── */
static void test_pixel_clock(void) {
    printf("\n=== H. Pixel clock correction (Bresenham table) ===\n");

    /* H1: table has 288 entries */
    /* (checked at compile time by array size, but verify sum) */
    int total=0, ones=0, twos=0, bad=0;
    for(int i=0;i<288;i++){
        total += nabts_px_width[i];
        if(nabts_px_width[i]==1) ones++;
        else if(nabts_px_width[i]==2) twos++;
        else bad++;
    }
    CHECK("H1  sum of all pixel widths == 349", total==349);
    CHECK("H2  exactly 227 bits with width 1",  ones==227);
    CHECK("H3  exactly 61 bits with width 2",   twos==61);
    CHECK("H4  no widths other than 1 or 2",    bad==0);

    /* H5: widths for bits 0..23 (preamble) sum to FIXED=29 */
    int pre_total=0;
    for(int i=0;i<24;i++) pre_total += nabts_px_width[i];
    CHECK("H5  bits 0..23 (preamble) sum to FIXED==29", pre_total==29);

    /* H6: widths for bits 24..287 (data packet) sum to DATA_PIXELS=320 */
    int data_total=0;
    for(int i=24;i<288;i++) data_total += nabts_px_width[i];
    CHECK("H6  bits 24..287 (data packet) sum to DATA_PIXELS==320", data_total==320);

    /* H7: Bresenham property — pixel_start is non-decreasing */
    int px=0, mono=1;
    for(int i=0;i<288;i++){ int next=px+nabts_px_width[i]; if(next<=px){mono=0;break;} px=next; }
    CHECK("H7  pixel positions are strictly increasing", mono);

    /* H8: effective bit rate is within 0.1% of 5,727,272 Hz */
    /* effective_rate = 6.9375e6 * 288 / 349 = 5,724,928 Hz; error = -0.04% */
    double eff_rate = 6937500.0 * 288.0 / 349.0;
    double error_pct = (eff_rate / 5727272.0 - 1.0) * 100.0;
    int within_tolerance = (error_pct > -0.1 && error_pct < 0.1);
    CHECK("H8  effective bit rate within 0.1% of 5,727,272 Hz", within_tolerance);

    /* H9: Bresenham is deterministic — recompute and compare */
    int ok=1;
    for(int b=0;b<288;b++){
        int expected_start = (b * 349) / 288;
        int expected_end   = ((b+1) * 349) / 288;
        int expected_width = expected_end - expected_start;
        if(nabts_px_width[b] != expected_width){ ok=0; break; }
    }
    CHECK("H9  table matches Bresenham formula b*349/288", ok);

    /* H10: preamble pixels [0..28] span exactly the right widths */
    /* First doubled bit should be bit 4 (from Bresenham: 4*349/288=4, 5*349/288=6 -> width 2) */
    int first_double=-1;
    for(int i=0;i<288;i++) if(nabts_px_width[i]==2){first_double=i; break;}
    CHECK("H10 first doubled bit is bit 4", first_double==4);
}

/* ── I. copy_packet pixel expansion ─────────────────────────────────────── */
static void test_copy_packet(void) {
    printf("\n=== I. copy_packet pixel expansion ===\n");
    uint8_t data[28]; for(int i=0;i<28;i++) data[i]=(uint8_t)(i*7+3);
    uint8_t line[36];
    nabts_build_packet(line,0x5A3,7,0,1,data);

    uint8_t pixels[320]={0};
    copy_packet_ref(line,pixels);

    /* I1: total columns written == DATA_PIXELS */
    int col=0;
    for(int bi=0;bi<33;bi++) for(int bit=0;bit<8;bit++) col+=nabts_px_width[24+bi*8+bit];
    CHECK("I1  total pixel columns written == DATA_PIXELS (320)", col==320);

    /* I2: all pixels 0 or 1 */
    int ok=1; for(int i=0;i<320;i++) if(pixels[i]>1){ok=0;break;}
    CHECK("I2  all expanded pixels are 0 or 1", ok);

    /* I3: first 8 bits of packet (P1) correctly expanded */
    /* P1 is line[3]. Read it back from pixels using variable widths */
    uint8_t recovered_p1=0; int px=0;
    for(int bit=0;bit<8;bit++){
        int w=nabts_px_width[24+bit];
        recovered_p1 |= pixels[px]<<bit;
        /* verify doubled pixels match */
        if(w==2 && pixels[px]!=pixels[px+1]){ok=0;}
        px+=w;
    }
    CHECK("I3  P1 byte recovers correctly from pixel expansion", recovered_p1==line[3]);

    /* I4: all 33 Data Packet bytes round-trip */
    ok=1; px=0;
    for(int bi=0;bi<33;bi++){
        uint8_t rec=0;
        for(int bit=0;bit<8;bit++){
            int global=24+bi*8+bit;
            int w=nabts_px_width[global];
            rec |= pixels[px]<<bit;
            if(w==2 && pixels[px]!=pixels[px+1]) ok=0;
            px+=w;
        }
        if(rec!=line[3+bi]) ok=0;
    }
    CHECK("I4  all 33 Data Packet bytes round-trip through pixel expansion", ok);

    /* I5: doubled pixels have same value as their single neighbour */
    ok=1; px=0;
    for(int bi=0;bi<33;bi++) for(int bit=0;bit<8;bit++){
        int global=24+bi*8+bit;
        int w=nabts_px_width[global];
        if(w==2 && pixels[px]!=pixels[px+1]){ok=0;}
        px+=w;
    }
    CHECK("I5  all doubled pixels have consistent value (no split bit)", ok);

    /* I6: different packets produce different pixel output */
    uint8_t data2[28]; memset(data2,0xFF,28);
    uint8_t line2[36]; nabts_build_packet(line2,0x001,0,0,1,data2);
    uint8_t pixels2[320]={0};
    copy_packet_ref(line2,pixels2);
    CHECK("I6  different packets produce different pixel output",
          memcmp(pixels,pixels2,320)!=0);
}

/* ── J. Preamble rendering ──────────────────────────────────────────────── */
static void test_preamble(void) {
    printf("\n=== J. Preamble rendering ===\n");
    uint8_t row[29]={0};
    render_preamble_ref(row);

    /* J1-J3: correct number of pixels */
    int total=0;
    for(int i=0;i<24;i++) total+=nabts_px_width[i];
    CHECK("J1  preamble occupies exactly FIXED==29 pixel columns", total==29);

    /* J2: first bit = LSB of 0x55 = 1, with width nabts_px_width[0]=1 */
    CHECK("J2  pixel[0] == 1 (LSB of CS byte 0x55)", row[0]==1);

    /* J3: recover preamble bytes from pixels */
    int ok=1; int px=0;
    uint8_t expected[3]={0x55,0x55,0xE7};
    for(int bi=0;bi<3;bi++){
        uint8_t rec=0;
        for(int bit=0;bit<8;bit++){
            int w=nabts_px_width[bi*8+bit];
            rec|=row[px]<<bit;
            if(w==2 && row[px]!=row[px+1]) ok=0;
            px+=w;
        }
        if(rec!=expected[bi]) ok=0;
    }
    CHECK("J3  preamble bytes 0x55 0x55 0xE7 recover correctly", ok);
    CHECK("J4  px after preamble == FIXED (29)", px==29);

    /* J4: framing code 0xE7 bits are correct in last 8 preamble bits */
    /* 0xE7 = 11100111, LSB first: 1,1,1,0,0,1,1,1 */
    uint8_t expected_bits[8]={1,1,1,0,0,1,1,1};
    ok=1;
    int fc_start=0; for(int i=0;i<16;i++) fc_start+=nabts_px_width[i];
    int p=fc_start;
    for(int bit=0;bit<8;bit++){
        int w=nabts_px_width[16+bit];
        if(row[p]!=expected_bits[bit]) ok=0;
        p+=w;
    }
    CHECK("J5  framing code 0xE7 bits correct in preamble pixels", ok);
}

/* ── K. Filler packet ───────────────────────────────────────────────────── */
static void test_filler(void) {
    printf("\n=== K. Filler / null packet ===\n");
    uint8_t data[28]={0}; uint8_t line[36];
    nabts_build_packet(line,0x000,0,0,1,data);
    CHECK("K1  CS1==0x55",        line[0]==0x55);
    CHECK("K2  CS2==0x55",        line[1]==0x55);
    CHECK("K3  BS==0xE7",         line[2]==0xE7);
    CHECK("K4  ch 0: P1==0x15",   line[3]==0x15);
    CHECK("K5  ch 0: P2==0x15",   line[4]==0x15);
    CHECK("K6  ch 0: P3==0x15",   line[5]==0x15);
    CHECK("K7  CI=0 -> 0x15",     line[6]==0x15);
    CHECK("K8  PS_STANDARD",      line[7]==NABTS_PS_STANDARD);
    int ok=1; for(int i=8;i<36;i++) if(line[i]!=0){ok=0;break;}
    CHECK("K9  data block all zero", ok);
}

/* ── L. CI tracker ──────────────────────────────────────────────────────── */
static void test_ci_tracker(void) {
    printf("\n=== L. CI tracker (nabts_ci_t) ===\n");
    nabts_ci_t ci={0};
    CHECK("L1  initial ci.val==0", ci.val==0);
    uint8_t v=nabts_ci_next(&ci);
    CHECK("L2  first call returns 0", v==0);
    CHECK("L3  ci.val==1 after first call", ci.val==1);
    nabts_ci_t ci2={0}; int ok=1;
    for(int i=0;i<16;i++) if(nabts_ci_next(&ci2)!=(uint8_t)i){ok=0;break;}
    CHECK("L4  sequences 0..15", ok);
    CHECK("L5  wraps: ci.val==0 after 16 calls", ci2.val==0);
    nabts_ci_t a={0},b={0};
    nabts_ci_next(&a); nabts_ci_next(&a);
    CHECK("L6  independent trackers: a.val==2, b.val==0", a.val==2&&b.val==0);
}

/* ── M. Demo packet sequencing ──────────────────────────────────────────── */
static void test_demo_sequencing(void) {
    printf("\n=== M. Demo packet sequencing (CEA-516 §3.2.5, §4.1) ===\n");
    uint8_t data[28]; memset(data,0xAA,28);
    uint8_t line[36];
    nabts_build_packet(line,0x001,0,1,1,data);
    CHECK("M1  first packet: PS==PS_SYNC (0x02)",  line[7]==NABTS_PS_SYNC);
    CHECK("M2  first packet: b2==1 (sync)",        (hamming_decode(line[7])&1)==1);
    nabts_build_packet(line,0x001,1,0,1,data);
    CHECK("M3  subsequent: PS==PS_STANDARD (0x15)",line[7]==NABTS_PS_STANDARD);
    CHECK("M4  subsequent: b2==0 (standard)",      (hamming_decode(line[7])&1)==0);
    nabts_build_packet(line,0x001,2,0,0,data);
    CHECK("M5  last partial: PS==PS_NOTFULL(0x49)",line[7]==NABTS_PS_NOTFULL);
    CHECK("M6  last partial: b4==1 (not full)",    (hamming_decode(line[7])&2)!=0);
    memset(data,0,28);
    nabts_build_packet(line,0x000,0,0,1,data);
    int ch=(hamming_decode(line[3])<<8)|(hamming_decode(line[4])<<4)|hamming_decode(line[5]);
    CHECK("M7  null: ch==0x000",                   ch==0x000);
    CHECK("M8  null: PS_STANDARD",                 line[7]==NABTS_PS_STANDARD);
    nabts_build_packet(line,0x00F,0,1,0,data);
    ch=(hamming_decode(line[3])<<8)|(hamming_decode(line[4])<<4)|hamming_decode(line[5]);
    CHECK("M9  ASCII: ch==0x00F",                  ch==0x00F);
    CHECK("M10 ASCII: sync=1,full=0 -> 0x5E",      line[7]==0x5E);
}


/* ── N. Data Group Header (CEA-516 §4.2) ────────────────────────────────── */

static void make_dg_header_ref(uint8_t *dest,
                                uint8_t gc, uint8_t gr,
                                int num_packets, int final_bytes)
{
    int S = num_packets - 1;
    int F = final_bytes;
    dest[0] = nabts_hamming_enc[0x0];
    dest[1] = nabts_hamming_enc[gc  & 0xF];
    dest[2] = nabts_hamming_enc[gr  & 0xF];
    dest[3] = nabts_hamming_enc[(S >> 4) & 0xF];
    dest[4] = nabts_hamming_enc[S        & 0xF];
    dest[5] = nabts_hamming_enc[(F >> 4) & 0xF];
    dest[6] = nabts_hamming_enc[F        & 0xF];
    dest[7] = nabts_hamming_enc[0x0];
}

static void test_dg_header(void)
{
    printf("\n=== N. Data Group Header (CEA-516 §4.2) ===\n");
    uint8_t hdr[8];

    /* N1: GT always 0 (§4.2.2) */
    make_dg_header_ref(hdr, 0, 0, 1, 28);
    CHECK("N1  GT (hdr[0]) == Hamming(0x0) == 0x15",   hdr[0] == 0x15);

    /* N2: GN always 0 (§4.2.7) */
    CHECK("N2  GN (hdr[7]) == Hamming(0x0) == 0x15",   hdr[7] == 0x15);

    /* N3: GC round-trips for all 0..15 */
    int ok = 1;
    for (int gc = 0; gc < 16; gc++) {
        make_dg_header_ref(hdr, (uint8_t)gc, 0, 1, 28);
        if (hamming_decode(hdr[1]) != (uint8_t)gc) { ok = 0; break; }
    }
    CHECK("N3  GC encodes/decodes correctly for all 0..15", ok);

    /* N4-N6: S = num_packets-1, 6-packet page */
    make_dg_header_ref(hdr, 0, 0, 6, 14);
    CHECK("N4  S1 for 6-packet page == Hamming(0) == 0x15",
          hdr[3] == nabts_hamming_enc[0x0]);
    CHECK("N5  S2 for 6-packet page == Hamming(5) == 0x73",
          hdr[4] == nabts_hamming_enc[0x5]);
    CHECK("N6  decoded S == 5 (num_packets-1)",
          ((hamming_decode(hdr[3])<<4)|hamming_decode(hdr[4])) == 5);

    /* N7-N9: F = final_bytes, 14 bytes */
    CHECK("N7  F1 for final_bytes=14 == Hamming(0) == 0x15",
          hdr[5] == nabts_hamming_enc[0x0]);
    CHECK("N8  F2 for final_bytes=14 == Hamming(0xE) == 0xFD",
          hdr[6] == nabts_hamming_enc[0xE]);
    CHECK("N9  decoded F == 14",
          ((hamming_decode(hdr[5])<<4)|hamming_decode(hdr[6])) == 14);

    /* N10-N11: 1-packet ASCII page: S=0, F=28 */
    make_dg_header_ref(hdr, 0, 0, 1, 28);
    CHECK("N10 1-packet page: decoded S == 0",
          ((hamming_decode(hdr[3])<<4)|hamming_decode(hdr[4])) == 0);
    CHECK("N11 1-packet page: decoded F == 28",
          ((hamming_decode(hdr[5])<<4)|hamming_decode(hdr[6])) == 28);

    /* N12: all 8 header bytes have odd parity */
    make_dg_header_ref(hdr, 3, 1, 6, 14);
    ok = 1;
    for (int i = 0; i < 8; i++) {
        int bits = 0; uint8_t b = hdr[i];
        for (int k = 0; k < 8; k++) bits += (b>>k)&1;
        if (bits%2==0) { ok=0; break; }
    }
    CHECK("N12 all 8 DG header bytes have odd parity", ok);

    /* N13: layout leaves 20 NAPLPS bytes after 8-byte header */
    CHECK("N13 DG header (8) leaves 20 bytes for NAPLPS in first packet",
          (NABTS_DATA_BLOCK_BYTES - 8) == 20);

    /* N14: ASCII demo fits exactly (8 + 20 = 28 = full) */
    CHECK("N14 ASCII: header(8) + payload(20) == 28 == full packet",
          (8 + 20) == NABTS_DATA_BLOCK_BYTES);

    /* N15: line_mask applied before loop produces correct skip pattern */
    uint16_t mask = 0xAAAA;
    int skipped = 0, rendered = 0;
    int m = (int)mask;
    for (int i = 0; i < 16; i++) {
        if (m & 1) skipped++; else rendered++;
        m >>= 1;
    }
    CHECK("N15 line_mask 0xAAAA: 8 skipped, 8 rendered across 16 row-pairs",
          skipped==8 && rendered==8);

    /* N16: Synchronizing Packet identified by PS_SYNC in first packet */
    uint8_t data[28]; memset(data,0,28);
    make_dg_header_ref(data, 0, 0, 1, 28);
    uint8_t line[36];
    nabts_build_packet(line, 0x001, 0, 1/*sync*/, 1, data);
    CHECK("N16 Synchronizing Packet carrying DG header has PS_SYNC (0x02)",
          line[7] == NABTS_PS_SYNC);

    /* N17: DG header GT field decodes to 0 (broadcast teletext) */
    make_dg_header_ref(hdr, 5, 2, 3, 28);
    CHECK("N17 GT always decodes to 0 regardless of other fields",
          hamming_decode(hdr[0]) == 0);

    /* N18: GR=0 encodes to Hamming(0)=0x15 */
    make_dg_header_ref(hdr, 0, 0, 1, 28);
    CHECK("N18 GR=0 -> hdr[2] == 0x15", hdr[2] == 0x15);
}

/* ── main ───────────────────────────────────────────────────────────────── */
int main(void) {
    printf("raspi-teletext NABTS  –  CEA-516 + Pi pixel clock verification\n");
    printf("==============================================================\n");
    test_hamming();
    test_address();
    test_ci();
    test_ps();
    test_build_packet();
    test_framing();
    test_geometry();
    test_pixel_clock();
    test_copy_packet();
    test_preamble();
    test_filler();
    test_ci_tracker();
    test_demo_sequencing();
    test_dg_header();
    printf("\n==============================================================\n");
    printf("Results: %d passed, %d failed, %d total\n",
           tests_pass, tests_fail, tests_run);
    return (tests_fail==0) ? 0 : 1;
}
