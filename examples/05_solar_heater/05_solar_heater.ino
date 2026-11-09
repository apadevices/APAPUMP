// APAPUMP — 05_solar_heater
//
// Solar heating + a heat pump on top of a filtration schedule, in a few lines:
//   • The schedule drives the pump; solar heating/cooling add extra runs.
//   • Solar valve on P3 follows the solar demand, also during a scheduled run.
//   • Heat pump on the AUX relay (P2) in HEATER_SIMPLE: allowed while the pump runs,
//     but not while solar heats (solar first) and never during a pump alarm.
//   • Mode, Max pool and heater mode are saved by the library and survive a reboot.
//
// Wiring: APA HMI board (PCF8574AT 0x3C: pump P0, AUX P2, valve P3).
// The AUX relay switches the heat pump's remote ON/OFF input or a fused contactor coil —
// never the compressor directly.

#include <Wire.h>
#include <APAPUMP.h>

ApaPump pump;

// Replace with your sensors. Return -127.0f until a reading is valid.
float collectorTemp() { return -127.0f; }
float poolTemp()      { return -127.0f; }
bool  scheduleOn()    { return false; }      // e.g. gui.isTimerActive(minuteOfDay)

void setup() {
    Serial.begin(115200);
    Wire.begin();
    pump.begin(scheduleOn);

    pump.setPoolTempCallback(poolTemp);
    pump.enableSolar(collectorTemp, nullptr, true);   // valve fitted; constants = defaults
    pump.enableAux(10, 120);                           // heat pump: 120 s after-run

    if (!pump.hasSavedSettings()) {                    // first boot only
        pump.setSolarMaxPool(30);                      // cool above 30 °C (0 = no limit)
        pump.setAuxMode(HEATER_SIMPLE);
    }
    pump.setStatusCallback([](const __FlashStringHelper* m) { Serial.println(m); });
}

void loop() {
    pump.update();

    static uint32_t last = 0;
    if (millis() - last >= 5000UL) {
        last = millis();
        Serial.print(F("pump "));   Serial.print(pump.isRunning()       ? F("ON")  : F("off"));
        Serial.print(F(" solar ")); Serial.print(pump.isSolarRunning()  ? F("run") : F("-"));
        Serial.print(F(" valve ")); Serial.print(pump.isSolarValveOpen()? F("open"): F("closed"));
        Serial.print(F(" heater "));Serial.print(pump.isHeaterOn()      ? F("ON")  : F("off"));
        if (!pump.isSolarSensorOk()) Serial.print(F(" SENSOR?"));
        Serial.println();
    }
}
