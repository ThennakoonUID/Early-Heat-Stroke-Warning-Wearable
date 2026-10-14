"""
features.py - shared feature code for the heat-strain core-temperature model.

This ONE module is imported by both the training notebook and the live Firebase
service (service.py), so the model sees exactly the same features in training and
in deployment (no training/serving skew).

Input of build_features(): one SESSION (one continuous wear period / one trial),
one row per minute, in time order, with these columns (Firebase equivalents):

    t_min   minutes since the start of the session (0, 1, 2, ...)
    hr      heart rate, bpm                       <- heart_rate       (NaN = null)
    skin    skin temperature near C7, deg C       <- skin_temp        (NaN = null / "ERR: ...")
    amb     ambient air temperature, deg C        <- outside_temp     (NaN = null)
    hi      heat index ("feels like"), deg C      <- feels_like_temp  (NaN = null)

Every feature is CAUSAL: the value for minute t only uses minutes <= t.
Missing values stay NaN (never 0).
"""
from __future__ import annotations

import math

import numpy as np
import pandas as pd

FEATURE_VERSION = "1.0"

# ------------------------------- settings -----------------------------------
BASELINE_MIN = 5        # personal baseline (HR0, Tsk0) = mean of the first 5 minutes
HR_FFILL_LIMIT = 5      # a missing HR minute is carried forward for at most 5 min
SKIN_FFILL_LIMIT = 2    # skin temperature: 2 min
ENV_FFILL_LIMIT = 10    # ambient / heat index change slowly: 10 min
T_CLIP_MIN = 120        # time-in-session feature is capped (training trials last ~2 h)
HR_GAP_CAP = 60         # "minutes since last valid HR" is capped at 60

RAW_COLS = ["hr", "skin", "amb", "hi"]

# Feature sets ---------------------------------------------------------------
FEATURES_FULL = [
    "hr", "skin", "amb", "hi", "t_min",
    "dhr", "dsk",
    "hr_m5", "hr_m10", "sk_m5", "sk_m10",
    "hr_s5", "hr_s10", "sk_s5", "sk_s10",
    "dhr_ema10", "dhr_ema30",
    "sk_minus_amb", "hr_gap",
]
FEATURES_HYBRID = FEATURES_FULL + ["ectemp"]
FEATURES_NOHR = [  # skin + environment only (used when HR is unavailable)
    "skin", "amb", "hi", "t_min", "dsk",
    "sk_m5", "sk_m10", "sk_s5", "sk_s10", "sk_minus_amb",
]
FEATURES_LINEAR_BASELINE = ["hr", "skin"]


# ---------------------------- environment maths ------------------------------
def heat_index_c(tc, rh):
    """NWS heat index (Rothfusz regression), identical to heatIndexC() in the
    firmware (HeatRisk.h). Inputs deg C and %RH (scalars or arrays), output deg C."""
    tc = np.asarray(tc, dtype=float)
    rh = np.asarray(rh, dtype=float)
    t = tc * 9.0 / 5.0 + 32.0
    hi = 0.5 * (t + 61.0 + (t - 68.0) * 1.2 + rh * 0.094)
    full = (hi + t) * 0.5 >= 80.0
    reg = (-42.379 + 2.04901523 * t + 10.14333127 * rh - 0.22475541 * t * rh
           - 0.00683783 * t * t - 0.05481717 * rh * rh + 0.00122874 * t * t * rh
           + 0.00085282 * t * rh * rh - 0.00000199 * t * t * rh * rh)
    low = (rh < 13.0) & (t >= 80.0) & (t <= 112.0)
    adj_low = ((13.0 - rh) / 4.0) * np.sqrt(np.clip((17.0 - np.abs(t - 95.0)) / 17.0, 0, None))
    high = (rh > 85.0) & (t >= 80.0) & (t <= 87.0)
    adj_high = ((rh - 85.0) / 10.0) * ((87.0 - t) / 5.0)
    reg = np.where(low, reg - adj_low, np.where(high, reg + adj_high, reg))
    hi = np.where(full, reg, hi)
    out = (hi - 32.0) * 5.0 / 9.0
    return float(out) if out.ndim == 0 else out


def quantise(df: pd.DataFrame) -> pd.DataFrame:
    """Simulate what Firebase carries: temperatures with 1 decimal, HR as integer bpm."""
    out = df.copy()
    for c in ("skin", "amb", "hi"):
        if c in out:
            out[c] = out[c].astype(float).round(1)
    if "hr" in out:
        out["hr"] = out["hr"].astype(float).round(0)
    return out


# --------------------------------- ECTemp -----------------------------------
# Buller et al. 2013, Physiol. Meas. 34:781-798. Extended Kalman filter, 1-min steps.
EC_GAMMA = 0.022          # process noise std (deg C per minute)
EC_B0, EC_B1, EC_B2 = -7887.1, 384.4286, -4.5714   # HR = b2*CT^2 + b1*CT + b0
EC_SIGMA = 18.88          # observation noise std (bpm)
EC_CT0, EC_V0 = 37.1, 0.0


def ectemp(hr, ct0: float = EC_CT0, v0: float = EC_V0) -> np.ndarray:
    """Core temperature estimate from a 1-min HR sequence. A NaN HR minute gets the
    time update only (no measurement), so missing HR is handled causally."""
    hr = np.asarray(hr, dtype=float)
    out = np.empty_like(hr)
    ct, v = ct0, v0
    for i, h in enumerate(hr):
        ct_p, v_p = ct, v + EC_GAMMA ** 2          # time update
        if np.isfinite(h):
            c = 2.0 * EC_B2 * ct_p + EC_B1          # dHR/dCT at the prediction
            k = v_p * c / (c * c * v_p + EC_SIGMA ** 2)
            hr_exp = EC_B2 * ct_p ** 2 + EC_B1 * ct_p + EC_B0
            ct = ct_p + k * (h - hr_exp)
            v = (1.0 - k * c) * v_p
        else:
            ct, v = ct_p, v_p
        out[i] = ct
    return out


# ------------------------------ helper filters --------------------------------
def _causal_baseline(x: pd.Series, t_min: pd.Series, n: int = BASELINE_MIN) -> pd.Series:
    """Mean of the valid values in the first n minutes of the session. While the
    session is younger than n minutes it is the mean of what exists so far (causal)."""
    in_win = (t_min < n).to_numpy()
    vals = np.where(in_win, x.to_numpy(dtype=float), np.nan)
    s = pd.Series(vals, index=x.index)
    csum = s.fillna(0).cumsum()
    ccnt = s.notna().cumsum()
    base = csum / ccnt.replace(0, np.nan)
    return base.ffill()


def _rolling_slope(y: pd.Series, w: int, min_periods: int) -> pd.Series:
    """Least-squares slope (units per minute) over the past w minutes (causal),
    ignoring NaNs inside the window."""
    arr = y.to_numpy(dtype=float)
    out = np.full(len(arr), np.nan)
    for i in range(len(arr)):
        lo = max(0, i - w + 1)
        seg = arr[lo:i + 1]
        tt = np.arange(lo, i + 1, dtype=float)
        m = np.isfinite(seg)
        if m.sum() >= min_periods:
            ts, ys = tt[m], seg[m]
            tc = ts - ts.mean()
            den = (tc * tc).sum()
            if den > 0:
                out[i] = (tc * (ys - ys.mean())).sum() / den
    return pd.Series(out, index=y.index)


def _ema(x: pd.Series, tau_min: float) -> pd.Series:
    alpha = 1.0 - math.exp(-1.0 / tau_min)
    return x.ewm(alpha=alpha, adjust=False, ignore_na=True).mean()


def _minutes_since_valid(x: pd.Series, cap: int = HR_GAP_CAP) -> pd.Series:
    out, last = [], None
    for i, v in enumerate(x.to_numpy(dtype=float)):
        if np.isfinite(v):
            last = i
        out.append(cap if last is None else min(i - last, cap))
    return pd.Series(out, index=x.index, dtype=float)


# ------------------------------- main function --------------------------------
def build_features(session: pd.DataFrame, do_quantise: bool = True) -> pd.DataFrame:
    """Features for ONE session (rows = minutes in time order). Returns a DataFrame
    with every column of FEATURES_HYBRID plus 'hr0', 'sk0' and 'warming_up'."""
    x = session.reset_index(drop=True).copy()
    for c in RAW_COLS:
        if c not in x:
            x[c] = np.nan
    if do_quantise:
        x = quantise(x)
    raw_hr = x["hr"].astype(float)

    f = pd.DataFrame(index=x.index)
    f["t_min"] = x["t_min"].astype(float).clip(upper=T_CLIP_MIN)
    f["hr"] = raw_hr.ffill(limit=HR_FFILL_LIMIT)
    f["skin"] = x["skin"].astype(float).ffill(limit=SKIN_FFILL_LIMIT)
    f["amb"] = x["amb"].astype(float).ffill(limit=ENV_FFILL_LIMIT)
    f["hi"] = x["hi"].astype(float).ffill(limit=ENV_FFILL_LIMIT)

    f["hr0"] = _causal_baseline(raw_hr, x["t_min"])
    f["sk0"] = _causal_baseline(x["skin"].astype(float), x["t_min"])
    f["dhr"] = f["hr"] - f["hr0"]
    f["dsk"] = f["skin"] - f["sk0"]

    for w in (5, 10):
        mp = max(2, w // 2)
        f[f"hr_m{w}"] = f["hr"].rolling(w, min_periods=mp).mean()
        f[f"sk_m{w}"] = f["skin"].rolling(w, min_periods=mp).mean()
        f[f"hr_s{w}"] = _rolling_slope(f["hr"], w, mp)
        f[f"sk_s{w}"] = _rolling_slope(f["skin"], w, mp)

    f["dhr_ema10"] = _ema(f["dhr"], 10.0)
    f["dhr_ema30"] = _ema(f["dhr"], 30.0)
    f["sk_minus_amb"] = f["skin"] - f["amb"]
    f["hr_gap"] = _minutes_since_valid(raw_hr)
    f["ectemp"] = ectemp(raw_hr.to_numpy())
    f["warming_up"] = x["t_min"].astype(float) < BASELINE_MIN
    return f


def psi(tc, hr, tc0: float, hr0):
    """Physiological Strain Index (Moran et al. 1998), 0..10 scale (not clipped)."""
    return 5.0 * (np.asarray(tc) - tc0) / (39.5 - tc0) + \
        5.0 * (np.asarray(hr) - hr0) / (180.0 - np.asarray(hr0))


def risk_level(tc_hat: float, high_th: float = 38.5, caution_th: float = 38.0) -> str:
    """Risk level from predicted core temperature. The service passes the alarm thresholds
    stored in the model bundle (chosen in the notebook for >= 80 % sensitivity)."""
    if not np.isfinite(tc_hat):
        return "insufficient_data"
    if tc_hat >= high_th:
        return "high"
    if tc_hat >= caution_th:
        return "caution"
    return "normal"
