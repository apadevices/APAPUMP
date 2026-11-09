// APAPUMP — 03_solar_freeze  (advanced)
//
// Full APA pool automation system:
//   • Solar heater with valve (absorber → pool temperature control), extra runs on top of the schedule
//   • Heat pump on the AUX relay (HEATER_SIMPLE: solar first, its own thermostat regulates)
//   • Freeze protection with dry-run interlock
//   • Pressure monitoring: dry-run + overpressure alarms, filter status (backwash reminder)
//   • Current monitoring: overcurrent alarm (direct-drive pumps only — see README)
//   • Pump stopped on any alarm until the operator acknowledges it
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
// Replace these with your sensor calls. Temperatures return -127.0f (DS18B20 "no sensor")
// until valid — APAPUMP never uses an invalid temperature (-20..100 °C, not 85.0, not -1.0).
// Pressure/current return -1.0f until calibrated — APAPUMP skips that feature.

float getSolarTemp()  { return -127.0f; } // absorber temperature (°C)
float getPoolTemp()   { return -127.0f; } // pool / freeze sensor (°C)
float getPressure()   { return -1.0f; }   // pipe pressure (bar)   — -1 = not calibrated
float getCurrent()    { return -1.0f; }   // pump current (A)      — -1 = not ready

// ---- APADOSE bridge ---------------------------------------------------------
// Post-shock: keep the pump running during a shock and for 4 hours after it, to
// circulate the chlorine. Replace the commented line with the real APADOSE call.
// Elapsed-time check (millis() - start), so it stays correct when millis() wraps.

const uint32_t POST_SHOCK_MS = 4UL * 3600UL * 1000UL;
uint32_t postShockEndMs = 0;   // 0 = no post-shock window

bool externalPumpRequest() {
    bool shockActive = false;  // = dose1.isShockActive();
    if (shockActive) {
        postShockEndMs = millis() | 1;   // |1: never 0, the "no window" marker
        return true;
    }
    if (postShockEndMs == 0) return false;
    if (millis() - postShockEndMs < POST_SHOCK_MS) return true;
    postShockEndMs = 0;
    return false;
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
        case PUMP_ALARM_HIGH_PRESSURE: Serial.println(F("High pressure — check valves/filter")); break;
        case PUMP_ALARM_NO_FLOW:       Serial.println(F("No flow — check valve/pipe")); break;
        default: break;
    }
    // The library does NOT stop the pump — loop() keeps it off until the operator
    // has checked it and calls pump.acknowledgeAlarm() (here: send 'a' on Serial).
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
    pump.enableAux(10, 120);  // AUX: on 10 s after the pump, pump runs 120 s after it (heat-pump after-run)
    // The relay switches the heat pump's remote ON/OFF input (or a fused contactor coil) —
    // never the compressor itself. HEATER_SIMPLE: the heat pump's own thermostat regulates,
    // APAPUMP adds solar first, no heating during a pump alarm, 10 min min ON/OFF.
    // First boot only — the owner's later choice is saved and survives a reboot.
    if (!pump.hasSavedSettings()) pump.setAuxMode(HEATER_SIMPLE);

    // ---- Solar heater with valve --------------------------------------------
    // The schedule keeps driving the pump; solar heating/cooling are extra runs.
    // Overheat protection (safetyTemp) runs the pump only in AUTO, never on a pump alarm.
    pump.enableSolar(
        getSolarTemp,          // absorber temperature callback
        getPoolTemp,           // pool temperature callback
        true,                  // useValve: open P3 relay when solar is running
        8.0f,                  // startDelta: start when absorber > pool + 8°C
        3.0f,                  // stopDelta:  stop  when absorber < pool + 3°C
        32.0f,                 // maxPoolTemp: cool pool if pool > 32°C
        2.0f,                  // coolDelta: cool when absorber < pool - 2°C
        60.0f                  // safetyTemp: overheat protection above 60°C (AUTO only)
    );
    // useValve and maxPoolTemp are first-boot defaults. Later changes (e.g. from a menu):
    // pump.setSolarMode(SOLAR_OFF / SOLAR_PUMP / SOLAR_VALVE); pump.setSolarMaxPool(30);

    // Heating only 06:00–21:00. A safety net for a collector sensor placed in a pipe near
    // the pump room: it can read "warm" at night and the pump would then cool the pool.
    // Cooling (pool > maxPoolTemp) stays temperature-based, day or night.
    // pump.setSolarDayNight([]() { return RTClib::now().unixtime(); }, 6, 21);

    // ---- Current monitoring -------------------------------------------------
    // Learns the pump's own normal current; no rating to enter. Below 0.5 A it stays
    // silent — e.g. when the board only switches an external contactor's coil.
    pump.setCurrentCallback(getCurrent);   // -1.0f until sensor ready — silently inactive

    // ---- Pressure monitoring ------------------------------------------------
    // enablePressure(callback, absoluteMaxBar)
    // pressureCb returns -1.0f until APASENSE is calibrated — all pressure checks skip.
    pump.enablePressure(getPressure, 2.5f);       // max = your filter's rated pressure (label)
    // pump.setPressurePeakAlarm(20);             // optional: SUDDEN rise > 20 % (closed valve).
    //                                            // Turning a multiport valve while running trips it.

    // Filter status (no alarm): the clean pressure is learned automatically on the
    // first run; after every backwash call pump.learnCleanPressure() (here: send 'l').

    // Bridge: tell APASENSE when pump stops so it can re-zero the pressure sensor
    pump.setPumpStateCallback([](bool on) {
        // adc.onPumpState(on);
        Serial.print(F("Pump relay: ")); Serial.println(on ? F("ON") : F("OFF"));
    });

    // ---- Freeze protection --------------------------------------------------
    // Uses pool temperature sensor (same as solar). Works below 0 °C; a missing reading
    // never ends an active freeze protection (it ends at threshold + 1 °C).
    // Dry-run interlock: if pressure EMA confirms no water, freeze pump is suppressed.
    pump.enableFreezeProtection(getPoolTemp);     // threshold = 4.5°C (default)

    // ---- Daily runtime ------------------------------------------------------
    pump.setDailyTarget(360);                     // 6-hour daily target (runtime counter)
    // pump.setMidnightCallback([]() { return RTClib::now().unixtime(); });  // real midnight
    // 1.2.0 style (solar is the only driver + catch-up 08:00–20:00) instead of the schedule:
    // pump.setSolarDrive(SOLAR_WITH_CATCHUP); pump.setCatchupWindow(8, 20);

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

    // Stop the pump while an alarm is active. Re-asserted every loop: FORCE_OFF alone
    // would return to AUTO after the manual timeout and restart e.g. a dry pump.
    if (pump.getAlarm() != PUMP_ALARM_NONE && pump.getManualMode() != FORCE_OFF)
        pump.setManualMode(FORCE_OFF);

    if (Serial.available()) {
        char c = Serial.read();
        if (c == 'a') { pump.acknowledgeAlarm(); pump.setManualMode(AUTO); }  // after checking
        if (c == 'l') pump.learnCleanPressure();                            // after a backwash
    }

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
        if (pump.isSolarRunning())   Serial.print(F(" [SOLAR]"));
        if (pump.isSolarValveOpen()) Serial.print(F(" [VALVE]"));
        if (pump.isSolarSafetyActive()) Serial.print(F(" [HOT]"));
        if (!pump.isSolarSensorOk()) Serial.print(F(" [SOLAR SENSOR]"));
        if (pump.isHeaterOn())       Serial.print(F(" [HEATER]"));
        if (pump.getAlarm() != PUMP_ALARM_NONE) {
            Serial.print(F("  ALARM: "));
            Serial.print(pump.getAlarm());
        }
        if (pump.isPressureCalibrated()) {
            Serial.print(F("  P="));
            Serial.print(pump.getPressure(), 2);
            Serial.print(F("bar (base="));
            Serial.print(pump.getPressureBaseline(), 2);
            Serial.print(F(") filter: "));
            switch (pump.getFilterStatus()) {
                case FILTER_CLEAN:           Serial.print(F("clean"));         break;
                case FILTER_FILLING:         Serial.print(F("filling"));       break;
                case FILTER_BACKWASH_NEEDED: Serial.print(F("BACKWASH NEEDED")); break;
                default:                     Serial.print(F("learning"));      break;
            }
        }
        Serial.println();
    }
}
