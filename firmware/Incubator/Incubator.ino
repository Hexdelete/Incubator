// Haier HVTEC12DABS controller, first run (Arduino Nano).
// Replaces the original board: answers the front console, reads two thermistors,
// heats two reversed Peltiers through MOSFET modules, drives the interior LED.
//
// Pins: D2 console data (470 ohm series) | D4 LED module | D9 upper Peltier | D10 lower Peltier
//       A0 upper thermistor divider | A1 lower thermistor divider (12k resistor + thermistor, see THERMISTOR_TO_5V)
//
// Serial (115200 baud, Enter after each):
//   su N / sl N   bench setpoint (console units, e.g. 60), only used when no console is connected; 0 = off
//   x             toggle the bench light
//   f A B         force the two raw reply bytes (0-255), e.g. f 128 128;   n = back to the sensors
//   r             clear a fault
//   ?             status

#include <avr/wdt.h>

// ================= settings =================
const uint8_t PIN_DATA  = 2;
const uint8_t PIN_LIGHT = 4;
const uint8_t PIN_UPPER = 9;      // Timer1 PWM
const uint8_t PIN_LOWER = 10;     // Timer1 PWM

const bool USE_WATCHDOG = true;   // set false if your Nano resets in a loop (old bootloader)
const bool SEND_REPLY   = true;

// thermistor wiring:
//   false = 12k from the Nano 5V pin to the junction, thermistor from the junction to GND (as built)
//   true  = thermistor from the Nano 5V pin to the junction, 12k from the junction to GND
// the junction goes to A0 / A1 either way
const bool THERMISTOR_TO_5V = false;

// console setpoint -> real target: upper 54-66 -> 104-116 F, lower 42-66 -> 67-91 F
const int8_t OFFSET_UPPER_F = 50;
const int8_t OFFSET_LOWER_F = 25;
const float  MAX_TARGET_F   = 120.0;
const float  MAX_TEMP_F     = 125.0;      // over this: fault, heaters off
const float  KP_DUTY_PER_F  = 64.0;       // PWM counts per degree below target (full power about 4 F below)

// thermistors: generic 10k NTC coefficients (placeholders)
const float R_FIXED = 12000.0;            // put your measured resistor value here
const float SH_A[2] = { 1.009249522e-3, 1.009249522e-3 };
const float SH_B[2] = { 2.378405444e-4, 2.378405444e-4 };
const float SH_C[2] = { 2.019202697e-7, 2.019202697e-7 };
const float CAL_OFFSET_F[2] = { 0.0, 0.0 };   // quick trim per sensor (upper, lower)

// "no temperature rise" fault
const uint32_t NO_RISE_MS     = 300000UL; // 5 minutes
const float    MIN_RISE_F     = 1.0;
const float    MIN_ERROR_F    = 3.0;
const uint8_t  MIN_DUTY_JUDGE = 128;

const uint32_t CONSOLE_TIMEOUT_MS = 5000UL;   // no console frames for this long: use bench setpoints (0 = heaters off)

// reply format seen from the original board (the 0-bit timing is a guess: 1 ms bit period)
const uint16_t REPLY_GAP_US = 150;            // wait after the console's frame ends
const uint16_t R_HDR_LOW1 = 1820, R_HDR_HIGH1 = 2860, R_HDR_LOW2 = 8490, R_HDR_HIGH2 = 478;
const uint16_t R_ONE_LOW = 1524, R_ZERO_LOW = 522, R_MARK = 478, R_BYTE_EXTRA = 100;

// ================= state =================
struct Zone {
  uint8_t  pwmPin;
  uint8_t  sensorPin;
  int8_t   offsetF;
  uint8_t  rawAdc8;     // reply byte: raw 8-bit divider reading
  bool     sensorOk;
  float    tempF;
  float    targetF;     // 0 = heating off
  uint8_t  duty;
  float    baseF;       // for the no-rise check
  uint32_t baseMs;
};
Zone zones[2] = {
  { PIN_UPPER, A0, OFFSET_UPPER_F, 255, false, 0, 0, 0, 0, 0 },
  { PIN_LOWER, A1, OFFSET_LOWER_F, 255, false, 0, 0, 0, 0, 0 }
};

const uint8_t FAULT_NONE = 0, FAULT_SENSOR = 1, FAULT_OVERTEMP = 2, FAULT_NORISE = 3;
uint8_t  fault = FAULT_NONE;

uint8_t  consoleSet[2] = { 0, 0 };    // from console frames: [0] = top, [1] = bottom
bool     consoleLight = false;
bool     haveFrame = false;
uint32_t lastFrameMs = 0, lastHouse = 0, lastStatus = 0;
uint8_t  benchSet[2] = { 0, 0 };
bool     benchLight = false;
int16_t  forceRaw[2] = { -1, -1 };
uint32_t replies = 0;

// ================= receive: interrupt-driven decoder for the console's frames =================
const uint8_t MAX_BYTES = 6;
volatile uint8_t  state = 0, bitCount = 0;
volatile uint8_t  rx[MAX_BYTES], frameBuf[MAX_BYTES];
volatile uint8_t  frameBits = 0;
volatile bool     frameReady = false;
volatile uint32_t lastEdge = 0;

inline bool inRange(uint32_t d, uint16_t lo, uint16_t hi) { return d >= lo && d <= hi; }

void onEdge() {
  uint32_t now = micros();
  uint32_t d = now - lastEdge;
  lastEdge = now;
  uint8_t ended = !digitalRead(PIN_DATA);   // 1 = a high period just ended, 0 = a low period just ended

  switch (state) {
    case 0:
      if (!ended && inRange(d, 1300, 1900)) state = 1;
      break;
    case 1:
      if (ended && inRange(d, 2200, 2800)) state = 2; else state = 0;
      break;
    case 2:
      if (!ended && inRange(d, 1600, 2200)) state = 3; else state = 0;
      break;
    case 3:
      if (ended && inRange(d, 300, 800)) {
        state = 4; bitCount = 0;
        for (uint8_t i = 0; i < MAX_BYTES; i++) rx[i] = 0;
      } else state = 0;
      break;
    case 4: {                               // low period = data bit
      if (ended) { state = 0; break; }
      uint8_t bit;
      if (d < 700) bit = 0;
      else if (inRange(d, 1100, 1700)) bit = 1;
      else { state = 0; break; }
      if (bitCount < MAX_BYTES * 8) {
        if (bit) rx[bitCount >> 3] |= (1 << (bitCount & 7));
        bitCount++;
        state = 5;
      } else state = 0;
      break;
    }
    case 5:                                 // high period: short = next bit, long = end of frame
      if (!ended) { state = 0; break; }
      if (inRange(d, 400, 900)) state = 4;
      else if (d > 3000) {
        for (uint8_t i = 0; i < MAX_BYTES; i++) frameBuf[i] = rx[i];
        frameBits = bitCount;
        frameReady = true;
        state = 0;
      } else state = 0;
      break;
  }
}

// ================= transmit: the board's reply (only ever pull low, then release) =================
void sendPulse(uint16_t lowUs, uint16_t highUs) {
  DDRD |=  _BV(PD2);                        // pull the line low
  delayMicroseconds(lowUs);
  DDRD &= ~_BV(PD2);                        // release: the pull-up brings it high
  if (highUs) delayMicroseconds(highUs);
}

void sendReply(uint8_t b1, uint8_t b2) {
  uint8_t data[2] = { b1, b2 };
  sendPulse(R_HDR_LOW1, R_HDR_HIGH1);
  sendPulse(R_HDR_LOW2, R_HDR_HIGH2);
  for (uint8_t i = 0; i < 16; i++) {
    uint8_t bit = (data[i >> 3] >> (i & 7)) & 1;          // LSB first
    uint16_t lowUs = bit ? R_ONE_LOW : R_ZERO_LOW;
    if (i == 7 || i == 15) lowUs += R_BYTE_EXTRA;         // the 8th and 16th lows were about 100 us longer
    sendPulse(lowUs, (i == 15) ? 0 : R_MARK);
  }
}

void reply() {
  if (!SEND_REPLY) return;
  uint8_t b1 = (forceRaw[0] >= 0) ? (uint8_t)forceRaw[0] : zones[0].rawAdc8;
  uint8_t b2 = (forceRaw[1] >= 0) ? (uint8_t)forceRaw[1] : zones[1].rawAdc8;

  uint32_t t0 = micros();
  while (!(PIND & _BV(PD2)) && (micros() - t0) < 12000UL) { }   // wait for the console's trailer low to end
  if (!(PIND & _BV(PD2))) return;                               // line stuck low: don't transmit
  delayMicroseconds(REPLY_GAP_US);

  detachInterrupt(digitalPinToInterrupt(PIN_DATA));             // ignore our own edges
  sendReply(b1, b2);
  EIFR = _BV(INTF0);                                            // clear any pending edge flag
  state = 0;
  lastEdge = micros();
  attachInterrupt(digitalPinToInterrupt(PIN_DATA), onEdge, CHANGE);
  replies++;
}

// ================= sensors and control =================
void readSensor(uint8_t i) {
  Zone &z = zones[i];
  analogRead(z.sensorPin);                                      // dummy read lets the ADC settle
  uint32_t sum = 0;
  for (uint8_t k = 0; k < 16; k++) sum += analogRead(z.sensorPin);
  float adcRaw = sum / 16.0;
  float adc = THERMISTOR_TO_5V ? (1024.0 - adcRaw) : adcRaw;   // treat it as if the thermistor were on the GND side

  float raw = adc / 4.0 + 0.5;
  z.rawAdc8 = (uint8_t)constrain(raw, 0.0, 255.0);

  if (adc < 20.0 || adc > 1000.0) { z.sensorOk = false; return; }   // shorted or open sensor
  z.sensorOk = true;
  float r = R_FIXED * adc / (1024.0 - adc);
  float L = log(r);
  float tK = 1.0 / (SH_A[i] + SH_B[i] * L + SH_C[i] * L * L * L);
  z.tempF = (tK - 273.15) * 1.8 + 32.0 + CAL_OFFSET_F[i];
}

bool consoleOk() { return haveFrame && (millis() - lastFrameMs < CONSOLE_TIMEOUT_MS); }

bool lightWanted() { return consoleOk() ? consoleLight : benchLight; }

void updateTargets() {
  for (uint8_t i = 0; i < 2; i++) {
    uint8_t s = consoleOk() ? consoleSet[i] : benchSet[i];
    float t = (s > 0) ? (float)s + zones[i].offsetF : 0.0;
    if (t > MAX_TARGET_F) t = MAX_TARGET_F;
    zones[i].targetF = t;
  }
}

void control(uint8_t i, bool allowed) {
  Zone &z = zones[i];
  uint8_t duty = 0;
  if (allowed && z.sensorOk && z.targetF > 0) {
    float err = z.targetF - z.tempF;
    if (err > 0) duty = (uint8_t)constrain(err * KP_DUTY_PER_F, 0.0, 255.0);
  }
  z.duty = duty;
  analogWrite(z.pwmPin, duty);
}

bool noRise(uint8_t i, uint32_t now) {
  Zone &z = zones[i];
  bool judging = (z.duty >= MIN_DUTY_JUDGE) && (z.targetF - z.tempF >= MIN_ERROR_F);
  if (!judging) { z.baseF = z.tempF; z.baseMs = now; return false; }
  if (z.tempF >= z.baseF + MIN_RISE_F) { z.baseF = z.tempF; z.baseMs = now; return false; }
  if (z.tempF < z.baseF) z.baseF = z.tempF;                     // temperature fell: lower the bar, keep the clock
  return (now - z.baseMs) >= NO_RISE_MS;
}

void printFault(uint8_t code) {
  switch (code) {
    case FAULT_NONE:     Serial.print(F("none")); break;
    case FAULT_SENSOR:   Serial.print(F("sensor missing or shorted while heating")); break;
    case FAULT_OVERTEMP: Serial.print(F("over-temperature")); break;
    case FAULT_NORISE:   Serial.print(F("no temperature rise")); break;
  }
}

void trip(uint8_t code) {
  if (fault != FAULT_NONE) return;
  fault = code;
  analogWrite(PIN_UPPER, 0);
  analogWrite(PIN_LOWER, 0);
  zones[0].duty = 0;
  zones[1].duty = 0;
  Serial.print(F("FAULT: ")); printFault(code); Serial.println(F("  (heaters off; r clears)"));
}

void printStatus() {
  for (uint8_t i = 0; i < 2; i++) {
    Zone &z = zones[i];
    Serial.print(i == 0 ? F("upper ") : F(" | lower "));
    if (z.sensorOk) { Serial.print(z.tempF, 1); Serial.print(F("F")); }
    else Serial.print(F("NO-SENSOR"));
    Serial.print(F(" raw=")); Serial.print(z.rawAdc8);
    Serial.print(F(" target=")); Serial.print(z.targetF, 0);
    Serial.print(F(" duty=")); Serial.print(z.duty);
  }
  Serial.print(F(" | console=")); Serial.print(consoleOk() ? F("ok") : F("none"));
  Serial.print(F(" replies=")); Serial.print(replies);
  Serial.print(F(" light=")); Serial.print(lightWanted() ? 1 : 0);
  Serial.print(F(" fault=")); printFault(fault);
  Serial.println();
}

void housekeeping() {
  lastHouse = millis();
  readSensor(0);
  readSensor(1);
  updateTargets();

  uint32_t now = millis();
  if (fault == FAULT_NONE) {
    for (uint8_t i = 0; i < 2; i++) {
      Zone &z = zones[i];
      if (z.sensorOk && z.tempF > MAX_TEMP_F) trip(FAULT_OVERTEMP);
      else if (!z.sensorOk && z.targetF > 0) trip(FAULT_SENSOR);
      else if (noRise(i, now)) trip(FAULT_NORISE);
    }
  }
  bool allowed = (fault == FAULT_NONE);
  control(0, allowed);
  control(1, allowed);

  if (millis() - lastStatus >= 2000) { lastStatus = millis(); printStatus(); }
}

void updateLight() {
  if (fault != FAULT_NONE) digitalWrite(PIN_LIGHT, ((millis() / 250) % 2) ? HIGH : LOW);   // blink on fault
  else digitalWrite(PIN_LIGHT, lightWanted() ? HIGH : LOW);
}

// ================= console frames =================
void acceptFrame(uint8_t b1, uint8_t b2) {
  static uint8_t pB1 = 0, pB2 = 0;
  bool stable = (b1 == pB1 && b2 == pB2);              // act only when two frames in a row agree
  pB1 = b1; pB2 = b2;
  lastFrameMs = millis();
  haveFrame = true;
  if (!stable) return;

  uint8_t top = b1, bottom = b2 & 0x7F;
  bool light = (b2 & 0x80) != 0;
  bool plausible = (top >= 30 && top <= 90 && bottom >= 30 && bottom <= 90);
  if (plausible && (top != consoleSet[0] || bottom != consoleSet[1] || light != consoleLight)) {
    Serial.print(F("console: top=")); Serial.print(top);
    Serial.print(F(" bottom=")); Serial.print(bottom);
    Serial.print(F(" light=")); Serial.println(light ? 1 : 0);
  }
  if (plausible) { consoleSet[0] = top; consoleSet[1] = bottom; }
  consoleLight = light;
}

// ================= serial commands =================
char    cmd[24];
uint8_t cmdLen = 0;

uint8_t clampByte(long v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }

void handleCommand(char *s) {
  char c = s[0];
  if (c == '?') { printStatus(); return; }
  if (c == 'r') {
    fault = FAULT_NONE;
    for (uint8_t i = 0; i < 2; i++) { zones[i].baseMs = millis(); zones[i].baseF = zones[i].tempF; }
    Serial.println(F("fault cleared"));
    return;
  }
  if (c == 'x') { benchLight = !benchLight; Serial.print(F("bench light ")); Serial.println(benchLight ? F("on") : F("off")); return; }
  if (c == 'n') { forceRaw[0] = forceRaw[1] = -1; Serial.println(F("reply uses the sensors")); return; }
  if (c == 'f') {
    char *p = s + 1;
    long a = strtol(p, &p, 10);
    long b = strtol(p, &p, 10);
    forceRaw[0] = clampByte(a); forceRaw[1] = clampByte(b);
    Serial.print(F("forcing reply bytes ")); Serial.print(forceRaw[0]); Serial.print(' '); Serial.println(forceRaw[1]);
    return;
  }
  if (c == 's' && (s[1] == 'u' || s[1] == 'l')) {
    uint8_t i = (s[1] == 'u') ? 0 : 1;
    benchSet[i] = clampByte(atol(s + 2));
    Serial.print(i == 0 ? F("upper") : F("lower")); Serial.print(F(" bench setpoint = ")); Serial.println(benchSet[i]);
    return;
  }
  Serial.println(F("commands: su N | sl N | x | f A B | n | r | ?"));
}

void pollSerial() {
  while (Serial.available()) {
    char c = Serial.read();
    if (c == '\n' || c == '\r') {
      if (cmdLen) { cmd[cmdLen] = 0; handleCommand(cmd); cmdLen = 0; }
    } else if (cmdLen < sizeof(cmd) - 1) {
      cmd[cmdLen++] = c;
    }
  }
}

// ================= main =================
void setup() {
  wdt_disable();
  Serial.begin(115200);
  pinMode(PIN_DATA, INPUT);                  // never driven high: the console is 3.3 V logic
  pinMode(PIN_LIGHT, OUTPUT); digitalWrite(PIN_LIGHT, LOW);
  pinMode(PIN_UPPER, OUTPUT); digitalWrite(PIN_UPPER, LOW);
  pinMode(PIN_LOWER, OUTPUT); digitalWrite(PIN_LOWER, LOW);
  TCCR1B = (TCCR1B & 0xF8) | 0x02;           // Timer1 prescaler 8: about 3.9 kHz PWM on D9/D10
  lastEdge = micros();
  attachInterrupt(digitalPinToInterrupt(PIN_DATA), onEdge, CHANGE);
  Serial.println(F("controller ready: su N | sl N | x | f A B | n | r | ?"));
  if (USE_WATCHDOG) wdt_enable(WDTO_4S);
}

void loop() {
  if (USE_WATCHDOG) wdt_reset();
  pollSerial();

  if (frameReady) {
    uint8_t bits, buf[MAX_BYTES];
    noInterrupts();
    bits = frameBits;
    for (uint8_t i = 0; i < MAX_BYTES; i++) buf[i] = frameBuf[i];
    frameReady = false;
    interrupts();

    if (bits == 16) {
      reply();                               // time-critical: answer first
      acceptFrame(buf[0], buf[1]);
      housekeeping();                        // then sensors, control, safety, status
    }
  } else if (millis() - lastFrameMs > 400 && millis() - lastHouse > 300) {
    housekeeping();                          // no console connected: run on a timer
  }
  updateLight();
}
