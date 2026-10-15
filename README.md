# Incubator

Turns a Haier HVTEC12DABS dual-zone thermoelectric wine cooler into a two-zone incubator (yogurt, kombucha and similar) by replacing its control board with an Arduino Nano. The original front console stays: it still sends the set temperatures and the light button, and the Nano answers it the way the original board did.

> **Status: first-run firmware.** The console protocol was reverse-engineered from captures. The reply format has not yet been tested against the real console, and the thermistor handling needs calibration. Read [Known unknowns](#known-unknowns) before connecting the Peltiers.

## How it works

- The Nano listens to the console on a single 3.3 V data wire and decodes its 16-bit frames: top-zone setpoint, bottom-zone setpoint and the light flag.
- It answers each frame in the original board's format, reporting the two thermistor readings as raw 8-bit values.
- Each console setpoint gets a fixed offset, so the cooler's 12-18 C range becomes an incubator range:

| Zone | Console range | Offset | Target range | Intended use |
|---|---|---|---|---|
| Upper | 54-66 F | +50 F | 104-116 F | yogurt |
| Lower | 42-66 F (minimum not yet verified) | +25 F | 67-91 F | kombucha |

- Each Peltier is wired in reverse so it heats, and is switched by a MOSFET module with PWM (about 3.9 kHz) on D9 / D10. Control is proportional: full power about 4 F below target.
- The interior LED follows the console's light button.
- Fans are wired straight to the supply and run whenever power is on.

## Hardware

- Arduino Nano (ATmega328P)
- 12 V DC supply, set to 10.5 V
- 3 x MOSFET modules (IRF520-type, Vetco VUPN1246): upper Peltier, lower Peltier, LED
- 5 V buck converter and a small 3.3 V regulator (for the console)
- 2 x thermistors (two-wire NTC; polarity does not matter)
- Resistors: 2 x 12 kOhm 1% (thermistor dividers), 1 x 470 Ohm (data line), 1 x 10 kOhm pull-up if the console line needs one, 10 kOhm pull-downs on MOSFET signal inputs if the modules lack them
- Fuses, a normally-closed bimetal thermostat for the Peltier feed, heatsinks for the MOSFETs
- The cooler's original Peltiers (reversed), fans and LED

Full wiring: [docs/wiring.md](docs/wiring.md).

## Pin map

| Nano pin | Connects to |
|---|---|
| D2 | console data wire (470 Ohm in series, 10 kOhm pull-up to 3.3 V if needed) |
| D4 | LED MOSFET module signal |
| D9 | upper Peltier MOSFET module signal |
| D10 | lower Peltier MOSFET module signal |
| A0 | upper thermistor divider junction |
| A1 | lower thermistor divider junction |
| 5V | top of each thermistor divider (12 kOhm resistor) |
| VIN | 10.5 V rail |
| GND | common ground |

## Build and upload

1. Open `firmware/Incubator/Incubator.ino` in the Arduino IDE.
2. Board: Arduino Nano, processor ATmega328P (choose "Old Bootloader" if uploads fail).
3. Upload, then open the Serial Monitor at 115200 baud.
4. If the Nano resets in a loop after a watchdog reset, set `USE_WATCHDOG` to `false`.

The sketch has not been compiled in CI. Report any compile errors as issues.

## Serial commands

| Command | What it does |
|---|---|
| `su N` / `sl N` | bench setpoint for the upper / lower zone (console units, e.g. 60); only used when no console is connected; 0 = off |
| `x` | toggle the bench light |
| `f A B` | force the two raw reply bytes (0-255) |
| `n` | go back to replying with the sensor values |
| `r` | clear a fault |
| `?` | print status |

## Safety

Hardware (not in code):
- Normally-closed bimetal thermostat in series with the Peltier feed, mounted on the interior heatsink.
- A fuse per Peltier channel and a separate fuse (about 1 A) on the fans.
- The fans run whenever power is present, so the heaters never run without airflow.

Software:
- Heaters stay off until a valid setpoint arrives and the sensor reads sensibly.
- Setpoints are accepted only after two identical frames in a row and within 30-90.
- Faults turn the heaters off and blink the light until `r` is typed: over 125 F, sensor missing or shorted while heating, or no 1 F rise in 5 minutes while heating hard.
- If the console disappears for 5 seconds, the zone falls back to the bench setpoint (0 = heaters off).
- Watchdog timer (4 s).

## Bring-up order

1. Nano on USB only: status lines should show `NO-SENSOR` and every output off.
2. Add the two thermistor dividers and compare the temperatures with a reference thermometer.
3. Test the MOSFET modules with dummy loads using `tools/mosfet_test`, or the bench setpoint commands.
4. Check each module at full duty with a Peltier connected: the voltage across the module (load output to ground) should be well under 0.3 V. If not, the module is not switching fully on a 5 V gate and needs a logic-level replacement.
5. Connect the console. Check the `console:` lines and what the display does with the Nano's replies. If it shows an error, use `f 128 128` to force fixed raw bytes and find what it accepts.
6. Connect the Peltiers last, with the thermostat, fuses and fans in place.

## Known unknowns

- The reply format is untested against the real console: the 0-bit low (522 us) and the reply gap (150 us) are assumptions, and two tiny pulses seen before the original board's reply header are not reproduced.
- The reply's second byte is assumed to be the lower sensor.
- How the console turns the raw reading into the displayed temperature is unknown.
- The thermistor type and curve: the sketch uses generic 10k NTC coefficients as placeholders.
- The IRF520-type modules on a 5 V gate: verify full switching (bring-up step 4).
- The lower zone's console minimum (42 F) is unverified.
- The Peltier ratings, and so the fuse sizes, are not yet known.

## Repository layout

- `firmware/Incubator/` - the controller sketch
- `tools/mosfet_test/` - bench test for the MOSFET outputs with typed setpoints and temperatures
- `tools/console_standin/` - plays the console toward the original board to study its replies
- `docs/protocol.md` - console and board frame formats
- `docs/wiring.md` - power, MOSFET modules, thermistors and the console connection
