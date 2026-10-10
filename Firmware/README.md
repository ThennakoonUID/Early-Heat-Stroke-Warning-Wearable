# Heat-stroke early-warning monitor (ESP32 DevKit V1)

C7 reflectance PPG + heart rate (original, unchanged logic) + 2 x NTC body temperature +
SHT30 ambient temperature / humidity + GSR (skin conductance, lock-in front end) +
a risk engine that reports **NORMAL / CAUTION / WARNING / HEAT STROKE ALERT**.

> This is a screening aid built from simple, adjustable rules. It is **not** a validated medical
> device and must not be the only safeguard. Tune the thresholds on real data first.

## Wiring

| Function | ESP32 pin | Notes |
|---|---|---|
| I2C SDA / SCL | GPIO21 / GPIO22 | MAX30102 (0x57), MPU6050 (0x68), **SHT30 (0x44, ADDR low; 0x45 if ADDR high)** |
| NTC 1 | GPIO34 | `3V3 - 10k - GPIO34 - NTC - GND`, plus 100 nF from GPIO34 to GND |
| NTC 2 | GPIO35 | same divider |
| GSR DAC (excitation) | GPIO25 | into the Sallen-Key filter (schematic block 2) |
| GSR SW1 (10k reference) | GPIO16 | 74HC4066 control |
| GSR SW2 (electrodes) | GPIO17 | 74HC4066 control |
| GSR ADC | GPIO32 | after U2B / 150 R / 470 pF |
| Alert LED | GPIO2 | on-board LED. Optional buzzer: set `kBuzzerPin` in `Config.h` |

Keep the SHT30 away from body heat (it measures the air around the wearer, not the skin).
Everything in `Config.h` (pins, NTC values, GSR passives, thresholds' inputs) is in one place.

## Run

1. Open the folder in VS Code (PlatformIO) and Upload. Close the PlatformIO serial monitor.
2. Teleplot: select the COM port, baud 115200.
3. Keep the wearer still for ~7 s (motion calibration) and ~10 s more (resting HR), as before.
   The risk engine shows `WARM-UP` for the first 60 s while the NTCs reach skin temperature.
4. Serial keys: `c` re-calibrate motion, `l` raw log mode, `s` PPG status, **`g` GSR / NTC / SHT30 diagnostics**.

## Output

Text (also visible in the serial monitor), once per 5 s and on every condition change:

```
# [NORMAL] score 1 | Body 36.52C (+0.02/10min) | HR 72 bpm (conf 91) | Amb 28.4C 55%RH HI 30C | GSR 4.21 uS
# >>> HEAT STROKE ALERT <<<   score 8   Tb 38.4C, rising 0.9C/10min, HR 151 (81% max), heat index 46C, sweating x4.5
```

New Teleplot channels (1 Hz): `ntc1_c ntc2_c body_c body_slope amb_c rh_pct hi_c gsr_us gsr_kohm
gsr_phase gsr_snr_db risk_score risk_level alert`. `risk_level`: -2 no data, -1 warm-up, 0 normal,
1 caution, 2 warning, 3 ALERT. The LED blinks briefly (caution), blinks 1 Hz (warning), is solid (alert).

## How the alert is decided (`include/HeatRisk.h`, thresholds in `HeatRiskConfig`)

| Input | Points |
|---|---|
| Body temp >= 37.5 / 38.0 / 38.5 / 39.0 C | 1 / 2 / 3 / 4 |
| Rise >= 0.5 / 1.0 C per 10 min (only above 36.5 C) | +1 / +2 |
| HR at rest (< 60 mg motion) >= 100 / 120 / 140 bpm | 1 / 2 / 3 |
| HR while working >= 75 / 85 / 95 % of HRmax (208 - 0.7 x age) | 1 / 2 / 3 |
| Heat index >= 32 / 39 / 51 C | 1 / 2 / 3 |
| GSR >= 2x / 4x own baseline (first 60 valid s) | 1 / 2 |
| "Sweat failure": GSR < 50 % of its recent peak while body temp >= 38 C | +2 |

Total >= 3 CAUTION, >= 5 WARNING, >= 7 HEAT STROKE ALERT. Override to ALERT: body temp >= 39.5 C, or
body temp >= 38.5 C together with HR points >= 2 or sweat failure. A higher level must persist for
10-15 s (3 s for the override) to be shown, and drops one level only after 30 s of lower readings.
HR is only used when `hr_conf >= 40`. Missing sensors simply contribute 0 points; no valid body
temperature for 10 s shows `NO DATA`.

**Calibrate the temperature.** NTCs touching skin read below core temperature. Compare with a
clinical thermometer and set `kNtcOffsetC[]` (per sensor) and `kBodyTempOffsetC` in `Config.h`
so the "body temperature" matches what the thresholds mean. Also set `kNtcBeta`, `kNtcRSeries`/`kNtcR0`
and `kAgeYears`.

## GSR front end (`include/Gsr.h`, `include/GsrFrontEnd.h`)

* A FreeRTOS task on **core 0** generates the 1 kHz sine (8-bit DAC, 20 samples per cycle) and reads
  the ADC right after every DAC update, driven by the CPU cycle counter: 20 kS/s, whole cycles.
  If the board cannot keep that rate (measured at start-up) it falls back to 10 kS/s.
* A software lock-in measures the 1 kHz phasor. Every 20 s the 10k 0.1 % reference path (SW1) is
  measured too; the ratio cancels the TIA phase lag (your 470 pF gives about -16 deg at 1 kHz),
  gains and delays. The 1 uF electrode capacitor is removed mathematically.
* Output: skin admittance `|Y|` in uS (`gsr_us`), `|Z|` in kohm, phase, and SNR. Below
  `kGsrContactMinUs` (0.1 uS) the electrodes count as "not on skin".
* Best accuracy is for skin impedances above ~20 kohm (Rs = 100 k limits the current to ~9 uA p-p).
* NTC readings are taken by the same task (about once per second) so ADC1 is never used from two cores.
* Check with key `g`: `ref=ok` and `refAmp` should be roughly 400-650 counts; `overruns` should stay near 0.

## PC tests (no hardware needed)

```
g++ -std=c++17 -O2 -Iinclude pc_sim/test_gsr.cpp  -o test_gsr  && ./test_gsr    # lock-in + impedance maths
g++ -std=c++17 -O2 -Iinclude pc_sim/test_risk.cpp -o test_risk && ./test_risk   # risk-engine scenarios
```

The original PPG tools (`sim`, `sim_work`, `replay`, `test_motion`) and documentation are unchanged:
see `README_PPG.md`.

## Status of this code

The PPG / heart-rate code is the original, untouched. The new code compiles cleanly against stub
headers and its maths / risk logic were tested on the PC, but it **has not been run on real hardware**.
First checks: key `g` (reference OK, overruns ~0, SHT30 found), NTC readings vs a thermometer,
GSR with a known resistor in place of the electrodes (e.g. 100 k -> about 10 uS).

---

## Firebase + buzzer version (current)

Teleplot output and the old risk engine are gone. Instead the firmware uploads five values to
Firebase and sounds a buzzer. Everything is readable on the serial monitor (115200 baud, one line per second).

**Before the first upload set in `include/Config.h`:** `kWifiSsid`, `kWifiPass` (and `kFirebaseAuth` if the
database rules are not public). Wire an active buzzer to GPIO27 (`kBuzzerPin`).

| Firebase key (`/devices/DEV-001`) | Source |
|---|---|
| `skin_temp` | mean of NTC1 + NTC2. If they differ by more than `kNtcMaxDiffC`, the one that suddenly jumped (lost contact) is dropped and only the other is shown. Both unusable (outside `kSkinMinC..kSkinMaxC`, or both jumped together) -> the text `kSkinErrorText` |
| `outside_temp` | SHT30 temperature |
| `feels_like_temp` | heat index from SHT30 temperature + humidity |
| `heart_rate` | bpm from the PPG (`null` while there is no confident value) |
| `sweat_level` | 1 (dry) .. 10 (very wet), log scale of the GSR conductance between `kSweatGMinUs` and `kSweatGMaxUs` (`null` without electrode contact) |

A value that is not available is sent as `null`, so the app never shows an old number.

**Buzzer (not sent to Firebase):** beeps while any of these holds: skin > 39 C, HR > 120, HR < 50, sweat level <= 1.
Skin / HR must hold for 3 s, low sweat for 20 s (`kAlertHoldSec`, `kSweatLowHoldSec`). All limits are in `Config.h`.

The logic lives in `include/HeatMetrics.h` (pure C++, PC-testable), the upload in `include/FirebaseUploader.h`
(own task on core 0, so it never stalls the PPG sampling).
