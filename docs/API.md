# APAPUMP API Reference — v1.0.3

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
| 5 | `pump.enableUVC()`, `enableAux()`, `enableSolar()` | Optional |
| 6 | `pump.enablePressure()`, `setCurrentCallback()` | Optional |
| 7 | `pump.enableFreezeProtection()`, `setPressurePeakAlarm()` | Optional |
| 8 | `pump.setMidnightCallback()` | Optional — enables RTC midnight reset |
| 9 | `pump.setDailyTarget()`, `setCatchupWindow()` | Optional |

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
| `FORCE_OFF` | Pump forced off. All timing guards suspended. Solar safety (P0) still applies. |
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
| `STOPPING` | AUX lead + UVC post-delay in progress |

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

Returns the learned normal-running pressure baseline in bar. Returns `0.0` until at least 5 pump run samples have been collected. Use for HMI display to verify sensor calibration.

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

| Alarm | Condition |
|-------|-----------|
| `PUMP_ALARM_OVERCURRENT` | Current > learned baseline × 1.5 |
| `PUMP_ALARM_LOW_PRESSURE` | Pressure too low — dry-run suspected |
| `PUMP_ALARM_HIGH_PRESSURE` | Pressure too high — filter dirty or blockage |
| `PUMP_ALARM_NO_FLOW` | Flow switch reports no flow after settle |

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

Enable AUX device relay (PCF P2 / DIRECT pin c — heat pump, ozone, etc.). AUX turns ON `startDelayS` seconds after pump starts; turns OFF `stopLeadS` seconds before pump stops. No-op if aux pin is `0xFF`.

---

## Solar heating

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

| Parameter | Default | Description |
|-----------|---------|-------------|
| `solarTempCb` | — | Absorber temperature callback (°C). Required. |
| `poolTempCb` | `nullptr` | Pool temperature callback (°C). If `nullptr`, pool temperature is treated as 0°C — `startDelta` becomes an absolute absorber threshold. |
| `useValve` | `false` | Open the solar valve relay (P3) when solar drives the pump. Valve closes in manual mode. |
| `startDelta` | 8.0 | Start when absorber > pool + startDelta (°C) |
| `stopDelta` | 3.0 | Stop when absorber < pool + stopDelta (°C). Must be < startDelta. |
| `maxPoolTemp` | 0.0 | Pool cooling threshold (°C). 0 = disabled. When pool > maxPoolTemp and absorber < pool − coolDelta, pump runs to cool the pool. |
| `coolDelta` | 2.0 | Cooling dead-band (°C) |
| `safetyTemp` | 50.0 | Absorber safety temperature (°C). Above this, pump is forced ON + valve OPEN regardless of manual mode (Priority 0). |
| `valveMode` | `VALVE_SUSTAINED` | `VALVE_SUSTAINED`: valve stays open while solar runs. `VALVE_PULSE`: brief pulse to actuate motor-driven valve (hardware stub). |
| `pulseDurationMs` | 500 | Pulse duration when `valveMode = VALVE_PULSE` |

When solar is enabled, solar logic is the sole on/off driver. The `scheduleCb` from `begin()` is used only to determine the daily target, not for on/off.

---

### `setSolarDayNight()`

```cpp
void setSolarDayNight(uint32_t (*epochCb)(),
                      uint8_t  dayStartHour   = 7,
                      uint8_t  nightStartHour = 20);
```

Restrict solar heating to daytime hours. Pool cooling is still allowed at night. Requires a Unix epoch callback (e.g. `[]() { return rtc.getEpoch(); }`).

---

## Current monitoring

### `setCurrentCallback()`

```cpp
void setCurrentCallback(float (*cb)());
```

Register a current reading callback (e.g. `[]() { return adc.getCurrent(); }`). Returns amperes; return `-1.0f` when sensor not ready. Enables `PUMP_ALARM_OVERCURRENT` detection.

---

### `resetCurrentBaseline()`

```cpp
void resetCurrentBaseline();
```

Clears the learned EMA baseline. Call after pump service or motor replacement so the new current draw is re-learned from scratch.

---

## Pressure monitoring

### `enablePressure()`

```cpp
void enablePressure(float (*pressureCb)(), float maxPressure);
```

Register a pressure callback. `pressureCb` must return `-1.0f` when APASENSE is not yet calibrated — APAPUMP skips all pressure logic until a valid reading arrives.

`maxPressure` is the absolute hard limit (bar). When running pressure exceeds this value, `PUMP_ALARM_HIGH_PRESSURE` fires immediately, even before the EMA baseline builds. Pass `0.0f` to disable the absolute check and rely only on the EMA-relative alarm.

Required init order: `adc.begin()` BEFORE `pump.begin()`.

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

Returns the learned normal-running pressure EMA (bar). Returns `0.0` until 5 pump run samples have been collected. Use for HMI display.

---

### `setPressurePeakAlarm()`

```cpp
void setPressurePeakAlarm(uint8_t peakPct = 20);
```

Enable EMA-relative overpressure alarm. Fires `PUMP_ALARM_HIGH_PRESSURE` when running pressure exceeds the learned EMA baseline × (1 + peakPct/100). Example: `setPressurePeakAlarm(20)` fires when pressure is 20% above normal. `0` = disabled. Requires `enablePressure()`.

---

### Filter status (deferred)

```cpp
void         setFilterThresholds(float warningDelta, float backwashDelta);
void         learnCleanPressure();
void         setCleanPressure(float bar);
float        getCleanPressure() const;
FilterStatus getFilterStatus() const;
```

Filter status reporting uses the manually-learned clean-filter pressure as a baseline. `getFilterStatus()` currently always returns `FILTER_UNKNOWN` — implementation deferred until pressure baseline is verified in field. `learnCleanPressure()` captures the current pressure as the clean-filter baseline and saves it to EEPROM.

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

Enable freeze protection at Priority 4. When `tempCb()` returns a value below `thresholdC` (default 4.5 °C), the pump **cycles** to prevent pipe freeze: it runs for `APAPUMP_FREEZE_ON_SEC` (5 min), then rests for `APAPUMP_FREEZE_OFF_SEC` (10 min), repeating until temperature rises above threshold. Brief periodic circulation is sufficient to prevent freezing and saves significant energy over continuous operation. `tempCb` returns `°C`; return `-1.0f` when sensor is not ready (protection stays inactive).

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

Restrict daily catch-up runtime to a specific hour window. Example: `setCatchupWindow(8, 20)` allows catch-up only between 08:00 and 20:00. Window wraps midnight if `startHour > endHour`. Silently ignored without `setMidnightCallback()`. Requires solar mode (catch-up logic is in the solar branch of the priority engine).

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

### `FilterStatus`

```cpp
enum FilterStatus : uint8_t {
    FILTER_UNKNOWN,
    FILTER_CLEAN,
    FILTER_FILLING,
    FILTER_BACKWASH_NEEDED
};
```

`getFilterStatus()` currently always returns `FILTER_UNKNOWN`.

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

## Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `APAPUMP_VERSION` | `"1.0.3"` | Library version string |
| `APAPUMP_MIN_OFF_SEC` | 60 | Minimum pause after pump stops (non-manual) |
| `APAPUMP_MIN_RUN_SEC` | 300 | Default minimum run time before stopping |
| `APAPUMP_VALVE_PULSE_MS` | 500 | Default pulse width for `VALVE_PULSE` mode |
| `APAPUMP_CURRENT_SAMPLE_MS` | 10 000 | EMA sample interval (ms) — current + pressure |
| `APAPUMP_CURRENT_SETTLE_SEC` | 30 | Seconds after pump-on before EMA sampling starts |
| `APAPUMP_FREEZE_THRESHOLD_C` | 4.5 | Default freeze protection temperature (°C) |
| `APAPUMP_FREEZE_ON_SEC` | 300 | Freeze cycle: pump run duration (5 min) |
| `APAPUMP_FREEZE_OFF_SEC` | 600 | Freeze cycle: rest between runs (10 min) |
| `APAPUMP_PRESSURE_DRYRUN_PCT` | 40 | Pressure must be ≥ this % of EMA baseline to confirm flow |
| `APAPUMP_PRESSURE_ABS_MIN` | 0.1 | Absolute minimum pressure (bar) used before EMA builds |

---

## Configurable limits

Define before `#include <APAPUMP.h>`:

```cpp
#define APAPUMP_EEPROM_ADDR  520   // EEPROM base address (default 520, uses 12 bytes)
```

---

## EEPROM layout

Base address: `APAPUMP_EEPROM_ADDR` (default 520). Total: **12 bytes** (520–531).

### Field map

| Offset | Address | Bytes | Content | Written by |
|--------|---------|-------|---------|-----------|
| 0 | 520 | 2 | Magic `0xA55A` | Validity marker — always written |
| 2 | 522 | 1 | Config version `1` | Mismatch → factory reset on next boot |
| 3 | 523 | 2 | Daily target (minutes) | `setDailyTarget()` |
| 5 | 525 | 2 | Yesterday's runtime (minutes) | Midnight rollover |
| 7 | 527 | 2 | Minimum run time (seconds) | `setMinRunTime()` |
| 9 | 529 | 2 | Clean pressure × 100 (bar) | `learnCleanPressure()` / `setCleanPressure()` |
| 11 | 531 | 1 | Checksum (byte sum of offsets 0–10) | Corruption guard |

### Write protection and EEPROM lifespan

`_saveEEPROM()` always writes the full 12-byte struct, but a physical write only occurs when a byte has actually changed:

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
| `learnCleanPressure()` | Manual, rare | Negligible |
| `setMinRunTime()` with same value | 0 writes | ∞ |
| First boot / corruption recovery | Once total | 1 cycle |

**Corruption recovery:** checksum verified on every `begin()`. Power-loss mid-write is detected on next boot → defaults loaded → clean struct written.

### Overriding the base address

```cpp
// Place before #include — use if another library conflicts with address 520:
#define APAPUMP_EEPROM_ADDR  532
#include <APAPUMP.h>
```

### APA EEPROM address map

| Range | Bytes | Status | Owner |
|-------|-------|--------|-------|
| 0–127 | 128 | **free** | — |
| 128–177 | 50 | used | APAPHX2_ADS1115 |
| 178–188 | 11 | **free** | — |
| 189–191 | 3 | used | APADOSE global (pool volume, dead-band) |
| 192–279 | 88 | used | APADOSE per-pump (22 bytes × up to 4 instances) |
| 280–499 | 220 | **free** | — |
| 500–501 | 2 | used | APALCDGUI brightness |
| 502–508 | 7 | used | APALCDGUI timers (default 3 slots; extends to 514 with `MAX_TIMERS=6`) |
| 509–519 | 11 | **free** | — intentional gap before APAPUMP ¹ |
| **520–531** | **12** | **used** | **APAPUMP** |
| 532– | — | **free** | — |

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
