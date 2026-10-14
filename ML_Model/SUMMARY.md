# Summary – core temperature from Firebase vest data

**Final model:** LightGBM on 19 causal features from `heart_rate`, `skin_temp`, `outside_temp` and `feels_like_temp`
(5-min personal baseline, 5/10-min means and slopes, 10/30-min EMAs of HR above baseline, time in session).
**Alarm:** risk `high` when $\hat T_c \ge 38.25$ °C (caution from 38.0 °C). The threshold was chosen on out-of-fold predictions
for ≥ 80 % sensitivity, then confirmed on the external cohort.

## Data actually used
* **PROSPIE cohort B** (22 subjects, conditions 6–11, continuous chest-strap HR): training + nested leave-one-subject-out (LOSO).
  10,486 evaluated minutes, 1,348 with true $T_c \ge 38.5$ °C.
* **PROSPIE cohort A** (18 *different* subjects, conditions 1–3, HR spot-measured every ~5 min by finger PPG): **external test**, run once.
* **Firefighters dataset: not usable.** It contains core temperature every 8 min plus per-subject summaries only, with no HR, skin or ambient
  time series, so no model input can be built from it. Cohort A replaces it.

## Results (RMSE in °C)

| Model | LOSO RMSE | LOSO RMSE for $T_c\ge38$ | External RMSE | External bias |
|---|---|---|---|---|
| Constant | 0.533 | 0.614 | 0.572 | −0.05 |
| ECTemp (HR only) | 0.332 | 0.426 | 0.471 | −0.33 |
| Linear HR + $T_{sk}$ | 0.469 | 0.545 | 0.484 | −0.07 |
| Ridge, full features | 0.266 | 0.313 | **0.282** | −0.09 |
| **LightGBM, full features** | **0.252** | **0.305** | 0.285 | −0.06 |
| LightGBM + ECTemp | 0.252 | 0.304 | 0.301 | −0.08 |
| LightGBM, no HR (fallback) | 0.304 | 0.353 | 0.291 | +0.06 |

Final LightGBM, LOSO: MAE 0.20 °C, Bland–Altman limits −0.50 to +0.49 °C, 95 % of minutes within ±0.5 °C. No measurable lag (best alignment at 0 min).

**Alarm performance** (alarm at 38.25 °C, judged against true $T_c \ge 38.5$):

| | LOSO cohort B | External cohort A |
|---|---|---|
| Minute sensitivity / precision | 84 % / 49 % | 83 % / 56 % |
| Hot trials detected | 54 / 56 (96 %) | 34 / 35 (97 %) |
| Alarm timing vs true crossing | median 5 min early, worst 39 min late | median 1.5 min early, worst 18 min late |
| Trials that never reached 38.5 °C but alarmed | 57 % | 61 % |

Most of those "false alarms" are near misses: the median peak of non-hot trials was 38.24–38.28 °C.

## Does ML beat ECTemp?
**Yes, clearly.** LOSO RMSE falls from 0.33 to 0.25 °C (−24 %), and in the range that matters ($T_c \ge 38$) from 0.43 to 0.31 °C.
At the same 80 % sensitivity, precision is 49 % against ECTemp's 31 %. On the external cohort, ECTemp collapses
(RMSE 0.47, bias −0.33, because coarse finger-PPG HR is not what it was calibrated on), while the ML models hold at 0.28–0.29.
Ridge (0.266) is within the 0.05 °C simplicity margin, but LightGBM was kept because it gives better alarms
(precision 49 % vs 42 %, 96 % vs 93 % of hot trials detected) and is more robust when HR drops out in blocks.

## Other findings
* **Strongest feature:** the 30-min EMA of HR above the personal baseline (cumulative workload), then time in session, then 10-min mean skin temperature and HR.
* **HR dropouts:** with 30 % of HR minutes missing in 3–10 min blocks (like motion dropouts), RMSE rises 0.252 → 0.286.
  When HR is missing for more than 5 min, the no-HR model takes over; it did well externally.
* **Insulating the skin sensor helps:** using the foam-insulated C7–T2 thermistor instead of the bare upper-back site improved LOSO RMSE
  from 0.252 to 0.233 and sensitivity at 38.5 °C from 57 % to 63 %. **Hardware suggestion: cover the vest NTC with a small foam block.**
* Age, sex and body mass gave no improvement.
* **PSI:** the $T_c \ge 38.5$ and PSI ≥ 7 rules agree on 93 % of minutes. PSI from predicted $T_c$ (with $T_{c,0}$ = 37.0) has an RMSE of 0.67 PSI units.
* **Replay test of the service** (unseen external subject): RMSE 0.27 °C, and every hot minute with data was flagged `high`.
  It also showed that a 5-min session gap is unsafe: after a 12-min break the wearer was still at 38.4 °C, and a new baseline
  made the model read "normal" at 39.1 °C. **The session gap is therefore 60 min** (core temperature needs ~30–60 min to recover).

## Limitations
* **Under-reads at the top end:** about −0.1 °C at 38.5 °C, −0.4 °C at 39 °C and −0.8 °C near 39.5 °C. The lowered alarm threshold
  compensates, but the displayed number should not be trusted above ~39 °C.
* 22 training subjects, healthy, mostly treadmill walking in a chamber, sessions ≤ 2 h. Time in session is capped at 120 min.
* Skin-site mismatch: iButton on the upper back vs your NTC at C7 under a vest. Expect an offset until you calibrate on your own data.
* No GSR in training, so `sweat_level` is logged but not used.
* Firebase keeps no history and only fires on changes. The service timestamps 2-s snapshots itself; stale detection is "no change for 30 s".
* PROSPIE ambient is true chamber air; your SHT30 sits on the vest and will read warmer.
* This is a heat-strain early-warning aid, not a diagnostic device.

## Data to collect next
1. Your own sessions with the vest (the service already logs them) plus a **core reference**: ingestible pill, or at least a tympanic reading every 5 min.
2. Field-like work: intermittent tasks, outdoor sun, sessions longer than 2 h, rest breaks with the vest off.
3. Enough minutes above 38.5 °C to fix the high-end under-reading, and GSR alongside, so `sweat_level` can become a feature.
4. A comparison of the foam-insulated and bare NTC.
