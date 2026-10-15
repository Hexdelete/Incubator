# Wiring

Join every ground at one point. Everything below shares that ground.

## Power

```
12 V DC supply, set to 10.5 V
  |
  +-- thermostat (NC, on interior heatsink)
  |     +-- fuse -- MOSFET module 1 -- upper Peltier
  |     +-- fuse -- MOSFET module 2 -- lower Peltier
  |
  +-- fan fuse (about 1 A) -- both fans (always on)
  |
  +-- Nano VIN (7-12 V)
  |
  +-- 5 V buck
        +-- MOSFET module 3 -- interior LED (5 V)
        +-- 3.3 V regulator -- console power pin
```

- The console is powered at 3.3 V (the original board measured 3.37 V on its power and data pins). Never feed it 5 V.
- The Nano runs from VIN, so USB and the supply can be connected together on most boards. If your clone has no isolating diode, do the first upload with the 10.5 V disconnected.
- Fuse sizes depend on the Peltier current, which is still to be measured.

## MOSFET modules (low-side switching)

For each module:

- SIG from the Nano pin (D9 upper Peltier, D10 lower Peltier, D4 LED), GND to the common ground.
- Power input from the 10.5 V feed (after the thermostat and fuse) for the Peltier modules, or from the 5 V buck for the LED module.
- Load terminals to the Peltier or LED. Swap the two Peltier wires from their original cooling polarity so it heats.
- Keep the LED module's own resistor. It runs from 5 V and needs no extra one.
- If a module has no pull-down on SIG, add 10 kOhm from SIG to GND so it stays off while the Nano resets. Check by measuring SIG to GND with the power off: no resistance means add one.
- If a module has no resistor in its signal path, add 100-220 Ohm in series with SIG.
- Fit the heatsinks. The MOSFET tab is at the output-side potential, so do not let it touch ground, the chassis or another module's heatsink.
- At full duty, measure the voltage across each module (load output to ground): well under 0.3 V is good.

## Thermistors

Each sensor is a voltage divider with the junction going to an analog input:

```
Nano 5V pin -- 12 kOhm -- junction -- thermistor -- GND
                             |
                             +-- wire to A0 (upper) or A1 (lower)
```

- The thermistor has two wires and uses two connections: one to the junction, one to GND. The third connection at the junction comes from the resistor and the wire to the analog pin.
- Thermistor polarity does not matter.
- Use the Nano's own 5V pin for the top of the divider, so the divider and the ADC share one reference.
- The junction can be made on a breadboard row, a screw terminal, or a twisted, soldered and heat-shrunk joint.
- The resistor value is a hypothesis (about 12 kOhm, matching the original board's apparent divider). Measure it and set `R_FIXED` in the sketch.
- If you put the thermistor on the 5 V side and the resistor on the GND side, set `THERMISTOR_TO_5V` to `true`.

## Console connector

Three pins: 3.3 V, GND and DATA.

```
console DATA --- 470 Ohm --- Nano D2
      |
      +--- 10 kOhm --- 3.3 V    (pull-up; skip if the line already idles at about 3.3 V)
```

- D2 only ever pulls the line low and then releases it, never driving it high, because the console is 3.3 V logic.
- The Nano reads the 3.3 V high level reliably, though with limited margin. Keep the wiring short.

## Resistors

| Qty | Value | Where |
|---|---|---|
| 2 | 12 kOhm, 1% | thermistor dividers (5V to A0 and A1) |
| 1 | 470 Ohm | D2 to the console data wire |
| 1 | 10 kOhm | data wire pull-up to 3.3 V, if needed |
| up to 3 | 10 kOhm | MOSFET SIG to GND, if the modules lack them |
| up to 3 | 100-220 Ohm | in series with SIG, if the modules lack them |

Optional once you have capacitors: 470-1000 uF across the 10.5 V rail near the Peltier modules, and 100 nF across each thermistor junction to ground.
