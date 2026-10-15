// Console stand-in: sends console-style frames to the ORIGINAL board and decodes the board's reply.
// Wiring: D2 -> ~470 ohm -> board's console data pin; Nano GND to board GND.
// D2 only ever pulls the line low, then releases it. Keep the board's Peltier outputs disconnected.
//
// Serial commands (115200 baud, press Enter after each):
//   s B1 B2   send one frame with these two bytes, then show the board's reply in detail
//   w B1 B2   keep sending that frame every ~180 ms (like the console); print the reply only when it changes
//   x         stop watching

const uint16_t MAXRUN = 120;
uint16_t runs[MAXRUN];                  // bit15 = level that lasted, bits0-14 = duration in us
uint16_t nruns = 0;

inline uint8_t  lvl(uint16_t i) { return runs[i] >> 15; }
inline uint16_t dur(uint16_t i) { return runs[i] & 0x7FFF; }

void sendPulse(uint16_t lowUs, uint16_t highUs) {
  DDRD |=  _BV(PD2);                    // pull the line low
  delayMicroseconds(lowUs);
  DDRD &= ~_BV(PD2);                    // release: the board's pull-up brings it high
  if (highUs) delayMicroseconds(highUs);
}

void sendFrame(uint8_t b1, uint8_t b2) {          // console-style frame, nominal timings
  uint8_t data[2] = { b1, b2 };
  sendPulse(1616, 2480);
  sendPulse(1872, 500);
  for (uint8_t i = 0; i < 16; i++) {
    uint8_t bit = (data[i >> 3] >> (i & 7)) & 1;  // LSB first
    uint16_t lowUs  = bit ? 1372 : 376;
    uint16_t highUs = (i == 7) ? 740 : (i == 15) ? 6192 : 616;
    sendPulse(lowUs, highUs);
  }
  sendPulse(5964, 0);
}

bool waitIdle(uint32_t idleUs, uint32_t timeoutMs) {
  uint32_t start = millis();
  uint32_t highSince = micros();
  while (millis() - start < timeoutMs) {
    if (!(PIND & _BV(PD2))) highSince = micros();
    else if (micros() - highSince >= idleUs) return true;
  }
  return false;
}

void capture(uint32_t windowMs) {
  nruns = 0;
  uint8_t level = (PIND >> 2) & 1;
  uint32_t last = micros();
  uint32_t endMs = millis() + windowMs;
  while ((int32_t)(millis() - endMs) < 0) {
    uint8_t now = (PIND >> 2) & 1;
    if (now != level) {
      uint32_t t = micros();
      uint32_t d = t - last;
      if (nruns < MAXRUN) {
        if (d > 32767UL) d = 32767UL;
        runs[nruns++] = (uint16_t)d | ((uint16_t)level << 15);
      }
      last = t;
      level = now;
    }
  }
}

bool exchange(uint8_t b1, uint8_t b2) {
  if (!waitIdle(12000UL, 1000)) return false;
  Serial.flush();                       // no serial interrupts during the frame
  noInterrupts();
  sendFrame(b1, b2);
  interrupts();
  capture(100);
  return true;
}

// reply header seen so far: low ~1820, high ~2860, low ~8490, high ~478, then the bits
int findHeader() {
  for (uint16_t i = 0; i + 3 < nruns; i++) {
    if (lvl(i) == 0 && lvl(i + 1) == 1 && lvl(i + 2) == 0 && lvl(i + 3) == 1 &&
        dur(i) >= 1500 && dur(i) <= 2200 &&
        dur(i + 1) >= 2200 && dur(i + 1) <= 3500 &&
        dur(i + 2) >= 6000 && dur(i + 2) <= 11000 &&
        dur(i + 3) >= 300 && dur(i + 3) <= 800) return i;
  }
  return -1;
}

// each data bit is a low run; 1000 us or longer counts as 1 (an assumption: we have only seen long lows so far)
uint8_t decodeReply(uint8_t *bytes, uint16_t *lows, uint8_t maxBits) {
  int h = findHeader();
  if (h < 0) return 0;
  bytes[0] = bytes[1] = bytes[2] = bytes[3] = 0;
  uint8_t n = 0;
  for (uint16_t i = h + 4; i < nruns && n < maxBits; i += 2) {
    if (lvl(i) != 0) break;
    uint16_t d = dur(i);
    lows[n] = d;
    if (d >= 1000) bytes[n >> 3] |= (1 << (n & 7));
    n++;
  }
  return n;
}

void dumpRuns() {
  for (uint16_t i = 0; i < nruns; i++) { Serial.print(lvl(i)); Serial.print(','); Serial.println(dur(i)); }
}

void printReply(bool detail) {
  uint8_t b[4];
  uint16_t lows[32];
  uint8_t n = decodeReply(b, lows, 32);
  if (!n) {
    Serial.println(F("no reply found"));
    if (detail) dumpRuns();
    return;
  }
  Serial.print(F("reply: bits=")); Serial.print(n);
  for (uint8_t i = 0; i < (n + 7) / 8; i++) {
    Serial.print(F("  B")); Serial.print(i + 1); Serial.print('='); Serial.print(b[i]);
  }
  Serial.println();
  if (detail) {
    Serial.print(F("  data lows (us):"));
    for (uint8_t i = 0; i < n; i++) { Serial.print(' '); Serial.print(lows[i]); }
    Serial.println();
  }
}

// ---------- serial commands ----------
char    cmd[24];
uint8_t cmdLen = 0;
bool    watching = false;
uint8_t wB1 = 0, wB2 = 0, lastN = 255, lastB[4] = {0, 0, 0, 0};
uint32_t lastSend = 0, frameNo = 0;

uint8_t clampByte(long v) { return v < 0 ? 0 : (v > 255 ? 255 : (uint8_t)v); }

void handleCommand(char *s) {
  char c = s[0];
  char *p = s + 1;
  long a = strtol(p, &p, 10);
  long b = strtol(p, &p, 10);
  switch (c) {
    case 's':
      watching = false;
      Serial.print(F("sending ")); Serial.print(clampByte(a)); Serial.print(' '); Serial.println(clampByte(b));
      if (exchange(clampByte(a), clampByte(b))) printReply(true);
      else Serial.println(F("line never went idle; nothing sent"));
      break;
    case 'w':
      wB1 = clampByte(a); wB2 = clampByte(b);
      watching = true; lastN = 255; lastSend = 0; frameNo = 0;
      Serial.print(F("watching, sending ")); Serial.print(wB1); Serial.print(' '); Serial.println(wB2);
      break;
    case 'x':
      watching = false;
      Serial.println(F("stopped"));
      break;
    default:
      Serial.println(F("commands: s B1 B2 | w B1 B2 | x"));
  }
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

void setup() {
  Serial.begin(115200);
  pinMode(2, INPUT);                    // PORTD bit 2 stays 0, so DDRD alone pulls low / releases
  Serial.println(F("console stand-in ready: s B1 B2 | w B1 B2 | x"));
}

void loop() {
  pollSerial();
  if (!watching || millis() - lastSend < 180) return;
  lastSend = millis();
  if (!exchange(wB1, wB2)) return;
  frameNo++;

  uint8_t b[4];
  uint16_t lows[32];
  uint8_t n = decodeReply(b, lows, 32);
  if (n != lastN || memcmp(b, lastB, 4) != 0) {
    Serial.print('#'); Serial.print(frameNo); Serial.print(' ');
    printReply(false);
    lastN = n;
    memcpy(lastB, b, 4);
  }
}
