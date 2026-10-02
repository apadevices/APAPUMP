# Changelog — APAPUMP

## [1.2.0] — 2026-10-01

### Added

- `setPressureEnabled(bool)` / `isPressureEnabled()` — switch pressure monitoring off and on at runtime (after `enablePressure()`), e.g. from an installer menu for pools without a pressure sensor. OFF stops the dry-run, high-pressure and filter-status checks and clears the last reading and both learned baselines (a stale reading would otherwise stay in the checks); the stored clean-filter pressure is kept. Before 1.2.0 pressure monitoring could only be enabled, never disabled. 0 bytes RAM.
- README section "Pressure sensor optional".

## [1.1.1] — 2026-10-01

### Fixed

- **The sample that raised an alarm was still learned.** 1.1.0 stopped learning while an alarm is active, but in `update()` the 10 s learning tick ran *before* the safety checks, so the one pressure sample that triggered the alarm was added to the baseline first; `_updateCurrentEma()` likewise added the overcurrent sample before comparing it. During the 5-sample start-up average one such sample weighs 20–100 % — found on the APA-CONTROLLER bench, where a 3.1 bar overpressure sample raised the baseline from 0.20 to 0.93 bar (and as the 5th sample it would have become the clean-filter pressure). Now the safety checks run first, the learning tick is skipped once an alarm is raised, and the current is compared before it is learned.

## [1.1.0] — 2026-09-30

### Added

- **Filter status is now real.** `getFilterStatus()` returns `FILTER_CLEAN` / `FILTER_FILLING` / `FILTER_BACKWASH_NEEDED` from the rise of the running pressure (solar valve closed) above the clean-filter pressure — previously it always returned `FILTER_UNKNOWN`. Status only, no alarm.
- **Clean pressure learned automatically** on the first run after installation (first stable baseline, ~80 s of running, pump running with solar valve closed, at least 0.1 bar) and saved to EEPROM.
- New overridable constants (`build_flags`): `APAPUMP_FILTER_WARN_DELTA` (0.4 bar), `APAPUMP_FILTER_BACKWASH_DELTA` (0.8 bar), `APAPUMP_CURRENT_MIN_A` (0.5 A).
- README: new sections "Filter status — when to backwash", "Overcurrent protection" (incl. direct drive vs. external contactor) and "Stopping the pump on an alarm".

### Changed

- `learnCleanPressure()` no longer copies one raw reading (a spike, or a stopped pump, could be stored). It clears the stored value and restarts the valve-closed baseline; the next ~80 s of running learns the new clean pressure. With the pump off, it waits for the next run.
- Default filter thresholds 0.3 / 0.6 bar → 0.4 / 0.8 bar (household pools).
- Current readings below `APAPUMP_CURRENT_MIN_A` (0.5 A) are ignored: no motor on the sensor (noise, or only an external contactor's coil). Overcurrent protection stays silent instead of learning noise and raising false alarms.
- `setCleanPressure()` clamps negative values to 0 (= not learned).

### Fixed

- **Safety: learned baselines kept learning during an alarm.** The library does not stop the pump on an alarm, so a dry-running pump taught the pressure baseline ~0 bar within ~50 s — after `acknowledgeAlarm()` the dry-run check (`pressure < baseline × 40 %`) could never fire again until reboot. An overcurrent likewise raised its own threshold. Now neither baseline is updated while an alarm is active, and pressure below `APAPUMP_PRESSURE_ABS_MIN` (0.1 bar) is never learned.
- **Baselines no longer rest on one reading.** The start-up EMA was seeded with the first sample and kept ~81 % of it after 5 samples (weight 5 %), so one spike at 30 s could set the dry-run, overcurrent and clean-filter reference. The first 5 samples are now averaged, then the slow EMA takes over.
- Docs: the relative high-pressure alarm (`setPressurePeakAlarm()`) was described as dirty-filter detection. Its baseline follows slow changes within minutes, so it only catches **sudden** rises — filter clogging is reported by `getFilterStatus()`.
- Docs: overcurrent is checked with each 10 s current sample, not "every update() tick".
- Docs: `getPressureBaseline()` needs 5 samples (~80 s of running), not "5 pump runs".
- Docs + example 03: post-shock bridge uses an elapsed-time check (the old `millis() < until` form breaks when `millis()` wraps); `maxPressure` example 4.0 → 2.5 bar (household filter rating).
- EEPROM address map updated: APADOSE 192–291 (25 B × 4), APASENSE 582–587.

## [1.0.3] — 2026-06-11

### Fixed

- Updated "Wiring a buzzer or alarm output" section to show both a direct GPIO pattern and the recommended APASENSE integration (`alert(BUZZER_ALARM, true)` / `stopAlert()`).
- Fixed commented `setLed` example: index corrected from 4 to 0 (APASENSE LED index is 0–3, mapping to PCF P4–P7).

## [1.0.2] — 2026-06-10

### Fixed

- Fixed state machine diagram alignment in README (consistent right-border column throughout).
- Fixed freeze protection feature bullet: "continuous run" → "cyclic run (5 min ON / 10 min rest)" to match v1.0.1 behaviour.
- Fixed SRAM footprint callout: now references Arduino Uno (2 KB) instead of Mega to better reflect the lightweight design goal.
- Added "Wiring a buzzer or alarm output" section to README showing the alarm callback pattern for physical indicators.

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
