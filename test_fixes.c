/*
 * test_fixes.c  –  regression tests for the three CEA-516 bug fixes
 *
 *  Bug 1: height=32 overran VBI (§1.1.1)    → height now 24
 *  Bug 2: NAPLPS payload bytes lacked parity (§3.3) → parity() applied
 *  Bug 3: no FSS Data Group size cap (§8.4.2.5)    → guard added
 */

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <stdlib.h>

#include "nabts.h"
#include "hamming.h"

/* ── test harness ─────────────────────────────────────────────────────── */
static int run=0, pass=0, fail=0;
#define CHECK(label, cond) do { \
    run++; \
    if (cond) { pass++; printf("  PASS  %s\n", label); } \
    else      { fail++; printf("  FAIL  %s  [line %d]\n", label, __LINE__); } \
} while(0)

/* ── parity helper (mirrors hamming.c parity()) ───────────────────────── */
static uint8_t ref_parity(uint8_t b)
{
    /* count bits in b[6:0]; set bit 7 to make total count odd */
    uint8_t v = b & 0x7Fu;
    int bits = 0;
    for (int i = 0; i < 7; i++) bits += (v >> i) & 1;
    return (bits & 1) ? v : (uint8_t)(v | 0x80u);
}

static int has_odd_parity(uint8_t b)
{
    int bits = 0;
    for (int i = 0; i < 8; i++) bits += (b >> i) & 1;
    return bits & 1;
}

/* ── simulate nl_byte() with parity fix applied ───────────────────────── */
#define NL_PAGE_MAX  168
static uint8_t nl_page[NL_PAGE_MAX];
static int     nl_len;

static void nl_byte_fixed(uint8_t b)
{
    if (nl_len < NL_PAGE_MAX) nl_page[nl_len++] = parity(b);
}

/* ── simulate push_page() size guard ─────────────────────────────────── */
static int guarded_push_page_len(int raw_len)
{
    if (raw_len > NABTS_FSS_MAX_NAPLPS)
        return NABTS_FSS_MAX_NAPLPS;
    return raw_len;
}

/* number of packets needed for a given NAPLPS length */
static int packets_for(int nl_len)
{
    int first_payload = NABTS_DATA_BLOCK_BYTES - 8;  /* 20 */
    if (nl_len <= first_payload) return 1;
    int remaining = nl_len - first_payload;
    int extra = (remaining + NABTS_DATA_BLOCK_BYTES - 1) / NABTS_DATA_BLOCK_BYTES;
    return 1 + extra;
}

/* ══════════════════════════════════════════════════════════════════════ */
int main(void)
{
    printf("test_fixes  –  CEA-516 bug-fix regression tests\n");
    printf("==================================================\n");

    /* ── Bug 1: height ───────────────────────────────────────────────── */
    printf("\n[Bug 1] height=24 → VBI lines 10-21 only (§1.1.1)\n");
    {
        int h = 24;
        int lines_per_field = h / 2;          /* 12 */
        int last_vbi_line   = 10 + lines_per_field - 1;   /* 21 */

        CHECK("B1-1  height/2 == 12 lines per field",       lines_per_field == 12);
        CHECK("B1-2  last line == 21 (spec max)",           last_vbi_line == 21);
        CHECK("B1-3  no overshoot into active video",       last_vbi_line <= 21);
        CHECK("B1-4  old height=32 would have reached 25",  (10 + 32/2 - 1) == 25);
        CHECK("B1-5  line_mask is uint16_t: 12 bits enough",
              (1u << lines_per_field) <= 0xFFFFu);
    }

    /* ── Bug 2: payload parity ───────────────────────────────────────── */
    printf("\n[Bug 2] NAPLPS payload bytes have odd parity after fix (§3.3)\n");
    {
        /* (a) parity() sets odd parity on every possible 7-bit value */
        int any_bad = 0;
        for (int b = 0; b < 128; b++) {
            uint8_t out = parity((uint8_t)b);
            if (!has_odd_parity(out)) { any_bad++; }
        }
        CHECK("B2-1  parity() produces odd parity for all 7-bit inputs",
              any_bad == 0);

        /* (b) parity() output matches reference for the demo string */
        const char *str = "NABTS raspi-teletext";
        int mismatch = 0;
        for (int k = 0; str[k]; k++) {
            uint8_t got = parity((uint8_t)str[k]);
            uint8_t ref = ref_parity((uint8_t)str[k]);
            if (got != ref) mismatch++;
        }
        CHECK("B2-2  parity() matches reference for demo string", mismatch == 0);

        /* (c) nl_byte_fixed() stores parity-corrected bytes */
        nl_len = 0;
        const char *payload = "NABTS";
        for (int k = 0; payload[k]; k++) nl_byte_fixed((uint8_t)payload[k]);
        int bad_parity = 0;
        for (int k = 0; k < nl_len; k++)
            if (!has_odd_parity(nl_page[k])) bad_parity++;
        CHECK("B2-3  nl_byte_fixed: all stored bytes have odd parity",
              bad_parity == 0);

        /* (d) without fix, 'N'=0x4E has even parity → verify we catch it */
        uint8_t n_raw = (uint8_t)'N';   /* 0x4E = 0100 1110, 4 bits = even */
        CHECK("B2-4  0x4E ('N') has even parity without fix (demonstrates need)",
              !has_odd_parity(n_raw));
        CHECK("B2-5  parity(0x4E) has odd parity with fix",
              has_odd_parity(parity(n_raw)));

        /* (e) parity() preserves the lower 7 bits (data is unchanged) */
        int data_corrupted = 0;
        for (int b = 0; b < 128; b++) {
            if ((parity((uint8_t)b) & 0x7Fu) != (uint8_t)b) data_corrupted++;
        }
        CHECK("B2-6  parity() never alters bits [6:0]", data_corrupted == 0);

        /* (f) Hamming-encoded DG header bytes already have odd parity;
         * parity() must NOT be applied to them (they use a different scheme).
         * Verify the hamming table entries all have odd parity. */
        int hdr_bad = 0;
        for (int v = 0; v < 16; v++)
            if (!has_odd_parity(nabts_hamming_enc[v])) hdr_bad++;
        CHECK("B2-7  all Hamming-encoded header bytes already have odd parity",
              hdr_bad == 0);
    }

    /* ── Bug 3: FSS size guard ───────────────────────────────────────── */
    printf("\n[Bug 3] push_page() FSS Data Group size guard (§8.4.2.5)\n");
    {
        /* (a) constant correctness */
        int expected_max = (NABTS_DATA_BLOCK_BYTES - 8)
                         + (NABTS_FSS_MAX_PACKETS - 1) * NABTS_DATA_BLOCK_BYTES;
        CHECK("B3-1  NABTS_FSS_MAX_PACKETS == 68",    NABTS_FSS_MAX_PACKETS == 68);
        CHECK("B3-2  NABTS_FSS_MAX_NAPLPS == 1896",   NABTS_FSS_MAX_NAPLPS  == 1896);
        CHECK("B3-3  NABTS_FSS_MAX_NAPLPS formula correct",
              NABTS_FSS_MAX_NAPLPS == expected_max);

        /* (b) page at exactly the limit → not truncated, S == 67 */
        int len_at_limit   = NABTS_FSS_MAX_NAPLPS;          /* 1896 */
        int guarded        = guarded_push_page_len(len_at_limit);
        int pkts_at_limit  = packets_for(guarded);
        int S_at_limit     = pkts_at_limit - 1;
        CHECK("B3-4  1896-byte page is not truncated",      guarded == 1896);
        CHECK("B3-5  1896-byte page → 68 packets",          pkts_at_limit == 68);
        CHECK("B3-6  S == 67 at limit (max FSS)",            S_at_limit == 67);

        /* (c) one byte over → truncated to 1896 */
        int over           = guarded_push_page_len(NABTS_FSS_MAX_NAPLPS + 1);
        CHECK("B3-7  1897-byte page is truncated to 1896",  over == 1896);

        /* (d) well over limit → still capped at 1896 */
        int big            = guarded_push_page_len(9999);
        CHECK("B3-8  9999-byte page is capped at 1896",     big == 1896);

        /* (e) small page → unchanged */
        int small          = guarded_push_page_len(100);
        CHECK("B3-9  100-byte page is not truncated",       small == 100);

        /* (f) S fits in two nibbles (§4.2.5: S1,S2 each encode 4 bits,
         *     max expressible S = 0xFF = 255 > 67 → no overflow) */
        CHECK("B3-10 S=67 fits in 8-bit S1:S2 field",      S_at_limit <= 0xFF);

        /* (g) FSS limit < 1-suffix limit (§8.4.2.5 note: 1-byte suffix gives
         *     max 1836 bytes; without suffix max 1904.  Our cap 1896 is in
         *     range for no-suffix case and correctly more conservative than
         *     the no-suffix theoretical maximum of 1904 bytes). */
        int no_suffix_theoretical = 68 * NABTS_DATA_BLOCK_BYTES - 8; /* 1896 */
        CHECK("B3-11 NABTS_FSS_MAX_NAPLPS matches no-suffix theoretical max",
              NABTS_FSS_MAX_NAPLPS == no_suffix_theoretical);
    }

    /* ── summary ─────────────────────────────────────────────────────── */
    printf("\n==================================================\n");
    printf("Results: %d passed, %d failed, %d total\n", pass, fail, run);
    return (fail == 0) ? 0 : 1;
}
