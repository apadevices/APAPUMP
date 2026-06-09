// APAPUMP — Pool filtration pump controller for APA Devices pool automation
//
// Controls the main filtration pump and optional followers (UVC lamp, AUX device,
// solar valve) via a PCF8574AT I2C relay expander or direct Arduino output pins.
// Designed to integrate with APADOSE (dosing) and APALCDGUI (HMI display).
//
// Minimal usage (APA HMI board v1.0 with PCF8574AT):
//   ApaPump pump;                         // 1. global — PCF at 0x3C, APA defaults
//   void setup() {                        // 2. setup
//       Wire.begin();                     //    required for PCF mode
//       pump.begin();                     //    init relays, all OFF
//   }
//   void loop() { pump.update(); }        // 3. loop — non-blocking state machine
//
// Direct GPIO mode:
//   ApaPump pump(RELAY_DIRECT, 7);        // pump relay on pin 7
//   void setup() { pump.begin(); }
//
// Manual override:
//   pump.setManualMode(FORCE_ON);         // force pump on (e.g. vacuuming)
//   pump.setManualMode(AUTO);             // return to automatic control

#pragma once

#include <Arduino.h>
#include <EEPROM.h>
#include <Wire.h>

// ---- Version ----------------------------------------------------------------
#define APAPUMP_VERSION "1.0.1"

// ---- EEPROM base address (12 bytes: 520–531) --------------------------------
// APA library address map — do not overlap these ranges:
//
//   0 –127   free  (128 bytes)
//   128–177  APAPHX2_ADS1115        50 bytes
//   178–188  free  (11 bytes)
//   189–191  APADOSE global          3 bytes  [pool volume, dead-band]
//   192–279  APADOSE per-pump       88 bytes  [22 B × up to 4 instances]
//   280–499  free  (220 bytes)
//   500–501  APALCDGUI brightness    2 bytes
//   502–508  APALCDGUI timers        7 bytes  [default 3 slots; up to 13 B with MAX_TIMERS=6 → ends 514]
//   509–519  free  (11 bytes min, 5 bytes min when MAX_TIMERS=6)
//   520–531  APAPUMP                12 bytes  ← this library
//   532–    free
//
// Override before #include if your project has a conflict:
//   #define APAPUMP_EEPROM_ADDR  532
//   #include <APAPUMP.h>
#ifndef APAPUMP_EEPROM_ADDR
#define APAPUMP_EEPROM_ADDR 520
#endif

// ---- Constants --------------------------------------------------------------
// APAPUMP_CURRENT_SAMPLE_MS: EMA baseline updated every 10 s while pump is running
// and past the inrush settle window. Overcurrent detection runs on every update()
// tick using the last sampled value. Documented so advanced users can reason about
// response time vs. baseline stability.
constexpr uint16_t APAPUMP_MIN_OFF_SEC        = 60;    // minimum pause after pump stops (non-manual)
constexpr uint16_t APAPUMP_MIN_RUN_SEC        = 300;   // default minimum run time before stopping
constexpr uint16_t APAPUMP_VALVE_PULSE_MS     = 500;   // default pulse width for VALVE_PULSE mode
constexpr uint32_t APAPUMP_CURRENT_SAMPLE_MS  = 10000; // EMA sample interval (current + pressure EMA)
constexpr uint16_t APAPUMP_CURRENT_SETTLE_SEC = 30;    // seconds after pump-on before EMA sampling starts

// Phase 2 — pressure safety and freeze protection
constexpr float    APAPUMP_FREEZE_THRESHOLD_C  = 4.5f; // freeze protection activates below this pool temp (°C)
constexpr uint16_t APAPUMP_FREEZE_ON_SEC       = 300;  // freeze cycle: pump run duration (5 min)
constexpr uint16_t APAPUMP_FREEZE_OFF_SEC      = 600;  // freeze cycle: rest between runs (10 min)
constexpr uint8_t  APAPUMP_PRESSURE_DRYRUN_PCT = 40;   // pressure must be >= this % of EMA baseline to confirm flow
constexpr float    APAPUMP_PRESSURE_ABS_MIN    = 0.1f; // absolute min pressure (bar) used before EMA is built

// ---- Enums ------------------------------------------------------------------

/** Relay driver mode — selects PCF8574AT I2C expander or direct Arduino GPIO. */
enum RelayDriver  : uint8_t { RELAY_DIRECT, RELAY_PCF };

/** Manual override mode.
 *  AUTO:      priority engine decides (schedule, solar, catch-up).
 *  FORCE_ON:  pump forced on, guards suspended, solar valve closed.
 *  FORCE_OFF: pump forced off, guards suspended; solar safety override still applies. */
enum ManualMode   : uint8_t { AUTO, FORCE_ON, FORCE_OFF };

/** Pump state machine state. */
enum PumpState    : uint8_t { IDLE, STARTING, RUNNING, STOPPING };

/** Solar valve operating mode. */
enum ValveMode    : uint8_t { VALVE_SUSTAINED, VALVE_PULSE };

/** Filter status based on pressure delta from clean baseline. */
enum FilterStatus : uint8_t { FILTER_UNKNOWN, FILTER_CLEAN, FILTER_FILLING, FILTER_BACKWASH_NEEDED };

/** Pump alarm types — all are latching (require acknowledgeAlarm()).
 *  OVERCURRENT: current > 1.5× learned baseline.
 *  LOW_PRESSURE: dry-run — pressure too low for a running pump.
 *  HIGH_PRESSURE: filter dirty — pressure significantly above learned baseline.
 *  NO_FLOW: flow switch reports no flow after settle (hardware stub). */
enum PumpAlarm    : uint8_t {
    PUMP_ALARM_NONE,
    PUMP_ALARM_OVERCURRENT,
    PUMP_ALARM_LOW_PRESSURE,
    PUMP_ALARM_HIGH_PRESSURE,
    PUMP_ALARM_NO_FLOW
};

// ---- Main class -------------------------------------------------------------

class ApaPump {
public:

    // ---- Constructors -------------------------------------------------------

    /** Zero-arg: PCF8574AT at 0x3C, pump=P0, uvc=P1, aux=P2, valve=P3.
     *  Targets APA Devices HMI board v1.0 — no arguments needed. */
    ApaPump();

    /** Configurable constructor.
     *  RELAY_DIRECT — a: pump pin; b/c/d: uvc/aux/valve pins (0xFF = not wired); e: unused.
     *  RELAY_PCF    — a: 7-bit I2C address; b: pump bit; c/d/e: uvc/aux/valve bits (0xFF = unused).
     *  manualTimeoutMin: auto-return to AUTO after N minutes in manual mode (0 = never). */
    ApaPump(RelayDriver mode, uint8_t a,
            uint8_t  b = 0xFF, uint8_t c = 0xFF,
            uint8_t  d = 0xFF, uint8_t e = 0xFF,
            uint16_t manualTimeoutMin = 0);

    // ---- Core init ----------------------------------------------------------

    /** Initialise relay outputs — all relays OFF.
     *  PCF mode: writes 0x00 to PCF immediately. Call Wire.begin() before begin().
     *  scheduleCb:    returns true when a scheduler timer slot is active.
     *                 Used as on/off driver when solar is disabled.
     *  externalRequestCb: returns true when an external source requests the pump on.
     *  dailyTargetCb: returns total scheduled minutes per day.
     *                 Bridge: pump.begin(..., []() { return gui.getTimerTotalMinutes(); }) */
    void begin(bool     (*scheduleCb)()        = nullptr,
               bool     (*externalRequestCb)() = nullptr,
               uint16_t (*dailyTargetCb)()     = nullptr);

    /** Run all pump logic. Must be called every loop() — never blocks, never calls delay(). */
    void update();

    // ---- Relay driver -------------------------------------------------------

    /** Configure relay modules energised by LOW signal. Call before begin().
     *  PCF mode: inverts all bit writes (PCF LOW = relay ON, HIGH = relay OFF).
     *  DIRECT mode: drives GPIO LOW to energise the relay.
     *  The zero-arg constructor (APA HMI board v1.0) sets this automatically —
     *  the 74HC240 inverting buffer makes the board active-low. */
    void setActiveLow();

    // ---- Extension board outputs (PCF P4–P7) --------------------------------

    /** Control an extension relay on PCF bits 4–7 (optional lights/equipment board).
     *  Bit state is tracked inside _pcfState — preserved on every APAPUMP relay write.
     *  No-op if pcfBit is outside 4–7 range or in DIRECT mode. */
    void setExtraOutput(uint8_t pcfBit, bool on);

    /** Returns the current commanded state of an extension relay (PCF bits 4–7). */
    bool getExtraOutput(uint8_t pcfBit) const;

    // ---- Manual mode --------------------------------------------------------

    /** Set manual override mode.
     *  FORCE_ON/FORCE_OFF suspend all guards and the priority engine.
     *  Solar safety override (absorber > safetyTemp) still applies even in FORCE_OFF. */
    void       setManualMode(ManualMode m);
    ManualMode getManualMode() const;

    /** Auto-return to AUTO after N minutes in manual mode. 0 = never. Also set in constructor. */
    void setManualTimeout(uint16_t minutes);

    /** Auto-return to AUTO at midnight when setMidnightCallback() is registered. */
    void setManualAutoReset(bool enabled);

    // ---- State query --------------------------------------------------------

    /** Returns true when the pump relay is currently energised. */
    bool      isRunning() const;

    /** Returns the current pump state machine state. */
    PumpState getState() const;

    /** Returns true when freeze protection is currently forcing the pump on. */
    bool isFreezeActive() const;

    /** Returns the learned normal-running pressure baseline (bar).
     *  0.0 until at least 5 pump runs have been sampled. Use for HMI display. */
    float getPressureBaseline() const;

    // ---- Callbacks ----------------------------------------------------------

    /** Fires on every pump relay state change (true = ON, false = OFF). */
    void setPumpStateCallback(void (*cb)(bool running));

    /** Fires when a pump alarm condition is detected.
     *  All alarms except PUMP_ALARM_NONE are latching — call acknowledgeAlarm() to clear.
     *  Fires with PUMP_ALARM_NONE after acknowledgeAlarm() to confirm the clear. */
    void setPumpAlarmCallback(void (*cb)(PumpAlarm alarm));
    void acknowledgeAlarm();

    /** Returns the currently active alarm type (PUMP_ALARM_NONE when no alarm). */
    PumpAlarm getAlarm() const;

    /** Fires status and reminder messages:
     *  manual mode entry/exit, 30-min manual reminders, solar safety events. */
    void setStatusCallback(void (*cb)(const __FlashStringHelper* msg));

    // ---- Pump timing config -------------------------------------------------

    /** Minimum time the pump must run before it can stop (default APAPUMP_MIN_RUN_SEC = 300 s). */
    void setMinRunTime(uint16_t seconds);

    // ---- Optional followers -------------------------------------------------

    /** UVC sanitizer relay (PCF P1 or DIRECT pin b):
     *  ON preDelayS before pump starts, OFF postDelayS after pump stops. */
    void enableUVC(uint16_t preDelayS = 5, uint16_t postDelayS = 30);

    /** AUX device relay (PCF P2 or DIRECT pin c — heat pump, ozone, etc.):
     *  ON startDelayS after pump starts, OFF stopLeadS before pump stops. */
    void enableAux(uint16_t startDelayS = 10, uint16_t stopLeadS = 30);

    // ---- Optional solar heater ----------------------------------------------

    /** Enable solar heating control.
     *  When enabled, solar logic is the sole pump on/off driver.
     *  scheduleCb (from begin()) is used only for daily target — not for on/off.
     *  Dead band: pump starts when absorber > pool + startDelta,
     *             stops when absorber < pool + stopDelta (startDelta - stopDelta = hysteresis window).
     *  Solar safety (Priority 0): absorber > safetyTemp forces pump ON + valve OPEN
     *  regardless of ManualMode. Auto-clears when absorber cools. */
    void enableSolar(
        float    (*solarTempCb)(),
        float    (*poolTempCb)()    = nullptr,
        bool     useValve           = false,
        float    startDelta         = 8.0f,
        float    stopDelta          = 3.0f,
        float    maxPoolTemp        = 0.0f,
        float    coolDelta          = 2.0f,
        float    safetyTemp         = 50.0f,
        ValveMode valveMode         = VALVE_SUSTAINED,
        uint16_t pulseDurationMs    = 500
    );

    /** Enable RTC-aware day/night boundary for solar logic (requires Unix epoch callback). */
    void setSolarDayNight(uint32_t (*epochCb)(),
                          uint8_t  dayStartHour   = 7,
                          uint8_t  nightStartHour = 20);

    // ---- Optional current monitoring (overcurrent protection) ---------------

    /** Register a current reading callback (APASENSE: adc.getCurrent()).
     *  EMA-learned baseline (alpha = 0.05). Cold-start gate: 5 samples before alarm arms.
     *  EMA preserved across pump cycles — reset only via resetCurrentBaseline().
     *  Overcurrent detection: current > baseline × 1.5 → PUMP_ALARM_OVERCURRENT (latching).
     *  EMA updated every APAPUMP_CURRENT_SAMPLE_MS (10 s); alarm checked every update() tick. */
    void setCurrentCallback(float (*cb)());

    /** Reset the learned current baseline — call after pump service or replacement. */
    void resetCurrentBaseline();

    // ---- Optional pressure sensor -------------------------------------------

    /** Register a calibrated pressure callback (APASENSE: adc.getPressure()).
     *  pressureCb must return -1.0f when APASENSE is not yet calibrated.
     *  APAPUMP skips all pressure-based logic until pressureCb returns >= 0.0.
     *  Required init order: adc.begin() BEFORE pump.begin().
     *  Trigger pressure zero-cal bridge: pump.setPumpStateCallback(
     *      [](bool on){ if (!on) adc.onPumpState(false); }); */
    void enablePressure(float (*pressureCb)(), float maxPressure);

    /** Returns the last calibrated pressure reading. 0.0 before first valid read. */
    float getPressure() const;

    /** Returns true once pressureCb has returned a value >= 0.0 (APASENSE calibrated). */
    bool  isPressureCalibrated() const;

    /** Filter status reporting — deferred until pressure baseline is verified in field.
     *  Uses the manually learned clean-filter pressure (learnCleanPressure / setCleanPressure).
     *  Currently always returns FILTER_UNKNOWN. */
    void         setFilterThresholds(float warningDelta, float backwashDelta);
    void         learnCleanPressure();
    void         setCleanPressure(float bar);
    float        getCleanPressure() const;
    FilterStatus getFilterStatus() const;

    // ---- Filtration tracking ------------------------------------------------

    /** Set daily pump runtime target in minutes. Overrides dailyTargetCb from begin().
     *  0 = use callback. Catch-up logic activates when solar is enabled AND target is set. */
    void     setDailyTarget(uint16_t minutes);
    uint16_t getDailyRuntimeMinutes() const;
    bool     isDailyTargetMet() const;
    void     resetDailyCounter();

    /** Register a Unix epoch callback for real midnight reset.
     *  Without it: millis() 24-hour rollover is used instead. */
    void setMidnightCallback(uint32_t (*epochCb)());

    // ---- Phase 2: freeze protection, pressure safety, flow, catch-up --------

    /** Enable freeze protection: pump cycles ON/OFF when pool temp < thresholdC.
     *  Cycle: APAPUMP_FREEZE_ON_SEC on, APAPUMP_FREEZE_OFF_SEC rest (default 5 min / 10 min).
     *  Dry-run interlock: if pressure is enabled and calibrated and EMA shows no flow,
     *  freeze protection is suppressed (pipes drained — forcing pump on would damage motor).
     *  tempCb returns °C; return -1.0f when sensor not ready (protection stays inactive). */
    void enableFreezeProtection(float (*tempCb)(),
                                float thresholdC = APAPUMP_FREEZE_THRESHOLD_C);

    /** Enable overpressure alarm at peakPct % above the learned pressure EMA baseline.
     *  Example: setPressurePeakAlarm(20) fires PUMP_ALARM_HIGH_PRESSURE when pressure
     *  exceeds EMA × 1.20. Requires enablePressure(). 0 = disable. */
    void setPressurePeakAlarm(uint8_t peakPct = 20);

    /** Register a flow switch callback (hardware stub — for future installation).
     *  cb returns true when flow is confirmed. PUMP_ALARM_NO_FLOW fires if false
     *  after the settle window. Pass nullptr to disable. */
    void setFlowCallback(bool (*cb)());

    /** Restrict daily catch-up runtime to a specific hour window (requires setMidnightCallback()).
     *  Example: setCatchupWindow(8, 20) allows catch-up only between 08:00 and 20:00.
     *  Window wraps midnight if startHour > endHour. Silently ignored without RTC. */
    void setCatchupWindow(uint8_t startHour, uint8_t endHour);

private:
    // ---- Relay hardware -----------------------------------------------------
    uint8_t _id[4];       // relay resource IDs: [0]=pump [1]=uvc [2]=aux [3]=valve
                          // DIRECT: Arduino pin numbers | PCF: bit indices (P0–P7)
    uint8_t _pcfAddr;     // PCF 7-bit I2C address (PCF mode only)
    uint8_t _pcfState;    // current PCF byte: HIGH bit = relay ON; 0x00 = all OFF

    // ---- Packed flags (3 bytes) ---------------------------------------------
    struct {
        // byte 1
        uint8_t ready             : 1;  // true after begin()
        uint8_t usePcf            : 1;  // true = PCF I2C mode, false = DIRECT GPIO
        uint8_t activeLow         : 1;  // DIRECT mode: relay energised by LOW
        uint8_t uvcEnabled        : 1;
        uint8_t auxEnabled        : 1;
        uint8_t solarEnabled      : 1;
        uint8_t solarValveEnabled : 1;  // useValve=true in enableSolar()
        uint8_t pressureEnabled   : 1;
        // byte 2
        uint8_t currentEnabled    : 1;
        uint8_t solarDayNight     : 1;
        uint8_t midnightEnabled   : 1;
        uint8_t manualAutoReset   : 1;
        uint8_t alarmActive       : 1;
        uint8_t solarSafetyActive : 1;  // solar safety override (Priority 0) in effect
        uint8_t baselineReady     : 1;  // current EMA cold-start gate passed
        uint8_t dailyTargetMet    : 1;
        // byte 3
        uint8_t solarRunning        : 1;  // solar logic is currently commanding pump on (hysteresis)
        uint8_t auxRunning          : 1;  // AUX relay is currently on
        uint8_t freezeEnabled       : 1;
        uint8_t freezeActive        : 1;  // freeze protection currently forcing pump on
        uint8_t freezeCycleOn       : 1;  // freeze cycle: currently in run phase (not rest)
        uint8_t pressurePeakEnabled : 1;  // overpressure alarm armed
        uint8_t flowEnabled         : 1;  // flow switch callback registered
        uint8_t catchupWindow       : 1;  // catch-up restricted to a time window
    } _flags;

    uint8_t  _relayBits;     // DIRECT mode: bit mask tracking relay states (bit 0–3 = pump/uvc/aux/valve)

    // ---- State --------------------------------------------------------------
    ManualMode _manualMode;
    PumpState  _pumpState;
    PumpAlarm  _activeAlarm;

    // ---- Callbacks ----------------------------------------------------------
    bool     (*_scheduleCb)();
    bool     (*_externalRequestCb)();
    uint16_t (*_dailyTargetCb)();
    void     (*_stateChangeCb)(bool);
    void     (*_alarmCb)(PumpAlarm);
    void     (*_statusCb)(const __FlashStringHelper*);
    float    (*_solarTempCb)();
    float    (*_poolTempCb)();
    uint32_t (*_solarEpochCb)();
    uint32_t (*_midnightCb)();
    float    (*_pressureCb)();
    float    (*_currentCb)();

    // ---- Timing config ------------------------------------------------------
    uint16_t _minRunTimeSec;
    uint16_t _uvcPreDelayS;
    uint16_t _uvcPostDelayS;
    uint16_t _auxStartDelayS;
    uint16_t _auxStopLeadS;
    uint16_t _manualTimeoutMin;

    // ---- Solar config -------------------------------------------------------
    float    _solarStartDelta;
    float    _solarStopDelta;
    float    _solarMaxPoolTemp;
    float    _solarCoolDelta;
    float    _solarSafetyTemp;
    ValveMode _solarValveMode;
    uint16_t _solarPulseDurationMs;
    uint8_t  _solarDayStartHour;
    uint8_t  _solarNightStartHour;

    // ---- Timestamps ---------------------------------------------------------
    uint32_t _pumpStartMs;
    uint32_t _pumpStopMs;        // STOPPING: AUX-off time; after pump off: pump-off time
    uint32_t _uvcOnMs;
    uint32_t _manualSetMs;
    uint32_t _lastEmaMs;            // shared 10 s tick: current EMA + pressure EMA
    uint32_t _lastMidnightMs;
    uint32_t _lastReminderMs;    // last manual-mode reminder message timestamp

    // ---- Daily tracking -----------------------------------------------------
    uint16_t _dailyTargetMin;
    uint16_t _dailyRuntimeMin;
    uint16_t _yesterdayRuntimeMin;
    uint32_t _lastAccumMs;    // runtime accumulation timestamp (per-instance, not static)
    uint16_t _lastEpochDay;   // last seen RTC day number; 0xFFFF = not yet initialised

    // ---- Pressure -----------------------------------------------------------
    float _maxPressure;
    float _cleanPressure;
    float _filterWarningDelta;
    float _filterBackwashDelta;
    float _lastPressure;

    // ---- Current EMA --------------------------------------------------------
    float   _currentEma;
    uint8_t _currentSampleCount;

    // ---- Pressure EMA (dual: valve-closed baseline / solar-valve-open baseline) ---
    float   _pressureEmaNormal;       // EMA while solar valve is closed
    float   _pressureEmaSolar;        // EMA while solar valve is open (higher due to head)
    uint8_t _pressureEmaCountNormal;  // cold-start sample counter for normal EMA
    uint8_t _pressureEmaCountSolar;   // cold-start sample counter for solar EMA
    uint8_t _pressurePeakPct;         // overpressure alarm threshold % above EMA (0 = disabled)

    // ---- Freeze protection --------------------------------------------------
    float  (*_freezeTempCb)();        // pool/pipe temperature callback (°C; -1.0f = not ready)
    float    _freezeThresholdC;       // freeze protection activates below this temperature

    // ---- Flow switch (stub) -------------------------------------------------
    bool   (*_flowCb)();              // returns true when flow is confirmed

    // ---- Catch-up window ----------------------------------------------------
    uint8_t _catchupStartHour;        // catch-up allowed from this hour (inclusive)
    uint8_t _catchupEndHour;          // catch-up allowed until this hour (exclusive)

    // ---- EEPROM layout (12 bytes at APAPUMP_EEPROM_ADDR) --------------------
    struct __attribute__((packed)) EepromData {
        uint16_t magic;                // 0xA55A
        uint8_t  version;              // config version — mismatch resets to defaults
        uint16_t dailyTargetMin;
        uint16_t yesterdayRuntimeMin;
        uint16_t minRunTimeSec;
        uint16_t cleanPressure100;     // bar × 100; 0 = not learned
        uint8_t  checksum;             // byte sum of preceding fields
    };
    static constexpr uint16_t EEPROM_MAGIC   = 0xA55A;
    static constexpr uint8_t  EEPROM_VERSION = 1;

    // ---- Private methods ----------------------------------------------------
    void _writePcf();
    void _setRelay(uint8_t idx, bool on);
    bool _isRelayOn(uint8_t idx) const;
    void _startPumpSequence(uint32_t now);
    void _stopPumpSequence(uint32_t now);
    bool _shouldPumpRun();
    bool _solarWantsRun();
    void _updateFollowers(uint32_t now);
    void _updateDailyCounter(uint32_t now);
    void _updateCurrentEma();
    void _updatePressureEma();
    void _checkManualTimeout(uint32_t now);
    void _checkManualReminder(uint32_t now);
    void _fireStatus(const __FlashStringHelper* msg);
    void _loadEEPROM();
    void _saveEEPROM();
};
