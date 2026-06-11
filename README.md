# raspi-teletext — NABTS fork

NABTS (North American Basic Teletext Specification) output on Raspberry Pi
composite video, targeting NTSC 525-line / 59.94 Hz systems.

**Standard:** CEA-516 / EIA-516, May 1988

This software generates a NABTS teletext signal entirely in software.
No hardware modifications are needed beyond a composite cable.

---

## Hardware requirements

- Raspberry Pi with composite video output (Pi Zero 2W recommended — ~$15)
- Composite RCA cable from the Pi's 3.5 mm TRRS header or test pads
- NABTS decoder connected to the same composite signal (e.g. Norpak TTX-650)
- At least 32 MB of GPU memory (Raspbian default is sufficient)

---

## Software setup

### 1. Force NTSC composite in `/boot/config.txt`

```
sdtv_mode=0
sdtv_aspect=1
enable_tvout=1
```

Reboot with the HDMI cable unplugged.

### 2. Prerequisites

Tested with Raspbian 12 (Raspbian 13 was reported to not work in the upstream project. We need the following libraries for our build process.

```bash
sudo apt install libraspberrypi-dev raspi-gpio
```

### 3. Build

```bash
make
```

Requires the Raspberry Pi userland SDK (`/opt/vc`) for `bcm_host` and
`dispmanx`. Build on the Pi itself or cross-compile with `SDKSTAGE` pointing
at a sysroot containing the SDK.

### 4. Shift the framebuffer into the VBI

```bash
sudo ./tvctl on
```

This writes to the Pi VEC registers to shift the display output upward into
the vertical blanking interval. You should see the top rows of the display
move above the visible picture — that is correct. Run `sudo ./tvctl off` to
restore normal output.

`tvctl` checks the register state before acting; it will not proceed if
the registers are in an unknown state.

### 5. Run

```bash
# NAPLPS graphics demo — colour bars, bouncing box, frame counter (default)
sudo ./teletext

# ASCII identification packets — start here to verify decoder lock
sudo ./teletext -d ascii

# Feed raw 36-byte NABTS Data Lines from an external encoder
your-encoder | sudo ./teletext -
```

---

## Command-line reference

```
sudo ./teletext [-l level] [-m even-mask] [-o odd-mask] [-d mode] [-]
```

| Flag | Argument | Description |
|------|----------|-------------|
| `-l` | 0–100 | White level (brightness of logic-1 bits). Default: 100 |
| `-m` | 16-bit hex | Line mask for even fields. Bit 0 = first VBI row, bit 11 = last. 1 = skip, 0 = transmit |
| `-o` | 16-bit hex | Line mask for odd fields. If only `-m` is given, the same mask applies to both fields |
| `-d` | `ascii` or `graphics` | Demo mode. Default: `graphics` |
| `-` | — | Read raw 36-byte NABTS Data Lines from stdin instead of running the demo |

**Line mask example** — transmit on the first four lines of even fields and
the last four lines of odd fields:

```bash
sudo ./teletext -m 0xFFF0 -o 0x0FFF
```

---

## Stdin packet format

When running with `-`, the program reads raw binary NABTS Data Lines from
stdin, one packet per read. Each packet is exactly **36 bytes**:

```
[0x55][0x55][0xE7][P1][P2][P3][CI][PS][28 data bytes]
```

Packets are transmitted once each in the order received. Send endlessly to
maintain a live signal. Use `nabts_build_packet()` from `nabts.h` to
construct packets; see the API section below.

> **Note for users of the upstream raspi-teletext WST branch:** the WST
> packet format is 42 bytes. NABTS packets are 36 bytes. The two formats are
> not interchangeable.

---

## Wire format (CEA-516 §2–3, Figure 6)

Each VBI Data Line is 288 bits = 36 bytes, transmitted LSB-first at NRZ:

```
Byte  0     0x55   CS1  Clock Synchronization (§2.2.2)
Byte  1     0x55   CS2  Clock Synchronization
Byte  2     0xE7   BS   Framing Code (§2.2.3)
Byte  3     P1     Hamming(channel[11:8])    ┐
Byte  4     P2     Hamming(channel[7:4])     ├ Packet Prefix (§3.2)
Byte  5     P3     Hamming(channel[3:0])     │  P1–P3: 12-bit Data Channel (0x000–0xFFF)
Byte  6     CI     Hamming(continuity 0–15)  │  CI:    Continuity Index
Byte  7     PS     Hamming(packet structure) ┘  PS:    sync / full / suffix flags
Bytes 8–35  [28 bytes]  Data Block (§3.3)
```

All five Packet Prefix bytes are Hamming-encoded (Figure 7), giving
single-bit error correction and double-bit error detection on the header.
All Data Block bytes carry odd parity in bit 7 (§3.3).

**Signal levels (§1.6):** Logic 1 = 70 ± 2 IRE, Logic 0 = 0 ± 2 IRE (blanking).  
**Bit rate (§1.3):** 5,727,272 bps nominal (see *Pixel clock* below).  
**VBI lines (§1.1.1):** lines 10–21, both fields (12 lines per field).

### Differences from upstream WST (raspi-teletext)

| Parameter | raspi-teletext (WST) | This fork (NABTS CEA-516) |
|---|---|---|
| Standard | EIA / CCIR System B | CEA-516 |
| Framing code | 0x27 | **0xE7** |
| Packet Prefix layout | WST magazine/row | **P1 P2 P3 CI PS — all Hamming-encoded** |
| Packet size (bytes) | 42 | **36** |
| Data Block (bytes) | 40 | **28** |
| Data Channel bits | — | **12 bits (4096 channels)** |
| Pixel total per line | 336 | **349** (Bresenham clock-corrected) |
| VBI rows used | 8/field | **12/field (lines 10–21)** |

---

## Packet construction API (nabts.h)

```c
#include "nabts.h"

uint8_t line[NABTS_LINE_BYTES];          /* 36 bytes */
uint8_t data[NABTS_DATA_BLOCK_BYTES];    /* 28 bytes */
memset(data, 0, sizeof(data));

nabts_ci_t ci = {0};    /* one nabts_ci_t per Data Channel */

/* First packet of a Data Group (Synchronizing Packet, §4.1) */
nabts_build_packet(line,
    0x001,                /* Data Channel                    */
    nabts_ci_next(&ci),   /* Continuity Index (auto-managed) */
    1,                    /* sync = 1                        */
    1,                    /* full = 1 (Data Block fully used)*/
    data);

/* Subsequent packets in the same Data Group */
nabts_build_packet(line, 0x001, nabts_ci_next(&ci), 0, 1, data);

/* Last packet when Data Block is not full */
nabts_build_packet(line, 0x001, nabts_ci_next(&ci), 0, 0, data);
```

### PS byte values (§3.2.5)

| sync | full | PS byte | Meaning |
|:----:|:----:|:-------:|---------|
| 1 | 1 | `0x02` | Synchronizing Packet, Data Block full |
| 0 | 1 | `0x15` | Standard Packet, Data Block full |
| 0 | 0 | `0x49` | Standard Packet, Data Block not full (last packet in group) |
| 1 | 0 | `0x5E` | Synchronizing Packet, Data Block not full |

### Reserved Data Channel assignments (§7.1.5)

| Channel | Meaning |
|---------|---------|
| `0x000` | Master Index Page; null/filler packets |
| `0xA00` | Entry point for captioning |
| `0xB00` | Entry point for Flash service |
| any `0xFFF` | Support Record |

### FSS Data Group size limit (§8.4.2.5)

The Fundamental Service Specification caps a Data Group at **68 packets**
(S ≤ 67). With no suffix and a 28-byte Data Block the maximum NAPLPS payload
per Data Group is:

```
First packet:   28 − 8 (DG header) = 20 bytes
Packets 2–68:   67 × 28            = 1876 bytes
Total:                               1896 bytes  (NABTS_FSS_MAX_NAPLPS)
```

`push_page()` in `demo.c` enforces this limit and logs to stderr if a page
is truncated.

---

## Pixel clock

The Pi VEC outputs at 13.5 MHz and dispmanx stretches `WIDTH=370` source
pixels to 720 output pixels, giving a source pixel rate of **6.9375 MHz** —
21 % faster than the NABTS bit rate.

A Bresenham bit-expansion table (`nabts_px_width[]` in `nabts.h`) spreads
288 NABTS bits across **349 source pixels** (227 bits at 1 pixel wide, 61 at
2 pixels wide), yielding an effective bit rate of **5,724,928 Hz**
(−2,344 Hz from the §1.3 nominal of 5,727,272 Hz).

The §1.3 tolerance of ±16 Hz is derived from PLL-locking to the NTSC colour
subcarrier (§1.3 note). The Pi VEC is not subcarrier-locked in software
composite mode, so this absolute tolerance is structurally unachievable.
N = 349 is the closest integer pixel count at WIDTH = 370; no integer value
meets ±16 Hz. NABTS decoder PLLs in practice have capture ranges orders of
magnitude wider than 16 Hz and lock reliably on this signal.

---

## Verification

The test suite runs on any Linux host — no Pi required:

```bash
gcc -std=c11 -o verify_spec verify_spec.c hamming.c && ./verify_spec
# Expected: 126 passed, 0 failed

gcc -std=c11 -o test_fixes test_fixes.c hamming.c && ./test_fixes
# Expected: 23 passed, 0 failed
```

---

## CEA-608 closed captions (upstream feature)

The upstream `cea608` binary is retained for NTSC closed-caption output
(EIA-608 / CEA-608):

```bash
# Demo
./cea608

# Feed raw two-byte field pairs from a capture tool
./cea608 -
```

Data format: binary, two parity-encoded bytes per field at 59.97 fields/sec.
See [EIA-608 on Wikipedia](https://en.wikipedia.org/wiki/EIA-608) for
protocol details.

---

## Credits

Original raspi-teletext by [Alistair Buxton](https://github.com/ali1234).  
NABTS fork by [intermod-cable](https://github.com/intermod-cable/raspi-teletext).
