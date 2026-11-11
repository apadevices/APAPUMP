# APAPUMP

<p align="center">
<img src="extras/apapump-logo.png" width="600" alt="APAPUMP">
</p>

**Autonomous filtration pump controller for APA Devices pool automation**
· ![v1.3.3](https://img.shields.io/badge/version-1.3.3-blue)
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
- **AUX device** — plain follower (ozone etc.), or a **heat pump** in `HEATER_SIMPLE` / `HEATER_FULL` ([details](#heater-heat-pump-on-the-aux-relay))
- **Solar valve** — follows the solar demand, also during a scheduled run; instantly closes in manual mode

### Solar heating
- Extra runs on top of your schedule: heating (collector warmer than the pool) and cooling (pool above Max pool)
- Absorber–pool temperature dead-band hysteresis (configurable start/stop deltas)
- Invalid temperatures (missing sensor, DS18B20 power-on value) stop all solar logic — never acted on
- Day/night gate for heating — a safety net for a badly placed collector sensor ([why](#daynight-gate--why-it-exists))
- **Overheat protection:** collector too hot → pump ON + valve OPEN, in AUTO only and never during a pump alarm

### Heater (heat pump)
- **Solar first** — the heat pump waits while solar heats
- Never heats without flow: off on any pump alarm; 10-minute minimum ON/OFF against compressor short-cycling
- `HEATER_SIMPLE` (the heat pump's own thermostat regulates) or `HEATER_FULL` (the library is the thermostat)
- Permit hook for a cheap-tariff input

### Saved settings
- Solar mode, Max pool, AUX/heater mode, heater target and pressure ON/OFF survive a reboot ([details](#saved-settings))

### Safety alarms
- **Overcurrent** — learns the pump's own normal current, alarms at × 1.5; no pump rating to enter; silent with an external contactor ([details](#overcurrent-protection))
- **Dry-run** — pressure below EMA baseline × 40% (or absolute minimum before EMA builds)
- **Overpressure** — absolute hard limit at `maxPressure` (the filter's rated pressure); optional sudden-rise alarm
- **Pressure monitoring can be switched off at runtime** — `setPressureEnabled(false)` for installations without a pressure sensor ([details](#pressure-sensor-optional))
- **Learned baselines never learn a fault** — no learning while an alarm is active, nor from a dry pipe or a motor-less current sensor
- **Freeze protection** — pool temp below 4.5 °C → cyclic run (5 min ON / 10 min rest), also below 0 °C; a missing reading never ends it; suppressed if dry-run detected
- **No-flow stub** — flow switch confirmed after settle; hardware-ready for future sensor
- All alarms are latching — explicit `acknowledgeAlarm()` required

### Filter maintenance
- **Filter status** — clean / filling / backwash needed, from the pressure rise above the clean-filter pressure ([details](#filter-status--when-to-backwash))
- Clean pressure learned automatically on the first run; one call after each backwash re-learns it

### Engineering
- **577 B SRAM** on Arduino Uno (example 01) — fits comfortably in 2 KB
- 32 boolean flags / small modes packed into 4 bytes; all string literals in flash (`F()` macro)
- EEPROM: 15 bytes, magic + version + checksum validation, persists target, clean pressure and settings
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

APAPUMP controls a pool filtration pump and up to three follower devices (UVC lamp, AUX device or heat pump, solar valve) through a PCF8574AT I2C relay expander or direct Arduino output pins. Each call to `update()` in your `loop()` advances the state machine, checks every safety guard in priority order, and fires alarm callbacks when something is wrong — all without blocking.

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
          (no AUX lead on FORCE_OFF or a pump alarm: AUX + pump OFF at once)
```

**Guards that bypass min-run time:** `FORCE_ON` and `FORCE_OFF` — manual mode suspends timing guards.

**Stops without the AUX lead (since 1.3.1):** `FORCE_OFF` and a latched pump alarm switch AUX and the
pump off together — OFF means stop now, and a pump running dry or overloaded must not keep running for an
after-run. Coming during a running lead, they cut it short. UVC still gets its post-delay.

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
With `FORCE_OFF` or a latched pump alarm there is no `lead`: AUX and the pump go off at `Ts` (since 1.3.1).

### Priority engine

Every `update()` tick calls the priority engine. The first rule that matches wins:

| Priority | Condition | Result |
|----------|-----------|--------|
| **P0** | Collector > safety temp (default 50 °C), **AUTO only, no pump alarm** | Pump ON + valve OPEN |
| **P1** | `FORCE_OFF` manual mode | Pump OFF |
| **P2** | `FORCE_ON` manual mode | Pump ON |
| **P3** | External request callback returns `true` | Pump ON |
| **P4** | Freeze: pool temp < threshold AND water present | Pump ON |
| **P5a** | Solar wants to run: heating (hysteresis) or cooling | Pump ON |
| **P5b** | Schedule callback returns `true` (default `SOLAR_WITH_SCHEDULE`, or no solar) | Pump ON |
| **P5b'** | `SOLAR_WITH_CATCHUP` only: daily runtime < target (window OK) — replaces P5b | Pump ON |
| — | None of the above | Pump OFF |

Before 1.3.0, P0 also overrode `FORCE_OFF`. It no longer does: manual OFF usually means someone is
working at the pump, and after a dry-run alarm there is no water to circulate. The collector has its own
vent valve for overheating.

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

The zero-arg constructor is **active-low** (the board's 74HC240 inverts): PCF LOW = relay ON. `begin()` writes all relays OFF immediately (`0xFF`), before any other code runs.

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

**Note:** solar overheat protection (P0) runs only in `AUTO` (since 1.3.0). In `FORCE_OFF`/`FORCE_ON` it is skipped — `isSolarSafetyActive()` still reports a hot collector so your sketch can warn.

---

## Followers: UVC, AUX, Solar Valve

Each follower is independent and optional. Wire the relay to the corresponding PCF bit (or GPIO pin), then call the enable method:

```cpp
// UVC lamp: ON 5 s before pump, OFF 30 s after pump stops
pump.enableUVC(5, 30);

// AUX device: ON 10 s after pump starts, OFF 30 s before pump stops (plain follower)
pump.enableAux(10, 30);

// Solar valve on P3 (or direct pin d): follows the solar demand
pump.enableSolar(absorberTempCb, poolTempCb, /*useValve=*/true, ...);
```

**UVC pre-delay** — when a pre-delay is configured, the pump enters STARTING state while UVC warms up. If the reason to start disappears during STARTING (e.g. schedule ends), the sequence is cancelled cleanly — UVC turns off and the pump never fires.

---

## Solar Heating

By default (`SOLAR_WITH_SCHEDULE`) your **schedule keeps driving the pump** and solar adds extra runs:
heating when the collector is warmer than the pool, cooling when the pool is above Max pool. During a
scheduled run the valve opens whenever solar heating or cooling wants it, and closes again when not.

```cpp
pump.setPoolTempCallback([]() { return poolTemp; });   // shared by solar, heater FULL
pump.enableSolar(
    []() { return collectorTemp; },   // collector (absorber) temperature, °C
    nullptr,                          // pool temp: nullptr = keep setPoolTempCallback()
    true,                             // useValve — first-boot default (SOLAR_VALVE / SOLAR_PUMP)
    8.0f,                             // startDelta: start when collector > pool + 8 °C
    3.0f,                             // stopDelta:  stop  when collector < pool + 3 °C
    32.0f,                            // maxPoolTemp — first-boot default, 0 = no limit
    2.0f,                             // coolDelta: cool when collector < pool - 2 °C
    60.0f                             // safetyTemp: overheat protection (AUTO only)
);
pump.setSolarDayNight([]() { return RTClib::now().unixtime(); }, 6, 21);   // heating 06:00-21:00

// From a menu (saved, survive a reboot):
pump.setSolarMode(SOLAR_OFF);         // or SOLAR_PUMP (no valve) / SOLAR_VALVE
pump.setSolarMaxPool(30);             // °C; 0 = no limit — no cooling AND no heating cap
```

- **Max pool is a cap:** solar heating does not start at or above Max pool, and a running heating run
  ends once the pool reaches it (since 1.3.3; before, a running run continued until the collector cooled,
  so the pool could end above Max pool). It may start again below Max pool − `APAPUMP_SOLAR_MAX_POOL_HYST_C`
  (0.5 °C). Above Max pool cooling runs when the collector is `coolDelta` colder than the pool.
- **`SOLAR_PUMP`** — no valve: solar only switches the filtration pump; the collector is always in the loop.
- **`SOLAR_VALVE`** — a 3-way valve on P3. Without a valve relay it is stored as `SOLAR_PUMP`.
- **Valve settle:** after every valve change the pressure checks and baseline learning pause for
  `APAPUMP_VALVE_SETTLE_SEC` (30 s) — air pushed out of the collector line must not trip a dry-run alarm.
- **1.2.0 behaviour** (solar is the only driver, the schedule only gives the daily target):
  `pump.setSolarDrive(SOLAR_WITH_CATCHUP);` — see [Daily runtime](#daily-runtime-target-and-catch-up).
- **Pump alarm:** while a pump alarm is latched, solar asks for no extra runs at all (after a dry run there
  is no water to circulate). Your schedule still runs — stop the pump on an alarm in your sketch.
- **Status:** `isSolarRunning()`, `isSolarValveOpen()`, `isSolarSafetyActive()` (collector above
  safetyTemp, reported in every mode), `isSolarSensorOk()`.

### Invalid temperatures

A temperature is used only if it is between `APAPUMP_TEMP_MIN_C` (−20) and `APAPUMP_TEMP_MAX_C` (100 °C),
and not exactly **85.0** (DS18B20 power-on value) or **−1.0** (the "not ready" value of APAPUMP ≤ 1.2.0).
Return **−127.0f** from a callback while a sensor is not ready. On an invalid collector **or** pool reading,
solar does nothing at all — no heating, no cooling, no overheat protection — and `isSolarSensorOk()` is
false (status "Solar sensor fault"). Before 1.3.0 a missing collector sensor (−127) made cooling run
nonstop, and a missing pool sensor made heating run all day. A pool temperature callback is now required
for solar.

### Day/night gate — why it exists

`setSolarDayNight()` allows **heating** only between the two hours; cooling is temperature-only, day or
night. The gate is a **safety net for a collector sensor in the wrong place**: mounted on a pipe near the
pump room instead of on the absorber (or its outlet), it can read "warmer than the pool" in the evening or
at night. Solar would then push water through a cold absorber — cooling the pool. Use **wide hours**
(e.g. 06–21): a dark roof often still gains heat after 20:00. The hours need the RTC on local time.

### Overheat protection

Collector above `safetyTemp` → the pump runs and the valve opens, but **only in `AUTO` and never while a
pump alarm is latched**. Manual OFF usually means someone is working at the pump; after a dry run there
is no water to circulate. Pool collectors have their own vent valve. Choose `safetyTemp` above the pool
temperatures you expect — every overheat run pushes hot water into the pool, even above Max pool.

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

## Heater (heat pump) on the AUX relay

```cpp
pump.enableAux(10, 120);              // on 10 s after the pump; pump runs 120 s after it (after-run)
if (!pump.hasSavedSettings()) pump.setAuxMode(HEATER_SIMPLE);   // first boot only — saved
```

| `AuxMode` | AUX relay |
|---|---|
| `AUX_DISABLED` | never on |
| `AUX_FOLLOWER` | on/off with the pump, exactly as before 1.3.0 (ozone etc.) — first-boot default of `enableAux()` |
| `HEATER_SIMPLE` | allowed while the pump runs; the heat pump's own thermostat regulates |
| `HEATER_FULL` | the library is the thermostat: on below `setHeaterTarget()` − 1.0 °C, off at the target (15–40 °C, default 28) |

In both heater modes the AUX relay is on only while **all** of these hold:
the pump runs (past the start delay) · no pump alarm (no flow = no heating) · solar is not heating, could
not heat right now (collector above pool + startDelta, inside the day gate) and the collector is not
over-hot (solar first) · the permit callback allows it (`setHeaterPermitCallback()`,
e.g. a cheap-tariff input) · the pool is not above Max pool. `HEATER_FULL` also needs a valid pool
reading. A 10-minute minimum ON and OFF time (`APAPUMP_HEATER_MIN_ON_SEC` / `_MIN_OFF_SEC`) protects the
compressor — also after power-up, so the heater waits 10 min after a (re)boot; a pump alarm, a pump stop
or running overheat protection (AUTO, collector above the safety temperature — since 1.3.2) always switch it
off at once. When normal solar heating takes over, a running heat pump finishes its minimum ON time. The heater never asks for extra
pump runs — it heats during your filtration schedule. When the heater was already off, the pump stops
without the after-run. `FORCE_OFF` and a pump alarm also stop the pump without the after-run (since
1.3.1) — the heat pump's own flow switch protects it then. `isHeaterOn()` reports the relay.

In `HEATER_FULL` set the heat pump's own thermostat to its maximum — otherwise the lower of the two settings wins.

> **Relay rule:** the AUX relay switches the heat pump's **remote ON/OFF input** (a few mA) or a **fused
> contactor coil** with suppression (RC snubber / varistor) — never the compressor or a heating element directly.

---

## Saved settings

The library saves only what an owner or installer changes, so it survives a reboot:
solar mode, Max pool, AUX/heater mode, heater target, pressure monitoring ON/OFF (plus, as before, the
daily target, minimum run time and clean-filter pressure). **Constants** — start/stop/cool deltas, safety
temperature, day gate hours, drive model, timings — are **not** saved: pass them in `setup()` every boot.

`enableSolar()` / `enableAux()` give **first-boot defaults** only; once saved values exist they win. A saved
mode never activates hardware by itself: solar needs `enableSolar()` and the AUX relay `enableAux()` (or
`setAuxMode()`) in every boot. To
apply your own first-boot defaults without overwriting the owner's choice on every boot:

```cpp
if (!pump.hasSavedSettings()) {       // true after the first boot with this version
    pump.setSolarMode(SOLAR_OFF);     // e.g. a product shipped with solar off
}
```

Saved setters write the EEPROM only after `begin()`. Upgrading from 1.2.0 resets the EEPROM once
(layout version 2): the clean-filter pressure and daily counters are learned again.

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

Catch-up is used only with `setSolarDrive(SOLAR_WITH_CATCHUP)` (the 1.2.0 model, where solar is the only
driver). It runs as soon as today's runtime is below the target — from midnight, or from the start of the
catch-up window:

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

### Pressure sensor optional

Not every installation has a pressure sensor (small garden pools often don't). Register the sensor once with `enablePressure()`, then switch monitoring on or off at any time — e.g. from an installer menu. Since 1.3.0 the library saves the choice (a saved OFF also wins over the next boot's `enablePressure()`), so call it only when the installer changes it:

```cpp
pump.enablePressure([]() { return adc.getPressure(); }, 2.5f);
pump.setPressureEnabled(false);   // from the menu — saved; OFF: no pressure checks at all
```

With pressure **off** there is **no dry-run, no high-pressure alarm and no filter status** — the pump then has no protection against running dry. Without a sensor, leaving it on is harmless (a sensor that never delivers a valid reading keeps every pressure check inactive), but an explicit OFF also ignores a disconnected input that floats to a plausible value. Switching off clears the last reading and the learned baselines; switching on re-learns them (~80 s of running). The stored clean-filter pressure is kept.

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

When pool temperature drops below the threshold, the pump **cycles** to prevent pipes from freezing: runs for `APAPUMP_FREEZE_ON_SEC` (default 5 min), rests for `APAPUMP_FREEZE_OFF_SEC` (default 10 min), and repeats until a valid reading reaches threshold + `APAPUMP_FREEZE_HYST_C` (1.0 °C). Readings below 0 °C are valid (before 1.3.0 they were ignored, so protection was off exactly when freezing). An invalid reading (−127, −1.0, 85.0) never ends an active protection — if the sensor is lost while it
is active, the cycles go on until a valid reading returns (fail-safe in frost). Both durations are overridable via `build_flags`. **Dry-run interlock:** if the pressure sensor is enabled, calibrated, and indicates no water in the pipes (pressure near zero), freeze protection is suppressed — forcing the pump without water would burn the motor.

---

## Integrating with APADOSE and APALCDGUI

APAPUMP, APADOSE, and APALCDGUI are independent libraries — your sketch is the bridge. No library knows about the others.

### Bridge: APALCDGUI timer → APAPUMP schedule

```cpp
// In begin(): pass a lambda that reads the timer state from the GUI
pump.begin(
    []() {
        // true inside any timer slot; isTimerActive() also handles slots across midnight
        return gui.isTimerActive(rtcHour * 60 + rtcMinute);
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
| `examples/03_solar_freeze/` | Advanced | Solar + valve, heat pump (HEATER_SIMPLE), freeze protection, pressure + current safety, filter status, pump stopped on alarm, APADOSE post-shock bridge |
| `examples/04_hardware_test/` | Bench | Interactive relay/sensor test on the APA HMI board (LCD menu) — uses 93 % of an Uno's flash |
| `examples/05_solar_heater/` | Intermediate | Schedule + solar + heat pump in a few lines; first-boot defaults with `hasSavedSettings()` |

---

## Platform Verification

Compiled with the `01_minimal` example. Zero errors, zero library warnings on all platforms.

| Platform | Board | RAM used | RAM total | Flash used | Flash total |
|----------|-------|----------|-----------|------------|-------------|
| Arduino Mega 2560 | ATmega2560 | 577 B | 8 192 B (7.0%) | 13 420 B | 253 952 B (5.3%) |
| Arduino Uno | ATmega328P | 577 B | 2 048 B (28.2%) | 12 658 B | 32 256 B (39.2%) |
| ESP32 DevKit | ESP32 | 22 040 B | 327 680 B (6.7%) | 292 113 B | 1 310 720 B (22.3%) |
| ESP8266 D1 Mini | ESP8266 | 28 868 B | 81 920 B (35.2%) | 275 539 B | 1 044 464 B (26.4%) |
| STM32 Bluepill | STM32F103C8 | 2 644 B | 20 480 B (12.9%) | 27 096 B | 65 536 B (41.3%) |

All five examples build on all five platforms (1.3.0). Against 1.2.0: +13–16 B RAM, about +1.5 KB flash.
1.3.1 (stop-sequence patch) was built on the Mega only: +0 B RAM, +32 B flash.
1.3.2 (heater cut by overheat protection) was built on AVR (Mega, Uno) only: +0 B RAM, +10 B flash.
1.3.3 (Max pool ends a running heating run) was built on AVR (Mega, Uno) only: +0 B RAM, +54 B flash.

> The Uno row uses 28.2% RAM — that is the library with all Phase 2 features **registered** in the example. A minimal sketch (no solar, no pressure, no freeze) sits below 20%. ESP32 and ESP8266 totals include the full Arduino framework regardless of use.

---

## EEPROM Layout

APAPUMP reserves **15 bytes** starting at `APAPUMP_EEPROM_ADDR` (default 520) — layout version 2 since 1.3.0 (12 bytes before).

### Field map

| Address | Bytes | Field | Saved when |
|---------|-------|-------|-----------|
| 520 | 2 | Magic `0xA55A` | — validity marker |
| 522 | 1 | Config version | — mismatch resets all fields to defaults |
| 523 | 2 | Daily target (minutes) | `setDailyTarget()` |
| 525 | 2 | Yesterday's runtime (minutes) | Midnight rollover (once per day) |
| 527 | 2 | Minimum run time (seconds) | `setMinRunTime()` |
| 529 | 2 | Clean pressure × 100 (bar; 0 = not learned) | automatic first learn, `learnCleanPressure()`, `setCleanPressure()` |
| 531 | 1 | Settings: bit 0 pressure ON, bits 1–2 `SolarMode`, bits 3–4 `AuxMode` | `setPressureEnabled()`, `setSolarMode()`, `setAuxMode()` |
| 532 | 1 | Max pool (°C, 0 = no limit) | `setSolarMaxPool()` |
| 533 | 1 | Heater target (°C) | `setHeaterTarget()` |
| 534 | 1 | Checksum (byte sum of bytes 520–533) | — corruption guard |

### Write protection and EEPROM lifespan

`_saveEEPROM()` writes the full 15-byte struct, but a physical write only occurs when a byte has actually changed:

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
build_flags = -DAPAPUMP_EEPROM_ADDR=535
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
| **520–534** | **15** | **used** | **APAPUMP** (12 bytes before 1.3.0) |
| 535–581 | 47 | **free** | — APAPUMP expansion reserve |
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
