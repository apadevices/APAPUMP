# APAPUMP

<p align="center">
<img src="extras/apapump-logo.png" width="600" alt="APAPUMP">
</p>

**Autonomous filtration pump controller for APA Devices pool automation**
· ![v1.1.1](https://img.shields.io/badge/version-1.1.1-blue)
· ![Platforms](https://img.shields.io/badge/platforms-AVR%20ESP8266%20ESP32%20STM32-brightgreen)

---

## Key Features

### Pump control
- PCF8574AT I2C relay expander (default) or direct Arduino GPIO — one constructor covers both
- Non-blocking state machine: IDLE → STARTING → RUNNING → STOPPING
- Safe boot: all relays start OFF regardless of previous relay or pin state
- Manual override: `FORCE_ON`, `FORCE_OFF`, `AUTO` — with optional timeout or midnight auto-return
- Extension relay outputs on PCF P4–P7 for lights or extra equipment

### Scheduling and daily tracking
- Schedule callback drives pump on/off without any external library
- Daily runtime target with catch-up logic and optional time window
- Midnight rollover via RTC epoch callback or 24-hour millis() fallback

### Follower devices
- **UVC sanitizer** — ON before pump, OFF after (configurable pre/post delay)
- **AUX device** (heat pump, ozone, etc.) — ON after pump, OFF before pump stops
- **Solar valve** — opens when solar drives the pump; instantly closes in manual mode

### Solar heating
- Absorber–pool temperature dead-band hysteresis (configurable start/stop deltas)
- Pool cooling: runs in reverse when pool temperature exceeds maximum
- Day/night boundary gate via RTC epoch callback
- **Solar safety (Priority 0):** absorber too hot → pump ON + valve OPEN, overrides even `FORCE_OFF`

### Safety alarms
- **Overcurrent** — learns the pump's own normal current, alarms at × 1.5; no pump rating to enter; silent with an external contactor ([details](#overcurrent-protection))
- **Dry-run** — pressure below EMA baseline × 40% (or absolute minimum before EMA builds)
- **Overpressure** — absolute hard limit at `maxPressure` (the filter's rated pressure); optional sudden-rise alarm
- **Learned baselines never learn a fault** — no learning while an alarm is active, nor from a dry pipe or a motor-less current sensor
- **Freeze protection** — pool temp below 4.5 °C → cyclic run (5 min ON / 10 min rest); suppressed if dry-run detected
- **No-flow stub** — flow switch confirmed after settle; hardware-ready for future sensor
- All alarms are latching — explicit `acknowledgeAlarm()` required

### Filter maintenance
- **Filter status** — clean / filling / backwash needed, from the pressure rise above the clean-filter pressure ([details](#filter-status--when-to-backwash))
- Clean pressure learned automatically on the first run; one call after each backwash re-learns it

### Engineering
- **564 B SRAM** on Arduino Uno with all features registered — fits comfortably in 2 KB
- 23 boolean flags packed into 3 bytes; all string literals in flash (`F()` macro)
- EEPROM: 12 bytes, magic + version + checksum validation, persists target and clean pressure
- Zero `delay()` calls — every path returns within one `loop()` iteration
- Designed to integrate with APADOSE and APALCDGUI via callback bridges — no coupling

---

## Installation

**Arduino IDE:** Sketch → Include Library → Add .ZIP Library → select the APAPUMP folder.

**PlatformIO:**
```ini
lib_deps =
    https://github.com/apadevices/APAPUMP
```

---

## What It Does

APAPUMP controls a pool filtration pump and up to three follower devices (UVC lamp, AUX device, solar valve) through a PCF8574AT I2C relay expander or direct Arduino output pins. Each call to `update()` in your `loop()` advances the state machine, checks every safety guard in priority order, and fires alarm callbacks when something is wrong — all without blocking.

Out of the box, the zero-arg constructor targets the APA Devices HMI board v1.0: PCF8574AT at 0x3C with pump on P0, UVC on P1, AUX on P2, and solar valve on P3. Every optional feature is **disabled by default** and activated with one method call.

---

## How It Works

### State machine

```
                                ┌──────────────────────────────────┐
                                │           pump.begin()           │
                                ▼                                  │
                             [ IDLE ]                              │
                                │                                  │
                   _shouldPumpRun() = true                         │
                   min OFF time OK (60 s)                          │
                                │                                  │
                          [ STARTING ]                             │
                                │                                  │
                   UVC pre-delay (default 5 s)                     │
                                │                                  │
                          [ RUNNING ] ◄── safety alarms checked:   │
                                │         current · pressure ·     │
                                │         flow · freeze            │
                   _shouldPumpRun() = false                        │
                   min run time OK (300 s)                         │
                                │                                  │
                         [ STOPPING ]                              │
                                │                                  │
          AUX lead ──► pump OFF ──► UVC post-delay ────────────────┘
```

**Guards that bypass min-run time:** `FORCE_ON` and `FORCE_OFF` — manual mode suspends timing guards.

### Follower timing

```
Time →    0         5s         15s       [pump runs…]   Ts-30s    Ts      Ts+30s
          │          │          │                │        │         │        │
UVC:      ├──ON──────┼──────────┼────────────────┼────────┼─────────┼──OFF───┤
Pump:               ON──────────────────────────────────OFF
AUX:                           ON───────────────OFF
          ◄─ pre ──►◄──────────────── RUNNING ──────────►◄─ lead ─►◄─ post─►
```

`Ts` = moment the stop sequence begins. `pre` = `enableUVC(preDelayS, …)`. `lead` = `enableAux(…, stopLeadS)`. `post` = `enableUVC(…, postDelayS)`.

### Priority engine

Every `update()` tick calls the priority engine. The first rule that matches wins:

| Priority | Condition | Result |
|----------|-----------|--------|
| **P0** | Solar absorber > safety temp (default 50 °C) | Pump ON + valve OPEN — overrides FORCE_OFF |
| **P1** | `FORCE_OFF` manual mode | Pump OFF |
| **P2** | `FORCE_ON` manual mode | Pump ON |
| **P3** | External request callback returns `true` | Pump ON |
| **P4** | Freeze: pool temp < threshold AND water present | Pump ON |
| **P5a** | Solar: absorber > pool + startDelta | Pump ON (hysteresis) |
| **P5b** | Catch-up: daily runtime < daily target (window OK) | Pump ON |
| **P5c** | Schedule callback returns `true` | Pump ON |
| — | None of the above | Pump OFF |

### PCF relay layout (APA HMI board v1.0 defaults)

```
PCF8574AT at I2C address 0x3C
│
├─ P0 ── Pump relay              (always required)
├─ P1 ── UVC relay               (optional — enableUVC())
├─ P2 ── AUX relay               (optional — enableAux())
└─ P3 ── Solar valve relay       (optional — enableSolar(…, useValve=true))
│
├─ P4 ── Extension relay         (optional — setExtraOutput(4, on/off))
├─ P5 ── Extension relay         (optional — setExtraOutput(5, on/off))
├─ P6 ── Extension relay         (optional — setExtraOutput(6, on/off))
└─ P7 ── Extension relay         (optional — setExtraOutput(7, on/off))
```

PCF HIGH = relay ON, LOW = relay OFF. `begin()` writes `0x00` immediately — all relays start OFF.

---

## Quick Start

### Minimal — APA HMI board defaults (zero wiring, zero config)

```cpp
#include <Wire.h>
#include <APAPUMP.h>

ApaPump pump;              // PCF at 0x3C — pump=P0, uvc=P1, aux=P2, valve=P3

void setup() {
    Wire.begin();          // required for PCF mode — call before pump.begin()
    pump.begin();          // all relays OFF, EEPROM loaded
}

void loop() {
    pump.update();         // non-blocking — call every loop()
}
```

### Alternative: direct GPIO

```cpp
// Active-high relay (most modules)
ApaPump pump(RELAY_DIRECT, 7);               // pump on pin 7

// Active-low relay (some modules)
ApaPump pump(RELAY_DIRECT, 7);
pump.setActiveLow();             // call BEFORE pump.begin()

// Multiple relays, direct GPIO
ApaPump pump(RELAY_DIRECT, 7, 8, 9, 10);    // pump=7, uvc=8, aux=9, valve=10
```

### Scheduled pump with alarm feedback

```cpp
#include <Wire.h>
#include <APAPUMP.h>

ApaPump pump;

bool scheduleActive() {
    // return true when your RTC time falls inside a timer slot
    return false;  // replace with real logic
}

void onAlarm(PumpAlarm alarm) {
    if (alarm == PUMP_ALARM_NONE) return;   // alarm was cleared
    Serial.print(F("Pump alarm: "));
    switch (alarm) {
        case PUMP_ALARM_OVERCURRENT:   Serial.println(F("overcurrent"));   break;
        case PUMP_ALARM_LOW_PRESSURE:  Serial.println(F("dry run"));       break;
        case PUMP_ALARM_HIGH_PRESSURE: Serial.println(F("high pressure")); break;
        case PUMP_ALARM_NO_FLOW:       Serial.println(F("no flow"));       break;
        default: break;
    }
    // pump continues running — call pump.acknowledgeAlarm() when investigated
}

void setup() {
    Wire.begin();
    pump.begin(scheduleActive);             // schedule drives on/off
    pump.enableUVC(5, 30);                  // UVC: 5 s pre, 30 s post
    pump.enableAux(10, 30);                 // AUX: 10 s delay, 30 s lead
    pump.setPumpAlarmCallback(onAlarm);
}

void loop() { pump.update(); }
```

---

## Manual Override

Force the pump on or off at any time. All timing guards and schedules are suspended:

```cpp
pump.setManualMode(FORCE_ON);   // force pump ON  (e.g. vacuuming, winterising)
pump.setManualMode(FORCE_OFF);  // force pump OFF (e.g. plumbing, service)
pump.setManualMode(AUTO);       // return to automatic control
```

**The solar valve closes immediately when entering FORCE_ON** — operator gets full system pressure.

**Auto-return options** — pick one or both:

```cpp
// Timeout: returns to AUTO after 60 minutes
ApaPump pump(RELAY_PCF, 0x3C, 0, 1, 2, 3, 60);   // 6th constructor parameter
pump.setManualTimeout(60);                           // or set at any time

// Midnight reset (requires setMidnightCallback())
pump.setManualAutoReset(true);
```

**Note:** Solar safety (P0) still applies even in `FORCE_OFF`. If the absorber reaches the safety temperature, the pump runs to prevent heat damage regardless of manual mode.

---

## Followers: UVC, AUX, Solar Valve

Each follower is independent and optional. Wire the relay to the corresponding PCF bit (or GPIO pin), then call the enable method:

```cpp
// UVC lamp: ON 5 s before pump, OFF 30 s after pump stops
pump.enableUVC(5, 30);

// AUX device (heat pump, ozone): ON 10 s after pump starts, OFF 30 s before pump stops
pump.enableAux(10, 30);

// Solar valve: open when solar logic drives the pump
// useValve=true enables the valve relay on P3 (or direct pin d)
pump.enableSolar(absorberTempCb, poolTempCb, /*useValve=*/true, ...);
```

**UVC pre-delay** — when a pre-delay is configured, the pump enters STARTING state while UVC warms up. If the reason to start disappears during STARTING (e.g. schedule ends), the sequence is cancelled cleanly — UVC turns off and the pump never fires.

---

## Solar Heating

Solar logic becomes the **sole on/off driver** when enabled — the schedule callback is only used for the daily runtime target.

```cpp
pump.enableSolar(
    []() { return adc.getSolarTemp(); },   // absorber temperature callback
    []() { return adc.getPoolTemp(); },    // pool temperature callback (optional)
    true,                                  // useValve — open valve when solar running
    8.0f,                                  // startDelta: start when absorber > pool + 8°C
    3.0f,                                  // stopDelta:  stop  when absorber < pool + 3°C
    32.0f,                                 // maxPoolTemp: pool cooling if pool > 32°C
    2.0f,                                  // coolDelta: cool when absorber < pool - 2°C
    50.0f                                  // safetyTemp: force pump ON if absorber > 50°C
);

// Optional: restrict solar heating to daytime hours
pump.setSolarDayNight(
    []() { return rtc.getEpoch(); },       // Unix epoch callback
    7,                                     // daytime starts at 07:00
    20                                     // daytime ends at 20:00
);
```

### Solar hysteresis

```
Absorber temp
     │
     │                     START threshold = pool + startDelta
─────┼──────────────────────────────────────────────────────
     │   ← pump ON                               pump ON →
     │
─────┼──────────────────────────────────────────────────────
     │                     STOP threshold = pool + stopDelta
     │
```

The gap between start and stop thresholds prevents rapid on/off cycling when temperature hovers near the boundary. Increase `startDelta − stopDelta` for wider hysteresis.

---

## Daily Runtime Target and Catch-Up

```cpp
// Option A: fixed target (user sets it once)
pump.setDailyTarget(360);                   // run 6 hours per day

// Option B: use the APALCDGUI timer sum as the target
pump.begin(scheduleActive, nullptr,
           []() { return gui.getTimerTotalMinutes(); });

// Query progress
uint16_t ran = pump.getDailyRuntimeMinutes();
bool     met = pump.isDailyTargetMet();
```

When solar is enabled and the daily target is not met, catch-up runs until the target is reached:

```cpp
// Optional: limit catch-up to daytime hours (requires setMidnightCallback)
pump.setCatchupWindow(8, 20);   // catch-up only between 08:00 and 20:00
```

---

## Safety Alarms

All alarms are **latching** — the pump does **not** stop automatically (your sketch decides the action). Call `acknowledgeAlarm()` after investigation to clear.

| Alarm | Triggers when | Requires |
|-------|--------------|----------|
| `PUMP_ALARM_OVERCURRENT` | current > learned baseline × 1.5 ([details](#overcurrent-protection)) | `setCurrentCallback()` |
| `PUMP_ALARM_LOW_PRESSURE` | pressure below 40% of EMA baseline (dry-run) | `enablePressure()` |
| `PUMP_ALARM_HIGH_PRESSURE` | pressure > `maxPressure` (absolute) | `enablePressure()` with `maxPressure` > 0 |
| `PUMP_ALARM_HIGH_PRESSURE` | **sudden** rise > baseline × (1 + peakPct %) | `setPressurePeakAlarm()` (off by default) |
| `PUMP_ALARM_NO_FLOW` | flow switch reports no flow after 30 s settle | `setFlowCallback()` |

Alarm checks run every `update()` once the pump has run 30 s; overcurrent is checked with each 10 s current sample. While an alarm is active, **nothing is learned** — a dry run or an overcurrent never becomes the new "normal".

> **The sudden-rise alarm does not detect a dirty filter.** Its baseline follows slow changes within minutes, so weeks of clogging never trip it — that is what [filter status](#filter-status--when-to-backwash) is for. It catches fast changes such as a valve closed while the pump runs. Turning a multiport valve (e.g. to backwash) while the pump runs can trip it, which is why it is off by default.

### Stopping the pump on an alarm (recommended)

A pump that keeps running dry or overloaded can be destroyed within minutes, so most sketches should stop it. Keep it stopped **while the alarm is active** — `setManualMode(FORCE_OFF)` alone returns to AUTO after the manual timeout and would restart the pump, and the library skips its checks while an alarm is active:

```cpp
void loop() {
    pump.update();
    if (pump.getAlarm() != PUMP_ALARM_NONE && pump.getManualMode() != FORCE_OFF)
        pump.setManualMode(FORCE_OFF);   // re-asserted until the operator acknowledges
}

// Operator has checked the pump (HMI button, Serial command, ...):
pump.acknowledgeAlarm();
pump.setManualMode(AUTO);
```

**Alarm callback pattern:**

```cpp
pump.setPumpAlarmCallback([](PumpAlarm alarm) {
    if (alarm == PUMP_ALARM_NONE) {
        // alarm was acknowledged — update LCD, clear indicator
        gui.cancelActiveAlert();
        return;
    }
    // alarm fired — notify operator
    gui.postActiveAlert(F("Pump alarm!"), F("Check equipment"),
                        ALERT_CRITICAL, []() { pump.acknowledgeAlarm(); });
});
```

### Wiring a buzzer or alarm output

The alarm callback is the right place to drive any physical indicator — buzzer, LED, relay. No extra library method needed.

**Direct GPIO (no APASENSE):**
```cpp
pump.setPumpAlarmCallback([](PumpAlarm alarm) {
    bool active = (alarm != PUMP_ALARM_NONE);
    digitalWrite(5, active ? HIGH : LOW);   // buzzer on pin 5
});
```

**With APASENSE** (v1.1.0+, recommended for APA hardware):
```cpp
pump.setPumpAlarmCallback([](PumpAlarm alarm) {
    bool active = (alarm != PUMP_ALARM_NONE);
    adc.setLed(0, active);                      // LED0 = PCF P4
    if (active) adc.alert(BUZZER_ALARM, true);  // repeating alarm pattern
    else         adc.stopAlert();               // silence when alarm clears
});
```

> **Tip:** APASENSE owns the PCF expander LEDs and buzzer. `alert(BUZZER_ALARM, true)` plays a repeating triple-beep until `stopAlert()` is called — no delay() or manual timer needed.

### Dry-run protection

When `enablePressure()` is registered, APAPUMP builds a dual pressure baseline (EMA): one for normal running, one for when the solar valve is open (higher pressure expected due to absorber resistance). One sample is taken every 10 s once the pump has run 30 s; after 5 samples the baseline is ready and `PUMP_ALARM_LOW_PRESSURE` fires when pressure drops below 40 % of it.

Before the baseline is ready, an absolute minimum of 0.1 bar (`APAPUMP_PRESSURE_ABS_MIN`) is used. Readings below 0.1 bar are never learned — a pump running dry cannot teach itself that "no pressure" is normal.

### Filter status — when to backwash

A sand or cartridge filter clogs slowly, and the pressure in front of it rises. `getFilterStatus()` compares the running pressure with the pressure measured when the filter was clean:

| Status | Rise above the clean pressure | Example (clean = 1.0 bar) |
|--------|------------------------------|---------------------------|
| `FILTER_CLEAN` | below 0.4 bar | below 1.4 bar |
| `FILTER_FILLING` | 0.4 bar or more | 1.4 bar or more — backwash soon |
| `FILTER_BACKWASH_NEEDED` | 0.8 bar or more | 1.8 bar or more — backwash now |
| `FILTER_UNKNOWN` | no clean pressure or no baseline yet | first ~80 s of running after each boot |

**The clean pressure is learned for you.** On the first run after installation (pump running, solar valve closed, at least 0.1 bar) the library takes its first stable pressure (~80 s of running) as the clean pressure and saves it to EEPROM. Every pool's pipework gives a different normal pressure — this way the limits follow your pool, not a fixed number.

**After every backwash** call `learnCleanPressure()` once (e.g. from an HMI button). It clears the stored value and learns the new clean pressure during the next ~80 s of running — if the pump is off, it simply waits for the next run. It never copies a single reading, so a spike or a stopped pump cannot be learned by mistake.

```cpp
pump.enablePressure([]() { return adc.getPressure(); }, 2.5f);

switch (pump.getFilterStatus()) {
    case FILTER_FILLING:         /* show "backwash soon" */            break;
    case FILTER_BACKWASH_NEEDED: /* show "backwash filter + LEARN" */  break;
    default: break;
}
// after backwashing:
pump.learnCleanPressure();
```

- It is a **status, not an alarm** — backwashing is maintenance, not an emergency. It keeps its value while the pump is stopped.
- The limits are household-pool values. Change them with `setFilterThresholds(warn, backwash)` at runtime, or `build_flags = -DAPAPUMP_FILTER_WARN_DELTA=0.5f -DAPAPUMP_FILTER_BACKWASH_DELTA=1.0f`.
- `setCleanPressure(bar)` sets the clean pressure by hand; `setCleanPressure(0)` forgets it (the current baseline is then taken as clean).
- Keep `maxPressure` in `enablePressure()` **above** the backwash level and at or below the filter's rated maximum (often 2.5 bar for household sand filters — check the label).

### Overcurrent protection

`setCurrentCallback()` gives APAPUMP the pump's current in amps (APASENSE: `adc.getCurrent()`). A current well above normal means the motor is struggling: a blocked or jammed impeller, failing bearings, or a motor winding fault.

**How it works**

1. **Learns the pump's own normal current** — no pump rating to enter. A 0.5 kW and a 1.5 kW pump each learn their own value.
2. One sample every 10 s once the pump has run 30 s (motor start-up current is ignored).
3. After 5 samples (~80 s of running) the baseline is ready and the alarm arms.
4. `PUMP_ALARM_OVERCURRENT` fires when a sample exceeds **1.5 × the baseline**. Checked every 10 s, so a fault is caught within 10 s.
5. The baseline adapts slowly to normal changes (weight 5 % per sample) and is kept across pump cycles.

**What it does not do**

- It does not stop the pump by itself — see [Stopping the pump on an alarm](#stopping-the-pump-on-an-alarm-recommended).
- It does not detect a dry run (a dry pump draws *less* current) — that is the pressure sensor's job.
- It is not a replacement for the motor's own thermal protection or the circuit breaker. It warns early; the breaker protects the wiring.

**Direct drive vs. external contactor** — check how your pump is wired:

| Wiring | What passes the current sensor | Overcurrent protection |
|--------|-------------------------------|------------------------|
| Pump powered directly through the board's relay | the pump's current (typically 2–8 A) | **active** |
| Board relay only switches an external contactor in the electrical box | only the contactor's coil (well under 0.5 A) | **silent** — no alarms, no false alarms |

> **Safety:** switching the pump through an external contactor (rated for motor loads) is always recommended over connecting it directly to a controller relay. Small board relays are often not rated for a motor's start-up current.

Readings below **0.5 A** (`APAPUMP_CURRENT_MIN_A`) are ignored: they are not a motor, just sensor noise or a contactor coil. So with a contactor the feature switches itself off safely — nothing to configure. **With a contactor, pressure monitoring (dry run, high pressure, filter status) is the pump's protection**; use a motor protection switch in the electrical box for the motor itself.

**Good to know**

- The baseline lives in RAM: after each boot it is re-learned during the first ~80 s of running, with no overcurrent alarm during that time.
- Nothing is learned while an alarm is active, so an overload never raises its own threshold.
- After replacing or servicing the pump, call `resetCurrentBaseline()` so the new motor's current is learned.
- A very small pump (under ~0.5 A): lower the floor with `build_flags = -DAPAPUMP_CURRENT_MIN_A=0.3f`.

### Freeze protection

```cpp
pump.enableFreezeProtection(
    []() { return adc.getPoolTemp(); },   // pool/pipe temperature
    4.5f                                  // threshold in °C (default)
);
```

When pool temperature drops below the threshold, the pump **cycles** to prevent pipes from freezing: runs for `APAPUMP_FREEZE_ON_SEC` (default 5 min), rests for `APAPUMP_FREEZE_OFF_SEC` (default 10 min), and repeats until temperature rises. Both durations are overridable via `build_flags`. **Dry-run interlock:** if the pressure sensor is enabled, calibrated, and indicates no water in the pipes (pressure near zero), freeze protection is suppressed — forcing the pump without water would burn the motor.

---

## Integrating with APADOSE and APALCDGUI

APAPUMP, APADOSE, and APALCDGUI are independent libraries — your sketch is the bridge. No library knows about the others.

### Bridge: APALCDGUI timer → APAPUMP schedule

```cpp
// In begin(): pass a lambda that reads the timer state from the GUI
pump.begin(
    []() {
        // return true when current time falls inside any timer slot
        uint16_t nowMin = rtcHour * 60 + rtcMinute;
        for (uint8_t i = 0; i < APA_LCD_MAX_TIMERS; i++) {
            if (gui.isTimerEnabled(i) &&
                nowMin >= gui.getTimerStart(i) &&
                nowMin <  gui.getTimerEnd(i)) return true;
        }
        return false;
    },
    nullptr,
    []() { return gui.getTimerTotalMinutes(); }  // daily target from timer sum
);
```

### Bridge: APADOSE post-shock → APAPUMP

```cpp
// Keep the pump running during a shock and for 4 hours after it, to circulate the
// chlorine (a schedule slot ending mid-shock would otherwise stop the pump and
// APADOSE would abort the shock). Uses the externalRequestCb — no new API needed.
// Elapsed-time check, so it stays correct when millis() wraps after ~49 days.

const uint32_t POST_SHOCK_MS = 4UL * 3600UL * 1000UL;
uint32_t postShockEndMs = 0;   // 0 = no post-shock window

bool postShockWantsPump() {
    if (dose1.isShockActive()) {
        postShockEndMs = millis() | 1;   // |1: never 0, the "no window" marker
        return true;
    }
    if (postShockEndMs == 0) return false;
    if (millis() - postShockEndMs < POST_SHOCK_MS) return true;
    postShockEndMs = 0;
    return false;
}

pump.begin(scheduleCb, postShockWantsPump);
```

### Bridge: pump manual mode → APALCDGUI status indicator

```cpp
// Show [M] in the LCD corner whenever the pump is in manual mode
pump.setStatusCallback([](const __FlashStringHelper* msg) {
    if (pump.getManualMode() != AUTO) gui.setStatusIndicator('M');
    else                              gui.clearStatusIndicator();
    // Also forward the status text to a scrolling message area
    Serial.println(msg);
});
```

### Bridge: APASENSE → APAPUMP (pressure and current)

```cpp
// Order: adc.begin() BEFORE pump.begin() — pressure auto-zeros on first pump stop.
// pressureCb returns -1.0f while APASENSE is uncalibrated — APAPUMP skips
// all pressure logic until a valid reading arrives.

adc.begin();    // zero-cal requires pump off — call first

pump.begin();
pump.enablePressure(
    []() { return adc.getPressure(); },    // -1.0f until calibrated
    2.5f                                   // alarm limit = filter's rated pressure (bar)
);
pump.setCurrentCallback([]() { return adc.getCurrent(); });   // amps; silent below 0.5 A

// Bridge: tell APASENSE when pump state changes so it can re-zero on pump stop
pump.setPumpStateCallback([](bool on) { adc.onPumpState(on); });
```

---

## Examples

| Sketch | Level | Description |
|--------|-------|-------------|
| `examples/01_minimal/` | Basic | PCF or GPIO, manual override via Serial, all three constructor options |
| `examples/02_scheduler/` | Intermediate | APALCDGUI timer schedule, UVC + AUX followers, alarm → active alert bridge |
| `examples/03_solar_freeze/` | Advanced | Solar heater + solar valve, freeze protection, pressure + current safety, filter status, pump stopped on alarm, APADOSE post-shock bridge |

---

## Platform Verification

Compiled with the `01_minimal` example. Zero errors, zero library warnings on all platforms.

| Platform | Board | RAM used | RAM total | Flash used | Flash total |
|----------|-------|----------|-----------|------------|-------------|
| Arduino Mega 2560 | ATmega2560 | 564 B | 8 192 B (6.9%) | 11 952 B | 253 952 B (4.7%) |
| Arduino Uno | ATmega328P | 564 B | 2 048 B (27.5%) | 11 186 B | 32 256 B (34.7%) |
| ESP32 DevKit | ESP32 | 22 024 B | 327 680 B (6.7%) | 290 713 B | 1 310 720 B (22.2%) |
| ESP8266 D1 Mini | ESP8266 | 28 852 B | 81 920 B (35.2%) | 274 271 B | 1 044 464 B (26.3%) |
| STM32 Bluepill | STM32F103C8 | 2 628 B | 20 480 B (12.8%) | 26 068 B | 65 536 B (39.8%) |

> The Uno row uses 27.5% RAM — that is the library with all Phase 2 features **registered** in the example. A minimal sketch (no solar, no pressure, no freeze) sits below 20%. ESP32 and ESP8266 totals include the full Arduino framework regardless of use.

---

## EEPROM Layout

APAPUMP reserves **12 bytes** starting at `APAPUMP_EEPROM_ADDR` (default 520).

### Field map

| Address | Bytes | Field | Saved when |
|---------|-------|-------|-----------|
| 520 | 2 | Magic `0xA55A` | — validity marker |
| 522 | 1 | Config version | — mismatch resets all fields to defaults |
| 523 | 2 | Daily target (minutes) | `setDailyTarget()` |
| 525 | 2 | Yesterday's runtime (minutes) | Midnight rollover (once per day) |
| 527 | 2 | Minimum run time (seconds) | `setMinRunTime()` |
| 529 | 2 | Clean pressure × 100 (bar; 0 = not learned) | automatic first learn, `learnCleanPressure()`, `setCleanPressure()` |
| 531 | 1 | Checksum (byte sum of bytes 520–530) | — corruption guard |

### Write protection and EEPROM lifespan

`_saveEEPROM()` writes the full 12-byte struct, but a physical write only occurs when a byte has actually changed:

- **AVR (Uno/Mega):** `EEPROM.put()` calls `EEPROM.update()` per byte — reads first, writes only if value differs.
- **ESP32/ESP8266:** `EEPROM.put()` updates a RAM buffer; `EEPROM.commit()` compares the buffer to flash before writing — no flash write if content unchanged.
- **STM32:** EEPROM emulation with compare-before-write.

**Practical write frequency** (worst case on a running system):

| Event | Max frequency | Cycles used at 100 k limit |
|-------|--------------|---------------------------|
| Midnight rollover | 1 per day | 100 k days ≈ 273 years |
| Clean pressure (first learn, `learnCleanPressure()`) | After install / after each backwash | Negligible |
| `setMinRunTime()` from `setup()` | 0 physical writes if value unchanged | None |
| First boot / corruption recovery | Once | 1 cycle |

**Corruption recovery:** the checksum is verified on every `begin()`. If power fails mid-write and data is corrupted, the mismatch is detected on the next boot, factory defaults are loaded, and a clean struct is written.

### Override base address

```ini
; platformio.ini — build_flags reach both your sketch and the library's own .cpp
build_flags = -DAPAPUMP_EEPROM_ADDR=532
```

### APA EEPROM address map

All ranges across the APA library suite — free blocks shown explicitly:

| Range | Bytes | Status | Owner |
|-------|-------|--------|-------|
| 0–127 | 128 | **free** | — |
| 128–177 | 50 | used | APAPHX2_ADS1115 (sensor calibration) |
| 178–188 | 11 | **free** | — |
| 189–191 | 3 | used | APADOSE global (pool volume, dead-band) |
| 192–291 | 100 | used | APADOSE per-pump config (25 bytes × up to 4 instances) |
| 292–499 | 208 | **free** | — |
| 500–501 | 2 | used | APALCDGUI brightness |
| 502–508 | 7 | used | APALCDGUI timer slots (default 3 slots) |
| 509–519 | 11 | **free** | — gap before APAPUMP ¹ |
| **520–531** | **12** | **used** | **APAPUMP** |
| 532–581 | 50 | **free** | — APAPUMP expansion reserve |
| 582–587 | 6 | used | APASENSE (pressure zero) |
| 588– | — | **free** | — |

> ¹ When `APA_LCD_MAX_TIMERS=6` the timer block extends to 514, leaving a 5-byte gap (515–519) before APAPUMP. This gap is intentional — do not place anything in 515–519 to keep the gap safe regardless of timer configuration.

---

## License

**Dual license — see LICENSE file for full terms.**

| Use | License |
|-----|---------|
| Personal, private, educational, hobby | Free — no charge, no paperwork |
| Commercial (products, services, OEM) | Separate written license required |

Commercial use includes selling or distributing hardware with this software, providing paid pool automation services, or integrating it into any revenue-generating product or system.

To obtain a commercial license: [kecup@vazac.eu](mailto:kecup@vazac.eu)

---

*APAPUMP — APA Devices · [kecup@vazac.eu](mailto:kecup@vazac.eu)*
