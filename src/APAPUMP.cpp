#include "APAPUMP.h"

// ---- EMA / safety constants (implementation detail) -------------------------
static constexpr uint8_t  CURRENT_COLD_START_SAMPLES  = 5;
static constexpr uint8_t  PRESSURE_COLD_START_SAMPLES = 5;
static constexpr float    CURRENT_EMA_ALPHA           = 0.05f;  // shared by current + pressure EMA
static constexpr float    CURRENT_ALARM_MULTIPLIER    = 1.5f;
static constexpr uint32_t MANUAL_REMINDER_MS          = 1800000UL; // 30 min
static constexpr uint32_t MILLIS_24H                  = 86400000UL;

// ============================================================================
// Constructors
// ============================================================================

ApaPump::ApaPump()
    : _pcfAddr(0x3C), _pcfState(0x00), _relayBits(0x00)
{
    _id[0] = 0; _id[1] = 1; _id[2] = 2; _id[3] = 3;
    memset(&_flags, 0, sizeof(_flags));
    _flags.usePcf      = 1;
    _flags.activeLow   = 1;  // APA HMI board: PCF→74HC240(inverting)→ULN2803 = active-low
    _manualMode        = AUTO;
    _pumpState         = IDLE;
    _activeAlarm       = PUMP_ALARM_NONE;
    _scheduleCb        = nullptr; _externalRequestCb  = nullptr;
    _dailyTargetCb     = nullptr; _stateChangeCb      = nullptr;
    _alarmCb           = nullptr; _statusCb           = nullptr;
    _solarTempCb       = nullptr; _poolTempCb         = nullptr;
    _solarEpochCb      = nullptr; _midnightCb         = nullptr;
    _pressureCb        = nullptr; _currentCb          = nullptr;
    _minRunTimeSec     = APAPUMP_MIN_RUN_SEC;
    _uvcPreDelayS      = 5;   _uvcPostDelayS       = 30;
    _auxStartDelayS    = 10;  _auxStopLeadS        = 30;
    _manualTimeoutMin  = 0;
    _solarStartDelta   = 8.0f; _solarStopDelta      = 3.0f;
    _solarMaxPoolTemp  = 0.0f; _solarCoolDelta       = 2.0f;
    _solarSafetyTemp   = 50.0f;
    _solarValveMode    = VALVE_SUSTAINED;
    _solarPulseDurationMs = APAPUMP_VALVE_PULSE_MS;
    _solarDayStartHour = 7;   _solarNightStartHour = 20;
    _pumpStartMs       = 0;   _pumpStopMs          = 0;
    _uvcOnMs           = 0;
    _manualSetMs       = 0;   _lastEmaMs           = 0;
    _lastMidnightMs    = 0;   _lastReminderMs      = 0;
    _dailyTargetMin    = 0;   _dailyRuntimeMin     = 0;
    _yesterdayRuntimeMin = 0; _lastAccumMs         = 0;
    _lastEpochDay      = 0xFFFF;
    _maxPressure       = 0.0f; _cleanPressure       = 0.0f;
    _filterWarningDelta = APAPUMP_FILTER_WARN_DELTA; _filterBackwashDelta = APAPUMP_FILTER_BACKWASH_DELTA;
    _lastPressure      = -1.0f;
    _currentEma        = 0.0f; _currentSampleCount  = 0;
    _pressureEmaNormal      = 0.0f; _pressureEmaSolar       = 0.0f;
    _pressureEmaCountNormal = 0;    _pressureEmaCountSolar  = 0;
    _pressurePeakPct        = 0;
    _freezeTempCb      = nullptr; _freezeThresholdC   = APAPUMP_FREEZE_THRESHOLD_C;
    _flowCb            = nullptr;
    _catchupStartHour  = 0;   _catchupEndHour      = 0;
    _heaterPermitCb    = nullptr; _heaterTargetC      = APAPUMP_HEATER_TARGET_DEFAULT;
    _auxChangeMs       = 0;   _valveChangeMs       = 0;
    _pressureUserOff   = false;
}

ApaPump::ApaPump(RelayDriver mode, uint8_t a,
                 uint8_t b, uint8_t c, uint8_t d, uint8_t e,
                 uint16_t manualTimeoutMin)
    : _pcfAddr(mode == RELAY_PCF ? a : 0x3C), _pcfState(0x00), _relayBits(0x00)
{
    memset(&_flags, 0, sizeof(_flags));
    if (mode == RELAY_PCF) {
        _flags.usePcf = 1;
        _id[0] = b; _id[1] = c; _id[2] = d; _id[3] = e;
    } else {
        _id[0] = a; _id[1] = b; _id[2] = c; _id[3] = d;
    }
    _manualMode        = AUTO;
    _pumpState         = IDLE;
    _activeAlarm       = PUMP_ALARM_NONE;
    _scheduleCb        = nullptr; _externalRequestCb  = nullptr;
    _dailyTargetCb     = nullptr; _stateChangeCb      = nullptr;
    _alarmCb           = nullptr; _statusCb           = nullptr;
    _solarTempCb       = nullptr; _poolTempCb         = nullptr;
    _solarEpochCb      = nullptr; _midnightCb         = nullptr;
    _pressureCb        = nullptr; _currentCb          = nullptr;
    _minRunTimeSec     = APAPUMP_MIN_RUN_SEC;
    _uvcPreDelayS      = 5;   _uvcPostDelayS       = 30;
    _auxStartDelayS    = 10;  _auxStopLeadS        = 30;
    _manualTimeoutMin  = manualTimeoutMin;
    _solarStartDelta   = 8.0f; _solarStopDelta      = 3.0f;
    _solarMaxPoolTemp  = 0.0f; _solarCoolDelta       = 2.0f;
    _solarSafetyTemp   = 50.0f;
    _solarValveMode    = VALVE_SUSTAINED;
    _solarPulseDurationMs = APAPUMP_VALVE_PULSE_MS;
    _solarDayStartHour = 7;   _solarNightStartHour = 20;
    _pumpStartMs       = 0;   _pumpStopMs          = 0;
    _uvcOnMs           = 0;
    _manualSetMs       = 0;   _lastEmaMs           = 0;
    _lastMidnightMs    = 0;   _lastReminderMs      = 0;
    _dailyTargetMin    = 0;   _dailyRuntimeMin     = 0;
    _yesterdayRuntimeMin = 0; _lastAccumMs         = 0;
    _lastEpochDay      = 0xFFFF;
    _maxPressure       = 0.0f; _cleanPressure       = 0.0f;
    _filterWarningDelta = APAPUMP_FILTER_WARN_DELTA; _filterBackwashDelta = APAPUMP_FILTER_BACKWASH_DELTA;
    _lastPressure      = -1.0f;
    _currentEma        = 0.0f; _currentSampleCount  = 0;
    _pressureEmaNormal      = 0.0f; _pressureEmaSolar       = 0.0f;
    _pressureEmaCountNormal = 0;    _pressureEmaCountSolar  = 0;
    _pressurePeakPct        = 0;
    _freezeTempCb      = nullptr; _freezeThresholdC   = APAPUMP_FREEZE_THRESHOLD_C;
    _flowCb            = nullptr;
    _catchupStartHour  = 0;   _catchupEndHour      = 0;
    _heaterPermitCb    = nullptr; _heaterTargetC      = APAPUMP_HEATER_TARGET_DEFAULT;
    _auxChangeMs       = 0;   _valveChangeMs       = 0;
    _pressureUserOff   = false;
}

// ============================================================================
// Solar / heater / saved-settings API (1.3.0). Setters of saved values write the
// EEPROM only after begin() (compare-before-write: unchanged bytes are not rewritten).
// ============================================================================

void ApaPump::setSolarMode(SolarMode m) {
    if (m == SOLAR_VALVE && !_flags.solarValveEnabled) m = SOLAR_PUMP;   // no valve relay wired
    _flags.solarMode = m;
    if (m == SOLAR_OFF) { _flags.solarRunning = 0; _flags.solarHeating = 0; }
    if (_flags.ready) _saveEEPROM();
}
SolarMode ApaPump::getSolarMode() const           { return (SolarMode)_flags.solarMode; }
void ApaPump::setSolarMaxPool(uint8_t celsius) {
    _solarMaxPoolTemp = celsius;
    if (_flags.ready) _saveEEPROM();
}
uint8_t ApaPump::getSolarMaxPool() const          { return (uint8_t)_solarMaxPoolTemp; }
void ApaPump::setSolarDrive(SolarDrive d)         { _flags.solarDrive = d; }
bool ApaPump::isSolarRunning() const              { return _flags.solarRunning; }
bool ApaPump::isSolarSafetyActive() const         { return _flags.solarSafetyActive; }
bool ApaPump::isSolarValveOpen() const            { return _flags.solarValveEnabled && _isRelayOn(3); }
bool ApaPump::isSolarSensorOk() const             { return !_flags.solarSensorFault; }
void ApaPump::setPoolTempCallback(float (*cb)())  { _poolTempCb = cb; }
void ApaPump::setAuxMode(AuxMode m) {
    if (m == AUX_DISABLED && _flags.auxRunning && _id[2] != 0xFF) {   // switch it off now
        _setRelay(2, false);
        _flags.auxRunning = 0;
        _auxChangeMs      = millis();
    }
    _flags.auxMode    = m;
    _flags.auxEnabled = (m != AUX_DISABLED && _id[2] != 0xFF);
    if (_flags.ready) _saveEEPROM();
}
AuxMode ApaPump::getAuxMode() const               { return (AuxMode)_flags.auxMode; }
void ApaPump::setHeaterTarget(uint8_t celsius) {
    _heaterTargetC = constrain(celsius, APAPUMP_HEATER_TARGET_MIN, APAPUMP_HEATER_TARGET_MAX);
    if (_flags.ready) _saveEEPROM();
}
uint8_t ApaPump::getHeaterTarget() const          { return _heaterTargetC; }
void ApaPump::setHeaterPermitCallback(bool (*cb)()) { _heaterPermitCb = cb; }
bool ApaPump::isHeaterOn() const                  { return _heaterMode() && _flags.auxRunning; }
bool ApaPump::hasSavedSettings() const            { return _flags.savedSettings; }

bool ApaPump::_solarActive() const {
    return _flags.solarEnabled && _flags.solarMode != SOLAR_OFF && _solarTempCb;
}

void ApaPump::_setValve(bool open, uint32_t now) {
    if (!_flags.solarValveEnabled || _isRelayOn(3) == open) return;
    _setRelay(3, open);
    _valveChangeMs = now;   // pressure checks and learning pause for APAPUMP_VALVE_SETTLE_SEC
}

bool ApaPump::_heaterMode() const {
    return _flags.auxMode == HEATER_SIMPLE || _flags.auxMode == HEATER_FULL;
}

// Heater rules (pump running past the AUX start delay is checked by the caller).
bool ApaPump::_heaterWanted(uint32_t now) const {
    if (_flags.alarmActive) return false;   // no flow = no heating, beats the min-ON guard

    float pool  = _poolTempCb ? _poolTempCb() : -127.0f;
    bool  valid = _tempValid(pool);

    // Solar first: not only while solar heats, but also while it CAN heat right now -- the
    // solar flags are cleared e.g. during an alarm, and the heater is checked before solar
    // re-evaluates, so a latched flag alone would start the heat pump for its min-ON time.
    bool solarHeats = (_flags.solarRunning && _flags.solarHeating) || _flags.solarSafetyActive;
    if (!solarHeats && valid && _solarActive() && _solarDaytime()) {
        float absorber = _solarTempCb();
        solarHeats = _tempValid(absorber) && absorber > pool + _solarStartDelta;
    }
    bool want = !solarHeats;
    if (want && _heaterPermitCb) want = _heaterPermitCb();
    if (want && valid && _solarMaxPoolTemp > 0.0f && pool > _solarMaxPoolTemp) want = false;
    if (want && _flags.auxMode == HEATER_FULL) {
        if (!valid) want = false;
        else want = _flags.auxRunning ? (pool < _heaterTargetC)
                                      : (pool < _heaterTargetC - APAPUMP_HEATER_HYST_C);
    }

    // Short-cycling guard for the heat pump's compressor
    uint32_t since = now - _auxChangeMs;
    if ( _flags.auxRunning && !want && since < (uint32_t)APAPUMP_HEATER_MIN_ON_SEC  * 1000UL) return true;
    if (!_flags.auxRunning &&  want && since < (uint32_t)APAPUMP_HEATER_MIN_OFF_SEC * 1000UL) return false;
    return want;
}

// Manual OFF means "stop now" (someone works at the pump), and a pump running dry or
// overloaded must not keep running for the heat pump's after-run (1.3.1).
bool ApaPump::_skipAfterRun() const {
    return _manualMode == FORCE_OFF || _flags.alarmActive;
}

bool ApaPump::_solarDaytime() const {
    if (!_flags.solarDayNight || !_solarEpochCb) return true;
    uint8_t hour = (uint8_t)((_solarEpochCb() % 86400UL) / 3600UL);
    return hour >= _solarDayStartHour && hour < _solarNightStartHour;
}

bool ApaPump::_tempValid(float t) {
    return t >= APAPUMP_TEMP_MIN_C && t <= APAPUMP_TEMP_MAX_C && t != 85.0f && t != -1.0f;
}

// ============================================================================
// begin()
// ============================================================================

void ApaPump::begin(bool (*scheduleCb)(), bool (*externalRequestCb)(), uint16_t (*dailyTargetCb)()) {
    _scheduleCb        = scheduleCb;
    _externalRequestCb = externalRequestCb;
    _dailyTargetCb     = dailyTargetCb;

    if (_flags.usePcf) {
        // Active-low: all bits HIGH = all relays OFF. Active-high: all bits LOW = all OFF.
        _pcfState = _flags.activeLow ? 0xFF : 0x00;
        _writePcf();
    } else {
        for (uint8_t i = 0; i < 4; i++) {
            if (_id[i] == 0xFF) continue;
            pinMode(_id[i], OUTPUT);
            digitalWrite(_id[i], _flags.activeLow ? HIGH : LOW);  // OFF
        }
    }

    _loadEEPROM();
    uint32_t now    = millis();
    _lastMidnightMs = now;
    _lastReminderMs = now;
    // Initialise epoch day so midnight detection doesn't fire spuriously at boot
    if (_flags.midnightEnabled && _midnightCb)
        _lastEpochDay = (uint16_t)(_midnightCb() / 86400UL);
    _flags.ready = 1;
}

// ============================================================================
// update()
// ============================================================================

void ApaPump::update() {
    if (!_flags.ready) return;
    uint32_t now = millis();

    // ---- Manual timeout & reminder -----------------------------------------
    _checkManualTimeout(now);
    _checkManualReminder(now);

    // ---- Cancel STARTING if request has disappeared -------------------------
    // Checked before advancing the state machine so the relay never fires.
    if (_pumpState == STARTING && !_shouldPumpRun()) {
        if (_flags.uvcEnabled && _id[1] != 0xFF) _setRelay(1, false);
        _pumpState  = IDLE;
        _pumpStopMs = now;   // seed min-OFF guard
    }

    // ---- Advance follower state machine ------------------------------------
    _updateFollowers(now);

    // ---- Daily runtime counter ---------------------------------------------
    _updateDailyCounter(now);

    // ---- Pressure reading --------------------------------------------------
    if (_flags.pressureEnabled && _pressureCb) {
        float p = _pressureCb();
        if (p >= 0.0f) _lastPressure = p;
    }

    // ---- Safety alarm checks (after inrush settle, pump running) -----------
    if (_pumpState == RUNNING && !_flags.alarmActive &&
        (now - _pumpStartMs)   >= (uint32_t)APAPUMP_CURRENT_SETTLE_SEC * 1000UL &&
        (now - _valveChangeMs) >= (uint32_t)APAPUMP_VALVE_SETTLE_SEC * 1000UL) {

        // Dry-run: pressure too low for a running pump
        if (_flags.pressureEnabled && _lastPressure >= 0.0f) {
            bool    valveOpen = _flags.solarValveEnabled && _isRelayOn(3);
            float   refEma    = valveOpen ? _pressureEmaSolar   : _pressureEmaNormal;
            uint8_t refCount  = valveOpen ? _pressureEmaCountSolar : _pressureEmaCountNormal;
            bool dryRun = (refCount >= PRESSURE_COLD_START_SAMPLES)
                          ? (_lastPressure < refEma * (APAPUMP_PRESSURE_DRYRUN_PCT / 100.0f))
                          : (_lastPressure < APAPUMP_PRESSURE_ABS_MIN);
            if (dryRun) {
                _flags.alarmActive = 1;
                _activeAlarm = PUMP_ALARM_LOW_PRESSURE;
                if (_alarmCb) _alarmCb(PUMP_ALARM_LOW_PRESSURE);
            }
        }

        // Overpressure: absolute hard limit — fires even before EMA baseline is built
        if (!_flags.alarmActive && _flags.pressureEnabled &&
            _maxPressure > 0.0f && _lastPressure > _maxPressure) {
            _flags.alarmActive = 1;
            _activeAlarm = PUMP_ALARM_HIGH_PRESSURE;
            if (_alarmCb) _alarmCb(PUMP_ALARM_HIGH_PRESSURE);
        }

        // Overpressure: EMA-based relative alarm — filter dirty, pressure above baseline + peakPct %
        if (!_flags.alarmActive && _flags.pressurePeakEnabled &&
            _lastPressure >= 0.0f) {
            bool    valveOpen = _flags.solarValveEnabled && _isRelayOn(3);
            float   refEma    = valveOpen ? _pressureEmaSolar   : _pressureEmaNormal;
            uint8_t refCount  = valveOpen ? _pressureEmaCountSolar : _pressureEmaCountNormal;
            if (refCount >= PRESSURE_COLD_START_SAMPLES) {
                if (_lastPressure > refEma * (1.0f + _pressurePeakPct / 100.0f)) {
                    _flags.alarmActive = 1;
                    _activeAlarm = PUMP_ALARM_HIGH_PRESSURE;
                    if (_alarmCb) _alarmCb(PUMP_ALARM_HIGH_PRESSURE);
                }
            }
        }

        // Flow switch: no flow confirmed after settle
        if (!_flags.alarmActive && _flags.flowEnabled && _flowCb && !_flowCb()) {
            _flags.alarmActive = 1;
            _activeAlarm = PUMP_ALARM_NO_FLOW;
            if (_alarmCb) _alarmCb(PUMP_ALARM_NO_FLOW);
        }
    }

    // ---- EMA tick (10 s after inrush settle, pump running) -----------------
    // AFTER the safety checks and never while an alarm is active: the pump keeps
    // running on an alarm unless the sketch stops it, and a dry run, an overpressure
    // or an overcurrent must not become the new "normal" -- not even the one sample
    // that raised the alarm (before 1.1.1 that sample was learned first).
    if (_pumpState == RUNNING && !_flags.alarmActive &&
        (now - _pumpStartMs)   >= (uint32_t)APAPUMP_CURRENT_SETTLE_SEC * 1000UL &&
        (now - _valveChangeMs) >= (uint32_t)APAPUMP_VALVE_SETTLE_SEC * 1000UL &&
        (now - _lastEmaMs)     >= APAPUMP_CURRENT_SAMPLE_MS) {
        _lastEmaMs = now;
        _updateCurrentEma();
        if (!_flags.alarmActive) _updatePressureEma();   // an overcurrent just raised -> skip
    }

    // ---- Solar sensors + overheat protection (Priority 0) ------------------
    // Runs the pump only in AUTO and never on a latched pump alarm: OFF usually means
    // someone works at the pump, and after a dry run there is no water to circulate.
    // The collector has its own vent valve. "Hot" is still reported in every mode.
    bool hot = false;
    if (_solarActive()) {
        float absorber = _solarTempCb();
        float pool     = _poolTempCb ? _poolTempCb() : -127.0f;
        bool  ok       = _tempValid(absorber) && _tempValid(pool);
        if (ok == (bool)_flags.solarSensorFault) {
            _flags.solarSensorFault = !ok;
            if (ok) _fireStatus(F("Solar sensor OK"));
            else    _fireStatus(F("Solar sensor fault"));
        }
        hot = ok && absorber > _solarSafetyTemp;
    } else {
        _flags.solarSensorFault = 0;
    }
    if (hot != (bool)_flags.solarSafetyActive) {
        _flags.solarSafetyActive = hot;
        if (hot) _fireStatus(F("Solar safety: absorber hot"));
        else     _fireStatus(F("Solar safety: absorber cooled"));
    }
    if (hot && _manualMode == AUTO && !_flags.alarmActive) {
        if (_pumpState == IDLE) _startPumpSequence(now);
        _setValve(_flags.solarMode == SOLAR_VALVE, now);
        return;  // skip normal engine while protection runs
    }

    // ---- Freeze cycle phase management -------------------------------------
    // Reuses existing _pumpStartMs / _pumpStopMs — no extra timestamp needed.
    if (_flags.freezeActive) {
        if (_flags.freezeCycleOn) {
            // Run phase: switch to rest after FREEZE_ON_SEC from pump-on
            if (_pumpState == RUNNING &&
                now - _pumpStartMs >= (uint32_t)APAPUMP_FREEZE_ON_SEC * 1000UL)
                _flags.freezeCycleOn = 0;
        } else {
            // Rest phase: switch to run after FREEZE_OFF_SEC from pump-off
            if (_pumpState == IDLE &&
                now - _pumpStopMs >= (uint32_t)APAPUMP_FREEZE_OFF_SEC * 1000UL)
                _flags.freezeCycleOn = 1;
        }
    }

    // ---- Normal priority engine (only valid in steady states) --------------
    if (_pumpState != IDLE && _pumpState != RUNNING) return;

    bool wantPump = _shouldPumpRun();

    if (wantPump && _pumpState == IDLE) {
        // Want to start — check min OFF time (suspended in manual mode)
        bool minOffOk = (_manualMode != AUTO) ||
                        (now - _pumpStopMs >= (uint32_t)APAPUMP_MIN_OFF_SEC * 1000UL);
        if (minOffOk) {
            _startPumpSequence(now);
        }

    } else if (!wantPump && _pumpState == RUNNING) {
        // Want to stop — check min run time (suspended in manual mode)
        bool minRunOk = (_manualMode != AUTO) ||
                        (now - _pumpStartMs >= (uint32_t)_minRunTimeSec * 1000UL);
        if (minRunOk) {
            _stopPumpSequence(now);
        }

    } else if (!wantPump && _pumpState == IDLE && _flags.solarRunning) {
        // Solar no longer wants to run — clear the solar hysteresis flag
        _flags.solarRunning = 0;
        _flags.solarHeating = 0;
    }

    // ---- Solar valve follows demand, also during a run (schedule + solar) ---
    // Closed in manual mode — the operator needs full pressure (vacuuming).
    _setValve((_pumpState == STARTING || _pumpState == RUNNING) &&
              _flags.solarMode == SOLAR_VALVE && _solarActive() &&
              _manualMode == AUTO && _flags.solarRunning, now);
}

// ============================================================================
// Priority engine
// ============================================================================

bool ApaPump::_shouldPumpRun() {
    // Priority 1: FORCE_OFF
    if (_manualMode == FORCE_OFF) return false;

    // Priority 2: FORCE_ON
    if (_manualMode == FORCE_ON)  return true;

    // Priority 3: external request
    if (_externalRequestCb && _externalRequestCb()) return true;

    // Priority 4: freeze protection — pool temp below threshold, water confirmed
    if (_flags.freezeEnabled && _freezeTempCb) {
        float t     = _freezeTempCb();
        bool  valid = _tempValid(t);
        // Ends only on a valid reading above threshold + hysteresis: an invalid reading
        // ("not ready", sensor lost) must never switch an active freeze protection off.
        if (_flags.freezeActive && valid && t >= _freezeThresholdC + APAPUMP_FREEZE_HYST_C) {
            _flags.freezeActive  = 0;
            _flags.freezeCycleOn = 0;
            _fireStatus(F("Freeze protection deactivated"));
        }
        if (_flags.freezeActive || (valid && t < _freezeThresholdC)) {
            // Dry-run interlock: if pressure EMA established and shows no flow, suppress
            bool waterPresent = true;
            if (_flags.pressureEnabled && _lastPressure >= 0.0f &&
                _pressureEmaCountNormal >= PRESSURE_COLD_START_SAMPLES) {
                waterPresent = (_lastPressure >= _pressureEmaNormal *
                                (APAPUMP_PRESSURE_DRYRUN_PCT / 100.0f));
            }
            if (waterPresent) {
                if (!_flags.freezeActive) {
                    _flags.freezeActive  = 1;
                    _flags.freezeCycleOn = 1;   // start in run phase immediately
                    _fireStatus(F("Freeze protection active"));
                }
                return _flags.freezeCycleOn;
            }
        }
    }

    // Solar: extra demand on top of the schedule (default), or the only driver with
    // catch-up (SOLAR_WITH_CATCHUP, 1.2.0 behaviour).
    if (_solarActive()) {
        if (_solarWantsRun()) return true;
        if (_flags.solarDrive == SOLAR_WITH_SCHEDULE) return _scheduleCb && _scheduleCb();
        // Priority 5: catch-up deficit
        uint16_t target = _dailyTargetMin;
        if (target == 0 && _dailyTargetCb) target = _dailyTargetCb();
        if (target > 0 && _dailyRuntimeMin < target) {
            // Catch-up window gate — restrict to configured hours when RTC available
            if (_flags.catchupWindow && _midnightCb) {
                uint8_t hour = (uint8_t)((_midnightCb() % 86400UL) / 3600UL);
                bool inWindow = (_catchupStartHour <= _catchupEndHour)
                    ? (hour >= _catchupStartHour && hour < _catchupEndHour)
                    : (hour >= _catchupStartHour || hour < _catchupEndHour);
                if (!inWindow) return false;
            }
            return true;
        }
        return false;
    }

    // No solar: schedule drives
    if (_scheduleCb && _scheduleCb()) return true;

    return false;
}

bool ApaPump::_solarWantsRun() {
    float absorber = _solarTempCb();
    float pool     = _poolTempCb ? _poolTempCb() : -127.0f;
    // No control on a bad reading, and no extra solar runs on a latched pump alarm
    // (after a dry run there is no water to circulate - same rule as overheat protection).
    if (_flags.alarmActive || !_tempValid(absorber) || !_tempValid(pool)) {
        _flags.solarRunning = 0;
        _flags.solarHeating = 0;
        return false;
    }

    // Day/night gate — cooling allowed at night, heating only during day
    bool isDaytime = _solarDaytime();

    // Pool cooling: pool overheated, absorber is cooler
    if (_solarMaxPoolTemp > 0.0f && pool > _solarMaxPoolTemp) {
        if (absorber < pool - _solarCoolDelta) {
            _flags.solarRunning = 1;
            _flags.solarHeating = 0;
            return true;
        }
    }

    // Heating: absorber warmer than pool (day only)
    if (!isDaytime) {
        // Night — not heating; cooling handled above
        if (_flags.solarRunning) {
            // Stop solar run at night (unless cooling)
            bool stillCooling = (_solarMaxPoolTemp > 0.0f && pool > _solarMaxPoolTemp &&
                                 absorber < pool - _solarCoolDelta);
            if (!stillCooling) {
                _flags.solarRunning = 0;
                _flags.solarHeating = 0;
                return false;
            }
        }
        return false;
    }

    if (_flags.solarRunning) {
        // Currently running — stop at stopDelta (hysteresis lower edge)
        if (absorber < pool + _solarStopDelta) {
            _flags.solarRunning = 0;
            _flags.solarHeating = 0;
            return false;
        }
        return true;
    } else {
        // Currently stopped — start at startDelta (hysteresis upper edge)
        bool poolNotOverheated = (_solarMaxPoolTemp <= 0.0f || pool < _solarMaxPoolTemp);
        if (absorber > pool + _solarStartDelta && poolNotOverheated) {
            _flags.solarRunning = 1;
            _flags.solarHeating = 1;
            return true;
        }
        return false;
    }
}

// ============================================================================
// Pump start / stop sequences
// ============================================================================

void ApaPump::_startPumpSequence(uint32_t now) {
    _pumpState = STARTING;
    _uvcOnMs   = now;   // marks sequence start for UVC pre-delay tracking

    if (_flags.uvcEnabled && _id[1] != 0xFF && _uvcPreDelayS > 0) {
        _setRelay(1, true);   // UVC ON; pump fires after pre-delay in _updateFollowers
    } else {
        if (_flags.uvcEnabled && _id[1] != 0xFF) _setRelay(1, true);
        _setRelay(0, true);
        _pumpStartMs = now;   // pump relay on — start timing from here
        _pumpState   = RUNNING;
        if (_stateChangeCb) _stateChangeCb(true);
    }
}

void ApaPump::_stopPumpSequence(uint32_t now) {
    _pumpState  = STOPPING;
    _pumpStopMs = now;

    // AUX off at start of STOPPING sequence
    bool auxWasOn = _flags.auxRunning;
    if (_flags.auxEnabled && _flags.auxRunning && _id[2] != 0xFF) {
        _setRelay(2, false);
        _flags.auxRunning = 0;
        _auxChangeMs      = now;
    }

    // No AUX stop lead needed (none set, a heater that was already off, or OFF / an alarm) -> pump off now
    if (!_flags.auxEnabled || _auxStopLeadS == 0 || (_heaterMode() && !auxWasOn) || _skipAfterRun()) {
        _setRelay(0, false);
        _pumpStopMs = now;   // reset: now tracks pump-off time for UVC post-delay
        if (_stateChangeCb) _stateChangeCb(false);

        if (!_flags.uvcEnabled || _uvcPostDelayS == 0) {
            if (_flags.uvcEnabled && _id[1] != 0xFF) _setRelay(1, false);
            _pumpState = IDLE;
        }
    }
}

// ============================================================================
// Follower state machine (advances STARTING → RUNNING, STOPPING → IDLE)
// ============================================================================

void ApaPump::_updateFollowers(uint32_t now) {
    switch (_pumpState) {

        case STARTING:
            // Wait for UVC pre-delay then turn pump on
            if (now - _uvcOnMs >= (uint32_t)_uvcPreDelayS * 1000UL) {
                _setRelay(0, true);
                _pumpStartMs = now;   // pump relay now on — min run time and AUX delay count from here
                _pumpState   = RUNNING;
                if (_stateChangeCb) _stateChangeCb(true);
            }
            break;

        case RUNNING:
            // AUX on after the pump has run long enough for flow (start delay)
            if (_flags.auxEnabled && _id[2] != 0xFF) {
                bool want = (now - _pumpStartMs >= (uint32_t)_auxStartDelayS * 1000UL);
                if (_heaterMode()) want = want && _heaterWanted(now);
                if (want != (bool)_flags.auxRunning) {
                    _setRelay(2, want);
                    _flags.auxRunning = want;
                    _auxChangeMs      = now;
                }
            }
            break;

        case STOPPING:
            if (_isRelayOn(0)) {
                // Phase A — pump still running, waiting for AUX stop lead (cut short by OFF / an alarm)
                if (now - _pumpStopMs >= (uint32_t)_auxStopLeadS * 1000UL || _skipAfterRun()) {
                    _setRelay(0, false);
                    _pumpStopMs = now;   // reset: now tracks pump-off time for UVC post-delay
                    if (_stateChangeCb) _stateChangeCb(false);
                }
            } else {
                // Phase B — pump off, waiting for UVC post-delay
                bool uvcDone = !_flags.uvcEnabled || _id[1] == 0xFF ||
                               (now - _pumpStopMs >= (uint32_t)_uvcPostDelayS * 1000UL);
                if (uvcDone) {
                    if (_flags.uvcEnabled && _id[1] != 0xFF) _setRelay(1, false);
                    _pumpState = IDLE;
                }
            }
            break;

        case IDLE:
        default:
            break;
    }
}

// ============================================================================
// Daily runtime counter + midnight rollover
// ============================================================================

void ApaPump::_updateDailyCounter(uint32_t now) {
    // Accumulate runtime while pump relay is on (_lastAccumMs is a member — per-instance)
    if (_pumpState == RUNNING) {
        if (_lastAccumMs == 0) _lastAccumMs = now;
        uint32_t elapsed = now - _lastAccumMs;
        if (elapsed >= 60000UL) {
            _dailyRuntimeMin += (uint16_t)(elapsed / 60000UL);
            _lastAccumMs      = now - (elapsed % 60000UL);
        }
    } else {
        _lastAccumMs = 0;
    }

    // Check daily target
    uint16_t target = _dailyTargetMin;
    if (target == 0 && _dailyTargetCb) target = _dailyTargetCb();
    if (target > 0 && _dailyRuntimeMin >= target) _flags.dailyTargetMet = 1;

    // Midnight rollover — RTC epoch or millis() 24h
    bool midnight = false;
    if (_flags.midnightEnabled && _midnightCb) {
        // Poll once per minute; detect any day change (not just 60-second window at midnight)
        if (now - _lastMidnightMs >= 60000UL) {
            _lastMidnightMs    = now;
            uint16_t dayNum    = (uint16_t)(_midnightCb() / 86400UL);
            if (_lastEpochDay == 0xFFFF) {
                _lastEpochDay  = dayNum;   // first poll after begin() — initialise, don't roll over
            } else if (dayNum != _lastEpochDay) {
                _lastEpochDay  = dayNum;
                midnight       = true;
            }
        }
    } else {
        // millis() 24h rollover
        if (now - _lastMidnightMs >= MILLIS_24H) {
            _lastMidnightMs = now;
            midnight        = true;
        }
    }

    if (midnight) {
        _yesterdayRuntimeMin = _dailyRuntimeMin;
        _dailyRuntimeMin     = 0;
        _flags.dailyTargetMet = 0;
        // Auto-return manual mode at midnight if configured
        if (_flags.manualAutoReset && _manualMode != AUTO) {
            _manualMode = AUTO;
            _fireStatus(F("Pump: AUTO restored (midnight)"));
        }
        _saveEEPROM();
    }
}

// ============================================================================
// Current EMA — overcurrent detection
// ============================================================================

void ApaPump::_updateCurrentEma() {
    // Called from update() EMA tick gate — timing and state guards handled by caller
    if (!_flags.currentEnabled || !_currentCb) return;

    float measured = _currentCb();
    // Also rejects -1.0f (sensor not ready). Below the floor there is no motor on the
    // sensor: noise would become the baseline and its spikes would read as overcurrent.
    if (measured < APAPUMP_CURRENT_MIN_A) return;

    // Compare BEFORE learning: the overload sample itself must not raise the baseline.
    if (_flags.baselineReady && _currentEma > 0.0f &&
        measured > _currentEma * CURRENT_ALARM_MULTIPLIER && !_flags.alarmActive) {
        _flags.alarmActive = 1;
        _activeAlarm       = PUMP_ALARM_OVERCURRENT;
        if (_alarmCb) _alarmCb(PUMP_ALARM_OVERCURRENT);
        return;
    }

    // Cold start: plain average of the first samples (an EMA seeded with the first
    // sample would keep ~81 % of that one reading), then the slow EMA.
    if (_currentSampleCount < CURRENT_COLD_START_SAMPLES) {
        _currentSampleCount++;
        _currentEma += (measured - _currentEma) / _currentSampleCount;
        if (_currentSampleCount >= CURRENT_COLD_START_SAMPLES) _flags.baselineReady = 1;
    } else {
        _currentEma += CURRENT_EMA_ALPHA * (measured - _currentEma);
    }
}

// ============================================================================
// Pressure EMA — dual baseline (valve-closed / valve-open)
// ============================================================================

void ApaPump::_updatePressureEma() {
    // Called from update() EMA tick gate — timing and state guards handled by caller
    if (!_flags.pressureEnabled || !_pressureCb) return;
    // Also rejects -1.0f (not calibrated). A running pump below this is dry: learning it
    // would drop the baseline to ~0 and disable the dry-run check.
    if (_lastPressure < APAPUMP_PRESSURE_ABS_MIN) return;

    bool valveOpen = _flags.solarValveEnabled && _isRelayOn(3);

    if (valveOpen) {
        if (_pressureEmaCountSolar < PRESSURE_COLD_START_SAMPLES) {   // cold start: plain average
            _pressureEmaCountSolar++;
            _pressureEmaSolar += (_lastPressure - _pressureEmaSolar) / _pressureEmaCountSolar;
        } else {
            _pressureEmaSolar += CURRENT_EMA_ALPHA * (_lastPressure - _pressureEmaSolar);
        }
    } else {
        if (_pressureEmaCountNormal < PRESSURE_COLD_START_SAMPLES) {  // cold start: plain average
            _pressureEmaCountNormal++;
            _pressureEmaNormal += (_lastPressure - _pressureEmaNormal) / _pressureEmaCountNormal;
        } else {
            _pressureEmaNormal += CURRENT_EMA_ALPHA * (_lastPressure - _pressureEmaNormal);
        }
        // First valid baseline after install or learnCleanPressure() = the clean filter.
        if (_cleanPressure <= 0.0f && _pressureEmaCountNormal >= PRESSURE_COLD_START_SAMPLES) {
            _cleanPressure = _pressureEmaNormal;
            _saveEEPROM();
        }
    }
}

// ============================================================================
// Manual mode helpers
// ============================================================================

void ApaPump::_checkManualTimeout(uint32_t now) {
    if (_manualMode == AUTO || _manualTimeoutMin == 0) return;
    if (now - _manualSetMs >= (uint32_t)_manualTimeoutMin * 60000UL) {
        _manualMode = AUTO;
        _fireStatus(F("Pump: AUTO restored (timeout)"));
    }
}

void ApaPump::_checkManualReminder(uint32_t now) {
    if (_manualMode == AUTO) return;
    if (now - _lastReminderMs >= MANUAL_REMINDER_MS) {
        _lastReminderMs = now;
        _fireStatus(F("Pump still in MANUAL mode"));
    }
}

// ============================================================================
// Relay hardware
// ============================================================================

void ApaPump::_writePcf() {
    Wire.beginTransmission(_pcfAddr);
    Wire.write(_pcfState);
    Wire.endTransmission();
}

void ApaPump::_setRelay(uint8_t idx, bool on) {
    if (_flags.usePcf) {
        uint8_t bit = _id[idx];
        if (bit == 0xFF) return;
        bool pcfHigh = _flags.activeLow ? !on : on;  // invert if active-low
        if (pcfHigh) _pcfState |=  (1 << bit);
        else         _pcfState &= ~(1 << bit);
        _writePcf();
    } else {
        uint8_t pin = _id[idx];
        if (pin == 0xFF) return;
        if (on) _relayBits |=  (1 << idx);
        else    _relayBits &= ~(1 << idx);
        bool level = _flags.activeLow ? !on : on;
        digitalWrite(pin, level ? HIGH : LOW);
    }
}

bool ApaPump::_isRelayOn(uint8_t idx) const {
    if (_flags.usePcf) {
        uint8_t bit = _id[idx];
        if (bit == 0xFF) return false;
        bool pcfHigh = (_pcfState >> bit) & 0x01;
        return _flags.activeLow ? !pcfHigh : pcfHigh;
    }
    return (_relayBits >> idx) & 0x01;
}

// ============================================================================
// Public API implementations
// ============================================================================

void ApaPump::setActiveLow() { _flags.activeLow = 1; }

void ApaPump::setExtraOutput(uint8_t pcfBit, bool on) {
    if (!_flags.usePcf || pcfBit < 4 || pcfBit > 7) return;
    bool pcfHigh = _flags.activeLow ? !on : on;
    if (pcfHigh) _pcfState |=  (1 << pcfBit);
    else         _pcfState &= ~(1 << pcfBit);
    if (_flags.ready) _writePcf();
}

bool ApaPump::getExtraOutput(uint8_t pcfBit) const {
    if (!_flags.usePcf || pcfBit < 4 || pcfBit > 7) return false;
    bool pcfHigh = (_pcfState >> pcfBit) & 0x01;
    return _flags.activeLow ? !pcfHigh : pcfHigh;
}

void ApaPump::setManualMode(ManualMode m) {
    if (!_flags.ready) return;
    if (m == _manualMode)  return;
    _manualMode     = m;
    _manualSetMs    = millis();
    _lastReminderMs = _manualSetMs;
    // Close solar valve immediately in manual mode — operator needs full pressure (vacuuming)
    if (m != AUTO) _setValve(false, _manualSetMs);
    if (m == AUTO) _fireStatus(F("Pump: AUTO restored"));
    else           _fireStatus(F("Pump: MANUAL mode"));
}
ManualMode ApaPump::getManualMode() const { return _manualMode; }
void       ApaPump::setManualTimeout(uint16_t minutes) { _manualTimeoutMin = minutes; }
void       ApaPump::setManualAutoReset(bool enabled)   { _flags.manualAutoReset = enabled ? 1 : 0; }

bool      ApaPump::isRunning() const { return (_pumpState == RUNNING); }
PumpState ApaPump::getState()  const { return _pumpState; }

void ApaPump::setPumpStateCallback(void (*cb)(bool))      { _stateChangeCb = cb; }
void ApaPump::setPumpAlarmCallback(void (*cb)(PumpAlarm)) { _alarmCb       = cb; }
void ApaPump::setStatusCallback(void (*cb)(const __FlashStringHelper*)) { _statusCb = cb; }

void ApaPump::acknowledgeAlarm() {
    if (!_flags.alarmActive) return;
    _flags.alarmActive = 0;
    _activeAlarm       = PUMP_ALARM_NONE;
    if (_alarmCb) _alarmCb(PUMP_ALARM_NONE);
}

PumpAlarm ApaPump::getAlarm()           const { return _activeAlarm; }
bool      ApaPump::isFreezeActive()     const { return _flags.freezeActive; }
float     ApaPump::getPressureBaseline() const { return _pressureEmaNormal; }

void ApaPump::setMinRunTime(uint16_t seconds) { _minRunTimeSec = seconds; _saveEEPROM(); }

void ApaPump::enableUVC(uint16_t preDelayS, uint16_t postDelayS) {
    if (_id[1] == 0xFF) return;
    _uvcPreDelayS  = preDelayS;
    _uvcPostDelayS = postDelayS;
    _flags.uvcEnabled = 1;
}

void ApaPump::enableAux(uint16_t startDelayS, uint16_t stopLeadS) {
    if (_id[2] == 0xFF) return;
    _auxStartDelayS = startDelayS;
    _auxStopLeadS   = stopLeadS;
    if (!_flags.savedSettings) {   // first boot; saved mode wins
        _flags.auxMode = AUX_FOLLOWER;
        if (_flags.ready) _saveEEPROM();
    }
    _flags.auxEnabled = (_flags.auxMode != AUX_DISABLED);
}

void ApaPump::enableSolar(
    float (*solarTempCb)(), float (*poolTempCb)(),
    bool useValve,
    float startDelta, float stopDelta,
    float maxPoolTemp, float coolDelta, float safetyTemp,
    ValveMode valveMode, uint16_t pulseDurationMs
) {
    _solarTempCb           = solarTempCb;
    if (poolTempCb) _poolTempCb = poolTempCb;   // else keep setPoolTempCallback()
    _flags.solarValveEnabled = (_id[3] != 0xFF) ? 1 : 0;   // valve relay wired
    _solarStartDelta       = startDelta;
    _solarStopDelta        = stopDelta;
    if (!_flags.savedSettings) {   // first boot: defaults from the sketch, else saved values win
        _solarMaxPoolTemp = maxPoolTemp;
        _flags.solarMode  = (useValve && _flags.solarValveEnabled) ? SOLAR_VALVE : SOLAR_PUMP;
        if (_flags.ready) _saveEEPROM();
    }
    _solarCoolDelta        = coolDelta;
    _solarSafetyTemp       = safetyTemp;
    _solarValveMode        = valveMode;
    _solarPulseDurationMs  = pulseDurationMs;
    _flags.solarEnabled    = 1;
}

void ApaPump::setSolarDayNight(uint32_t (*epochCb)(), uint8_t dayStartHour, uint8_t nightStartHour) {
    _solarEpochCb        = epochCb;
    _solarDayStartHour   = dayStartHour;
    _solarNightStartHour = nightStartHour;
    _flags.solarDayNight = 1;
}

void ApaPump::setCurrentCallback(float (*cb)()) {
    _currentCb            = cb;
    _flags.currentEnabled = 1;
}

void ApaPump::resetCurrentBaseline() {
    _currentEma          = 0.0f;
    _currentSampleCount  = 0;
    _flags.baselineReady = 0;
}

void ApaPump::enablePressure(float (*pressureCb)(), float maxPressure) {
    _pressureCb            = pressureCb;
    _maxPressure           = maxPressure;
    _flags.pressureEnabled = !_pressureUserOff;   // a saved setPressureEnabled(false) wins
}

// Clearing _lastPressure matters: update() only overwrites it with valid readings, so a
// stale value would otherwise stay in the dry-run / overpressure checks after OFF -> ON.
void ApaPump::setPressureEnabled(bool on) {
    if (on && !_pressureCb) return;   // enablePressure() was never called
    _flags.pressureEnabled  = on ? 1 : 0;
    _pressureUserOff        = !on;
    if (_flags.ready) _saveEEPROM();
    if (on) return;
    _lastPressure           = -1.0f;
    _pressureEmaNormal      = 0.0f;
    _pressureEmaSolar       = 0.0f;
    _pressureEmaCountNormal = 0;
    _pressureEmaCountSolar  = 0;
}
bool  ApaPump::isPressureEnabled()    const { return _flags.pressureEnabled; }

float ApaPump::getPressure()          const { return (_lastPressure >= 0.0f) ? _lastPressure : 0.0f; }
bool  ApaPump::isPressureCalibrated() const { return _flags.pressureEnabled && _lastPressure >= 0.0f; }

void  ApaPump::setFilterThresholds(float w, float b) { _filterWarningDelta = w; _filterBackwashDelta = b; }
void  ApaPump::setCleanPressure(float bar)           { _cleanPressure = (bar > 0.0f) ? bar : 0.0f; _saveEEPROM(); }
float ApaPump::getCleanPressure()       const        { return _cleanPressure; }

// A single reading could be a spike or taken with the pump off — instead restart the
// valve-closed baseline; _updatePressureEma() stores its first valid value as clean.
void ApaPump::learnCleanPressure() {
    if (!_flags.pressureEnabled) return;
    _cleanPressure          = 0.0f;
    _pressureEmaNormal      = 0.0f;
    _pressureEmaCountNormal = 0;
    _saveEEPROM();   // a reboot before the new value is learned still re-learns
}

FilterStatus ApaPump::getFilterStatus() const {
    if (!_flags.pressureEnabled || _cleanPressure <= 0.0f ||
        _pressureEmaCountNormal < PRESSURE_COLD_START_SAMPLES) return FILTER_UNKNOWN;
    float rise = _pressureEmaNormal - _cleanPressure;
    if (rise >= _filterBackwashDelta) return FILTER_BACKWASH_NEEDED;
    if (rise >= _filterWarningDelta)  return FILTER_FILLING;
    return FILTER_CLEAN;
}

void ApaPump::setDailyTarget(uint16_t minutes) { _dailyTargetMin = minutes; _saveEEPROM(); }
uint16_t ApaPump::getDailyRuntimeMinutes() const { return _dailyRuntimeMin; }
bool     ApaPump::isDailyTargetMet()       const { return _flags.dailyTargetMet; }
void     ApaPump::resetDailyCounter()            { _dailyRuntimeMin = 0; _flags.dailyTargetMet = 0; }

void ApaPump::setMidnightCallback(uint32_t (*epochCb)()) {
    _midnightCb           = epochCb;
    _flags.midnightEnabled = 1;
}

void ApaPump::enableFreezeProtection(float (*tempCb)(), float thresholdC) {
    _freezeTempCb        = tempCb;
    _freezeThresholdC    = thresholdC;
    _flags.freezeEnabled = 1;
}

void ApaPump::setPressurePeakAlarm(uint8_t peakPct) {
    _pressurePeakPct            = peakPct;
    _flags.pressurePeakEnabled  = (peakPct > 0) ? 1 : 0;
}

void ApaPump::setFlowCallback(bool (*cb)()) {
    _flowCb            = cb;
    _flags.flowEnabled = (cb != nullptr) ? 1 : 0;
}

void ApaPump::setCatchupWindow(uint8_t startHour, uint8_t endHour) {
    _catchupStartHour    = startHour;
    _catchupEndHour      = endHour;
    _flags.catchupWindow = 1;
}

// ============================================================================
// Status helper
// ============================================================================

void ApaPump::_fireStatus(const __FlashStringHelper* msg) {
    if (_statusCb) _statusCb(msg);
}

// ============================================================================
// EEPROM
// ============================================================================

void ApaPump::_loadEEPROM() {
    EepromData d;
    EEPROM.get(APAPUMP_EEPROM_ADDR, d);

    uint8_t cs = 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&d);
    for (uint8_t i = 0; i < sizeof(d) - 1; i++) cs += p[i];

    if (d.magic != EEPROM_MAGIC || d.version != EEPROM_VERSION || cs != d.checksum) {
        // First boot (or older layout): the solar/AUX/Max pool values set by enable*() so far
        // are the defaults; enable*() called after begin() save their defaults themselves.
        _dailyTargetMin      = 0;
        _yesterdayRuntimeMin = 0;
        _minRunTimeSec       = APAPUMP_MIN_RUN_SEC;
        _cleanPressure       = 0.0f;
        _saveEEPROM();
        return;
    }

    _dailyTargetMin      = d.dailyTargetMin;
    _yesterdayRuntimeMin = d.yesterdayRuntimeMin;
    _minRunTimeSec       = d.minRunTimeSec;
    _cleanPressure       = (float)d.cleanPressure100 / 100.0f;
    _pressureUserOff     = !(d.settings & 0x01);
    if (_pressureCb) _flags.pressureEnabled = !_pressureUserOff;
    _flags.solarMode     = (d.settings >> 1) & 0x03;
    if (_flags.solarMode > SOLAR_VALVE) _flags.solarMode = SOLAR_OFF;
    _flags.auxMode       = (d.settings >> 3) & 0x03;
    // A saved mode never switches on a relay this sketch did not register: only when
    // enableAux() already ran (enableAux() / setAuxMode() after begin() decide themselves).
    if (_flags.auxEnabled) _flags.auxEnabled = (_flags.auxMode != AUX_DISABLED);
    _solarMaxPoolTemp    = d.solarMaxPoolC;
    _heaterTargetC       = constrain(d.heaterTargetC, APAPUMP_HEATER_TARGET_MIN, APAPUMP_HEATER_TARGET_MAX);
    _flags.savedSettings = 1;
}

void ApaPump::_saveEEPROM() {
    EepromData d;
    d.magic               = EEPROM_MAGIC;
    d.version             = EEPROM_VERSION;
    d.dailyTargetMin      = _dailyTargetMin;
    d.yesterdayRuntimeMin = _yesterdayRuntimeMin;
    d.minRunTimeSec       = _minRunTimeSec;
    d.cleanPressure100    = (uint16_t)(_cleanPressure * 100.0f);
    d.settings            = (_pressureUserOff ? 0 : 0x01) | (_flags.solarMode << 1) | (_flags.auxMode << 3);
    d.solarMaxPoolC       = (uint8_t)_solarMaxPoolTemp;
    d.heaterTargetC       = _heaterTargetC;

    uint8_t cs = 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&d);
    for (uint8_t i = 0; i < sizeof(d) - 1; i++) cs += p[i];
    d.checksum = cs;

    EEPROM.put(APAPUMP_EEPROM_ADDR, d);
#if defined(ESP32) || defined(ESP8266)
    EEPROM.commit();
#endif
}
