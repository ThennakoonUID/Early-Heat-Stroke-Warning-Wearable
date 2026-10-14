"""Turns one PROSPIE trial (default: subject 14, condition 1, from the EXTERNAL cohort A, never used for training) into a 2-s Firebase-style snapshot CSV for `service.py --replay`.
Adds the situations the live service must handle: HR null for 12 min (motion),
skin_temp = "ERR: no skin contact" for 2 min, and a 12-min device gap (new session)."""
import sys
import numpy as np, pandas as pd
sys.path.insert(0, ".")
import features as F

COLS = 52
def main(xlsx="data/prospie.xlsx", pid=14, cond=1, out="examples/replay_example.csv"):
    d = pd.read_excel(xlsx, sheet_name="Data", header=None, skiprows=2).iloc[:, :COLS].replace(9999, np.nan)
    g = d[(d[3] == pid) & (d[0] == cond)]
    tmin = g[1].apply(lambda t: t.hour * 60 + t.minute).to_numpy()
    hr, skin, tamb, rh = g[45].to_numpy(), g[41].to_numpy(), g[14].to_numpy(), g[13].to_numpy()
    t0 = pd.Timestamp("2026-10-10T08:00:00Z")
    rows = []
    for i, m in enumerate(tmin):
        if 70 <= m < 82:          # device off / out of range for 12 min -> new session afterwards
            continue
        for k in range(30):       # one snapshot every 2 s
            ts = t0 + pd.Timedelta(minutes=float(m)) + pd.Timedelta(seconds=2 * k + 1)
            h = None if (30 <= m < 42 or not np.isfinite(hr[i])) else int(round(hr[i]))
            s = "ERR: no skin contact" if 60 <= m < 62 else round(float(skin[i]), 1)
            rows.append({"timestamp": ts.isoformat(), "skin_temp": s, "outside_temp": round(float(tamb[i]), 1),
                         "feels_like_temp": round(float(F.heat_index_c(tamb[i], rh[i])), 1),
                         "heart_rate": h, "sweat_level": None})
    pd.DataFrame(rows).to_csv(out, index=False)
    t_true = [{"minute_utc": (t0 + pd.Timedelta(minutes=float(m))).isoformat(), "true_rectal_temp": g[29].to_numpy()[i]}
              for i, m in enumerate(tmin)]
    pd.DataFrame(t_true).to_csv(out.replace(".csv", "_truth.csv"), index=False)   # for checking only, not a model input
    print("wrote", out, len(rows), "snapshots")

if __name__ == "__main__":
    main()
