# raspi-teletext — NABTS fork (CEA-516 compliant)

NABTS (North American Basic Teletext Specification) output on Raspberry Pi
composite video, targeting NTSC 525-line / 59.94 Hz systems.

Standard: **CEA-516 / EIA-516** (May 1988)

---

## Wire format (CEA-516 §2, §3, Figure 6)

Each VBI Data Line = **288 bits = 36 bytes**, transmitted LSB-first:

```
Byte  0     0x55   CS1  Clock Synchronization (§2.2.2)
Byte  1     0x55   CS2  Clock Synchronization
Byte  2     0xE7   BS   Framing Code (§2.2.3)
Byte  3     P1     Hamming(channel[11:8])   ┐
Byte  4     P2     Hamming(channel[7:4])    ├ Packet Prefix (§3.2)
Byte  5     P3     Hamming(channel[3:0])    │  P1–P3: 12-bit Data Channel
Byte  6     CI     Hamming(continuity 0–15) │  CI:    Continuity Index
Byte  7     PS     Hamming(packet structure)┘  PS:    sync/full/suffix flags
Bytes 8–35  [28 bytes]  Data Block (§3.3)
```

All five Packet Prefix bytes are **Hamming-encoded** per Figure 7, providing
single-bit error correction and double-bit error detection on the header.

Signal levels (§1.6): Logic 1 = **70 ± 2 IRE**, Logic 0 = 0 ± 2 IRE.
Bit rate (§1.3): **5,727,272 bps**.
VBI lines (§1.1.1): lines **10–21**, both fields.

---

## What changed from upstream raspi-teletext

| Parameter | raspi-teletext (WST) | This fork (NABTS CEA-516) |
|---|---|---|
| Standard | EIA / CCIR System B | CEA-516 |
| CS bytes | 2 × 0x55 | 2 × 0x55 (same) |
| Framing code | 0x27 | **0xE7** |
| Packet Prefix | WST magazine/row | **P1 P2 P3 CI PS — Hamming-encoded** |
| Packet size | 42 bytes | **36 bytes** |
| FIXED preamble | 24 pixels | **24 pixels** (unchanged) |
| Data Block | 42 bytes | **28 bytes** |
| Error correction | Hamming per-byte | **Hamming prefix + optional LPC suffix** |
| Data Channel bits | — | **12-bit address (4096 channels)** |
| Pixel total per line | 336 | **288** |

---

## Hardware setup

1. Raspberry Pi with composite output (Zero 2W recommended — ~$15)
2. Composite RCA cable from Pi 3.5mm TRRS header or test pads
3. NABTS decoder (e.g. Norpak TTX-650) connected to the same composite signal

---

## Software setup

### 1. `/boot/config.txt` — force NTSC composite

```
sdtv_mode=0
sdtv_aspect=1
enable_tvout=1
```

Reboot with HDMI cable unplugged.

### 2. Shift the framebuffer into the VBI

```bash
sudo ./tvctl on
```

The top rows of the display shift up into the blanking interval — correct.

### 3. Build

```bash
make
```

### 4. Run

```bash
# NAPLPS graphics demo (colour bars, bouncing box, frame counter)
sudo ./teletext

# ASCII identification packets — start here to verify lock
sudo ./teletext -d ascii

# Feed raw 36-byte packets from another process
sudo your-encoder | sudo ./teletext -
```

Stdin format: raw 36-byte Data Lines back-to-back, no framing:
```
[0x55][0x55][0xE7][P1][P2][P3][CI][PS][28 data bytes]
```
Use `nabts_build_packet()` from `nabts.h` to construct them.

---

## Packet construction (nabts.h)

```c
#include "nabts.h"

uint8_t line[NABTS_LINE_BYTES];   /* 36 bytes */
uint8_t data[NABTS_DATA_BLOCK_BYTES] = {0};  /* 28 bytes */

nabts_ci_t ci = {0};              /* one per channel */

/* Synchronizing Packet — first packet of a Data Group */
nabts_build_packet(line,
    0x001,                /* Data Channel 0x001        */
    nabts_ci_next(&ci),   /* Continuity Index          */
    1,                    /* sync = 1 (§4.1)           */
    1,                    /* full = 1 (data block full)*/
    data);

push_packet(line);        /* or write(fd, line, 36)    */

/* Standard Packet — subsequent packets in the same Data Group */
nabts_build_packet(line, 0x001, nabts_ci_next(&ci), 0, 1, data);
```

### PS flag summary (§3.2.5)

| sync | full | PS byte | Meaning |
|------|------|---------|---------|
| 1 | 1 | 0x02 | Synchronizing Packet, Data Block full |
| 0 | 1 | 0x15 | Standard Packet, Data Block full |
| 0 | 0 | 0x49 | Standard Packet, Data Block not full (last in group) |
| 1 | 0 | 0x5E | Synchronizing Packet, Data Block not full |

### Data Channel allocation (§7.1.5)

| Channel | Reserved meaning |
|---------|-----------------|
| 0x000 | Master Index Page / null packets |
| 0xA00 | Start of captioning |
| 0xB00 | Start of Flash |
| 0xFFF (any) | Support Record |

---

## Verification

```bash
# On a Linux host (no Pi required):
gcc -std=c11 -o verify_spec verify_spec.c && ./verify_spec
# Expected: 105 passed, 0 failed
```

---

## VBI line assignment (NTSC)

CEA-516 §1.1.1 specifies lines 10–21, both fields.
The Pi NTSC_ON register state maps framebuffer rows to approximately
lines 10–17 field 1 and 272–279 field 2, giving 8 usable lines per field
= 16 total = ~480 packets/second at 30 fps.

Use `-m` / `-o` to mask specific lines if needed:
```bash
sudo ./teletext -m 0xFFF0 -o 0x0FFF
```
