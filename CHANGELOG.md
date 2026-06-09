# Changelog — APAPUMP

## [1.0.1] — 2026-06-09

### Fixed

- **Freeze protection now cycles instead of running continuously.** When pool temp drops below threshold, the pump runs for `APAPUMP_FREEZE_ON_SEC` (5 min default) then rests for `APAPUMP_FREEZE_OFF_SEC` (10 min default), repeating until temp rises. Brief periodic circulation is sufficient to prevent pipe freeze and saves significant energy compared to continuous operation. Override defaults via `build_flags`: `-DAPAPUMP_FREEZE_ON_SEC=180`.
- Cycle timing reuses existing `_pumpStartMs` / `_pumpStopMs` timestamps — zero extra SRAM.
- Fixed `setActiveLow()` example in README: the call was shown commented-out, implying it was optional rather than required.
- Fixed state machine diagram: alarm list annotation was merged onto the `_shouldPumpRun() = false` line, making both unreadable.
- Fixed README logo path (`apapump.png` → `apapump-logo.png`).

## [1.0.0] — 2026-06-05

### Added

**Core state machine**
- Non-blocking priority engine: IDLE → STARTING → RUNNING → STOPPING
- PCF8574AT I2C relay expander mode (default, 0x3C) and direct Arduino GPIO mode
- Zero-arg constructor targets APA Devices HMI board v1.0 (pump=P0, uvc=P1, aux=P2, valve=P3)
- Safe boot: all relays written OFF at `begin()`
- `setActiveLow()` for active-low relay modules (GPIO mode)
- Extension relay management: `setExtraOutput()` / `getExtraOutput()` for PCF bits 4–7
- Min OFF time (60 s) between pump cycles; min run time (300 s, configurable)
- STARTING cancellation: sequence aborts cleanly if request disappears during UVC pre-delay

**Manual override**
- `setManualMode(FORCE_ON / FORCE_OFF / AUTO)` with full guard suspension
- Auto-return timeout via `setManualTimeout(minutes)` or constructor parameter
- Auto-return at midnight via `setManualAutoReset(true)` (requires RTC callback)
- 30-minute manual-mode status reminder via `setStatusCallback()`
- Solar valve closes immediately on `FORCE_ON` entry (full system pressure for vacuuming)

**Follower devices**
- UVC sanitizer: configurable pre-delay (5 s default) and post-delay (30 s default)
- AUX device: configurable start-delay (10 s) and stop-lead (30 s)

**Solar heating**
- Absorber–pool temperature dead-band hysteresis (`startDelta` / `stopDelta`)
- Pool cooling mode: runs when absorber < pool − coolDelta and pool > maxPoolTemp
- Solar safety (Priority 0): absorber > safetyTemp (50 °C) forces pump ON + valve OPEN; overrides `FORCE_OFF`; auto-clears with status message
- Day/night boundary gate via `setSolarDayNight(epochCb, startHour, endHour)`
- Solar valve mode: sustained (default) or pulse

**Scheduling and daily tracking**
- `scheduleCb` drives pump on/off in non-solar mode
- `externalRequestCb` for priority-3 on requests (post-shock bridge pattern)
- `dailyTargetCb` for live target from APALCDGUI timer sum
- Daily runtime accumulation with minute-resolution counter
- Midnight rollover via RTC epoch (any day change detected) or millis() 24-hour fallback
- `setDailyTarget()` / `getDailyRuntimeMinutes()` / `isDailyTargetMet()` / `resetDailyCounter()`

**Safety: overcurrent**
- EMA-learned current baseline (alpha = 0.05, 10-second sample interval)
- 5-sample cold-start gate before alarm arms; 1.5× threshold fires `PUMP_ALARM_OVERCURRENT` (latching)
- 30-second inrush settle before EMA sampling begins
- `resetCurrentBaseline()` for post-service recalibration

**Safety: pressure**
- Dual pressure EMA baselines: normal running and solar-valve-open (valve adds head resistance)
- Dry-run detection: pressure < 40% of EMA baseline → `PUMP_ALARM_LOW_PRESSURE`
- Pre-EMA absolute guard: pressure < 0.1 bar fires dry-run alarm before EMA builds
- Absolute overpressure: pressure > `maxPressure` → `PUMP_ALARM_HIGH_PRESSURE`
- EMA-relative overpressure: pressure > baseline × (1 + peakPct%) → `PUMP_ALARM_HIGH_PRESSURE`
- `getPressureBaseline()` returns learned normal-running pressure for HMI display
- Filter status stubs (`learnCleanPressure`, `getFilterStatus`…) — deferred to post-field-testing

**Safety: freeze protection**
- `enableFreezeProtection(tempCb, thresholdC = 4.5f)` at Priority 4
- Dry-run interlock: suppressed if pressure EMA confirms no water in pipes
- `isFreezeActive()` for HMI status display

**Safety: flow switch stub**
- `setFlowCallback(cb)` — fires `PUMP_ALARM_NO_FLOW` if no flow after 30 s settle

**Catch-up window**
- `setCatchupWindow(startHour, endHour)` restricts daily catch-up to configured hours
- Window wraps midnight if startHour > endHour; silently ignored without RTC

**Callbacks and state query**
- `setPumpStateCallback(cb)`, `setPumpAlarmCallback(cb)`, `setStatusCallback(cb)`
- `getAlarm()` — poll current alarm type at any time
- `getState()`, `isRunning()`, `isFreezeActive()`, `getPressureBaseline()`

**EEPROM**
- 12-byte config block at address 520 (configurable via `APAPUMP_EEPROM_ADDR`)
- Magic number + version + checksum; mismatch resets to factory defaults
- ESP32 / ESP8266: `EEPROM.commit()` called automatically on every save
