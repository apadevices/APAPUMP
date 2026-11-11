# APAPUMP API Reference — v1.3.3

## Quick-start examples

**PCF mode (APA HMI board defaults):**
```cpp
#include <Wire.h>
#include <APAPUMP.h>

ApaPump pump;           // PCF at 0x3C — pump=P0, uvc=P1, aux=P2, valve=P3

void setup() {
    Wire.begin();
    pump.begin();
}
void loop() { pump.update(); }
```

**Direct GPIO:**
```cpp
#include <APAPUMP.h>

ApaPump pump(RELAY_DIRECT, 7);   // pump relay on pin 7

void setup() { pump.begin(); }
void loop()  { pump.update(); }
```

---

## Setup order

Call these in order — some are optional:

| Step | Call | Required? |
|------|------|-----------|
| 1 | `Wire.begin()` | PCF mode only — before `pump.begin()` |
| 2 | `adc.begin()` | If using APASENSE — must precede `pump.begin()` |
| 3 | `pump.setActiveLow()` | DIRECT mode only, active-low modules |
| 4 | `pump.begin(scheduleCb, externalRequestCb, dailyTargetCb)` | Always |
| 5 | `pump.setPoolTempCallback()`, `enableUVC()`, `enableAux()`, `enableSolar()`, `setSolarDayNight()` | Optional |
| 6 | `pump.enablePressure()`, `setCurrentCallback()` | Optional |
| 7 | `pump.enableFreezeProtection()`, `setPressurePeakAlarm()` | Optional |
| 8 | `pump.setMidnightCallback()` | Optional — enables RTC midnight reset |
| 9 | `pump.setDailyTarget()`, `setCatchupWindow()` | Optional |
| 10 | `if (!pump.hasSavedSettings()) { … }` — your first-boot defaults (`setSolarMode()`, `setAuxMode()` …) | Optional |

Saved settings (solar mode, Max pool, AUX mode, heater target, pressure ON/OFF) are loaded by `begin()`;
`enableSolar()` / `enableAux()` give first-boot defaults only. See [Saved settings](#saved-settings).

---

## Constructors

### Zero-arg (APA HMI board v1.0 defaults)

```cpp
ApaPump();
```

PCF8574AT at address `0x3C`. Relay mapping: pump=P0, uvc=P1, aux=P2, valve=P3. Call `Wire.begin()` before `pump.begin()`.

---

### Configurable constructor

```cpp
ApaPump(RelayDriver mode, uint8_t a,
        uint8_t  b = 0xFF, uint8_t c = 0xFF,
        uint8_t  d = 0xFF, uint8_t e = 0xFF,
        uint16_t manualTimeoutMin = 0);
```

| Parameter | Description |
|-----------|-------------|
| `mode` | `RELAY_PCF` — I2C expander; `RELAY_DIRECT` — Arduino GPIO |
| **PCF mode** | `a` = 7-bit I2C address; `b` = pump bit; `c`/`d`/`e` = uvc/aux/valve bits (0xFF = unused) |
| **DIRECT mode** | `a` = pump pin; `b`/`c`/`d` = uvc/aux/valve pins (0xFF = not wired); `e` = unused |
| `manualTimeoutMin` | Auto-return to AUTO after N minutes in manual mode (0 = never) |

**Examples:**
```cpp
// PCF at 0x38, custom bit mapping
ApaPump pump(RELAY_PCF, 0x38, 0, 1, 2, 3);

// Direct GPIO: pump=7, uvc=8, aux=9, valve=10
ApaPump pump(RELAY_DIRECT, 7, 8, 9, 10);

// PCF with 60-minute manual timeout
ApaPump pump(RELAY_PCF, 0x3C, 0, 1, 2, 3, 60);
```

---

## Core methods

### `begin()`

```cpp
void begin(bool     (*scheduleCb)()        = nullptr,
           bool     (*externalRequestCb)() = nullptr,
           uint16_t (*dailyTargetCb)()     = nullptr);
```

Initialises relay hardware (all relays OFF), loads EEPROM, arms the state machine. Must be called once in `setup()`. Call `Wire.begin()` first in PCF mode.

| Callback | Returns | Used for |
|----------|---------|----------|
| `scheduleCb` | `true` when a timer slot is active | On/off driver in non-solar mode |
| `externalRequestCb` | `true` when an external source requests pump on | Priority 3 override (post-shock bridge, etc.) |
| `dailyTargetCb` | total scheduled minutes per day | Daily target — bridge: `[]() { return gui.getTimerTotalMinutes(); }` |

All three callbacks are optional (pass `nullptr`). Without `scheduleCb`, the pump only runs when manually forced or when external/solar/freeze logic requests it.

---

### `update()`

```cpp
void update();
```

Runs all pump logic — call every `loop()`. Never blocks, never calls `delay()`. Handles: manual timeout/reminder, follower state machine, daily counter, EMA sampling, safety alarm checks, priority engine.

---

## Relay driver

### `setActiveLow()`

```cpp
void setActiveLow();
```

Relay modules energised by LOW signal. Call before `begin()`. Ignored in PCF mode (PCF HIGH = relay ON, always).

---

### `setExtraOutput()` / `getExtraOutput()`

```cpp
void setExtraOutput(uint8_t pcfBit, bool on);
bool getExtraOutput(uint8_t pcfBit) const;
```

Control an extension relay on PCF bits 4–7 (optional lights/equipment board). Bit state is preserved on every APAPUMP relay write. No-op in DIRECT mode or if `pcfBit` is outside 4–7.

---

## Manual override

### `setManualMode()`

```cpp
void setManualMode(ManualMode m);
```

| Value | Behaviour |
|-------|-----------|
| `FORCE_ON` | Pump forced on. All timing guards suspended. Solar valve closes immediately. |
| `FORCE_OFF` | Pump forced off. All timing guards suspended. Solar overheat protection is skipped (since 1.3.0; it runs only in `AUTO`). Stops without the AUX after-run: AUX and the pump go off together (since 1.3.1). |
| `AUTO` | Return to automatic control. Fires status callback. |

---

### `getManualMode()`

```cpp
ManualMode getManualMode() const;
```

Returns `AUTO`, `FORCE_ON`, or `FORCE_OFF`.

---

### `setManualTimeout()`

```cpp
void setManualTimeout(uint16_t minutes);
```

Auto-return to `AUTO` after N minutes in manual mode. `0` = never (default). Can also be set in the constructor (7th parameter).

---

### `setManualAutoReset()`

```cpp
void setManualAutoReset(bool enabled);
```

When `true`, returns to `AUTO` at midnight (requires `setMidnightCallback()`).

---

## State query

### `isRunning()`

```cpp
bool isRunning() const;
```

Returns `true` when the pump relay is currently energised (state == RUNNING).

---

### `getState()`

```cpp
PumpState getState() const;
```

Returns the current state machine state.

| Value | Meaning |
|-------|---------|
| `IDLE` | Pump off, waiting for a start request |
| `STARTING` | UVC pre-delay in progress — pump not yet on |
| `RUNNING` | Pump relay on |
| `STOPPING` | AUX lead + UVC post-delay in progress (no AUX lead on `FORCE_OFF` or a pump alarm) |

---

### `isFreezeActive()`

```cpp
bool isFreezeActive() const;
```

Returns `true` when freeze protection is active (temperature below threshold). The pump may be in the rest phase of its cycle — use `isRunning()` to know whether the relay is actually on. Use to display a freeze indicator on the HMI.

---

### `getPressureBaseline()`

```cpp
float getPressureBaseline() const;
```

Returns the learned normal-running pressure baseline in bar (solar valve closed). Returns `0.0` until 5 samples have been collected — about 80 s of running after each boot. Use for HMI display to verify sensor calibration.

---

## Callbacks

### `setPumpStateCallback()`

```cpp
void setPumpStateCallback(void (*cb)(bool running));
```

Fires on every pump relay change: `true` = pump ON, `false` = pump OFF. Use to bridge pump state to APASENSE (pressure re-zero on stop) or to log runtime.

---

### `setPumpAlarmCallback()`

```cpp
void setPumpAlarmCallback(void (*cb)(PumpAlarm alarm));
```

Fires when an alarm condition is detected. All alarms are latching — call `acknowledgeAlarm()` to clear. Fires again with `PUMP_ALARM_NONE` after acknowledgment.

**The library never stops the pump on an alarm** — your sketch decides. Recommended: keep the pump in `FORCE_OFF` while `getAlarm() != PUMP_ALARM_NONE` (see README, "Stopping the pump on an alarm"). While an alarm is active the current and pressure baselines are not updated.

| Alarm | Condition |
|-------|-----------|
| `PUMP_ALARM_OVERCURRENT` | Current > learned baseline × 1.5 (direct-drive pumps; silent below 0.5 A) |
| `PUMP_ALARM_LOW_PRESSURE` | Pressure too low — dry-run suspected |
| `PUMP_ALARM_HIGH_PRESSURE` | Pressure above `maxPressure`, or a sudden rise (if `setPressurePeakAlarm()` is on) — closed valve or blockage |
| `PUMP_ALARM_NO_FLOW` | Flow switch reports no flow after settle |

A slowly clogging filter does not raise an alarm — see `getFilterStatus()`.

---

### `acknowledgeAlarm()`

```cpp
void acknowledgeAlarm();
```

Clears the active alarm and fires `alarmCb(PUMP_ALARM_NONE)` to confirm. The pump does not stop automatically on alarm — your callback decides whether to call `setManualMode(FORCE_OFF)`.

---

### `getAlarm()`

```cpp
PumpAlarm getAlarm() const;
```

Returns the currently active alarm type (`PUMP_ALARM_NONE` when no alarm). Use to poll alarm state on an HMI home screen without relying solely on the callback.

---

### `setStatusCallback()`

```cpp
void setStatusCallback(void (*cb)(const __FlashStringHelper* msg));
```

Fires status and reminder messages. All strings are stored in flash — print directly to Serial or forward to APALCDGUI:

```cpp
pump.setStatusCallback([](const __FlashStringHelper* msg) {
    Serial.println(msg);
});
```

Messages fired:
- `"Pump: MANUAL mode"` — on entry to FORCE_ON or FORCE_OFF
- `"Pump: AUTO restored"` — on return to AUTO
- `"Pump: AUTO restored (timeout)"` — after manual timeout
- `"Pump: AUTO restored (midnight)"` — at midnight if manualAutoReset enabled
- `"Pump still in MANUAL mode"` — every 30 minutes while in manual
- `"Solar safety: absorber hot"` — P0 override activated
- `"Solar safety: absorber cooled"` — P0 override cleared
- `"Freeze protection active"` — freeze P4 override activated
- `"Freeze protection deactivated"` — freeze P4 override cleared

---

## Pump timing

### `setMinRunTime()`

```cpp
void setMinRunTime(uint16_t seconds);
```

Minimum time the pump must run before it can stop (default `APAPUMP_MIN_RUN_SEC` = 300 s). Persisted to EEPROM. Suspended in manual mode.

---

## Followers

### `enableUVC()`

```cpp
void enableUVC(uint16_t preDelayS = 5, uint16_t postDelayS = 30);
```

Enable UVC sanitizer relay (PCF P1 / DIRECT pin b). UVC turns ON `preDelayS` seconds before pump; turns OFF `postDelayS` seconds after pump stops. No-op if uvc pin is `0xFF`.

---

### `enableAux()`

```cpp
void enableAux(uint16_t startDelayS = 10, uint16_t stopLeadS = 30);
```

Enable AUX device relay (PCF P2 / DIRECT pin c — heat pump, ozone, etc.). AUX turns ON `startDelayS` seconds after pump starts; turns OFF `stopLeadS` seconds before pump stops (= the pump's after-run; heat pumps usually want 60–180 s). No after-run when the stop comes from `FORCE_OFF` or a latched pump alarm — AUX and the pump go off together, and an after-run already counting is cut short (since 1.3.1). No-op if aux pin is `0xFF`. First-boot mode `AUX_FOLLOWER` (1.2.0 behaviour); a saved `setAuxMode()` wins.

---

### `setAuxMode()` / `getAuxMode()`

```cpp
void    setAuxMode(AuxMode m);   // saved
AuxMode getAuxMode() const;
```

| Mode | AUX relay |
|---|---|
| `AUX_DISABLED` | never on (switched off at once if on) |
| `AUX_FOLLOWER` | on/off with the pump — unchanged from 1.2.0 |
| `HEATER_SIMPLE` | heat pump allowed; its own thermostat regulates |
| `HEATER_FULL` | the library is the thermostat (`setHeaterTarget()`) |

Heater modes switch AUX on only while: the pump runs past `startDelayS` · no pump alarm is latched ·
solar is not heating, could not heat right now (valid collector above pool + startDelta inside the day
gate) and the collector is not above `safetyTemp` (solar first) · the permit callback (if
any) returns `true` · the pool is not above Max pool (when set and valid). `HEATER_FULL` adds: valid pool
reading, on below target − `APAPUMP_HEATER_HYST_C`, off at the target. Minimum ON/OFF time
`APAPUMP_HEATER_MIN_ON_SEC` / `APAPUMP_HEATER_MIN_OFF_SEC` (600 s), also after power-up; a pump alarm, a
pump stop or running overheat protection (`AUTO`, collector above `safetyTemp`; since 1.3.2) ends it at once.
When normal solar heating takes over, a running heater finishes its minimum ON time. No extra pump runs are requested. In heater modes the stop lead is skipped when AUX was already off;
in every AUX mode it is skipped on `FORCE_OFF` and on a latched pump alarm (since 1.3.1).

---

### `setHeaterTarget()` / `getHeaterTarget()`

```cpp
void    setHeaterTarget(uint8_t celsius);   // saved, clamped to 15–40, default 28
uint8_t getHeaterTarget() const;
```

`HEATER_FULL` only. Set the heat pump's own thermostat to its maximum, otherwise the lower setting wins.

---

### `setHeaterPermitCallback()` / `isHeaterOn()`

```cpp
void setHeaterPermitCallback(bool (*cb)());   // nullptr = always allowed (default)
bool isHeaterOn() const;                      // AUX relay on in a heater mode
```

The permit callback can carry a cheap-tariff input. Heater modes only.

---

## Solar heating

### `setPoolTempCallback()`

```cpp
void setPoolTempCallback(float (*cb)());
```

Pool temperature (°C) for solar and `HEATER_FULL`. `enableSolar()`'s `poolTempCb` sets the same callback
(`nullptr` there keeps this one). Return `-127.0f` while not ready. See [Valid temperatures](#valid-temperatures).

---

### `enableSolar()`

```cpp
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
```

Registers the sensors and constants — call it every boot.

| Parameter | Default | Description |
|-----------|---------|-------------|
| `solarTempCb` | — | Collector (absorber) temperature callback (°C). Required. |
| `poolTempCb` | `nullptr` | Pool temperature callback (°C). `nullptr` keeps `setPoolTempCallback()`. **Required for solar** — without a valid pool reading solar does nothing (since 1.3.0; before, a missing pool reading counted as 0 °C). |
| `useValve` | `false` | First-boot default: `true` → `SOLAR_VALVE`, `false` → `SOLAR_PUMP`. A saved `setSolarMode()` wins. |
| `startDelta` | 8.0 | Start heating when collector > pool + startDelta (°C) |
| `stopDelta` | 3.0 | Stop heating when collector < pool + stopDelta (°C). Must be < startDelta. |
| `maxPoolTemp` | 0.0 | First-boot Max pool (°C, 0 = no limit). Solar heating stops at it — a running run ends once the pool reaches Max pool (since 1.3.3) and may start again below Max pool − `APAPUMP_SOLAR_MAX_POOL_HYST_C` (0.5 °C); above it cooling runs when collector < pool − coolDelta. A saved `setSolarMaxPool()` wins. |
| `coolDelta` | 2.0 | Cooling dead-band (°C) |
| `safetyTemp` | 50.0 | Overheat protection (°C): pump ON + valve OPEN — **only in `AUTO` and never during a pump alarm** (since 1.3.0). |
| `valveMode` | `VALVE_SUSTAINED` | `VALVE_SUSTAINED`: valve relay stays on while open. `VALVE_PULSE`: hardware stub, not implemented. |
| `pulseDurationMs` | 500 | Pulse duration when `valveMode = VALVE_PULSE` |

Default drive `SOLAR_WITH_SCHEDULE`: the schedule keeps driving the pump; solar heating and cooling are
extra runs. The valve follows the solar demand on every `update()`, also during a scheduled run, and is
closed in manual mode. After every valve change the pressure checks and baseline learning pause for
`APAPUMP_VALVE_SETTLE_SEC` (30 s). While a pump alarm is latched, solar requests no runs at all (heating,
cooling, overheat).

---

### `setSolarMode()` / `getSolarMode()`

```cpp
void      setSolarMode(SolarMode m);   // saved
SolarMode getSolarMode() const;
```

`SOLAR_OFF`: no solar logic, no overheat protection, valve closed. `SOLAR_PUMP`: no valve — solar only
switches the filtration pump. `SOLAR_VALVE`: 3-way valve on P3; stored as `SOLAR_PUMP` if no valve relay is wired.

---

### `setSolarMaxPool()` / `getSolarMaxPool()`

```cpp
void    setSolarMaxPool(uint8_t celsius);   // saved; 0 = no limit
uint8_t getSolarMaxPool() const;
```

Whole °C. 0 = no limit — **no cooling and no heating cap**. Solar heating ends once the pool reaches it (also a
run already going, since 1.3.3) and restarts below Max pool − `APAPUMP_SOLAR_MAX_POOL_HYST_C`. Also caps both
heater modes (heater off while the pool is above it).

---

### `setSolarDrive()`

```cpp
void setSolarDrive(SolarDrive d);   // not saved — call in setup()
```

`SOLAR_WITH_SCHEDULE` (default) or `SOLAR_WITH_CATCHUP` (1.2.0 behaviour: solar is the only driver, the
schedule gives the daily target, catch-up fills the gap — see `setCatchupWindow()`).

---

### Solar status

```cpp
bool isSolarRunning() const;       // solar heating or cooling is commanding the pump
bool isSolarValveOpen() const;     // valve relay commanded open
bool isSolarSafetyActive() const;  // collector above safetyTemp — reported in every mode
bool isSolarSensorOk() const;      // false while solar is on and collector or pool is invalid
```

`isSolarSafetyActive()` is true even when the pump is held off (manual mode, pump alarm), so a sketch can warn.

---

### `setSolarDayNight()`

```cpp
void setSolarDayNight(uint32_t (*epochCb)(),
                      uint8_t  dayStartHour   = 7,
                      uint8_t  nightStartHour = 20);
```

Allow solar **heating** only between the two hours; cooling stays temperature-based, day or night.
Callback: a Unix epoch in **local time**, e.g. `[]() { return RTClib::now().unixtime(); }` (NorthernWidget DS3231).

**Why it exists:** a safety net for a collector sensor in the wrong place. On a pipe near the pump room
instead of on the absorber, it can read "warmer than the pool" in the evening or at night, and solar would
push water through a cold absorber — cooling the pool. Use wide hours (e.g. `6, 21`): a dark roof often
still gains heat after 20:00.

---

### Valid temperatures

A reading is used only if it is within `APAPUMP_TEMP_MIN_C` … `APAPUMP_TEMP_MAX_C` (−20 … 100 °C) and not
exactly 85.0 (DS18B20 power-on value) or −1.0 (the "not ready" value of APAPUMP ≤ 1.2.0). Return
`-127.0f` while a sensor is not ready. Used by solar (collector and pool), `HEATER_FULL` and freeze protection.

---

## Current monitoring

### `setCurrentCallback()`

```cpp
void setCurrentCallback(float (*cb)());
```

Register a current reading callback (e.g. `[]() { return adc.getCurrent(); }`). Returns amperes; return `-1.0f` when sensor not ready. Enables `PUMP_ALARM_OVERCURRENT` detection.

| Behaviour | Detail |
|-----------|--------|
| Baseline | Learns the pump's own normal current (EMA, weight 5 %), no rating to configure |
| Sampling | Every 10 s (`APAPUMP_CURRENT_SAMPLE_MS`), starting 30 s after pump-on (`APAPUMP_CURRENT_SETTLE_SEC`) |
| Arming | After 5 samples (~80 s of running); re-learned after every boot (RAM only) |
| Alarm | Sample > baseline × 1.5, checked on each 10 s sample |
| Floor | Readings below `APAPUMP_CURRENT_MIN_A` (0.5 A) are never learned and never alarm |
| During an alarm | Baseline frozen — an overload never raises its own threshold |

**External contactor:** when the board relay only switches a contactor in the electrical box, the sensor sees just the coil current (< 0.5 A), so overcurrent protection stays silent — no false alarms, nothing to configure. The pump is then protected by pressure monitoring. See README, "Overcurrent protection".

---

### `resetCurrentBaseline()`

```cpp
void resetCurrentBaseline();
```

Clears the learned current baseline. Call after pump service or motor replacement so the new current draw is re-learned from scratch (~80 s of running; no overcurrent alarm meanwhile).

---

## Pressure monitoring

### `enablePressure()`

```cpp
void enablePressure(float (*pressureCb)(), float maxPressure);
```

Register a pressure callback. `pressureCb` must return `-1.0f` when APASENSE is not yet calibrated — APAPUMP skips all pressure logic until a valid reading arrives.

`maxPressure` is the absolute hard limit (bar). When running pressure exceeds this value, `PUMP_ALARM_HIGH_PRESSURE` fires immediately, even before the EMA baseline builds. Use the filter's rated maximum working pressure (household sand filters: often 2.5 bar — check the label), and keep it above the filter-status backwash level. Pass `0.0f` to disable the absolute check.

Readings below `APAPUMP_PRESSURE_ABS_MIN` (0.1 bar) are never learned into the baseline, and nothing is learned while an alarm is active.

Required init order: `adc.begin()` BEFORE `pump.begin()`.

---

### `setPressureEnabled()` / `isPressureEnabled()`

```cpp
void setPressureEnabled(bool on);
bool isPressureEnabled() const;
```

Switch pressure monitoring off or on at runtime, after `enablePressure()` — for installations without a pressure sensor, typically from an installer menu. **Saved** since 1.3.0: a saved OFF also wins over the next boot's `enablePressure()`.

| | OFF | ON |
|---|---|---|
| Dry-run / high-pressure alarms | not checked | checked |
| Filter status | `FILTER_UNKNOWN` | normal |
| Last reading, learned baselines | cleared | re-learned (~80 s of running) |
| Stored clean-filter pressure | kept | used again |

An alarm already raised stays until `acknowledgeAlarm()`. `setPressureEnabled(true)` does nothing if `enablePressure()` was never called. First-boot default: ON.

---

### `getPressure()`

```cpp
float getPressure() const;
```

Returns the last valid pressure reading (bar). Returns `0.0` before the first calibrated reading.

---

### `isPressureCalibrated()`

```cpp
bool isPressureCalibrated() const;
```

Returns `true` once `pressureCb` has returned a value ≥ 0.0 (APASENSE calibrated and zeroed).

---

### `getPressureBaseline()`

```cpp
float getPressureBaseline() const;
```

Returns the learned normal-running pressure EMA (bar, solar valve closed). Returns `0.0` until 5 samples have been collected (~80 s of running after each boot). Use for HMI display.

---

### `setPressurePeakAlarm()`

```cpp
void setPressurePeakAlarm(uint8_t peakPct = 20);
```

Enable a **sudden-rise** overpressure alarm. Fires `PUMP_ALARM_HIGH_PRESSURE` when running pressure exceeds the learned EMA baseline × (1 + peakPct/100). Example: `setPressurePeakAlarm(20)` fires when pressure jumps 20 % above normal, e.g. a valve closed while running. `0` = disabled (default). Requires `enablePressure()`.

The baseline follows slow changes within minutes, so this alarm does **not** detect a clogging filter — use `getFilterStatus()`. Turning a multiport valve while the pump runs can trip it.

---

## Filter status

Requires `enablePressure()`. Status only — no alarm. See README, "Filter status — when to backwash".

### `getFilterStatus()`

```cpp
FilterStatus getFilterStatus() const;
```

Compares the running pressure baseline (solar valve closed) with the clean-filter pressure:

| Returns | When |
|---------|------|
| `FILTER_CLEAN` | rise < warning delta (default 0.4 bar) |
| `FILTER_FILLING` | rise ≥ warning delta — backwash soon |
| `FILTER_BACKWASH_NEEDED` | rise ≥ backwash delta (default 0.8 bar) |
| `FILTER_UNKNOWN` | no clean pressure learned yet, or no baseline yet (~80 s of running after each boot) |

Keeps its value while the pump is stopped.

### `learnCleanPressure()`

```cpp
void learnCleanPressure();
```

Call after every backwash or filter clean. Clears the stored clean pressure and restarts the valve-closed baseline; the next ~80 s of running learns the new clean pressure and saves it to EEPROM. With the pump off, it waits for the next run. Never copies a single reading. No-op without `enablePressure()`.

On a first install the clean pressure is learned the same way **automatically** — no call needed.

### `setCleanPressure()` / `getCleanPressure()`

```cpp
void  setCleanPressure(float bar);
float getCleanPressure() const;
```

Set the clean pressure by hand (bar, saved to EEPROM). `0` forgets it — the current baseline is then taken as the clean pressure on the next pressure sample. `getCleanPressure()` returns `0.0` while not learned.

### `setFilterThresholds()`

```cpp
void setFilterThresholds(float warningDelta, float backwashDelta);
```

Rise above the clean pressure (bar) for `FILTER_FILLING` / `FILTER_BACKWASH_NEEDED`. Defaults `APAPUMP_FILTER_WARN_DELTA` 0.4 and `APAPUMP_FILTER_BACKWASH_DELTA` 0.8 (household pools). Not saved to EEPROM — call in `setup()`, or override the defaults with `build_flags`.

---

## Daily tracking

### `setDailyTarget()`

```cpp
void setDailyTarget(uint16_t minutes);
```

Set daily pump runtime target in minutes. Overrides `dailyTargetCb` from `begin()` if non-zero. `0` = use callback. Persisted to EEPROM.

---

### `getDailyRuntimeMinutes()`

```cpp
uint16_t getDailyRuntimeMinutes() const;
```

Returns today's accumulated pump runtime in minutes (resets at midnight).

---

### `isDailyTargetMet()` / `resetDailyCounter()`

```cpp
bool isDailyTargetMet() const;
void resetDailyCounter();
```

`isDailyTargetMet()` returns `true` once today's runtime ≥ target. `resetDailyCounter()` clears today's runtime and the met flag (no EEPROM write — midnight rollover handles persistence).

---

### `setMidnightCallback()`

```cpp
void setMidnightCallback(uint32_t (*epochCb)());
```

Register a Unix epoch callback for real midnight detection. Without it, a millis() 24-hour rollover is used. With it, the library polls once per minute and detects any calendar day change. Required for catch-up window gating and midnight manual reset.

---

## Freeze protection

### `enableFreezeProtection()`

```cpp
void enableFreezeProtection(float (*tempCb)(),
                             float thresholdC = APAPUMP_FREEZE_THRESHOLD_C);
```

Enable freeze protection at Priority 4. When `tempCb()` returns a value below `thresholdC` (default 4.5 °C), the pump **cycles** to prevent pipe freeze: it runs for `APAPUMP_FREEZE_ON_SEC` (5 min), then rests for `APAPUMP_FREEZE_OFF_SEC` (10 min), repeating until a valid reading reaches `thresholdC + APAPUMP_FREEZE_HYST_C` (1.0 °C). Brief periodic circulation is sufficient to prevent freezing and saves significant energy over continuous operation. `tempCb` returns °C; readings below 0 °C are valid (since 1.3.0 — before, protection was off exactly below 0 °C). Return `-127.0f` (or `-1.0f`) while the sensor is not ready: an invalid reading never starts **or ends** a protection.

**Dry-run interlock:** if `enablePressure()` is active, pressure is calibrated, and the EMA baseline confirms no water in the pipes, freeze protection is suppressed — forcing the pump on dry would damage the motor.

---

### `isFreezeActive()`

```cpp
bool isFreezeActive() const;
```

Returns `true` when freeze protection is active (temperature below threshold). The pump cycles on/off — use `isRunning()` to check the actual relay state.

---

## Catch-up window

### `setCatchupWindow()`

```cpp
void setCatchupWindow(uint8_t startHour, uint8_t endHour);
```

Restrict daily catch-up runtime to a specific hour window. Example: `setCatchupWindow(8, 20)` allows catch-up only between 08:00 and 20:00. Window wraps midnight if `startHour > endHour`. Silently ignored without `setMidnightCallback()`. Catch-up is used only with `setSolarDrive(SOLAR_WITH_CATCHUP)`; it runs as soon as today's runtime is below the target (from midnight or the window start).

---

## Flow switch

### `setFlowCallback()`

```cpp
void setFlowCallback(bool (*cb)());
```

Register a flow switch callback. `cb` returns `true` when flow is confirmed. `PUMP_ALARM_NO_FLOW` fires if `cb` returns `false` after 30 seconds of pump running. Pass `nullptr` to disable. Hardware stub — ready for a future flow sensor.

---

## Enums

### `RelayDriver`

```cpp
enum RelayDriver : uint8_t { RELAY_DIRECT, RELAY_PCF };
```

### `ManualMode`

```cpp
enum ManualMode : uint8_t { AUTO, FORCE_ON, FORCE_OFF };
```

### `PumpState`

```cpp
enum PumpState : uint8_t { IDLE, STARTING, RUNNING, STOPPING };
```

### `ValveMode`

```cpp
enum ValveMode : uint8_t { VALVE_SUSTAINED, VALVE_PULSE };
```

`VALVE_SUSTAINED`: valve relay stays on while solar runs. `VALVE_PULSE`: brief pulse for motor-driven valves (hardware stub — pulse not yet implemented).

### `SolarMode` / `SolarDrive` / `AuxMode`

```cpp
enum SolarMode  : uint8_t { SOLAR_OFF, SOLAR_PUMP, SOLAR_VALVE };                    // saved
enum SolarDrive : uint8_t { SOLAR_WITH_SCHEDULE, SOLAR_WITH_CATCHUP };               // not saved
enum AuxMode    : uint8_t { AUX_DISABLED, AUX_FOLLOWER, HEATER_SIMPLE, HEATER_FULL }; // saved
```

See `setSolarMode()`, `setSolarDrive()`, `setAuxMode()`.

### `FilterStatus`

```cpp
enum FilterStatus : uint8_t {
    FILTER_UNKNOWN,
    FILTER_CLEAN,
    FILTER_FILLING,
    FILTER_BACKWASH_NEEDED
};
```

See `getFilterStatus()`.

### `PumpAlarm`

```cpp
enum PumpAlarm : uint8_t {
    PUMP_ALARM_NONE,
    PUMP_ALARM_OVERCURRENT,
    PUMP_ALARM_LOW_PRESSURE,
    PUMP_ALARM_HIGH_PRESSURE,
    PUMP_ALARM_NO_FLOW
};
```

All non-NONE alarms are latching — call `acknowledgeAlarm()` to clear.

---

## Saved settings

The library saves only owner/installer settings: solar mode, Max pool, AUX mode, heater target, pressure
ON/OFF (plus daily target, minimum run time, clean pressure as before). Constants — deltas, safety
temperature, day-gate hours, drive model, timings — are not saved; pass them in `setup()` every boot.
Saved setters write the EEPROM only after `begin()`. A saved mode never activates hardware by itself:
solar needs `enableSolar()`, the AUX relay `enableAux()` or `setAuxMode()`, in every boot.

```cpp
bool hasSavedSettings() const;   // true if valid saved settings were loaded by begin()
```

Use it for your own first-boot defaults: `if (!pump.hasSavedSettings()) pump.setSolarMode(SOLAR_OFF);`

---

## Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `APAPUMP_VERSION` | `"1.3.3"` | Library version string |
| `APAPUMP_MIN_OFF_SEC` | 60 | Minimum pause after pump stops (non-manual) |
| `APAPUMP_MIN_RUN_SEC` | 300 | Default minimum run time before stopping |
| `APAPUMP_VALVE_PULSE_MS` | 500 | Default pulse width for `VALVE_PULSE` mode |
| `APAPUMP_CURRENT_SAMPLE_MS` | 10 000 | EMA sample interval (ms) — current + pressure |
| `APAPUMP_CURRENT_SETTLE_SEC` | 30 | Seconds after pump-on before EMA sampling starts |
| `APAPUMP_FREEZE_THRESHOLD_C` | 4.5 | Default freeze protection temperature (°C) |
| `APAPUMP_FREEZE_ON_SEC` | 300 | Freeze cycle: pump run duration (5 min) |
| `APAPUMP_FREEZE_OFF_SEC` | 600 | Freeze cycle: rest between runs (10 min) |
| `APAPUMP_PRESSURE_DRYRUN_PCT` | 40 | Pressure must be ≥ this % of EMA baseline to confirm flow |
| `APAPUMP_PRESSURE_ABS_MIN` | 0.1 | Minimum pressure (bar) of a running pump: dry-run limit before the EMA builds; lower readings are never learned |
| `APAPUMP_HEATER_TARGET_MIN` / `_MAX` | 15 / 40 | `setHeaterTarget()` range (°C) |

---

## Configurable limits

Set in `build_flags` (PlatformIO) so the library and your sketch see the same value — a `#define` in the sketch does not reach the library's own `.cpp`:

| Define | Default | Meaning |
|--------|---------|---------|
| `APAPUMP_EEPROM_ADDR` | 520 | EEPROM base address (uses 15 bytes) |
| `APAPUMP_CURRENT_MIN_A` | 0.5 | Current (A) below which readings are ignored — no motor on the sensor |
| `APAPUMP_FILTER_WARN_DELTA` | 0.4 | Filter status: `FILTER_FILLING` at clean + this (bar) |
| `APAPUMP_FILTER_BACKWASH_DELTA` | 0.8 | Filter status: `FILTER_BACKWASH_NEEDED` at clean + this (bar) |
| `APAPUMP_TEMP_MIN_C` / `APAPUMP_TEMP_MAX_C` | −20 / 100 | Valid temperature range (°C) |
| `APAPUMP_FREEZE_HYST_C` | 1.0 | Freeze protection ends at threshold + this (°C) |
| `APAPUMP_VALVE_SETTLE_SEC` | 30 | No pressure checks / learning after a valve change (s) |
| `APAPUMP_HEATER_HYST_C` | 1.0 | `HEATER_FULL`: on below target − this (°C) |
| `APAPUMP_SOLAR_MAX_POOL_HYST_C` | 0.5 | Solar heating stops at Max pool, may start again below Max pool − this (°C) (since 1.3.3) |
| `APAPUMP_HEATER_MIN_ON_SEC` / `_MIN_OFF_SEC` | 600 / 600 | Heater short-cycling guard (s) |
| `APAPUMP_HEATER_TARGET_DEFAULT` | 28 | First-boot heater target (°C) |

```ini
build_flags = -DAPAPUMP_CURRENT_MIN_A=0.3f -DAPAPUMP_FILTER_BACKWASH_DELTA=1.0f
```

---

## EEPROM layout

Base address: `APAPUMP_EEPROM_ADDR` (default 520). Total: **15 bytes** (520–534), layout version 2 (1.3.0). Upgrading from 1.2.0 resets the block once.

### Field map

| Offset | Address | Bytes | Content | Written by |
|--------|---------|-------|---------|-----------|
| 0 | 520 | 2 | Magic `0xA55A` | Validity marker — always written |
| 2 | 522 | 1 | Config version `2` | Mismatch → factory reset on next boot |
| 3 | 523 | 2 | Daily target (minutes) | `setDailyTarget()` |
| 5 | 525 | 2 | Yesterday's runtime (minutes) | Midnight rollover |
| 7 | 527 | 2 | Minimum run time (seconds) | `setMinRunTime()` |
| 9 | 529 | 2 | Clean pressure × 100 (bar; 0 = not learned) | Automatic first learn, `learnCleanPressure()`, `setCleanPressure()` |
| 11 | 531 | 1 | Settings: bit 0 pressure ON, bits 1–2 `SolarMode`, bits 3–4 `AuxMode` | `setPressureEnabled()`, `setSolarMode()`, `setAuxMode()` |
| 12 | 532 | 1 | Max pool (°C, 0 = no limit) | `setSolarMaxPool()` |
| 13 | 533 | 1 | Heater target (°C) | `setHeaterTarget()` |
| 14 | 534 | 1 | Checksum (byte sum of offsets 0–13) | Corruption guard |

### Write protection and EEPROM lifespan

`_saveEEPROM()` always writes the full 15-byte struct, but a physical write only occurs when a byte has actually changed:

| Platform | Mechanism | Granularity |
|----------|-----------|-------------|
| AVR (Uno/Mega) | `EEPROM.put()` → `EEPROM.update()` per byte | Byte-level |
| ESP32 / ESP8266 | `EEPROM.put()` → RAM buffer; `EEPROM.commit()` compares buffer to flash | Buffer-level |
| STM32 | EEPROM flash emulation with compare-before-write | Page-level |

Result: calling `setMinRunTime(300)` from `setup()` every boot with 300 already stored causes **zero physical writes** on all platforms.

**Worst-case write budget:**

| Trigger | Max rate | Cycles at 100 k limit |
|---------|---------|----------------------|
| Midnight rollover | 1 per day | ~273 years |
| Clean pressure (first learn, `learnCleanPressure()`) | After install / each backwash | Negligible |
| `setMinRunTime()` with same value | 0 writes | ∞ |
| First boot / corruption recovery | Once total | 1 cycle |

**Corruption recovery:** checksum verified on every `begin()`. Power-loss mid-write is detected on next boot → defaults loaded → clean struct written.

### Overriding the base address

```ini
; platformio.ini — a #define in the sketch would not reach the library's own .cpp
build_flags = -DAPAPUMP_EEPROM_ADDR=535
```

### APA EEPROM address map

| Range | Bytes | Status | Owner |
|-------|-------|--------|-------|
| 0–127 | 128 | **free** | — |
| 128–177 | 50 | used | APAPHX2_ADS1115 |
| 178–188 | 11 | **free** | — |
| 189–191 | 3 | used | APADOSE global (pool volume, dead-band) |
| 192–291 | 100 | used | APADOSE per-pump (25 bytes × up to 4 instances) |
| 292–499 | 208 | **free** | — |
| 500–501 | 2 | used | APALCDGUI brightness |
| 502–508 | 7 | used | APALCDGUI timers (default 3 slots; extends to 514 with `MAX_TIMERS=6`) |
| 509–519 | 11 | **free** | — intentional gap before APAPUMP ¹ |
| **520–534** | **15** | **used** | **APAPUMP** (12 bytes before 1.3.0) |
| 535–581 | 47 | **free** | — APAPUMP expansion reserve |
| 582–587 | 6 | used | APASENSE (pressure zero) |
| 588– | — | **free** | — |

> ¹ With `APA_LCD_MAX_TIMERS=6` the gap shrinks to 5 bytes (515–519). Do not place anything in 515–519 so the gap remains safe regardless of timer configuration.

---

## Platform notes

| Topic | AVR (Uno/Mega) | ESP32 / ESP8266 | STM32 |
|-------|---------------|-----------------|-------|
| `F()` macro | Stores strings in flash | No-op (strings stay in RAM) | No-op |
| `EEPROM.commit()` | Not needed | Called automatically after every `_saveEEPROM()` | Not needed |
| EEPROM write protection | Byte-level compare (`EEPROM.update`) | Buffer-vs-flash compare at commit | Flash emulation with compare |
| `Wire.begin()` | Call once before `pump.begin()` | Same | Same |
| Interrupt-safe | Single-core, no concern | ISR on same core as `loop()` — callbacks are not ISR-safe | Single-core |

---

*APAPUMP — APA Devices · [kecup@vazac.eu](mailto:kecup@vazac.eu)*
