# C7 PPG + Heart Rate Monitor, also during work (ESP32 + MAX30102 + MPU6050)

Reflectance PPG and heart rate from the upper back (C7). Heart rate keeps being
measured while the wearer is working (walking, hammering, carrying), using the
accelerometer to identify and remove the motion frequencies. Output goes to
**Teleplot**: `ppg`, `ir_raw`, `hr_bpm`, `hr_conf`, `state`, `act_mg`, `cad_lock`.

## Wiring

| Signal | ESP32-WROOM-32D | MAX30102 | MPU6050 |
|---|---|---|---|
| SDA | GPIO21 | SDA | SDA |
| SCL | GPIO22 | SCL | SCL |
| Power | 3V3 | VIN | VCC |
| Ground | GND | GND | GND |
| Address | | 0x57 | 0x68 (AD0 → GND) |

No libraries needed; both sensors use small register-level drivers in `include/`.

## Running it

1. Open the folder in VS Code (PlatformIO) → **Upload**.
2. Close the PlatformIO serial monitor (only one program can hold the COM port).
3. Teleplot → select the COM port → baud **115200** → Open.
4. Keep the wearer **still for ~7 s** after reset (motion calibration), then
   ~10 s more so the resting heart rate is measured. Type `c` to re-calibrate.

## Teleplot channels

| Channel | Rate | Meaning |
|---|---|---|
| `ppg` | 100 Hz | PPG waveform. Clean when still, best-effort during moderate motion, 0 when unusable |
| `ir_raw` | 100 Hz | Raw 18-bit IR ADC counts |
| `hr_bpm` | 1 Hz | Heart rate. **0 = unknown** (not yet locked, or no reliable data for > 20 s) |
| `hr_conf` | 1 Hz | **0-100 trust in `hr_bpm`**. Combines autocorrelation quality, per-beat template match, spectral confidence, whether HR sits on a motion frequency, and age of the last real measurement. Use this in alarm logic, not just "non-zero" |
| `state` | 1 Hz | 0 calibrating, 1 no contact, 2 heavy motion, 3 settling, 4 still, 5 motion tracking |
| `act_mg` | 1 Hz | Dynamic body acceleration RMS (mg): a workload proxy, so HR can be judged against effort |
| `cad_lock` | 1 Hz | 1 = HR is within 12 bpm of the step cadence (or 1/2, 2x, 3x of it): the spectral result is suspect |
| `red_raw` | optional | Off by default (`kRedDiv` in `main.cpp`) |

Link budget at 115200 baud (about 11.5 KB/s): plot channels ~4.6 KB/s (40 %), raw log mode ~6.6 KB/s (57 %).
If the serial buffer ever fills, a plot sample is dropped (counted, see `s` command) instead of stalling the sensors.

## Serial commands

| Key | Action |
|---|---|
| `c` | Re-calibrate the still baseline (sit still ~7 s) |
| `l` | Toggle **raw log mode** (see the next section). Plot channels are off while logging |
| `s` | Print status: FIFO overflows, data gaps, ambient-light overflows, dropped plot lines, LED current |

## Validating on real data (the most important step)

The thresholds were tuned in simulation. To tune them on real C7 data:

1. Wear the device **and a chest strap (Polar H10 or ECG)**. Start the strap's recording at the same moment you reset the board.
2. In a serial terminal capturing to a file (not Teleplot), type `l`. Each line is
   `L,n,ir,red,ax_mg,ay_mg,az_mg,gx_0.1dps,gy,gz,vib_0.1mg,pa`. Do the tasks: rest, walking, hammering, lifting, bending, sweating.
3. Export the strap HR as `t_s,bpm` (seconds from the start).
4. Replay it through the **same** code as the firmware:

```
g++ -std=c++17 -O2 -Iinclude pc_sim/replay.cpp -o replay
./replay session.txt strap.csv                  # baseline
./replay session.txt strap.csv --gyro           # A/B: gyro as NLMS reference
./replay session.txt strap.csv --red            # A/B: red as NLMS reference
./replay session.txt strap.csv --offset 2.5     # shift the reference if clocks differ
```

It prints MAE, % of seconds within 5 bpm, and coverage overall, per state, and for confident seconds (`hr_conf >= 50`) only.
It also writes `replay_out.csv` (per second). Lines lost on the serial link are detected from the `n` column and treated as data gaps.
Change thresholds in the headers and re-run in seconds, without re-recording.

## What changed in this version

| Change | Where | Why |
|---|---|---|
| Spectral window 8 s -> 12 s | `SpectralHr.h` | True resolution 7.5 -> 5 bpm; lower error in simulation (see results) |
| Band-pass, reference and motion high-pass filters in **double** precision | `Filters.h`, `MotionDetector.h` | 0.1-0.5 Hz sections at 100 Hz have poles very near z = 1; float32 loses accuracy there |
| Per-beat template matching (`BeatQuality`) | `HeartRate.h` | Scores every beat against a running template learned only from good beats; sets the Kalman measurement noise (2.5-9.5 bpm) |
| `hr_conf`, `cad_lock`, `act_mg`, `state` outputs | `PpgProcessor.h`, `main.cpp` | The alarm logic can weigh readings instead of treating them all equally |
| Data-integrity handling | `main.cpp`, `Max30102.h`, `PpgProcessor.h` | MAX30102 / MPU6050 FIFO overflows and the MAX30102 ambient-light-overflow flag (strong sunlight) now exclude the next 2.5 s from all estimators (`notifyGap`) |
| Calibration fallback + slow auto-recalibration | `MotionDetector.h` | Workers cannot always sit still: after 4 failed attempts the floors are used; while quiet the thresholds follow slow drift (bounded to 2x) |
| Optional gyro and red NLMS references (**off**) | `PpgProcessor.h` | Switches `kUseGyroRef`, `kUseRedRef`; see below |
| Raw log mode + `replay` tool + `test_motion` | `main.cpp`, `pc_sim/` | Real-data validation and tuning |
| TX-buffer guard | `main.cpp` | Serial can never block sampling at 115200 |

### The optional references are off on purpose

- **Gyro** (`kUseGyroRef`): in simulation it raises coverage (81 -> 91 % while walking) but also error (overall 2.32 -> 2.47 bpm). The simulation has no rotation-to-optics coupling, so it cannot show the benefit that rotation references may give on a real back. Decide with `replay --gyro`.
- **Red** (`kUseRedRef`): the pulse is present in the red channel too, so using it as a motion reference can cancel part of the pulse. A synthetic test (red = scaled IR) made accuracy clearly worse. Only enable it if `replay --red` beats the baseline on real data, and then raise the red LED current (the firmware does this automatically when the switch is on).

## How heart rate is measured (three motion levels)

| Motion level | Detected by | HR method | `ppg` |
|---|---|---|---|
| Still | IMU below the calibrated resting level | Time domain: beat peaks + autocorrelation (most accurate) | clean |
| Moderate / periodic work | IMU above resting level | Spectral tracking with motion frequencies removed | best effort |
| Heavy (lifting, bumps, pressure jumps) | PPG swing > 3 % of DC, IR jump > 5 %, or > 0.7 g | None: tracker predicts from HR + trend for up to 20 s, then 0 | 0 |

**Spectral tracking** (`SpectralHr.h`), every 2 s on the last 12 s:
1. FFT of the PPG and of the 3 accelerometer axes (25 Hz, 12 s Hann window, zero-padded to 1024 points → 1.5 bpm/bin, 5 bpm true resolution).
2. Find the motion peaks (step rate, stride rate, arm rate) in the accelerometer spectrum.
3. Build a motion template: each motion peak's spectral shape (same window
   leakage as in the PPG) plus copies at its 2nd–5th harmonics. Non-linear
   coupling creates harmonics and sum frequencies (step + stride) in the PPG.
4. Weight the PPG spectrum down where the template is high, then pick the peak
   closest to the tracker's prediction.

**Kalman tracker** (state = HR and HR trend in bpm/s):
- Follows HR rising at work onset and falling during recovery without lag;
  the 6 s window delay is part of its measurement model.
- Weak peaks can only nudge the estimate; strong peaks correct it.
- When the predicted HR sits on a motion frequency (HR crossing the step rate),
  it coasts on the trend instead of trusting the distorted peak.
- Never acquires or re-locks onto a peak that sits on a motion frequency.
- During motion, HR is not allowed below (resting HR − 10 bpm).

## Simulation results (`pc_sim/`)

```
g++ -std=c++17 -O2 -Iinclude pc_sim/sim_work.cpp -o sim_work && ./sim_work [seed] [artifact_scale]
g++ -std=c++17 -O2 -Iinclude pc_sim/sim.cpp -o sim && ./sim
g++ -std=c++17 -O2 -Iinclude pc_sim/test_motion.cpp -o test_motion && ./test_motion
```

`sim_work`: 300 s worker scenario (rest, walking with HR rising 78 -> 120 and crossing the 108 /min step rate,
hammering, heavy lifting, walking, recovery). 8 seeds x 3 artifact strengths (1x, 2x, 3x), while `hr_bpm` was non-zero:

| Segment | Original: coverage / mean error | This version: coverage / mean error |
|---|---|---|
| Rest | 100 % / 1.27 bpm | 100 % / 1.27 bpm |
| Walking, HR crossing the step rate | 76 % / 5.54 bpm | 81 % / 4.78 bpm |
| Hammering | 85 % / 1.63 bpm | 92 % / 2.04 bpm |
| Heavy lifting (predicted) | 100 % / 1.82 bpm | 100 % / 1.22 bpm |
| Walking, HR away from step rate | 88 % / 1.41 bpm | 85 % / 1.00 bpm |
| Recovery | 100 % / 1.59 bpm | 100 % / 1.58 bpm |
| **Overall** | **2.52 bpm** | **2.32 bpm** |

Where the error is large, `hr_conf` is low. In a single run (seed 3, 1x artifacts) the mean confidence was 98 at rest
and about 13 on the seconds where the error exceeded 6 bpm; while walking, seconds flagged by `cad_lock` had 9.5 bpm
mean error against 3.1 bpm for unflagged seconds. `simulation_work.png` shows seed 3 at 2x artifacts.

These are simulated artifacts. Verify against a chest strap during real tasks (see "Validating on real data").

## Main tuning knobs

| Setting | File | Default | Effect |
|---|---|---|---|
| `heavyOpticalFrac`, `heavyAccG`, `opticalStepFrac` | `PpgProcessor.h` | 3 %, 0.7 g, 5 % | When motion counts as "heavy" (no HR measurement) |
| `hrHoldSec` | `PpgProcessor.h` | 20 s | How long the predicted HR is shown without data |
| `restMarginBpm` | `PpgProcessor.h` | 10 | HR floor during motion = resting HR − this |
| `contactDcAtMax` | `PpgProcessor.h` | 25000 | IR DC for "on skin". **Set to ~30 % of your on-skin `ir_raw`** |
| `motionWeight`, `harmonicLevel` | `SpectralHr.h` | 20, 0.5 | How hard motion frequencies are suppressed |
| `minConfMotion`, `acqConfMotion` | `SpectralHr.h` (HrTracker) | 0.2, 0.35 | Confidence to update / acquire during motion |
| `kWin`, `specDelay` | `SpectralHr.h` | 300 (12 s), 6 s | Spectral window; keep `specDelay` = half the window. 16 s was not better in simulation |
| `cadenceBandBpm` | `PpgProcessor.h` | 12 | Width of the `cad_lock` zone |
| `dataGapSec` | `PpgProcessor.h` | 2.5 s | Hold-off after a FIFO / ambient-light overflow |
| `maxCalRestarts`, `autoTauSec`, `autoMaxGrow` | `MotionDetector.h` | 4, 60 s, 2x | Calibration fallback and auto-recalibration |
| `kMul`, floors | `MotionDetector.h` | 2.5 | Still vs moderate-motion threshold |

## Limitations

- Results are simulated. The most likely real-world difference is the motion artifact shape on the back: tune with the raw log + `replay`.
- A longer spectral window means a slower first lock and slower recovery after heavy motion (about 12 s of clean data).
- Not implemented: RLS / cascaded canceller, ML models, interrupt-driven FIFO reads (the INT pin is not wired; 5 ms polling is used).
- The ambient-light overflow detection reads an interrupt-status flag that I could not test without the hardware; check with `s` in strong light.

- When HR is within ~10 bpm of the step rate (or its harmonics), the spectral
  peak is hidden; the output follows the trend and may show 0 (unknown) for a while.
- The waveform during motion is not a clean PPG; use it for viewing only.
- Results are from simulation. Verify against a chest-strap HR monitor during real tasks.
