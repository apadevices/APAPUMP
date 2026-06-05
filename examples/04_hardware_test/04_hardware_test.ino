// APAPUMP + APALCDGUI — 04_hardware_test
//
// Combined hardware test for the APA Devices pool controller board.
// Tests all 8 relay outputs on the PCF8574AT and exercises every
// APAPUMP feature that can be verified without external sensors.
//
// ---- Relay map --------------------------------------------------------------
//   P0  Pump relay      — APAPUMP auto-managed; toggled via "Manual mode" menu
//   P1  UVC relay       — APAPUMP follower: ON 2 s before pump, OFF 5 s after pump
//   P2  AUX relay       — APAPUMP follower: ON 3 s after pump, OFF 5 s before pump
//   P3  Solar valve     — APAPUMP Priority-0 safety: fires when "Valve test" is ON
//   P4  Extension A     — menu BOOL field, direct toggle
//   P5  Extension B     — menu BOOL field, direct toggle
//   P6  Extension C     — menu BOOL field, direct toggle
//   P7  Extension D     — menu BOOL field, direct toggle
//
// ---- Navigation -------------------------------------------------------------
//   RIGHT knob (KB1) — rotate to change screen
//   LEFT  knob (KB2) — rotate to move cursor, press to edit, press again to confirm
//   SAVE  row        — commits all fields on the screen, fires onSave callback
//   BACK  row        — returns to home screen without committing
//
// ---- Test sequence ----------------------------------------------------------
//   1. HOME screen — watch relay states update in real time (row 3)
//   2. RIGHT → "Pump ctrl" — set Mode to F.ON, press SAVE → P0 on
//      After 2 s: P1 (UVC) stays on from pre-delay
//      After 3 s from pump start: P2 (AUX) turns on
//   3. RIGHT → "Pump ctrl" — set Mode to FOFF, press SAVE
//      P2 turns off → 5 s later P0 turns off → 5 s later P1 turns off
//   4. LEFT → "Relays A" — toggle "Valve test" ON, press SAVE
//      APAPUMP solar safety fires: P0+P3 both turn on (Priority-0, even in FOFF)
//   5. LEFT → "Relays A" — toggle "Valve test" OFF, press SAVE → valve closes
//   6. LEFT → "Relays A/B" — toggle P4–P7 individually
//
// ---- Wiring -----------------------------------------------------------------
//   APA Devices HMI board v1.0 — all default pins, no configuration needed.
//   PCF8574AT at I2C 0x3C  SCL→21(Mega)/A5(Uno)  SDA→20(Mega)/A4(Uno)
//
// ---- Required libraries -----------------------------------------------------
//   APAPUMP    (this library)
//   APALCDGUI  https://github.com/apadevices/APALCDGUI

#include <Wire.h>
#include <APAPUMP.h>
#include <APALCDGUI.h>

// ---- Objects ----------------------------------------------------------------

ApaPump   pump;   // PCF at 0x3C — P0=pump P1=uvc P2=aux P3=valve
APALCDGUI gui;    // APA HMI board v1.0 — all defaults

// ============================================================================
// Menu-bound variables
// ============================================================================

// "Pump ctrl" screen (right side)
static const char* modeChoices[] = { "AUTO", "F.ON", "FOFF", nullptr };
// CHOICE strings must be exactly 4 chars. "AUTO"=4 "F.ON"=4 "FOFF"=4 ✓
uint8_t  modeIdx   = 0;    // 0=AUTO  1=FORCE_ON  2=FORCE_OFF
int16_t  minRunMin = 0;    // minimum run time in minutes (0=none for testing, 1–30 for real use)

// "Relays A" screen (left side)
bool valveTest = false;    // ON → absorber reported 55°C → solar safety fires → P3 opens
bool relayP4   = false;
bool relayP5   = false;

// "Relays B" screen (left side)
bool relayP6 = false;
bool relayP7 = false;

// ============================================================================
// Solar valve test callbacks
// ============================================================================
// When valveTest is ON, absorberTemp() returns 55°C.
// That exceeds the solar safety threshold (50°C default) — Priority 0.
// APAPUMP forces pump ON + valve OPEN regardless of Manual mode.
// Turn valveTest OFF to return to normal priority engine.

float absorberTemp() { return valveTest ? 55.0f : 20.0f; }
float poolTemp()     { return 26.0f; }

// ============================================================================
// Home screen
// ============================================================================
// Row 0: pump state + manual mode (20 chars)
// Row 1: daily runtime + min run time setting (20 chars)
// Row 2: status / alarm text (cols 0–16) | alert indicator (cols 17–19, library-owned)
// Row 3: relay test states — V=P3 valve, 4–7=extension relays (20 chars)

void drawHome(LiquidCrystal& lcd) {
    char buf[21];

    // ---- Row 0: pump state (cols 0–15) + manual mode (cols 16–19) ----------
    lcd.setCursor(0, 0);
    switch (pump.getState()) {
        case IDLE:     lcd.print(F("PUMP IDLE       ")); break;  // 16 chars
        case STARTING: lcd.print(F("PUMP STARTING   ")); break;
        case RUNNING:  lcd.print(F("PUMP RUNNING    ")); break;
        case STOPPING: lcd.print(F("PUMP STOPPING   ")); break;
    }
    lcd.setCursor(16, 0);   // cols 16–19 = manual mode (4 chars)
    switch (pump.getManualMode()) {
        case AUTO:      lcd.print(F("AUTO")); break;
        case FORCE_ON:  lcd.print(F("F.ON")); break;
        case FORCE_OFF: lcd.print(F("FOFF")); break;
    }

    // ---- Row 1: today runtime + min run time --------------------------------
    uint16_t ran = pump.getDailyRuntimeMinutes();
    snprintf(buf, sizeof(buf), "Run %2uh%02um  Set %2um ",
             ran / 60, ran % 60, (uint16_t)minRunMin);
    lcd.setCursor(0, 1); lcd.print(buf);   // "Run  0h00m  Set  5m " = 20 chars

    // ---- Row 2: status (cols 0–16 only; 17–19 overwritten by alert lib) -----
    lcd.setCursor(0, 2);
    if (pump.isFreezeActive()) {
        lcd.print(F("** FREEZE ACTIVE ** "));   // 20 chars
    } else {
        switch (pump.getAlarm()) {
            case PUMP_ALARM_NONE:          lcd.print(F("                    ")); break;
            case PUMP_ALARM_OVERCURRENT:   lcd.print(F("ALARM:Overcurrent   ")); break;
            case PUMP_ALARM_LOW_PRESSURE:  lcd.print(F("ALARM:Dry run       ")); break;
            case PUMP_ALARM_HIGH_PRESSURE: lcd.print(F("ALARM:Hi pressure   ")); break;
            case PUMP_ALARM_NO_FLOW:       lcd.print(F("ALARM:No flow       ")); break;
        }
    }

    // ---- Row 3: relay states — V=valve P4 P5 P6 P7 -------------------------
    // V=1 while pump is RUNNING — valve is physically open during that time.
    // V returns to 0 when pump stops (STOPPING sequence closes the valve relay).
    // P4-P7 show the last value applied via the extension relay screens.
    snprintf(buf, sizeof(buf), "V:%d 4:%d 5:%d 6:%d 7:%d",
             (int)pump.isRunning(),
             (int)relayP4, (int)relayP5, (int)relayP6, (int)relayP7);
    lcd.setCursor(0, 3); lcd.print(buf);   // "V:0 4:0 5:0 6:0 7:0" = 20 chars
}

// ============================================================================
// onSave callbacks
// ============================================================================

void onPumpSave() {
    // Apply manual mode — the CHOICE index maps directly to ManualMode enum values
    switch (modeIdx) {
        case 0: pump.setManualMode(AUTO);      break;
        case 1: pump.setManualMode(FORCE_ON);  break;
        case 2: pump.setManualMode(FORCE_OFF); break;
    }
    pump.setMinRunTime((uint16_t)minRunMin * 60U);   // convert minutes → seconds
}

void onRelaysASave() {
    // valveTest feeds absorberTemp() on the next pump.update() — no explicit call needed.
    pump.setExtraOutput(4, relayP4);
    pump.setExtraOutput(5, relayP5);
}

void onRelaysBSave() {
    pump.setExtraOutput(6, relayP6);
    pump.setExtraOutput(7, relayP7);
}

// ============================================================================
// Alarm callback
// ============================================================================

void onAlarm(PumpAlarm alarm) {
    if (alarm == PUMP_ALARM_NONE) {
        // Alarm was acknowledged — clear APALCDGUI alerts and redraw
        gui.cancelActiveAlert();
        gui.clearAlert();
        gui.markDirty();
        return;
    }

    const __FlashStringHelper* detail;
    switch (alarm) {
        case PUMP_ALARM_OVERCURRENT:   detail = F("Check motor/wiring");  break;
        case PUMP_ALARM_LOW_PRESSURE:  detail = F("Check water/pipes");   break;
        case PUMP_ALARM_HIGH_PRESSURE: detail = F("Check filter/valves"); break;
        case PUMP_ALARM_NO_FLOW:       detail = F("Check flow sensor");   break;
        default:                       detail = F("Check installation");  break;
    }

    // Route to APALCDGUI active alert — blocks home screen, requires KB2 press to ACK
    gui.postActiveAlert(
        F("Pump alarm!"), detail,
        ALERT_CRITICAL,
        []() { pump.acknowledgeAlarm(); }   // KB2 press → acknowledges and clears
    );

    Serial.print(F("ALARM: ")); Serial.println(detail);
}

// ============================================================================
// Status callback
// ============================================================================
// Fires on: manual mode entry/exit, timeouts, solar safety events, freeze events,
// 30-min manual reminders. All message strings live in flash.

void onStatus(const __FlashStringHelper* msg) {
    Serial.println(msg);

    // Keep modeIdx in sync with the actual pump mode
    // (pump may change mode via timeout, midnight reset, or solar safety)
    switch (pump.getManualMode()) {
        case AUTO:      modeIdx = 0; break;
        case FORCE_ON:  modeIdx = 1; break;
        case FORCE_OFF: modeIdx = 2; break;
    }

    // [M] corner indicator while pump is in manual mode (alert takes priority)
    if (pump.getManualMode() != AUTO) gui.setStatusIndicator('M');
    else                              gui.clearStatusIndicator();

    gui.markDirty();   // home screen picks up the new state on next update()
}

// ============================================================================
// setup()
// ============================================================================

void setup() {
    Serial.begin(115200);
    Wire.begin();   // I2C bus — must come before pump.begin()

    // ---- APALCDGUI init -----------------------------------------------------
    gui.begin();    // all defaults match APA HMI board v1.0
    gui.addHomeScreen(drawHome);

    // RIGHT side — pump control + timer schedule
    gui.addScreen(SCREEN_RIGHT,
        APALCDGUI::fieldChoice(F("Manual mode"),  &modeIdx,   modeChoices),
        APALCDGUI::fieldInt(   F("Min run time"), F("m "), &minRunMin, 0, 30, 1),
        onPumpSave
    );
    // Timer schedule screen — set up to 3 on/off slots per day.
    // Cosmetic in this test (no RTC wired). Wire up scheduleCb in begin() for real use.
    gui.addTimerScreen(SCREEN_RIGHT, nullptr);

    // LEFT side — relay test controls
    gui.addScreen(SCREEN_LEFT,
        APALCDGUI::fieldBool(F("Valve test"),  &valveTest),  // P3 via solar safety
        APALCDGUI::fieldBool(F("Relay P4"),    &relayP4),
        APALCDGUI::fieldBool(F("Relay P5"),    &relayP5),
        onRelaysASave
    );
    gui.addScreen(SCREEN_LEFT,
        APALCDGUI::fieldBool(F("Relay P6"),    &relayP6),
        APALCDGUI::fieldBool(F("Relay P7"),    &relayP7),
        onRelaysBSave
    );

    // ---- APAPUMP init -------------------------------------------------------
    pump.begin(nullptr, nullptr, nullptr);   // no schedule in this test — manual only

    // Followers — short delays so state transitions are easy to observe
    pump.enableUVC(2, 5);    // P1: UVC on 2 s before pump; off 5 s after pump stops
    pump.enableAux(3, 5);    // P2: AUX on 3 s after pump starts; off 5 s before pump stops

    // Solar with valve enabled — P3 is exercised via the "Valve test" menu field.
    // When valveTest = true: absorberTemp() returns 55°C → exceeds safetyTemp (50°C)
    // → Priority-0 override fires: pump ON + valve OPEN (beats even FORCE_OFF).
    pump.enableSolar(
        absorberTemp,           // absorber temp callback
        poolTemp,               // pool temp callback
        /*useValve=*/ true      // enable P3 (solar valve) as follower
    );

    pump.setMinRunTime(0);        // no minimum run time — relays respond immediately (test mode)
    pump.setManualTimeout(120);   // auto-return to AUTO after 2 hours if forgotten

    pump.setPumpAlarmCallback(onAlarm);
    pump.setStatusCallback(onStatus);

    // ---- Serial test log ----------------------------------------------------
    Serial.println(F("=== APAPUMP + APALCDGUI hardware test ==="));
    Serial.println(F("Relay map:"));
    Serial.println(F("  P0 Pump      P1 UVC      P2 AUX     P3 Valve"));
    Serial.println(F("  P4 Ext-A     P5 Ext-B    P6 Ext-C   P7 Ext-D"));
    Serial.println(F("Test steps:"));
    Serial.println(F("  RIGHT → Pump ctrl → Mode:F.ON → SAVE  (P0 on; P1 pre, P2 follows)"));
    Serial.println(F("  RIGHT → Pump ctrl → Mode:FOFF → SAVE  (P2 off → P0 off → P1 off)"));
    Serial.println(F("  LEFT  → Relays A  → Valve test:ON → SAVE  (P0+P3 via solar safety)"));
    Serial.println(F("  LEFT  → Relays A  → Valve test:OFF → SAVE (valve closes)"));
    Serial.println(F("  LEFT  → Relays A/B → toggle P4-P7"));
}

// ============================================================================
// loop()
// ============================================================================

void loop() {
    gui.update();    // process encoders, redraw LCD
    pump.update();   // advance state machine, check safety guards
}
