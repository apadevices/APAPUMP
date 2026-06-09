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
    _filterWarningDelta = 0.3f; _filterBackwashDelta = 0.6f;
    _lastPressure      = -1.0f;
    _currentEma        = 0.0f; _currentSampleCount  = 0;
    _pressureEmaNormal      = 0.0f; _pressureEmaSolar       = 0.0f;
    _pressureEmaCountNormal = 0;    _pressureEmaCountSolar  = 0;
    _pressurePeakPct        = 0;
    _freezeTempCb      = nullptr; _freezeThresholdC   = APAPUMP_FREEZE_THRESHOLD_C;
    _flowCb            = nullptr;
    _catchupStartHour  = 0;   _catchupEndHour      = 0;
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
    _filterWarningDelta = 0.3f; _filterBackwashDelta = 0.6f;
    _lastPressure      = -1.0f;
    _currentEma        = 0.0f; _currentSampleCount  = 0;
    _pressureEmaNormal      = 0.0f; _pressureEmaSolar       = 0.0f;
    _pressureEmaCountNormal = 0;    _pressureEmaCountSolar  = 0;
    _pressurePeakPct        = 0;
    _freezeTempCb      = nullptr; _freezeThresholdC   = APAPUMP_FREEZE_THRESHOLD_C;
    _flowCb            = nullptr;
    _catchupStartHour  = 0;   _catchupEndHour      = 0;
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

    // ---- EMA tick (10 s after inrush settle, pump running) -----------------
    if (_pumpState == RUNNING &&
        (now - _pumpStartMs) >= (uint32_t)APAPUMP_CURRENT_SETTLE_SEC * 1000UL &&
        (now - _lastEmaMs)   >= APAPUMP_CURRENT_SAMPLE_MS) {
        _lastEmaMs = now;
        _updateCurrentEma();
        _updatePressureEma();
    }

    // ---- Safety alarm checks (after inrush settle, pump running) -----------
    if (_pumpState == RUNNING && !_flags.alarmActive &&
        (now - _pumpStartMs) >= (uint32_t)APAPUMP_CURRENT_SETTLE_SEC * 1000UL) {

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

    // ---- Solar safety override (Priority 0) --------------------------------
    // Applies only when solar is enabled. Beats FORCE_OFF — absorber must circulate.
    if (_flags.solarEnabled && _solarTempCb) {
        float absorber = _solarTempCb();
        if (absorber > _solarSafetyTemp) {
            if (!_flags.solarSafetyActive) {
                _flags.solarSafetyActive = 1;
                _fireStatus(F("Solar safety: absorber hot"));
            }
            if (_pumpState == IDLE)     _startPumpSequence(now);
            if (_flags.solarValveEnabled) _setRelay(3, true);
            return;  // skip normal engine while safety is active
        }
        if (_flags.solarSafetyActive) {
            _flags.solarSafetyActive = 0;
            _fireStatus(F("Solar safety: absorber cooled"));
        }
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
            // Solar valve: open when solar drives; closed in manual mode (max pressure)
            if (_flags.solarValveEnabled) {
                bool valveOn = (_flags.solarEnabled && _manualMode == AUTO && _flags.solarRunning);
                _setRelay(3, valveOn);
            }
        }

    } else if (!wantPump && _pumpState == RUNNING) {
        // Want to stop — check min run time (suspended in manual mode)
        bool minRunOk = (_manualMode != AUTO) ||
                        (now - _pumpStartMs >= (uint32_t)_minRunTimeSec * 1000UL);
        if (minRunOk) {
            _stopPumpSequence(now);
            if (_flags.solarValveEnabled) _setRelay(3, false);
        }

    } else if (!wantPump && _pumpState == IDLE && _flags.solarRunning) {
        // Solar no longer wants to run — clear the solar hysteresis flag
        _flags.solarRunning = 0;
    }
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
        float t = _freezeTempCb();
        if (t >= 0.0f && t < _freezeThresholdC) {
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
        } else if (_flags.freezeActive) {
            _flags.freezeActive  = 0;
            _flags.freezeCycleOn = 0;
            _fireStatus(F("Freeze protection deactivated"));
        }
    }

    // Solar enabled: solar is the on/off driver
    if (_flags.solarEnabled) {
        if (_solarWantsRun()) return true;
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

    // Solar disabled: schedule drives
    if (_scheduleCb && _scheduleCb()) return true;

    return false;
}

bool ApaPump::_solarWantsRun() {
    if (!_solarTempCb) return false;

    float absorber = _solarTempCb();
    float pool     = _poolTempCb ? _poolTempCb() : 0.0f;

    // Day/night gate — cooling allowed at night, heating only during day
    bool isDaytime = true;
    if (_flags.solarDayNight && _solarEpochCb) {
        uint32_t epoch = _solarEpochCb();
        uint8_t  hour  = (uint8_t)((epoch % 86400UL) / 3600UL);
        isDaytime = (hour >= _solarDayStartHour && hour < _solarNightStartHour);
    }

    // Pool cooling: pool overheated, absorber is cooler
    if (_solarMaxPoolTemp > 0.0f && pool > _solarMaxPoolTemp) {
        if (absorber < pool - _solarCoolDelta) {
            _flags.solarRunning = 1;
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
                return false;
            }
        }
        return false;
    }

    if (_flags.solarRunning) {
        // Currently running — stop at stopDelta (hysteresis lower edge)
        if (absorber < pool + _solarStopDelta) {
            _flags.solarRunning = 0;
            return false;
        }
        return true;
    } else {
        // Currently stopped — start at startDelta (hysteresis upper edge)
        bool poolNotOverheated = (_solarMaxPoolTemp <= 0.0f || pool < _solarMaxPoolTemp);
        if (absorber > pool + _solarStartDelta && poolNotOverheated) {
            _flags.solarRunning = 1;
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
    if (_flags.auxEnabled && _flags.auxRunning && _id[2] != 0xFF) {
        _setRelay(2, false);
        _flags.auxRunning = 0;
    }

    // If no AUX stop lead needed, go straight to turning pump off
    if (!_flags.auxEnabled || _auxStopLeadS == 0) {
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
            // AUX start delay: turn AUX on after pump has run long enough for flow
            if (_flags.auxEnabled && !_flags.auxRunning && _id[2] != 0xFF) {
                if (now - _pumpStartMs >= (uint32_t)_auxStartDelayS * 1000UL) {
                    _setRelay(2, true);
                    _flags.auxRunning = 1;
                }
            }
            break;

        case STOPPING:
            if (_isRelayOn(0)) {
                // Phase A — pump still running, waiting for AUX stop lead
                if (now - _pumpStopMs >= (uint32_t)_auxStopLeadS * 1000UL) {
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
    if (measured < 0.0f) return;

    if (_currentSampleCount < CURRENT_COLD_START_SAMPLES) {
        _currentSampleCount++;
        _currentEma = (_currentSampleCount == 1)
                      ? measured
                      : _currentEma + CURRENT_EMA_ALPHA * (measured - _currentEma);
        if (_currentSampleCount >= CURRENT_COLD_START_SAMPLES) _flags.baselineReady = 1;
    } else {
        _currentEma += CURRENT_EMA_ALPHA * (measured - _currentEma);
    }

    if (_flags.baselineReady && _currentEma > 0.0f) {
        if (measured > _currentEma * CURRENT_ALARM_MULTIPLIER && !_flags.alarmActive) {
            _flags.alarmActive = 1;
            _activeAlarm       = PUMP_ALARM_OVERCURRENT;
            if (_alarmCb) _alarmCb(PUMP_ALARM_OVERCURRENT);
        }
    }
}

// ============================================================================
// Pressure EMA — dual baseline (valve-closed / valve-open)
// ============================================================================

void ApaPump::_updatePressureEma() {
    // Called from update() EMA tick gate — timing and state guards handled by caller
    if (!_flags.pressureEnabled || !_pressureCb) return;
    if (_lastPressure < 0.0f) return;  // not yet calibrated

    bool valveOpen = _flags.solarValveEnabled && _isRelayOn(3);

    if (valveOpen) {
        if (_pressureEmaCountSolar < PRESSURE_COLD_START_SAMPLES) {
            _pressureEmaCountSolar++;
            _pressureEmaSolar = (_pressureEmaCountSolar == 1)
                                ? _lastPressure
                                : _pressureEmaSolar + CURRENT_EMA_ALPHA * (_lastPressure - _pressureEmaSolar);
        } else {
            _pressureEmaSolar += CURRENT_EMA_ALPHA * (_lastPressure - _pressureEmaSolar);
        }
    } else {
        if (_pressureEmaCountNormal < PRESSURE_COLD_START_SAMPLES) {
            _pressureEmaCountNormal++;
            _pressureEmaNormal = (_pressureEmaCountNormal == 1)
                                 ? _lastPressure
                                 : _pressureEmaNormal + CURRENT_EMA_ALPHA * (_lastPressure - _pressureEmaNormal);
        } else {
            _pressureEmaNormal += CURRENT_EMA_ALPHA * (_lastPressure - _pressureEmaNormal);
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
    if (m != AUTO && _flags.solarValveEnabled) _setRelay(3, false);
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
    _flags.auxEnabled = 1;
}

void ApaPump::enableSolar(
    float (*solarTempCb)(), float (*poolTempCb)(),
    bool useValve,
    float startDelta, float stopDelta,
    float maxPoolTemp, float coolDelta, float safetyTemp,
    ValveMode valveMode, uint16_t pulseDurationMs
) {
    _solarTempCb           = solarTempCb;
    _poolTempCb            = poolTempCb;
    _flags.solarValveEnabled = (useValve && _id[3] != 0xFF) ? 1 : 0;
    _solarStartDelta       = startDelta;
    _solarStopDelta        = stopDelta;
    _solarMaxPoolTemp      = maxPoolTemp;
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
    _flags.pressureEnabled = 1;
}

float ApaPump::getPressure()          const { return (_lastPressure >= 0.0f) ? _lastPressure : 0.0f; }
bool  ApaPump::isPressureCalibrated() const { return _flags.pressureEnabled && _lastPressure >= 0.0f; }

void         ApaPump::setFilterThresholds(float w, float b) { _filterWarningDelta = w; _filterBackwashDelta = b; }
void         ApaPump::learnCleanPressure()                  { if (isPressureCalibrated()) { _cleanPressure = _lastPressure; _saveEEPROM(); } }
void         ApaPump::setCleanPressure(float bar)           { _cleanPressure = bar; _saveEEPROM(); }
float        ApaPump::getCleanPressure()       const        { return _cleanPressure; }
FilterStatus ApaPump::getFilterStatus()        const        { return FILTER_UNKNOWN; }  // Phase 2

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
}

void ApaPump::_saveEEPROM() {
    EepromData d;
    d.magic               = EEPROM_MAGIC;
    d.version             = EEPROM_VERSION;
    d.dailyTargetMin      = _dailyTargetMin;
    d.yesterdayRuntimeMin = _yesterdayRuntimeMin;
    d.minRunTimeSec       = _minRunTimeSec;
    d.cleanPressure100    = (uint16_t)(_cleanPressure * 100.0f);

    uint8_t cs = 0;
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&d);
    for (uint8_t i = 0; i < sizeof(d) - 1; i++) cs += p[i];
    d.checksum = cs;

    EEPROM.put(APAPUMP_EEPROM_ADDR, d);
#if defined(ESP32) || defined(ESP8266)
    EEPROM.commit();
#endif
}
