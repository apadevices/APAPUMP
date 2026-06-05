// APAPUMP — 02_scheduler  (intermediate)
//
// Pool pump with:
//   • Schedule driven by APALCDGUI timer screen (3 on/off slots)
//   • UVC lamp and AUX heat pump as follower devices
//   • Alarm feedback routed to APALCDGUI active alert
//   • [M] status indicator in the LCD corner during manual override
//   • Daily runtime display on the LCD home screen
//   • Midnight rollover via DS3231 RTC
//
// Wiring:
//   PCF8574AT at 0x3C  — pump=P0, uvc=P1, aux=P2
//   DS3231 RTC         — SDA/SCL shared I2C bus
//   APALCDGUI          — see APALCDGUI examples for pin mapping
//
// Required libraries:  APAPUMP, APALCDGUI, DS3231, LiquidCrystal

#define APA_LCD_USE_DS3231          // compile in DS3231 RTC modal
#include <Wire.h>
#include <APAPUMP.h>
#include <APALCDGUI.h>

// ---- Objects -----------------------------------------------------------------

ApaPump   pump;
APALCDGUI gui;
DS3231    rtc;

// ---- Schedule helper ---------------------------------------------------------
// Returns true when current time falls inside any enabled timer slot.
// Called every update() — keep it fast (no I2C here; use a cached time variable).

uint8_t  rtcHour = 0, rtcMinute = 0;   // refreshed in loop()

bool scheduleActive() {
    uint16_t nowMin = rtcHour * 60 + rtcMinute;
    for (uint8_t i = 0; i < APA_LCD_MAX_TIMERS; i++) {
        if (gui.isTimerEnabled(i) &&
            nowMin >= gui.getTimerStart(i) &&
            nowMin <  gui.getTimerEnd(i)) return true;
    }
    return false;
}

// ---- Home screen -------------------------------------------------------------

void drawHome(LiquidCrystal& lcd) {
    // Row 0: time and pump state
    char buf[21];
    snprintf(buf, sizeof(buf), "%02u:%02u  Pump: %-6s",
             rtcHour, rtcMinute,
             pump.isRunning() ? "ON" : "OFF");
    lcd.setCursor(0, 0); lcd.print(buf);

    // Row 1: daily runtime vs target
    uint16_t ran    = pump.getDailyRuntimeMinutes();
    uint16_t target = gui.getTimerTotalMinutes();
    snprintf(buf, sizeof(buf), "Run:%3uh%02um / %3um",
             ran / 60, ran % 60, target);
    lcd.setCursor(0, 1); lcd.print(buf);

    // Row 2: free for user (cols 0-16; 17-19 = library alert indicator)
    lcd.setCursor(0, 2);
    lcd.print(pump.isFreezeActive() ? F("FREEZE PROTECT      ") : F("                    "));

    // Row 3: free (cols 0-16; 17-19 = page indicator if multi-page)
    lcd.setCursor(0, 3); lcd.print(F("                    "));
}

// ---- Alarm callback ----------------------------------------------------------

void onAlarm(PumpAlarm alarm) {
    if (alarm == PUMP_ALARM_NONE) {
        // Alarm cleared — remove the active alert
        gui.cancelActiveAlert();
        gui.clearAlert();
        return;
    }

    const __FlashStringHelper* detail = nullptr;
    switch (alarm) {
        case PUMP_ALARM_OVERCURRENT:   detail = F("Overcurrent");   break;
        case PUMP_ALARM_LOW_PRESSURE:  detail = F("Dry run");       break;
        case PUMP_ALARM_HIGH_PRESSURE: detail = F("High pressure"); break;
        case PUMP_ALARM_NO_FLOW:       detail = F("No flow");       break;
        default: break;
    }

    // Show an active alert — operator must press KB2 to acknowledge
    gui.postActiveAlert(
        F("Pump alarm!"),
        detail,
        ALERT_CRITICAL,
        []() { pump.acknowledgeAlarm(); }
    );
}

// ---- Status callback ---------------------------------------------------------

void onStatus(const __FlashStringHelper* msg) {
    // Forward to Serial
    Serial.println(msg);

    // Update [M] corner indicator when manual mode changes
    if (pump.getManualMode() != AUTO) gui.setStatusIndicator('M');
    else                              gui.clearStatusIndicator();
}

// ---- setup() -----------------------------------------------------------------

void setup() {
    Serial.begin(115200);
    Wire.begin();

    // GUI must be initialised before pump so timer callbacks are ready
    gui.begin();
    gui.setRTC(&rtc);                            // enable RTC modal (both-buttons hold)
    gui.addHomeScreen(drawHome);
    gui.addTimerScreen(SCREEN_RIGHT, nullptr);   // timer screen on right side

    // Pump: schedule from APALCDGUI timers, daily target from timer sum
    pump.begin(
        scheduleActive,
        nullptr,
        []() { return gui.getTimerTotalMinutes(); }
    );
    pump.enableUVC(5, 30);                       // UVC: 5 s pre, 30 s post
    pump.enableAux(10, 30);                      // AUX: on 10 s after pump, off 30 s before pump

    pump.setManualTimeout(120);                  // auto-return to AUTO after 2 hours

    pump.setPumpAlarmCallback(onAlarm);
    pump.setStatusCallback(onStatus);

    // Midnight callback — drives daily counter rollover and manual-auto-reset
    pump.setMidnightCallback([]() { return rtc.getEpoch(); });
    pump.setManualAutoReset(true);               // return to AUTO at midnight
}

// ---- loop() ------------------------------------------------------------------

// Refresh RTC time once per second so schedule checks are current
static uint32_t lastRtcMs = 0;

void loop() {
    if (millis() - lastRtcMs >= 1000UL) {
        lastRtcMs = millis();
        RTClib::now().hour();   // read via DS3231
        // Replace with your RTC library's time access:
        // DateTime now = rtc.now(); rtcHour = now.hour(); rtcMinute = now.minute();
    }

    gui.update();
    pump.update();
}
