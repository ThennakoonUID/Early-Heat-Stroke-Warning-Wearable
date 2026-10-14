# Heat-strain core temperature model (Firebase-fed)

Estimates core body temperature from what the vest's ESP32 uploads to Firebase, converts it to a
risk level and the Physiological Strain Index (PSI), and writes the result back to Firebase.

```
ESP32 ──PATCH every 2 s──► /devices/DEV-001/{skin_temp, outside_temp, feels_like_temp, heart_rate, sweat_level}
                                   │  (listen + 30 s catch-up read)
                                   ▼
                             service.py ── 2-s snapshots ─► 1-min medians ─► features.py ─► model
                                   │                                                        │
            data/live_readings.sqlite (raw history = future training data)                 ▼
                                   ◄──────── update() ── /devices/DEV-001/prediction ◄──────┘
```

## Files

| File | What it is |
|---|---|
| `heatstrain_model.ipynb` | Steps 0–6: data inspection, features, baselines, models, nested LOSO, robustness, external test, SHAP, plots. Executed, with outputs. |
| `features.py` | **Shared** feature code (imported by the notebook *and* the service), heat index, ECTemp, PSI, risk levels |
| `service.py` | Live Firebase inference service (and `--replay` mode for testing without the vest) |
| `models/core_temp_model.joblib` | Trained model bundle: model, feature list, no-HR fallback model, alarm thresholds, version |
| `results/*.csv` | Results tables (LOSO + external), robustness, experiments, per-subject RMSE, alarm comparison |
| `figures/*.png` | SHAP, example time series, Bland–Altman / error vs Tc / per-subject RMSE |
| `SUMMARY.md` | One-page summary of the results and limitations |
| `tools/make_replay_csv.py`, `examples/` | Builds a Firebase-style 2-s CSV from an external-cohort PROSPIE trial for `--replay`; `replay_output.csv` is the service output, `replay_example_truth.csv` the true rectal temperature for comparison |
| `config.example.json` | Service settings |

`data/` holds the two source spreadsheets (needed only to re-run the notebook).

## Run the notebook

```bash
pip install -r requirements.txt
jupyter notebook heatstrain_model.ipynb      # or: jupyter nbconvert --execute --to notebook --inplace heatstrain_model.ipynb
```
Takes ~6 min on a laptop (nested cross-validation). It rewrites `models/`, `results/` and `figures/`.

## Run the service

1. **Service-account key.** Firebase console → Project settings → *Service accounts* → *Generate new private key*.
   Save the JSON **outside** this folder (never commit it) and point an environment variable at it:
   ```bash
   export FIREBASE_CREDENTIALS=/path/to/heatstrockapp-service-account.json      # Windows: set FIREBASE_CREDENTIALS=...
   ```
2. `cp config.example.json config.json` and adjust if needed.
3. `python service.py --config config.json`

Every minute it logs one line and writes:

```json
/devices/DEV-001/prediction = {
  "core_temp_est": 38.1, "psi": 5.6,
  "risk_level": "normal | caution | high | warming_up | insufficient_data",
  "model_version": "...", "model_used": "<model> | no-HR fallback",
  "device_status": "online | stale", "updated_at": <server timestamp> }
```

**Test without the vest:**
```bash
python service.py --replay examples/replay_example.csv --out replay_out.csv
```

### Behaviour worth knowing

* **Firebase keeps only the latest values and has no timestamps.** The service timestamps everything itself: it
  snapshots the current values every 2 s, takes the median per minute, and stores both in `data/live_readings.sqlite`
  (`readings`, `events`, `predictions` tables). That database is your future training data, **including `sweat_level`**.
* **Firebase only fires a listen event when a value changes.** Identical PATCHes are invisible. So "no update for 30 s"
  means "nothing changed for 30 s", which is a good but not perfect stale-device signal. To make it exact, have the
  firmware add a changing field to its PATCH, e.g. a counter (`"seq": <FbPayload.seq>`). The service does not need any change for that.
* **Sessions:** a gap of more than **60 min** without valid HR or skin data ends a session. The next valid minute starts a new one,
  with a new 5-min personal baseline (`warming_up` meanwhile). Put the vest on and sit still for the first 5 minutes.
  The gap is 60 min, not 5, on purpose: core temperature needs ~30–60 min to recover, and re-baselining a person who is
  still hot after a short break made the model under-read by ~1 °C in the replay test.
* **Risk levels:** `high` when $\hat T_c \ge$ the alarm threshold stored in the model (38.25 °C; 38.15 °C for the no-HR model),
  `caution` from 38.0 °C. These sit below 38.5 °C on purpose: the model under-reads at the hot end, and the lower threshold catches
  ~83 % of truly hot minutes and ~97 % of hot trials (see `SUMMARY.md`).
* **Stale device:** no change event for 30 s → `device_status: "stale"` and no estimate (`insufficient_data`), so a vest that
  stopped sending never shows "normal".
* **Missing data:** `null` and `"ERR: no skin contact"` become NaN, never 0. Skin temperature missing for 3 min →
  `insufficient_data`. HR missing for more than 5 min → the skin-and-environment-only fallback model is used
  (`model_used: "no-HR fallback"`), or `insufficient_data` if you set `"hr_missing_policy": "insufficient"`.
* **Writes use `update()` on `/prediction`**, so the ESP32's five keys are never touched, and the service ignores its own writes.
* **Reconnects:** the connection is retried with back-off, a catch-up read every 30 s re-syncs values and restarts a dead listener.

### Firebase security
`kFirebaseAuth` in `Config.h` is empty, so the database currently accepts public writes (test mode). The service
authenticates with the service-account key, which bypasses rules, so you can lock the rules down without breaking it.
The ESP32 would then need its own token.
