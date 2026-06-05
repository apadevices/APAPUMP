// APAPUMP — 03_solar_freeze  (advanced)
//
// Full APA pool automation system:
//   • Solar heater with valve (absorber → pool temperature control)
//   • Freeze protection with dry-run interlock
//   • Pressure monitoring: dry-run + overpressure alarms
//   • Current monitoring: overcurrent alarm
//   • Catch-up window: make up missed daily target between 08:00 and 20:00
//   • APADOSE post-shock bridge: keep pump running 4 h after a shock dose
//   • Manual override with auto-return at midnight
//
// Sensor hardware:
//   APASENSE (ADS1015 at 0x4B) provides pressure, current, and temperature
//   DS3231 RTC on the I2C bus
//
// Required libraries:  APAPUMP, APADOSE, APASENSE (when available), DS3231

#include <Wire.h>
#include <APAPUMP.h>

// ---- Placeholder sensor functions -------------------------------------------
// Replace these with your APASENSE calls once that library is available.
// Each returns -1.0f until the sensor is calibrated — APAPUMP skips that feature.

float getSolarTemp()  { return -1.0f; }   // absorber temperature (°C)
float getPoolTemp()   { return -1.0f; }   // pool / freeze sensor (°C)
float getPressure()   { return -1.0f; }   // pipe pressure (bar)   — -1 = not calibrated
float getCurrent()    { return -1.0f; }   // pump current (A)      — -1 = not ready

// ---- APADOSE bridge ---------------------------------------------------------
// Post-shock: keep pump running for 4 hours after a shock dose to circulate chlorine.
// Replace dose1.isShockActive() with the real APADOSE call when wired in.

static uint32_t postShockUntil = 0;

bool externalPumpRequest() {
    // Extend window while shock is running
    // if (dose1.isShockActive()) {
    //     postShockUntil = millis() + 4UL * 3600UL * 1000UL;
    //     return true;
    // }
    return (millis() < postShockUntil);
}

// ---- Objects ----------------------------------------------------------------

ApaPump pump;

// ---- Schedule helper --------------------------------------------------------
// Replace with your real schedule source (APALCDGUI timer or RTC comparison).

bool scheduleActive() { return false; }

// ---- Alarm callback ---------------------------------------------------------

void onAlarm(PumpAlarm alarm) {
    if (alarm == PUMP_ALARM_NONE) {
        Serial.println(F("Pump alarm cleared"));
        return;
    }
    Serial.print(F("PUMP ALARM: "));
    switch (alarm) {
        case PUMP_ALARM_OVERCURRENT:   Serial.println(F("Overcurrent — check motor")); break;
        case PUMP_ALARM_LOW_PRESSURE:  Serial.println(F("Dry run — check water level")); break;
        case PUMP_ALARM_HIGH_PRESSURE: Serial.println(F("High pressure — check filter")); break;
        case PUMP_ALARM_NO_FLOW:       Serial.println(F("No flow — check valve/pipe")); break;
        default: break;
    }
    // Pump keeps running — call pump.acknowledgeAlarm() after investigation.
    // In a real system: route to gui.postActiveAlert(...) and let the operator decide.
}

// ---- Status callback --------------------------------------------------------

void onStatus(const __FlashStringHelper* msg) {
    Serial.println(msg);
}

// ---- setup() ----------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    Wire.begin();

    // APASENSE init goes here (must be before pump.begin so pressure zeros on pump stop)
    // adc.begin();

    // Pump init — schedule + post-shock bridge + daily target from scheduler
    pump.begin(
        scheduleActive,
        externalPumpRequest,
        nullptr           // replace with []() { return gui.getTimerTotalMinutes(); }
    );

    // ---- Follower devices ---------------------------------------------------
    pump.enableUVC(5, 30);    // UVC: 5 s pre-delay, 30 s post-delay
    pump.enableAux(10, 30);   // AUX heat pump: on after 10 s, off 30 s before pump stops

    // ---- Solar heater with valve --------------------------------------------
    pump.enableSolar(
        getSolarTemp,          // absorber temperature callback
        getPoolTemp,           // pool temperature callback
        true,                  // useValve: open P3 relay when solar is running
        8.0f,                  // startDelta: start when absorber > pool + 8°C
        3.0f,                  // stopDelta:  stop  when absorber < pool + 3°C
        32.0f,                 // maxPoolTemp: cool pool if pool > 32°C
        2.0f,                  // coolDelta: cool when absorber < pool - 2°C
        50.0f                  // safetyTemp: force on if absorber > 50°C (Priority 0)
    );

    // Restrict solar heating to daytime (07:00–20:00)
    // pump.setSolarDayNight([]() { return rtc.getEpoch(); }, 7, 20);

    // ---- Current monitoring -------------------------------------------------
    pump.setCurrentCallback(getCurrent);   // -1.0f until sensor ready — silently inactive

    // ---- Pressure monitoring ------------------------------------------------
    // enablePressure(callback, absoluteMaxBar)
    // pressureCb returns -1.0f until APASENSE is calibrated — all pressure checks skip.
    pump.enablePressure(getPressure, 4.0f);       // absolute max 4 bar
    pump.setPressurePeakAlarm(20);                 // also alarm at +20% above EMA baseline

    // Bridge: tell APASENSE when pump stops so it can re-zero the pressure sensor
    pump.setPumpStateCallback([](bool on) {
        // adc.onPumpState(on);
        Serial.print(F("Pump relay: ")); Serial.println(on ? F("ON") : F("OFF"));
    });

    // ---- Freeze protection --------------------------------------------------
    // Uses pool temperature sensor (same as solar).
    // Dry-run interlock: if pressure EMA confirms no water, freeze pump is suppressed.
    pump.enableFreezeProtection(getPoolTemp);     // threshold = 4.5°C (default)

    // ---- Catch-up window ----------------------------------------------------
    // Keep pump running to make up missed daily target, but only 08:00–20:00.
    pump.setDailyTarget(360);                     // 6-hour daily target
    // pump.setCatchupWindow(8, 20);              // uncomment when RTC is wired
    // pump.setMidnightCallback([]() { return rtc.getEpoch(); });

    // ---- Manual override config --------------------------------------------
    pump.setManualTimeout(120);                   // auto-return after 2 hours
    pump.setManualAutoReset(true);                // also return at midnight

    // ---- Callbacks ----------------------------------------------------------
    pump.setPumpAlarmCallback(onAlarm);
    pump.setStatusCallback(onStatus);

    Serial.println(F("APAPUMP ready — solar + freeze + pressure + shock bridge"));
}

// ---- loop() -----------------------------------------------------------------

void loop() {
    pump.update();

    // Display periodic status on Serial (every 10 s)
    static uint32_t lastPrint = 0;
    if (millis() - lastPrint >= 10000UL) {
        lastPrint = millis();

        Serial.print(F("State: "));
        switch (pump.getState()) {
            case IDLE:     Serial.print(F("IDLE"));     break;
            case STARTING: Serial.print(F("STARTING")); break;
            case RUNNING:  Serial.print(F("RUNNING"));  break;
            case STOPPING: Serial.print(F("STOPPING")); break;
        }
        Serial.print(F("  Daily: "));
        Serial.print(pump.getDailyRuntimeMinutes());
        Serial.print(F(" min"));
        if (pump.isDailyTargetMet()) Serial.print(F(" [TARGET MET]"));
        if (pump.isFreezeActive())   Serial.print(F(" [FREEZE]"));
        if (pump.getAlarm() != PUMP_ALARM_NONE) {
            Serial.print(F("  ALARM: "));
            Serial.print(pump.getAlarm());
        }
        if (pump.isPressureCalibrated()) {
            Serial.print(F("  P="));
            Serial.print(pump.getPressure(), 2);
            Serial.print(F("bar (base="));
            Serial.print(pump.getPressureBaseline(), 2);
            Serial.print(F(")"));
        }
        Serial.println();
    }
}
