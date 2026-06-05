// APAPUMP — 01_minimal
//
// The smallest possible working sketch: construct, begin, update.
// Demonstrates PCF mode (default), direct GPIO, and manual override via Serial.
//
// Wiring (PCF mode — APA HMI board v1.0):
//   SCL → Mega pin 21 (or Uno A5)
//   SDA → Mega pin 20 (or Uno A4)
//   PCF8574AT at 0x3C drives the relay board via the SN74HC240PWR + ULN2803ADWR chain.
//   Relay assignment: P0=pump  P1=UVC  P2=AUX  P3=solar valve
//
// Serial commands (115200 baud):
//   '1' → FORCE_ON     '0' → FORCE_OFF     'a' → AUTO

#include <Wire.h>
#include <APAPUMP.h>

// ---- Choose one constructor ---------------------------------------------------

// Option A: APA HMI board defaults — no arguments, zero config
ApaPump pump;

// Option B: PCF at a different address or non-default bit mapping
// ApaPump pump(RELAY_PCF, 0x38, 0, 1, 2, 3);

// Option C: direct GPIO — pump relay on pin 7 (active-high module)
// ApaPump pump(RELAY_DIRECT, 7);

// Option D: direct GPIO — active-low module (jumper on module selects level)
// ApaPump pump(RELAY_DIRECT, 7);
// Call pump.setActiveLow(); in setup(), BEFORE pump.begin()

// ------------------------------------------------------------------------------

void onStatus(const __FlashStringHelper* msg) {
    Serial.println(msg);
}

void setup() {
    Serial.begin(115200);

    Wire.begin();       // required for PCF mode — must come before pump.begin()
    pump.begin();       // initialise all relays OFF, load EEPROM
    pump.setStatusCallback(onStatus);

    Serial.println(F("APAPUMP ready"));
    Serial.println(F("Commands: '1'=FORCE_ON  '0'=FORCE_OFF  'a'=AUTO"));
}

void loop() {
    pump.update();      // non-blocking — call every loop()

    // Manual override via Serial
    if (Serial.available()) {
        char c = Serial.read();
        if (c == '1') pump.setManualMode(FORCE_ON);
        if (c == '0') pump.setManualMode(FORCE_OFF);
        if (c == 'a') pump.setManualMode(AUTO);
    }
}
