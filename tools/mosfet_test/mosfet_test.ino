// MOSFET test: setpoints and "detected" temperatures are typed over serial.
// An output is ON while detected < setpoint, and OFF otherwise (plain on/off, full duty).
//
// Serial Monitor: 115200 baud, press Enter after each command
//   su 100   upper setpoint        sl 80   lower setpoint
//   tu 99    upper detected temp   tl 79   lower detected temp
//   x        toggle the LED MOSFET on D4
//   r        clear a safety trip
//   ?        show status

const uint8_t PIN_UPPER = 9;     // upper Peltier MOSFET module signal
const uint8_t PIN_LOWER = 10;    // lower Peltier MOSFET module signal
const uint8_t PIN_LED   = 4;     // interior LED MOSFET module signal

const float    HYST = 0.0;               // degrees: turns off at setpoint + HYST (0 = off at the setpoint)
const uint32_t MAX_ON_MS = 120000UL;     // safety: force an output off after this long continuously on (0 = no limit)

const uint8_t pins[2] = { PIN_UPPER, PIN_LOWER };
const char*   names[2] = { "upper", "lower" };

float    setpoint[2] = { 70.0, 70.0 };   // [0] = upper, [1] = lower
float    detected[2] = { 72.0, 72.0 };
bool     on[2] = { false, false };
bool     tripped[2] = { false, false };
uint32_t onSince[2] = { 0, 0 };
bool     ledOn = false;

char    cmd[24];
uint8_t cmdLen = 0;

void printState(uint8_t i) {
  Serial.print(names[i]);
  Serial.print(F(": setpoint=")); Serial.print(setpoint[i], 1);
  Serial.print(F("  detected=")); Serial.print(detected[i], 1);
  Serial.print(F("  output="));   Serial.println(on[i] ? F("ON") : F("OFF"));
}

void status() {
  printState(0);
  printState(1);
  Serial.print(F("LED: ")); Serial.println(ledOn ? F("on") : F("off"));
  if (tripped[0] || tripped[1]) Serial.println(F("a safety trip is active: type r to clear"));
}

void updateOutputs() {
  for (uint8_t i = 0; i < 2; i++) {
    bool want = on[i];
    if (!on[i] && detected[i] < setpoint[i]) want = true;
    else if (on[i] && detected[i] >= setpoint[i] + HYST) want = false;

    if (on[i] && MAX_ON_MS && (millis() - onSince[i] > MAX_ON_MS)) {
      want = false;
      tripped[i] = true;
      Serial.print(F("SAFETY: ")); Serial.print(names[i]);
      Serial.println(F(" was on too long, forced off. Type r to clear."));
    }
    if (tripped[i]) want = false;

    if (want != on[i]) {
      on[i] = want;
      digitalWrite(pins[i], on[i] ? HIGH : LOW);
      if (on[i]) onSince[i] = millis();
      printState(i);
    }
  }
}

void handleCommand(char *s) {
  if (s[0] == '?') { status(); return; }
  if (s[0] == 'r') {
    tripped[0] = tripped[1] = false;
    Serial.println(F("trips cleared"));
    return;
  }
  if (s[0] == 'x') {
    ledOn = !ledOn;
    digitalWrite(PIN_LED, ledOn ? HIGH : LOW);
    Serial.print(F("LED ")); Serial.println(ledOn ? F("on") : F("off"));
    return;
  }
  if ((s[0] == 's' || s[0] == 't') && (s[1] == 'u' || s[1] == 'l')) {
    float v = atof(s + 2);
    uint8_t i = (s[1] == 'u') ? 0 : 1;
    if (s[0] == 's') setpoint[i] = v; else detected[i] = v;
    Serial.print(names[i]);
    Serial.print(s[0] == 's' ? F(" setpoint = ") : F(" detected = "));
    Serial.println(v, 1);
    return;
  }
  Serial.println(F("commands: su N | sl N | tu N | tl N | x | r | ?"));
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
  pinMode(PIN_UPPER, OUTPUT); digitalWrite(PIN_UPPER, LOW);
  pinMode(PIN_LOWER, OUTPUT); digitalWrite(PIN_LOWER, LOW);
  pinMode(PIN_LED, OUTPUT);   digitalWrite(PIN_LED, LOW);
  Serial.println(F("MOSFET test ready: su N | sl N | tu N | tl N | x | r | ?"));
  status();
}

void loop() {
  pollSerial();
  updateOutputs();
}
