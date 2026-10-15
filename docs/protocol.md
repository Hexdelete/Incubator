# Console and board protocol

A single data wire carries both directions between the front console and the control board. It idles high at about 3.3 V. The connector has three pins: 3.3 V power, GND and DATA. The console starts every exchange; the board answers.

All timings are in microseconds and come from captures made with an Arduino Nano. They repeat to within a few microseconds.

## Console to board (observed)

The console sends one frame about every 178 ms. A frame lasts about 42 ms.

| Part | Timing |
|---|---|
| Header | low ~1616, high ~2480, low ~1872, high ~500 |
| Each data bit | low ~376 (bit 0) or ~1372 (bit 1), then a high mark ~616 |
| Mark after bit 7 (the 8th bit) | ~740 instead of ~616 |
| Trailer | high ~6192 after the last bit, then low ~5968, then idle |

There are 16 data bits, sent LSB first, forming two bytes. There is no checksum.

| Byte | Meaning |
|---|---|
| B1 | top-zone setpoint in F (54-66 observed) |
| B2 bits 0-6 | bottom-zone setpoint in F (42-66 per the console's range; minimum unverified) |
| B2 bit 7 | light flag; latched, each press of the light button toggles it |

Examples: B1 = 54, B2 = 56 is top 54, bottom 56, light off. B2 = 182 is bottom 54 with the light on.

Nominal timings (the values above) are accepted by the original board just as well as exact copies of the captured frames.

## Board to console (observed)

The original board replies after every console frame, starting about 0.2 ms after the console's trailer ends. The reply frame is about 46 ms long.

| Part | Timing |
|---|---|
| Header | low ~1820, high ~2860, low ~8490, high ~478 |
| Each data bit | a low, then a high mark ~478 |
| Bit 1 low | ~1524 |
| 8th and 16th lows | about 100 longer than normal (~1620 for a 1) |
| Bit 0 low | not yet observed; assumed ~522 (a 1 ms bit period, like the console's) |
| After the last bit | the line is released and stays high |

Two tiny pulses (about 92 low, then 84-140 high) appeared right before the header in the captures. They may be the original board's output switching on, and are not reproduced by the firmware.

### Reply contents

- The reply carries two bytes. B1 follows the top sensor input.
- With the sensor inputs open, both bytes read 255.
- With a 10 kOhm resistor on the top sensor input, B1 read 115. With 1 kOhm it read lower still (value not recorded).
- A good fit so far is a raw divider reading, `B = 255 * R / (R + ~12 kOhm)`, which suggests a pull-up of about 12 kOhm on the board. Open reads 255, and the byte rises with resistance, so a warmer NTC gives a lower number.
- The reply did not change with the console frame's content (tested 54/56, 54/184, 60/60 and 66/66).

### Untested

- The second reply byte (assumed to be the bottom sensor).
- How the console converts the raw byte into the displayed temperature.
- Whether the console accepts the Nano's reply (the 0-bit low, the reply gap and the missing start pulses are guesses).

## Capturing more

`tools/console_standin` plays the console toward the original board. Use `s B1 B2` to send one frame and see the decoded reply with every bit's low duration, and `w B1 B2` to watch the reply change while you vary a sensor input. Substitute resistors for the thermistors to map resistance to reply bytes, and look for a short low (a 0 bit) to confirm the bit-0 timing.
